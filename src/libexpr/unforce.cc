/* Unforcing thunks between jobs.

   Thunk update in `forceValue` is the evaluator's only mutation of existing
   cells. While a job (an outermost unforce scope) runs, the pre-image of
   every *old* cell it forces is logged; at the end of the job each logged
   cell is either reverted to its pre-image, replaced by a derivation stub,
   or promoted (its forced value copied out of the job region). Afterwards no
   old cell points into the job region, which is then released wholesale. */

#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/value.hh"
#include "nix/util/environment-variables.hh"

#include <sys/mman.h>
#include <sys/resource.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <algorithm>
#include <atomic>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "expr-config-private.hh"

#if NIX_USE_BOEHMGC
#  include <gc/gc_mark.h>
#endif

#if NIX_UNFORCE_MIMALLOC
#  include <mimalloc.h>
#  include <mimalloc-stats.h>
#endif

namespace nix {

/* inc2c-1: per-worker bump region (initial-exec, constant-initialized -- see the
   extern declaration in eval.hh). `evalBaseArena` stays a plain global: it is the
   NIX_NOGC_BUMP debug allocator, which the EvalState ctor rejects with
   eval-cores > 1 (its cursor is unlocked and never thread-safe), and it holds a
   std::vector (non-trivial ctor) that would put a TLS-init guard on the alloc
   fallback -- so there is no reason to make it thread_local. */
[[gnu::tls_model("initial-exec")]] thread_local EvalRegion evalRegion;
BumpArena evalBaseArena;

/* inc2c-1: the per-worker undo buffer (declared in eval.hh as nix::undoLog).
   See EvalState::isolationDepth / the undoLog note in eval.hh. */
[[gnu::tls_model("initial-exec")]] thread_local EvalUndoLog undoLog;

const bool evalUseBaseArena = [] {
    auto s = getenv("NIX_NOGC_BUMP");
    return s && std::string_view(s) != "0";
}();

static const bool useHugePages = [] {
    auto s = getenv("NIX_NOGC_THP");
    return s && std::string_view(s) != "0";
}();

static void * mapArena(size_t n)
{
    auto p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        throw std::bad_alloc();
    if (useHugePages)
        madvise(p, n, MADV_HUGEPAGE);
    return p;
}

void * BumpArena::refill(size_t n)
{
    static constexpr size_t chunk = size_t(64) << 20;
    /* A large object gets its own mapping, so the current chunk stays. */
    if (n > chunk / 4) {
        auto p = (char *) mapArena(n);
        chunks.emplace_back(p, p + n);
        return p;
    }
    auto p = mapArena(chunk);
    cur = (char *) p;
    chunks.emplace_back(cur, cur + chunk);
    end = cur + chunk;
    return alloc(n);
}

namespace {

/* Whether unforcing happens at all is the `eval-unforce` setting, and
   `eval-unforce-batch-size` sizes its batches; those two are the only user
   settings. The fields below were experiments whose measured winners are now
   hardcoded; the only remaining NIX_UNFORCE_* variable is the single hidden
   developer dial `NIX_UNFORCE_DEBUG` (see `DebugDial`). */
struct Settings
{
    /* DECIDED winners (shipping): an all-types bump region holding the owner
       timeline (both now hardcoded, not fields), promoting a cell forced in
       two jobs, nominating consumers, and keeping printers' `seen` sets exact.
       The owner timeline closes each job's promotion set under "producer-of"
       (`decideJob`), which makes promotion pointer-equality-safe: a promoted
       cell keeps the address its holders already compared, so eqValues'
       `&v1 == &v2` short-cut still holds. At the 64 MiB default batch this is
       byte-identical to base, ~1.12x its instructions, with 0 pointer-equality
       flips on 3491 real Nixpkgs comparisons.
       Previously NIX_UNFORCE_PROMOTE, _OWNER_CONSUMERS, _SEEN. */
    uint32_t promoteAfter = 2;
    bool ownerConsumers = true;
    bool keepSeen = true;
    /* 0 derives these from the batch size (`eval-unforce-batch-size`):
       countsCap = max(256 Ki, batch / 256); ownerBudget and copyLimit =
       max(batch / 4, 4 MiB). Previously NIX_UNFORCE_COUNTS_CAP,
       _OWNER_BUDGET, _COPY_LIMIT. */
    uint64_t countsCap = 0;
    size_t ownerBudget = 0;
    size_t copyLimit = 0;

    /* The statistics summary is requested with NIX_SHOW_STATS, the same dial
       as the evaluator's own statistics (whose JSON already carries the
       unforce numbers); there is no separate NIX_UNFORCE_STATS. */
    bool stats = getEnv("NIX_SHOW_STATS").value_or("0") != "0";
};

const Settings & unforceSettings()
{
    static const Settings s;
    return s;
}

/* The single hidden developer dial. NIX_UNFORCE_DEBUG is a comma-separated
   list of flags, each enabling one cross-check or trace that used to have its
   own NIX_UNFORCE_* variable; all default off (unset means none). */
struct DebugDial
{
    bool verify = false;     /* was NIX_UNFORCE_VERIFY */
    bool verifyDump = false; /* was NIX_UNFORCE_VERIFY_DUMP */
    bool tripwire = false;   /* was NIX_UNFORCE_TRIPWIRE */
    bool failDecide = false; /* was NIX_UNFORCE_FAIL_DECIDE */
};

const DebugDial & unforceDebug()
{
    static const DebugDial d = [] {
        DebugDial r;
        auto spec = getEnv("NIX_UNFORCE_DEBUG").value_or("");
        for (size_t i = 0; i < spec.size();) {
            auto j = spec.find(',', i);
            if (j == std::string::npos)
                j = spec.size();
            auto tok = spec.substr(i, j - i);
            i = j + 1;
            if (tok.empty())
                continue;
            else if (tok == "verify")
                r.verify = true;
            else if (tok == "verify-dump")
                r.verifyDump = true;
            else if (tok == "tripwire")
                r.tripwire = true;
            else if (tok == "fail-decide")
                r.failDecide = true;
            else
                std::cerr << fmt("unforce: unknown NIX_UNFORCE_DEBUG flag '%s'\n", tok);
        }
        return r;
    }();
    return d;
}

/* Whether the per-force statistics counters are ever observed: only the
   NIX_SHOW_STATS summary and JSON dump read them, so when stats are off the
   hot in-job force path skips them entirely. */
static const bool statsObserved = unforceSettings().stats;

/* The tripwire keeps every released region reserved, so it uses smaller
   ones: 64 GiB regions would exhaust a 48-bit address space after about
   4000 jobs. */
size_t regionSize()
{
    return unforceDebug().tripwire ? size_t(4) << 30 : size_t(64) << 30;
}

/* ---------------------------------------------------------------------------
   Striped arena (parallel-eval increment 0).

   The reserved value arena is subdivided into `nStripes` equal power-of-two
   stripes, one per worker thread, carved from a single MAP_NORESERVE
   reservation `stripesBase`. Single-threaded evaluation runs ONE worker in ONE
   active stripe, so the layout is behaviour-identical to the single bump arena
   it replaces (with one stripe the reservation is exactly `regionSize()` bytes,
   as the pre-striping per-job mapping was). The striping exists so the
   owner-timeline clock can be TWO-LEVEL -- `(arena_id, offset)`, the high and
   low halves of one `p - stripesBase` subtraction -- which decouples a worker's
   allocation timeline from the single global address order ahead of any
   concurrency. One stripe spans a whole 64 GiB region, so the within-stripe
   offset `(p - evalRegion.base) >> 4` is exactly the pre-striping clock. See
   notes/research/PARALLEL-EVAL-UNFORCING.md Section 6.C.

   The HIGH level is `EvalRegion::arenaId`, a cold field set when the region
   opens; the LOW level is the within-stripe offset that `ownerOf`/`segStart`/
   `setOwner` already compute (one subtraction, one shift) -- the hot path is
   unchanged. (The trimmed base deleted the vestigial `CellClock` struct, which
   only ever named the raw address and was never read; there is no standalone
   clock object to repackage. The two-level clock is expressed entirely by
   `arenaId` plus that offset arithmetic.)

   The nursery recycles within the active stripe: a job allocates above the
   stripe's kept floor and `releaseRegion` returns `[floor, cur)` to the kernel
   (MADV_DONTNEED, which re-zeroes it), so a later job reuses the same
   addresses -- the per-job owner timeline keys on offsets, so address reuse
   does not change any decision. Kept jobs raise the floor; their objects stay
   in place below it. `releaseRegion`/`retainRegion` are unchanged: the
   per-stripe recycling falls out of the existing kept-mapping floor branch.

   Not used under the `tripwire` debug dial, which keeps per-job standalone
   mappings so a missed escape faults on freed addresses rather than reading
   reused memory; that path is unchanged. --------------------------------------
*/
static constexpr size_t stripeShift = 36; /* 64 GiB per stripe */
static constexpr size_t stripeBytes = size_t(1) << stripeShift;
static constexpr uintptr_t stripeMask = stripeBytes - 1;

/* Reserved worker stripes, one per worker thread. inc2c-2 binds this to the
   eval's `eval-cores` at the first scope open (`bindStripeCount`): at
   eval-cores == 1 it is 1 (a single 64 GiB reservation, exactly as the
   pre-striping per-job mapping), and the cores > 1 case is reserved for 2c-3
   (the EvalState ctor still rejects eval-cores > 1 with unforce until then). A
   dev override (`NIX_UNFORCE_STRIPES`), if set, wins and is fixed at static
   init. `nStripes == 0` means "not yet bound": its only reader is `stripeBase`,
   which is not reached until a scope opens (after `bindStripeCount`). */
static const size_t stripeOverride = [] {
    auto s = getenv("NIX_UNFORCE_STRIPES");
    size_t n = s ? strtoul(s, nullptr, 10) : 0;
    return n ? std::max<size_t>(1, n) : size_t(0);
}();
static size_t nStripes = stripeOverride;

/* Bind `nStripes` to this eval's `eval-cores` at the first scope open, unless
   the dev override already fixed it. Idempotent (the bound value is the same
   for every worker of one eval, and the reservation is made once, after this).
   NOTE for 2c-3: with `evalCores` workers plus the main thread there are up to
   `evalCores + 1` forcing threads (ids [0, evalCores]); giving the main thread
   its own stripe therefore needs `nStripes >= evalCores + 1` (or sharing stripe
   0). Moot in 2c-2 (eval-cores == 1 while unforce is on), flagged for 2c-3. */
static void bindStripeCount(size_t evalCores)
{
    if (!stripeOverride)
        nStripes = std::max<size_t>(1, evalCores);
}

/* The single reserved striped mapping; stripe `id` is at base + id*stripeBytes. */
static char * stripesBase = nullptr;

/* inc2c-1/2c-2: this worker's stripe. Per-worker (initial-exec,
   constant-initialized); bound to `myEvalThreadId` at the first scope open
   (`beginUnforceScope`). At eval-cores == 1 the one (main) worker has
   `myEvalThreadId == 0`, so this is 0 -- the whole arena, exactly as the former
   `static const activeStripe`. */
[[gnu::tls_model("initial-exec")]] static thread_local uint32_t myStripe = 0;

/* Whether allocation comes from the striped reservation (always, except under
   the tripwire dial). */
static bool useStripes()
{
    return !unforceDebug().tripwire;
}

/* The base of stripe `id`, reserving the striped mapping on first use. With
   one stripe this reserves exactly `regionSize()` (64 GiB), as the pre-striping
   per-job mapping did. */
static char * stripeBase(size_t id)
{
    if (!stripesBase) {
        assert(stripeBytes == regionSize());
        /* `bindStripeCount` runs before the first scope's `openRegion` reaches
           here, so the count is bound (>= 1). */
        assert(nStripes >= 1 && id < nStripes);
        stripesBase = (char *) mapArena(nStripes << stripeShift);
    }
    return stripesBase + (id << stripeShift);
}

struct Stats
{
    size_t jobs = 0, logged = 0, reverted = 0, stubbed = 0, promoted = 0, copiedBytes = 0, regionPeak = 0;
    /* Targets logged in a job that an earlier job had forced and reverted. */
    size_t reforced = 0;
    /* Job memory released, summed over jobs. */
    size_t freedBytes = 0;
    /* The owner closure: promotions it added, and jobs whose closure
       changed anything. */
    size_t markerPromoted = 0, markerJobs = 0;
    /* The force counts: the most entries they held, and how often they
       were rebuilt from one job's targets for outgrowing their cap. */
    size_t countsPeak = 0, countsResets = 0;
    /* Counted targets on the thread's stack (stack slots the job restores),
       whose addresses a later job may reuse for another cell (P5). */
    size_t stackTargets = 0;
    /* Jobs that promoted nothing because client code ran in them, and the
       cells reverted at their ends. */
    size_t clientJobs = 0, revertedInClientJobs = 0;
    /* `UnforceSeen`: targets nominated because a printer printed the
       container they hold, and printers' `seen` entries rewritten to a
       copy's address at job ends. No entry is ever dropped: a job that
       would drop one keeps all its memory instead (`keptJobs`, and the
       bytes those jobs used, `keptJobBytes`, which is what they retain:
       kept jobs share one mapping). */
    size_t seenNominated = 0, seenKept = 0, keptJobs = 0, keptJobBytes = 0;
    /* Owner timeline: forces of young cells outside the current segment
       (each looks up its owner), those that changed the owner, the most
       segments one job had, owner lookups at job ends (one per copied or
       marked identity-bearing object), jobs whose closure added promotions,
       and consumers nominated. */
    size_t ownerLookups = 0, ownerChanges = 0, ownerSegsPeak = 0, ownerJobLookups = 0, ownerJobs = 0,
           ownerConsumerNominated = 0, ownerOverflowJobs = 0, ownerConsumerPairsPeak = 0, ownerCopyLimitJobs = 0;
};
/* inc2c-1: per-worker counters (incremented on the warm owner-timeline hooks
   and at job ends, never on the inlined hot path). initial-exec +
   constant-initialized. At eval-cores == 1 one worker accumulates everything,
   so the NIX_SHOW_STATS/NIX_UNFORCE_STATS counters are unchanged; summing
   across workers at teardown is inc2c-3's step. */
[[gnu::tls_model("initial-exec")]] thread_local Stats stats;

/* This thread's stack, [low, high). */
static thread_local const std::pair<const char *, const char *> threadStack = [] {
    pthread_attr_t attr;
    void * addr = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
    }
    return std::pair{(const char *) addr, (const char *) addr + size};
}();

static bool onThreadStack(const void * p)
{
    return threadStack.first && (const char *) p >= threadStack.first && (const char *) p < threadStack.second;
}

/* Whether client code ran in the open job (`unforceNoteClientCode`). Per
   thread, like the scope marks that `unforceNoteClientCode` tests: a C-API
   host may call client code on several threads, none of which has a job
   open. */
thread_local bool clientCodeInJob = false;

/* With NIX_SHOW_STATS: print a summary of all scopes when the process
   exits. */
static void printStatsAtExit()
{
    [[maybe_unused]] static bool registered =
        (std::atexit([] {
             std::cerr << fmt(
                 "unforce: %s %d, logged %d, reverted %d, stubbed %d, promoted %d, copied %d MiB, "
                 "largest scope %d MiB; reforced %d, freed %d MiB; owner closure: promoted %d, jobs %d; "
                 "counts peak %d, counts resets %d, stack targets %d; client-code jobs %d, reverted in client-code jobs %d; "
                 "seen nominated %d, kept %d; jobs kept whole %d (%d KiB)\n",
                 "jobs",
                 stats.jobs,
                 stats.logged,
                 stats.reverted,
                 stats.stubbed,
                 stats.promoted,
                 stats.copiedBytes >> 20,
                 stats.regionPeak >> 20,
                 stats.reforced,
                 stats.freedBytes >> 20,
                 stats.markerPromoted,
                 stats.markerJobs,
                 stats.countsPeak,
                 stats.countsResets,
                 stats.stackTargets,
                 stats.clientJobs,
                 stats.revertedInClientJobs,
                 stats.seenNominated,
                 stats.seenKept,
                 stats.keptJobs,
                 stats.keptJobBytes >> 10);
         }),
         true);
}

/* ---------------------------------------------------------------------------
   The owner timeline (bump backend, identity = owners).

   owner(x) of a young object x (the policy comment below)
   is the first old cell on its creator chain: the target whose re-force is
   the only thing that can build x again, or none (the driver). It is a
   function of allocation time: at any moment one force is the innermost
   *owner activation*, and everything allocated then has its owner.
   - A logged force of an old cell t makes t the current owner until it
     returns or throws.
   - A force of a young cell c makes owner(c) the current owner until it
     returns or throws: what c's evaluation builds is built again only by
     re-forcing the target that rebuilds c.
   - Any other force (a stack slot, a cell of the current segment) keeps the
     owner.
   Owner changes cut the job's bump range [base, cur) into segments with one
   owner each, in address order, because the bump cursor is the allocation
   clock. The open segment is [ownerSeg, cur), owned by `curOwner`;
   `ownerSegs` holds the closed ones below it. Adjacent segments never have
   the same owner: a change with nothing allocated since the open segment
   began touches no closed segment, and a change back to the owner of the
   segment just below takes that segment back. Owners are named by the log
   index of their logged force (`noOwner`: the driver).

   The hooks keep markers' inline shape (one test of
   `isolationDepth`, one cold out-of-line call); every test below runs in
   the out-of-line part. A force that pushes
   something (a logged force, or a young force that changed the owner)
   pushes `v` on `opens`, and its end pops it when the top is `v`'s. An end
   is matched by the innermost frame that sees its cell on top: an infinite
   recursion's inner frame may pop the outer one's entry first, which is
   harmless because nothing is allocated in the region between the two
   frames' ends, and the error cannot be caught in the language.

   Memory. The closed segments, 8 bytes
   each, live in a reserved mapping, so they never move or reallocate; the
   consumer pairs are a set, so a pair costs once. Together they stay
   within the timeline's budget (a quarter of the batch size, at least
   `ownerBudgetMin`; the rest of the batch is for the machinery that every
   unforcing job has): three quarters for the segments, one for the pairs. Both are charged to the
   batch (`unforceOwnerBytes`). A job that would exceed the budget stops
   tracking owners and is kept whole (`ownersOverflowed`), which is exact:
   nothing is reverted. Updates are reserved first, so a hook that fails to
   allocate has changed nothing (N2).

   At the job's end owner(x) is exact for every copied object, so the
   closure needs no walk: promote S0, look up the owner of every
   identity-bearing object the Evacuator copies, promote it if it is a
   reverted target, until nothing changes (`OwnerClosure`).
   --------------------------------------------------------------------------- */

/* A closed segment: its start, in 16-byte units from the region's base
   (allocations are 16-byte aligned, and the region is 64 GiB), and its
   owner's log index. */
struct OwnerSeg
{
    uint32_t off;
    uint32_t owner;
};

static_assert(sizeof(OwnerSeg) == 8);

static constexpr uint32_t noOwner = UINT32_MAX;
/* Below every closed segment: a start no young address reaches before
   `ownerSeg`, and an owner no force has. */
static constexpr OwnerSeg segSentinel{UINT32_MAX, UINT32_MAX - 1};
static constexpr size_t ownerBudgetMin = 4 << 20;

/* inc2c-1: the owner-timeline state below is per-worker (`thread_local`), so
   each worker tracks owners on its own stripe with no cross-worker contention.
   They use the initial-exec TLS model: a single TP-relative load, not the
   general-dynamic `__tls_get_addr` call the default model would pay (valid
   because libnixexpr is linked at startup, not dlopen'd). The scalar statics
   are constant-initialized (no TLS-init guard); the few with non-trivial
   initializers (`ownerSegs`, `opens`, `openLogIndices`, `ownerConsumers`) are
   dynamically initialized, but every access is on a WARM out-of-line symbol
   (`setOwner`, `logPreImageSlow`, `endLoggedForceSlow`, ...), never on the
   inlined `forceValue`/alloc path, so the TLS-init guard cannot reach a
   stack-parity-critical frame. At eval-cores == 1 there is one worker, so all
   of this is a pure relocation of where the state lives. The owner timeline is
   defined with the scope-admission comment near `beginUnforceScope` below. */

/* The closed segments: a reserved mapping, so pushes never move them or
   allocate (`limit`: the budget). Until a job maps it, and whenever owners
   are off, it is `segBottom`: the sentinel alone. Shared read-only across
   workers (the "owners off" sentinel is never written while `b == segBottom`),
   so it stays a plain static. */
static OwnerSeg segBottom[1] = {segSentinel};

struct SegStack
{
    OwnerSeg *b = segBottom, *t = segBottom + 1, *limit = segBottom + 1, *high = segBottom + 1;
    /* The reserved mapping, kept across jobs. */
    OwnerSeg * map = nullptr;
    size_t mapped = 0;

    /* Start a job's timeline in a mapping of `bytes`: the sentinel, and
       room up to `bytes`. False if the mapping fails. */
    bool open(size_t bytes)
    {
        if (mapped < bytes) {
            bottom();
            if (map)
                munmap(map, mapped);
            map = nullptr;
            mapped = 0;
            auto p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p == MAP_FAILED)
                return false;
            map = (OwnerSeg *) p;
            mapped = bytes;
        }
        b = map;
        b[0] = segSentinel;
        t = high = b + 1;
        /* Three quarters of the budget; the consumer pairs get the rest. */
        limit = b + bytes / 4 * 3 / sizeof(OwnerSeg);
        return true;
    }

    /* The sentinel alone (owners off). */
    void bottom()
    {
        b = segBottom;
        t = high = limit = segBottom + 1;
    }

    size_t usedBytes() const
    {
        return b == map ? (char *) high - (char *) b : 0;
    }

    /* Return what a job touched to the kernel, if it is more than a little,
       and go back to the sentinel alone. */
    void release()
    {
        auto used = usedBytes() & ~size_t(4095);
        if (used > (1 << 20))
            madvise(map, used, MADV_DONTNEED);
        bottom();
    }
};

[[gnu::tls_model("initial-exec")]] thread_local SegStack ownerSegs;
[[gnu::tls_model("initial-exec")]] thread_local char * ownerSeg = nullptr;
[[gnu::tls_model("initial-exec")]] thread_local uint32_t curOwner = noOwner;
[[gnu::tls_model("initial-exec")]] thread_local bool ownersOn = false;
/* This job's timeline ran out of budget: keep the job whole. */
[[gnu::tls_model("initial-exec")]] thread_local bool ownersOverflowed = false;
[[gnu::tls_model("initial-exec")]] thread_local bool ownerConsumersOn = false;
[[gnu::tls_model("initial-exec")]] thread_local size_t ownerBudget = 0;
/* The undo log, whose indices name owners. */
[[gnu::tls_model("initial-exec")]] thread_local const EvalUndoLog * ownerLog = nullptr;

/* A stack whose push is a compare and a store, for the hooks. `reserve`
   before an update that must not fail halfway. */
template<typename T>
struct HookStack
{
    T *b = nullptr, *t = nullptr, *e = nullptr;

    [[gnu::noinline]] void grow()
    {
        size_t n = t - b, cap = std::max<size_t>(64, 2 * (e - b));
        auto nb = (T *) std::realloc(b, cap * sizeof(T));
        if (!nb)
            throw std::bad_alloc();
        b = nb;
        t = nb + n;
        e = nb + cap;
    }

    /* Room for one more push, or `bad_alloc` with nothing changed. */
    [[gnu::always_inline]] void reserve()
    {
        if (t == e) [[unlikely]]
            grow();
    }

    /* Free the storage if it holds more than `keep` entries, so that one
       large job's high-water mark does not stay allocated for the rest of
       the run. The caller then refills it. */
    void shrink(size_t keep)
    {
        if (size_t(e - b) > keep) {
            std::free(b);
            b = t = e = nullptr;
        }
    }

    void pop() { --t; }
    T & back() const { return t[-1]; }
    bool empty() const { return t == b; }
    size_t size() const { return t - b; }
    void clear() { t = b; }
};

/* Forces that pushed something, innermost last: a logged force, or a young
   force that changed the owner. `saved` is the owner to restore, shifted
   left by one, with bit 0 set for a logged force (whose owner changed iff
   `ownersOn` when it started). The bottom entry is a sentinel whose cell is
   null. Bounded by the force depth. */
struct OpenForce
{
    const Value * cell;
    uint64_t saved;
};

static_assert(std::is_trivially_copyable_v<OpenForce>);
[[gnu::tls_model("initial-exec")]] thread_local HookStack<OpenForce> opens = [] {
    HookStack<OpenForce> s;
    s.grow();
    *s.t++ = {nullptr, 0};
    return s;
}();
/* The log index of each logged force on `opens`, innermost last. */
[[gnu::tls_model("initial-exec")]] thread_local HookStack<size_t> openLogIndices;

static bool opensEmpty()
{
    return opens.size() <= 1;
}

/* What `opens` keeps allocated between jobs: 64 Ki entries, 1 MiB. */
static constexpr size_t ownerKeep = 64 * 1024;

static void clearOpens()
{
    opens.shrink(ownerKeep);
    openLogIndices.shrink(ownerKeep);
    opens.clear();
    opens.reserve();
    *opens.t++ = {nullptr, 0};
    openLogIndices.clear();
}

/* (consumer, producer) owner pairs: the target that was the owner when a
   young cell of another target was forced (the `ownerConsumers` winner),
   each once. Open addressing; `empty` marks a free slot (no pair has the
   driver as consumer). */
struct PairSet
{
    static constexpr uint64_t empty = UINT64_MAX;
    std::vector<uint64_t> slots;
    size_t count = 0;

    static size_t hash(uint64_t k)
    {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        return k;
    }

    bool contains(uint64_t k) const
    {
        if (slots.empty())
            return false;
        auto mask = slots.size() - 1;
        for (auto i = hash(k) & mask;; i = (i + 1) & mask) {
            if (slots[i] == k)
                return true;
            if (slots[i] == empty)
                return false;
        }
    }

    /* Insert, if the table can hold it within `maxBytes`. False if it
       cannot (the pair is then not recorded). May throw `bad_alloc`, with
       nothing changed. */
    bool insert(uint64_t k, size_t maxBytes)
    {
        if (2 * (count + 1) > slots.size()) {
            auto n = std::max<size_t>(64, 2 * slots.size());
            if (n * sizeof(uint64_t) > maxBytes)
                return false;
            std::vector<uint64_t> bigger(n, empty);
            for (auto s : slots)
                if (s != empty)
                    place(bigger, s);
            slots.swap(bigger);
        }
        if (place(slots, k))
            count++;
        return true;
    }

    static bool place(std::vector<uint64_t> & slots, uint64_t k)
    {
        auto mask = slots.size() - 1;
        for (auto i = hash(k) & mask;; i = (i + 1) & mask) {
            if (slots[i] == k)
                return false;
            if (slots[i] == empty) {
                slots[i] = k;
                return true;
            }
        }
    }

    size_t bytes() const
    {
        return slots.size() * sizeof(uint64_t);
    }

    void clear()
    {
        if (bytes() > (1 << 20))
            slots = {};
        else
            std::fill(slots.begin(), slots.end(), empty);
        count = 0;
    }
};

[[gnu::tls_model("initial-exec")]] thread_local PairSet ownerConsumers;
/* The last pair recorded, which most consumer forces repeat. */
[[gnu::tls_model("initial-exec")]] thread_local uint64_t lastConsumerPair = PairSet::empty;

/* Bytes of this job's timeline, for the batch's bookkeeping. */
size_t unforceOwnerBytes()
{
    return ownerSegs.usedBytes() + ownerConsumers.bytes();
}

/* The start address a segment offset names. The offset is the within-stripe
   granule of the active region's base; `arena_id` (`evalRegion.arenaId`) is
   implicit -- every segment of an open job is in the one active stripe. */
[[gnu::always_inline]] inline static char * segStart(const OwnerSeg & s)
{
    return evalRegion.base + (size_t(s.off) << 4);
}

/* The timeline ran out of budget: stop tracking owners for the rest of the
   job, which is then kept whole. Every young cell is then in the open
   segment, so the hooks leave it alone. */
[[gnu::noinline, gnu::cold]] static void overflowOwners()
{
    ownersOn = false;
    ownersOverflowed = true;
    ownerSeg = evalRegion.base;
    stats.ownerOverflowJobs++;
    stats.ownerSegsPeak = std::max(stats.ownerSegsPeak, size_t(ownerSegs.high - ownerSegs.b));
    stats.ownerConsumerPairsPeak = std::max(stats.ownerConsumerPairsPeak, ownerConsumers.count);
    /* What the timeline touched goes back now, not at the job's end. */
    ownerSegs.release();
    ownerConsumers.clear();
}

static void setOwner(uint32_t o);

/* `setOwner` for the hooks' common paths: the same, except that a full
   segment stack is left to `setOwner` (a tail call), so that the hooks
   need no stack frame. Never throws. */
[[gnu::always_inline]] inline static void setOwnerLast(uint32_t o)
{
    auto c = evalRegion.cur;
    if (c == ownerSeg) [[likely]] {
        /* Nothing was allocated under the current owner since it began. */
        if (ownerSegs.t[-1].owner == o) {
            ownerSeg = segStart(ownerSegs.t[-1]);
            --ownerSegs.t;
        }
    } else {
        if (ownerSegs.t == ownerSegs.limit) [[unlikely]]
            return setOwner(o);
        *ownerSegs.t++ = {uint32_t((ownerSeg - evalRegion.base) >> 4), curOwner};
        if (ownerSegs.t > ownerSegs.high) [[unlikely]]
            ownerSegs.high = ownerSegs.t;
        ownerSeg = c;
    }
    curOwner = o;
}

/* `o` differs from `curOwner` at every call (a young force that changes
   the owner, the start of a logged force of `o`, or the end of either). If
   it did not, the timeline would only gain a needless boundary. */
[[gnu::noinline]] static void setOwner(uint32_t o)
{
    if (evalRegion.cur != ownerSeg && ownerSegs.t == ownerSegs.limit) [[unlikely]]
        return overflowOwners();
    setOwnerLast(o);
}

/* The owner of the young object at `p` (an address in the active stripe's open
   region [base, cur)). `p`'s two-level clock is `(arena_id, offset)`: the high
   level `arena_id = evalRegion.arenaId` is constant across a single-threaded
   job (every young cell is in the one active stripe), so it is not needed to
   disambiguate cells here -- the segment map is per active stripe. The low
   level, the within-stripe offset `(p - evalRegion.base) >> 4`, is what the
   map keys on. Hot path unchanged from the pre-striping clock: one
   subtraction, one shift. */
[[gnu::tls_model("initial-exec")]] thread_local size_t ownerHint = 0;

[[gnu::always_inline]] inline static uint32_t ownerOf(const void * p)
{
    auto a = (const char *) p;
    if (a >= ownerSeg)
        return curOwner;
    auto off = uint32_t((a - evalRegion.base) >> 4);
    /* Lookups come in runs (the Evacuator copies depth first). The
       sentinel is entry 0. */
    size_t n = ownerSegs.t - ownerSegs.b;
    auto s = ownerSegs.b;
    if (auto h = ownerHint; h >= 1 && h < n && s[h].off <= off && (h + 1 == n || off < s[h + 1].off))
        return s[h].owner;
    auto i = std::upper_bound(s + 1, s + n, off, [](uint32_t off, const OwnerSeg & s) { return off < s.off; });
    assert(i != s + 1);
    ownerHint = i - s - 1;
    return i[-1].owner;
}

/* The cell an owner names. */
static const Value * ownerCell(uint32_t o)
{
    return (*ownerLog)[o].target;
}

/* A job opens: everything is the driver's until a force says otherwise. */
static void openOwners(EvalState & state)
{
    auto & cfg = unforceSettings();
    ownersOn = evalRegion.cur != nullptr;
    ownersOverflowed = false;
    ownerConsumersOn = cfg.ownerConsumers;
    ownerConsumers.clear();
    lastConsumerPair = PairSet::empty;
    curOwner = noOwner;
    ownerHint = 0;
    ownerLog = &undoLog;
    ownerSeg = evalRegion.base;
    ownerSegs.bottom();
    if (!ownersOn)
        return;
    ownerBudget = cfg.ownerBudget ? cfg.ownerBudget : std::max<size_t>(state.settings.unforceBatchSize / 4, ownerBudgetMin);
    ownerBudget = std::max<size_t>((ownerBudget + 4095) & ~size_t(4095), 4096);
    if (!ownerSegs.open(ownerBudget)) {
        /* No timeline: keep the job whole. */
        ownersOn = false;
        ownersOverflowed = true;
    }
}

static void closeOwners()
{
    ownersOn = false;
    if (ownerSegs.high > ownerSegs.b)
        stats.ownerSegsPeak = std::max(stats.ownerSegsPeak, size_t(ownerSegs.high - ownerSegs.b));
    stats.ownerConsumerPairsPeak = std::max(stats.ownerConsumerPairsPeak, ownerConsumers.count);
    ownerSegs.release();
    ownerConsumers.clear();
    clearOpens();
    curOwner = noOwner;
}

/* Drop the `opens` entries of running forces logged at or after `mark`
   (none should be; logged ones are pushed whatever the identity mode, so
   that `endLoggedForce` finds them). */
static void dropOpenForces(size_t mark)
{
    while (!opensEmpty() && (!(opens.back().saved & 1) || openLogIndices.back() >= mark)) {
        if (opens.back().saved & 1)
            openLogIndices.pop();
        opens.pop();
    }
}

#if NIX_USE_BOEHMGC
GC_push_other_roots_proc prevPushOtherRoots = nullptr;

/* Objects outside the region that only the region refers to must survive
   collections during the job. */
void pushRegionRoots()
{
    if (prevPushOtherRoots)
        prevPushOtherRoots();
    if (evalRegion.cur)
        GC_push_all(evalRegion.base, evalRegion.cur);
}
#endif

void hookCollector()
{
#if NIX_USE_BOEHMGC
    static bool hooked = [] {
        prevPushOtherRoots = GC_get_push_other_roots();
        GC_set_push_other_roots(pushRegionRoots);
        return true;
    }();
    (void) hooked;
#endif
}

static size_t pageSize()
{
    static const size_t page = sysconf(_SC_PAGESIZE);
    return page;
}

static char * pageUp(char * p)
{
    return (char *) (((uintptr_t) p + pageSize() - 1) & ~(uintptr_t) (pageSize() - 1));
}

/* ---------------------------------------------------------------------------
   Per-task unforcing state (parallel-eval increment 1).

   The unforcing machinery keeps about a dozen groups of mutable state that
   are logically PER-TASK (per outermost eval job), not truly global: under a
   future thread-parallel evaluator each task would get its own copy. This
   context collects the COLD per-task groups -- those touched only at job
   boundaries, promotion and reclamation, and NEVER on the inlined
   force/alloc hot path -- into one object. There is EXACTLY ONE instance
   today (`rootTask`, the single root task), so moving a global in here is a
   pure rename and single-threaded behaviour is byte-identical.

   inc2c-1 made the hot owner-timeline state per-worker (`thread_local`,
   initial-exec) -- `evalRegion`, the owner-timeline statics (`ownerSegs`,
   `ownerSeg`, `curOwner`, `ownersOn`, `ownersOverflowed`, `ownerConsumersOn`,
   `ownerBudget`, `ownerLog`, `ownerHint`, `ownerConsumers`, `lastConsumerPair`),
   `opens`/`openLogIndices`, the undo buffer (`nix::undoLog`), `stats` and
   `isolationDepth`/`myStripe`. Still LEFT global in inc2c-1 (each for a reason
   that does not bite at eval-cores == 1):
     - `EvalMemory::stats` (nrValues/nrEnvs/...) -- an EvalMemory MEMBER read by
       allocValue/allocEnv/allocBytes; moving it would add a TLS load to the
       inlined alloc path for no eval-cores == 1 benefit (per-worker/summed in
       inc2c-3 when threads turn on). Not a NIX_SHOW_STATS owner counter.
     - `evalBaseArena` / `evalUseBaseArena` -- the NIX_NOGC_BUMP debug arena,
       rejected with eval-cores > 1 by the EvalState ctor; a std::vector member
       would put a TLS-init guard on the alloc fallback.
     - `allocatorErrors` -- a process-global mimalloc error counter, not
       per-task.
   inc2c-2 DELETED `jobOwner` (the process-global single-job arbiter + panic):
   per-worker disjoint stripes make it moot. Admission is per-worker lazy (the
   first force on a worker binds its stripe + timeline; `beginUnforceScope`).
   `EvalMemory::stats` and the per-worker `stats` merge remain for inc2c-3.
   See notes/research/PARALLEL-EVAL-INC2C-DESIGN.md Sec 1/6.
*/
struct TaskUnforceState
{
    /* Nursery / region recycling. Kept jobs (`retainRegion`) share one
       mapping: [keptStart, keptFloor) holds their objects, which are
       ordinary memory from then on, and the jobs after them allocate above
       it, up to keptEnd, so a kept job costs no page or mapping of its own. */
    char * keptStart = nullptr;
    char * keptFloor = nullptr;
    char * keptEnd = nullptr;

    /* The side table of Env sizes (`EvalRegion::envSizes`) of the bump
       region's mapping `envSizeMapping`, and the sizes too large for it
       (`bigEnvSizes`, the one unlocked side map the note flags). */
    uint8_t * envSizeTable = nullptr;
    const char * envSizeMapping = nullptr;
    std::unordered_map<const void *, size_t> bigEnvSizes;

    /* Monotonic job counter, one tick per task at its join. Its cross-task
       ordering is an open decision for a later increment; today it is a
       single per-task counter, exactly as the global it replaces. */
    size_t generation = 0;

    /* inc2c-2: Inc3 escape scaffolding. A cell this task published while another
       task was awaiting it (a cross-thread consumer) escapes this task's private
       foam and must be PINNED at the join, not reverted. The signal is
       `old & awaitedBit` at the publish (Value::publishCompletionSlow); Inc3
       will append the escaping cell here. INERT in 2c-2: never appended to, never
       read -- revert is OFF, so nothing is reverted and every surviving cell is
       pinned trivially (retain-only). */
    std::vector<Value *> escaped;

    /* Task-join / reclamation seam. Runs once today, at the root task's end
       (`endUnforceScope`), where the single task reclaims its nursery
       segment: a kept-whole job RETAINS it (its objects become ordinary old
       memory and later jobs allocate above them), otherwise the segment is
       RELEASED back to the stripe for reuse. Under a future thread-parallel
       evaluator a child task calls this at ITS join to reclaim ITS segment
       -- one call per task -- which is why reclamation lives on the per-task
       context rather than inline in `endUnforceScope`. Defined out-of-line
       below, after `retainRegion`/`releaseRegion`. */
    void joinAndReclaim(bool keepWhole);
};

/* The single root task: one instance today, so this is a pure rename.
   NOTE for 2c-3/Inc3: `rootTask` is still process-global, so its nursery
   bookkeeping (`keptStart`/`keptFloor`/`keptEnd`, env-size table) is shared
   across workers. It must become per-worker/per-task before a worker's
   retain-only join (below) can actually run concurrently; moot in 2c-2 (only
   the main thread forces), flagged for 2c-3. */
static TaskUnforceState rootTask;

/* inc2c-2: the task whose unforce scope is open on THIS worker (Inc3 escape
   scaffolding). Set to the worker's task at scope open, cleared at close.
   `publishCompletionSlow`'s awaited branch (the cross-thread-escape seam) will,
   in Inc3, append the escaping cell to `curTask->escaped`. INERT in 2c-2:
   tracked (set/cleared) but never read to act on -- a new per-worker TLS set on
   a cold path, so it changes no value and no counter. */
[[gnu::tls_model("initial-exec")]] static thread_local TaskUnforceState * curTask = nullptr;

static size_t envSizeTableBytes()
{
    return regionSize() / 16;
}

static void noteBigEnv(const void * env, size_t size)
{
    rootTask.bigEnvSizes[env] = size;
}

/* The size of a young Env, from the side table. */
static size_t envSizeOf(const Env * e)
{
    auto s = rootTask.envSizeTable[((const char *) e - rootTask.envSizeMapping) >> 4];
    return s < 255 ? s : rootTask.bigEnvSizes.at(e);
}

static void unmapEnvSizeTable()
{
    if (rootTask.envSizeTable)
        munmap(rootTask.envSizeTable, envSizeTableBytes());
    rootTask.envSizeTable = nullptr;
    rootTask.envSizeMapping = nullptr;
}

/* Point the region at the side table of `mapping`, mapping one if it has
   none yet. */
static void useEnvSizeTable(const char * mapping)
{
    if (rootTask.envSizeMapping != mapping) {
        unmapEnvSizeTable();
        rootTask.envSizeTable = (uint8_t *) mapArena(envSizeTableBytes());
        rootTask.envSizeMapping = mapping;
    }
    evalRegion.envSizes = rootTask.envSizeTable;
    evalRegion.envSizesBase = rootTask.envSizeMapping;
}

/* At a job's end its Envs are copied or kept as old objects, or freed:
   none of their sizes is read again. */
static void dropEnvSizes(const char * base, const char * cur)
{
    std::unordered_map<const void *, size_t>().swap(rootTask.bigEnvSizes);
    if (!rootTask.envSizeTable || cur <= base)
        return;
    auto from = (uintptr_t) (rootTask.envSizeTable + ((base - rootTask.envSizeMapping) >> 4)) & ~(uintptr_t) (pageSize() - 1);
    auto to = (char *) rootTask.envSizeTable + ((cur - rootTask.envSizeMapping + 15) >> 4);
    /* Entries below `base` belong to kept jobs, those above `cur` to none
       yet: the whole pages can go. */
    madvise((void *) from, pageUp(to) - (char *) from, MADV_DONTNEED);
}

/* Stop allocating above the kept jobs: their mapping keeps only its prefix. */
static void leaveKeptMapping()
{
    if (!rootTask.keptStart)
        return;
    auto tail = pageUp(rootTask.keptFloor);
    if (tail < rootTask.keptEnd)
        munmap(tail, rootTask.keptEnd - tail);
    rootTask.keptStart = rootTask.keptFloor = rootTask.keptEnd = nullptr;
}

void openRegion()
{
    hookCollector();
    if (useStripes() && !rootTask.keptStart) {
        /* First job: anchor the active worker's recyclable nursery at the base
           of its stripe, reserving the striped mapping. The nursery floor is
           `keptFloor`; kept jobs raise it (their objects stay in place below
           it), non-kept jobs recycle `[keptFloor, cur)`. The kept-mapping
           bookkeeping (`keptStart`/`keptFloor`/`keptEnd`) thus drives recycling
           within the one stripe, so `releaseRegion`/`retainRegion` need no
           change. With one stripe the nursery is never abandoned
           (`leaveKeptMapping`): there is nowhere else to go, and a single eval
           never fills 32 GiB. Multi-stripe increments advance to the next
           stripe when a stripe fills. */
        rootTask.keptStart = rootTask.keptFloor = stripeBase(myStripe);
        rootTask.keptEnd = rootTask.keptStart + stripeBytes;
    }
    if (rootTask.keptStart) {
        /* Continue above the kept jobs. In stripe mode the whole stripe span
           is the nursery, so always continue there; otherwise (tripwire) only
           while at least half a region is left, and never under the tripwire
           (released memory is not reused). The memory released above the floor
           is zero pages, and the floor's own page is zeroed above it
           (`releaseRegion`). */
        bool cont = useStripes() || (!unforceDebug().tripwire && size_t(rootTask.keptEnd - rootTask.keptFloor) >= regionSize() / 2);
        if (cont) {
            evalRegion.base = evalRegion.cur = rootTask.keptFloor;
            evalRegion.end = rootTask.keptEnd;
            /* The HIGH level of the two-level clock, derived from the address:
               `(floor - stripesBase) >> stripeShift` is the worker's stripe.
               Single-threaded it is always the active stripe, and the whole
               nursery span [floor, end) stays within that one stripe (the LOW
               level, masked by `stripeMask`, never overflows into the next). */
            auto d = (uintptr_t) (rootTask.keptFloor - stripesBase);
            evalRegion.arenaId = uint32_t(d >> stripeShift);
            assert(
                evalRegion.arenaId == myStripe
                && ((uintptr_t) (rootTask.keptEnd - 1 - stripesBase) >> stripeShift) == evalRegion.arenaId
                && (d & stripeMask) == d - (uintptr_t(myStripe) << stripeShift));
            useEnvSizeTable(rootTask.keptStart);
            return;
        }
        leaveKeptMapping();
    }
    /* Tripwire fallback: a fresh standalone region per job (no striping). */
    auto base = (char *) mapArena(regionSize());
    evalRegion.base = evalRegion.cur = base;
    evalRegion.end = evalRegion.base + regionSize();
    evalRegion.arenaId = 0;
    useEnvSizeTable(base);
}

/* With NIX_UNFORCE_DEBUG=verify, check the invariant that makes releasing a
   region safe: no live object outside it still points into it. Every live
   mimalloc block is scanned word by word, so with Boehm disabled (and
   without NIX_NOGC_BUMP) this covers all long-lived evaluator objects; it is
   conservative, so a hit names a word that merely looks like a pointer. */
void verifyNoEscapes()
{
    /* Precise enough: the base arena holds only evaluator objects, so a hit
       is an evaluator object (or string bytes) pointing into the region. */
    if (evalUseBaseArena) {
        uintptr_t lo = (uintptr_t) evalRegion.base, hi = (uintptr_t) evalRegion.cur;
        size_t bytes = 0, hits = 0;
        for (auto [start, end] : evalBaseArena.chunks) {
            /* The current chunk is only filled up to `cur`. */
            if (evalBaseArena.cur >= start && evalBaseArena.cur <= end)
                end = evalBaseArena.cur;
            bytes += end - start;
            for (auto w = (const uintptr_t *) start; w < (const uintptr_t *) end; ++w)
                if (auto v = *w & ~uintptr_t(7); v >= lo && v < hi) {
                    if (hits++ < 5)
                        std::cerr << fmt(
                            "unforce verify: arena word at %x = %x points into the region\n", (uintptr_t) w, *w);
                }
        }
        std::cerr << fmt(
            "unforce verify: job %d scanned the base arena (%d MiB), %d pointers into the region\n",
            stats.jobs,
            bytes >> 20,
            hits);
        return;
    }
#if NIX_UNFORCE_MIMALLOC
    struct Scan
    {
        uintptr_t lo, hi;
        size_t blocks = 0, bytes = 0, hits = 0;
    } scan{(uintptr_t) evalRegion.base, (uintptr_t) evalRegion.cur};

    mi_heap_visit_blocks(
        mi_heap_main(),
        true,
        [](const mi_heap_t *, const mi_heap_area_t *, void * block, size_t size, void * arg) -> bool {
            auto & scan = *(Scan *) arg;
            if (!block)
                return true;
            scan.blocks++;
            scan.bytes += size;
            auto words = (const uintptr_t *) block;
            for (size_t i = 0; i < size / sizeof(uintptr_t); ++i)
                /* Strip Value's tag bits. */
                if (auto w = words[i] & ~uintptr_t(7); w >= scan.lo && w < scan.hi) {
                    if (scan.hits++ < 3) {
                        std::cerr << fmt(
                            "unforce verify: block %x (%d bytes) word %d points into the region\n",
                            (uintptr_t) block,
                            size,
                            i);
                        if (unforceDebug().verifyDump) {
                            std::string line;
                            for (size_t j = 0; j < std::min<size_t>(size / 8, 40); ++j)
                                line += fmt(" %016x", words[j]);
                            std::cerr << "  words:" << line << "\n";
                            std::string text;
                            for (size_t j = 0; j < std::min<size_t>(size, 320); ++j) {
                                char c = ((const char *) block)[j];
                                text += c >= 32 && c < 127 ? c : '.';
                            }
                            std::cerr << "  text: " << text << "\n";
                        }
                    }
                }
            return true;
        },
        &scan);
    std::cerr << fmt(
        "unforce verify: job %d scanned %d blocks (%d MiB), %d pointers into the region\n",
        stats.jobs,
        scan.blocks,
        scan.bytes >> 20,
        scan.hits);
#endif
}

/* With the tripwire, the pages stay reserved but inaccessible, so a missed
   escape faults instead of reading reused memory. */
void releaseRegion()
{
    if (unforceDebug().verify)
        verifyNoEscapes();
    auto base = evalRegion.base;
    auto cur = evalRegion.cur;
    stats.regionPeak = std::max(stats.regionPeak, size_t(cur - base));
    evalRegion = EvalRegion{};
    dropEnvSizes(base, cur);
    if (rootTask.keptStart && base == rootTask.keptFloor) {
        /* Above kept jobs: free this job's pages, and zero what it used of
           the floor's page, which the kept objects share. */
        auto up = pageUp(base);
        memset(base, 0, std::min(cur, up) - base);
        if (cur > up)
            madvise(up, pageUp(cur) - up, MADV_DONTNEED);
        return;
    }
    if (unforceDebug().tripwire) {
        mprotect(base, regionSize(), PROT_NONE);
        madvise(base, regionSize(), MADV_DONTNEED);
    } else
        munmap(base, regionSize());
    if (rootTask.envSizeMapping == base)
        unmapEnvSizeTable();
}

/* A job that keeps all its memory (see `UnforceSeen`): its objects become
   ordinary memory for the rest of the process, never reused or reported as
   freed. Later jobs allocate above them in the same mapping (`keptStart`). */
static void retainRegion()
{
    auto base = evalRegion.base;
    auto cur = evalRegion.cur;
    stats.regionPeak = std::max(stats.regionPeak, size_t(cur - base));
    stats.keptJobBytes += cur - base;
    dropEnvSizes(base, cur);
    if (rootTask.keptStart && base == rootTask.keptFloor)
        rootTask.keptFloor = cur;
    else {
        leaveKeptMapping();
        rootTask.keptStart = base;
        rootTask.keptFloor = cur;
        rootTask.keptEnd = evalRegion.end;
    }
    evalRegion = EvalRegion{};
}

/* Task-join / reclamation seam (see TaskUnforceState). Today the single root
   task calls this once, at `endUnforceScope`, to reclaim its nursery segment.
   A future child task calls it at its own join to reclaim its own segment. */
void TaskUnforceState::joinAndReclaim(bool keepWhole)
{
    if (!evalRegion.base)
        return;
    if (keepWhole)
        retainRegion();
    else
        releaseRegion();
}

/* Copies a graph out of the job region, precisely: objects outside the
   region are shared, objects inside are copied once (sharing and cycles
   are preserved through the forwarding table). Callers pause the region. */
struct Evacuator
{
    EvalState & state;
    std::unordered_map<const void *, void *> forward;
    std::vector<Value *> values;
    std::vector<std::pair<Env *, size_t>> envs;

    /* If set, called with every identity-bearing young object as it is
       copied (cells, Bindings layers, element arrays): the owner closure. */
    void (*onIdentity)(void *, const void *) = nullptr;
    void * onIdentityCtx = nullptr;

    void note(const void * p, void * n)
    {
        forward.emplace(p, n);
    }

    bool young(const void * p) const
    {
        return evalRegion.contains(p);
    }

    template<typename T>
    T * forwarded(const T * p)
    {
        auto i = forward.find(p);
        return i == forward.end() ? nullptr : (T *) i->second;
    }

    void * copyBytes(const void * p, size_t n)
    {
        stats.copiedBytes += n;
        auto q = state.mem.allocBytes(n);
        memcpy(q, p, n);
        return q;
    }

    Value * value(Value * v)
    {
        if (!v || !young(v))
            return v;
        if (auto f = forwarded(v))
            return f;
        auto n = state.allocValue();
        stats.copiedBytes += sizeof(Value);
        *n = *v;
        note(v, n);
        if (onIdentity)
            onIdentity(onIdentityCtx, v);
        values.push_back(n);
        return n;
    }

    Env * env(Env * e)
    {
        if (!e || !young(e))
            return e;
        if (auto f = forwarded(e))
            return f;
        auto size = envSizeOf(e);
        auto & n = state.mem.allocEnv(size);
        stats.copiedBytes += sizeof(Env) + size * sizeof(Value *);
        n.up = e->up;
        std::copy_n(e->values, size, n.values);
        note(e, &n);
        envs.emplace_back(&n, size);
        return &n;
    }

    const StringData * string(const StringData * s)
    {
        if (!s || !young(s))
            return s;
        if (auto f = forwarded(s))
            return f;
        auto n = (const StringData *) copyBytes(s, sizeof(StringData) + s->size() + 1);
        note(s, (void *) n);
        return n;
    }

    /* A list's element array, copied once however many list values share
       it (lists are immutable, and every array has the size it was built
       with, so the values sharing one also share its size). */
    Value * const * elements(Value * const * elems, size_t size)
    {
        if (!young(elems))
            return elems;
        if (auto f = forwarded(elems))
            return f;
        auto n = (Value **) state.mem.allocBytes(size * sizeof(Value *));
        stats.copiedBytes += size * sizeof(Value *);
        note(elems, n);
        if (onIdentity)
            onIdentity(onIdentityCtx, elems);
        for (size_t i = 0; i < size; ++i)
            n[i] = value(elems[i]);
        return n;
    }

    using Context = Value::StringWithContext::Context;

    const Context * context(const Context * c)
    {
        if (!c || !young(c))
            return c;
        if (auto f = forwarded(c))
            return f;
        auto n =
            new (state.mem.allocBytes(sizeof(Context) + c->size() * sizeof(Context::value_type))) Context(c->size());
        auto elems = const_cast<Context::value_type *>(n->begin());
        for (size_t i = 0; i < c->size(); ++i)
            elems[i] = string(c->begin()[i]);
        note(c, n);
        return n;
    }

    const Bindings * bindings(const Bindings * b)
    {
        if (!young(b))
            return b;
        if (auto f = forwarded(b))
            return f;
        auto bytes = sizeof(Bindings) + b->layerSize() * sizeof(Attr);
        stats.copiedBytes += bytes;
        auto n = b->copyLayer(
            state.mem.allocBytes(bytes),
            [&](Value * v) { return value(v); },
            [&](const Bindings * base) { return bindings(base); });
        note(b, (void *) n);
        if (onIdentity)
            onIdentity(onIdentityCtx, b);
        return n;
    }

    /* Rewrite the pointers of a copied value. */
    void scan(Value & v)
    {
        switch (v.internalType()) {
        case tString:
            v.mkStringNoCopy(*string(v.stringData()), context(v.context()));
            break;
        case tPath:
            v.mkPath(v.pathAccessor(), *string(v.pathData()));
            break;
        case tAttrs:
            v.mkAttrs(bindings(v.attrs()));
            break;
        case tListSmall: {
            auto l = v.rawSmallList();
            v.setRawSmallList({value(l[0]), value(l[1])});
            break;
        }
        case tListN: {
            auto l = v.rawList();
            v.setRawList({.size = l.size, .elems = elements(l.elems, l.size)});
            break;
        }
        case tThunk:
            if (auto t = v.thunk(); t.env)
                v.mkThunk(env(t.env), t.expr);
            break;
        case tApp:
            v.mkApp(value(v.app().left), value(v.app().right));
            break;
        case tPrimOpApp:
            v.mkPrimOpApp(value(v.primOpApp().left), value(v.primOpApp().right));
            break;
        case tLambda:
            v.mkLambda(env(v.lambda().env), v.lambda().fun);
            break;
        case tFailed:
            if (young(v.failed().recoveryValue))
                v.setRawFailed(new Value::Failed(v.failed().exRef->ex, value(v.failed().recoveryValue)));
            break;
        case tUninitialized:
        case tInt:
        case tBool:
        case tNull:
        case tFloat:
        case tExternal:
        case tPrimOp:
        case tNumberOfInternalTypes:
            break;
        }
    }

    /* Copy everything still pending. */
    void drain()
    {
        while (!values.empty() || !envs.empty()) {
            if (!values.empty()) {
                auto v = values.back();
                values.pop_back();
                scan(*v);
            } else {
                auto [e, size] = envs.back();
                envs.pop_back();
                e->up = env(e->up);
                for (size_t i = 0; i < size; ++i)
                    e->values[i] = value(e->values[i]);
            }
        }
    }

    Value copy(const Value & root)
    {
        Value res = root;
        scan(res);
        drain();
        return res;
    }
};

/* ---------------------------------------------------------------------------
   Promotion correctness: closing a job's promotion set under "producer-of"
   (the policy comment; owner(x) is computed by the owner timeline above).

   Promotion breaks the evaluator's pointer identity (`eqValues` short-cuts on
   `&v1 == &v2`; Nixpkgs relies on it for `hostPlatform == buildPlatform`)
   when a promoted cell keeps a copy of a cell that a reverted target rebuilds
   when it is re-forced: two cells then stand for one cell of an evaluation
   without unforcing. Force counts cannot rule this out (an earlier promotion
   creates new old thunks with count 0, so a consumer can reach the threshold
   before its producer), so the counts only nominate promotions; this closure
   makes them safe.

   Definitions, for one job (outermost scope).
   - Targets T: old cells forced in the job (the undo log).
   - The creator of a young object x is the innermost cell (young or old)
     being forced when x was allocated; owner(x) is the first old cell on the
     creator chain x, creator(x), creator(creator(x)), ... (a target), or
     none (allocated by the driver outside any force). Re-forcing owner(x) in
     a later job is the only thing that can build x again.
   - c(t) is t's content at job end; R(c) the young objects reachable from
     it (through cells, Bindings layers, lists, envs, thunk and app
     operands, lambdas, failure recovery values), as the Evacuator copies.
   - S ⊆ T is the promoted set and Evac(S) = ∪_{r∈S} R(c(r)), copied with one
     forwarding table.
   - Observation model: what an evaluation without unforcing can tell
     apart by address. `eqValues` (and so `==`, `elem`, `genericClosure`,
     `CompareValues`) short-cuts on the cell address and has no Bindings or
     list shortcut; the printers' «repeated» tracking (`print.cc`,
     `print-ambiguous.cc`; stdout, `trace:` and error text) keys attrsets on
     their Bindings and lists on their cell; the derivation walker's
     `done` set keys on Bindings (off while unforcing). Identity-bearing
     objects are therefore cells and Bindings. Element arrays are counted
     with them for safety, although nothing compares them by address; envs
     and strings are never compared by address.

   Invariant (OC, ownership closure): every young identity-bearing object X
   ∈ Evac(S) has owner(X) ∈ S or no owner. OC is sufficient for
   pointer-identity safety (a reverted target re-forced later rebuilds only
   fresh cells it owns, none of which survives) and, in general, necessary
   (a surviving X owned by a reverted t is rebuilt as X', and `X == X'` is
   observable when X holds a function).

   Lemma V (no upward visibility). While t is being forced, cells owned by t
   are accessible only to t's own evaluation and to forces of young cells
   owned by t.

   Lemma C (closed region). If t owns X and X is reachable at job end from a
   promoted r ≠ t, then X ∈ R(c(t)), along a path whose interior consists of
   objects owned by t.

   Computing owner(X). The owner timeline (the block above) records, for
   every young address, the target that owned the allocation clock when it
   was allocated; `ownerOf(X)` reads it off the closed/open segments with no
   walk of R(c(t)) and no per-object stamp. The timeline is exact because the
   current owner follows the creator chain, so owner(X) names X's one
   producer -- unlike an allocation-interval attribution (unsound) or plain
   reachability (over-promotes every consumer that reads a producer's data).

   Rule. S is the least superset of the count-nominated set S0 that satisfies
   OC under the producer-of test (`OwnerClosure`: owner(x) ∈ S for every
   identity-bearing x in Evac(S)), computed to a fixpoint; the promoted set is
   copied with one Evacuator, so shared objects keep one copy. The decision,
   copies included, runs before the job's memory is released and writes no
   target (`decideJob`); the release then only assigns, reverts and frees. The
   counts (K) stay a heuristic for which cells to consider; correctness
   depends only on the closure.

   Premises: A1–A4 of the review (every write to old memory is logged;
   old memory allocated during a job, i.e. fileEvalCache cells and C-API
   objects, is inert; no escapes; deterministic evaluation), and exceptions
   carry no evaluator values.

   The full proofs -- OC sufficiency and necessity, the Lemma V and C
   reasoning, the option-C (reachability) / option-E (interval) comparison,
   and the greatest-subset starvation argument for the Rule -- are in
   notes/reviews/identity-owners-v1.md and notes/reviews/identity-comparison.md
   (the text moved out of this comment is also preserved in
   notes/reviews/unforce-trim-src.md, P10).
   --------------------------------------------------------------------------- */

/* A job's targets, in the order of their first log entries (= the order
   their first forces started). A cell may have been forced more than once
   (after a recoverable error); its oldest pre-image is the one from before
   the job.

   Memory. Built at every job's end, beside the log, so it is kept small:
   the pre-images stay in the log (which lives until the release), and the
   repeated cells are found by sorting the log indices by cell, not with a
   hash map (the map and copied pre-images
   took about 70 bytes per target, more than the log itself). */
struct JobTargets
{
    const EvalUndoLog & log;
    std::vector<Value *> cells;
    /* The log index of each target's first entry, and whether it has more. */
    std::vector<size_t> first;
    std::vector<bool> repeated;

    JobTargets(const EvalUndoLog & log, size_t mark)
        : log(log)
    {
        auto n = log.size() - mark;
        /* Log indices by (cell, index); the first of each run is a target's
           first entry. */
        std::vector<size_t> byCell(n);
        for (size_t i = 0; i < n; ++i)
            byCell[i] = mark + i;
        std::sort(byCell.begin(), byCell.end(), [&](size_t a, size_t b) {
            auto ca = log[a].target, cb = log[b].target;
            return ca != cb ? std::less<const Value *>{}(ca, cb) : a < b;
        });
        /* The first entries in log order, with the repeated flag in the
           top bit. */
        constexpr size_t more = size_t(1) << (sizeof(size_t) * 8 - 1);
        size_t targets = 0;
        const Value * prev = nullptr;
        for (size_t i = 0; i < n; ++i) {
            auto cell = log[byCell[i]].target;
            if (!targets || cell != prev)
                byCell[targets++] = byCell[i];
            else
                byCell[targets - 1] |= more;
            prev = cell;
        }
        byCell.resize(targets);
        std::sort(byCell.begin(), byCell.end(), [&](size_t a, size_t b) { return (a & ~more) < (b & ~more); });
        cells.reserve(targets);
        repeated.reserve(targets);
        for (auto & f : byCell) {
            repeated.push_back(f & more);
            f &= ~more;
            cells.push_back(log[f].target);
        }
        first = std::move(byCell);
    }

    size_t size() const
    {
        return cells.size();
    }

    const Value & preImage(size_t i) const
    {
        return log[first[i]].preImage;
    }
};

/* Which objects belong to the job: its bump region. */
struct YoungSpace
{
    bool operator()(const void * p) const
    {
        return evalRegion.contains(p);
    }
};

/* The one closure flag per young object: whether it is in Evac(S). */
enum Flag : uint8_t { inEvac = 1 };

/* The flag of each of the bump region's objects: a nibble per 16-byte
   granule (bump allocations are 16-byte aligned; only `inEvac`, bit 0, is
   used), in pieces covering 64 KiB of the region that are allocated when
   first touched. At most 3.1 % of the job's memory. */
struct RegionMarks
{
    static constexpr size_t granuleShift = 4, pieceShift = 16;
    static constexpr size_t words = (size_t(1) << (pieceShift - granuleShift)) / 16;

    struct Piece
    {
        uint64_t nibbles[words] = {};
    };

    uintptr_t base;
    std::vector<std::unique_ptr<Piece>> pieces;
    size_t allocated = 0;

    RegionMarks(const char * base, const char * cur)
        : base((uintptr_t) base)
        , pieces(((cur - base) >> pieceShift) + 1)
    {
    }

    /* Set `flags` on `p`; returns the flags it had. */
    uint8_t add(const void * p, uint8_t flags)
    {
        auto g = ((uintptr_t) p - base) >> granuleShift;
        auto & piece = pieces[g >> (pieceShift - granuleShift)];
        if (!piece) [[unlikely]] {
            piece = std::make_unique<Piece>();
            allocated++;
        }
        auto & w = piece->nibbles[(g >> 4) % words];
        auto shift = (g & 15) * 4;
        uint8_t old = (w >> shift) & 15;
        w |= uint64_t(flags) << shift;
        return old;
    }
};

/* The closure of a job ran out of its step budget. */
struct OverBudget
{};

/* Marks Evac(S) for the owner closure: `mark(content)` adds R(content) —
   every young object reachable from `content` without pruning — to Evac(S),
   calling `onMark` for each identity-bearing object it adds (so the owner
   closure can promote that object's owner). The marking is linear in Evac(S)
   (each object marked once, string data excluded), charged against `budget`,
   and bounded by `markedLimit` via `checkMarked` (the copy limit; exceeding
   it throws `OverBudget` and the job is kept whole). A list's element array
   is marked once however many list values share it. */
template<typename Marks>
struct Closure
{
    enum Kind : uint8_t { kValue, kEnv, kBindings };

    const YoungSpace & young;
    Marks marks;
    std::vector<std::pair<const void *, Kind>> stack;
    /* `mark`'s element arrays whose elements are still to be added, and
       how many: taken one element at a time once `stack` is empty, so that
       marking a list of n young elements does not push n entries at once
       (a 4 M-element list took 64 MiB of
       stack). */
    std::vector<std::pair<Value * const *, size_t>> arrays;
    /* Steps taken in this job, and how many it may take. */
    uint64_t steps = 0, budget = UINT64_MAX;
    /* If set, `mark` calls it with every identity-bearing object it adds
       (the owner closure, when it marks before copying). */
    void (*onMark)(void *, const void *) = nullptr;
    void * onMarkCtx = nullptr;
    /* What `mark` added to Evac(S), in bytes (string data, which it does
       not walk, excluded). */
    size_t markedBytes = 0;
    /* `mark` throws `OverBudget` once what it marked, with the marks
       themselves, exceeds this (the owner closure's copy limit). */
    size_t markedLimit = SIZE_MAX;

    void checkMarked()
    {
        size_t marksBytes = 0;
        if constexpr (requires { marks.allocated; })
            marksBytes = marks.allocated * sizeof(typename Marks::Piece);
        if (markedBytes + marksBytes > markedLimit) [[unlikely]]
            throw OverBudget{};
    }

    void charge(uint64_t n)
    {
        steps += n;
        if (steps > budget) [[unlikely]]
            throw OverBudget{};
    }

    /* Call `f(p, kind)` for each object `v` refers to directly, except a
       young list element array, which goes to `array(elems, size)`. */
    template<typename F, typename A>
    void children(const Value & v, F && f, A && array) const
    {
        switch (v.internalType()) {
        case tAttrs:
            f(v.attrs(), kBindings);
            break;
        case tListSmall: {
            auto l = v.rawSmallList();
            f(l[0], kValue);
            f(l[1], kValue);
            break;
        }
        case tListN: {
            auto l = v.rawList();
            if (young(l.elems))
                array(l.elems, l.size);
            break;
        }
        case tThunk:
            f(v.thunk().env, kEnv);
            break;
        case tApp:
            f(v.app().left, kValue);
            f(v.app().right, kValue);
            break;
        case tPrimOpApp:
            f(v.primOpApp().left, kValue);
            f(v.primOpApp().right, kValue);
            break;
        case tLambda:
            f(v.lambda().env, kEnv);
            break;
        case tFailed:
            f(v.failed().recoveryValue, kValue);
            break;
        case tUninitialized:
        case tInt:
        case tBool:
        case tNull:
        case tFloat:
        case tString:
        case tPath:
        case tExternal:
        case tPrimOp:
        case tNumberOfInternalTypes:
            break;
        }
    }

    template<typename F, typename A>
    void children(const void * p, Kind kind, F && f, A && array) const
    {
        switch (kind) {
        case kValue:
            children(*(const Value *) p, f, array);
            break;
        case kEnv: {
            auto e = (const Env *) p;
            auto size = envSizeOf(e);
            f(e->up, kEnv);
            for (size_t i = 0; i < size; ++i)
                f(e->values[i], kValue);
            break;
        }
        case kBindings: {
            auto b = (const Bindings *) p;
            b->forEachLayerValue([&](const Value * v) { f(v, kValue); });
            f(b->layerBase(), kBindings);
            break;
        }
        }
    }

    /* How many children `children(p, kind, ...)` scans beyond a value's
       few: an env's slots, a Bindings layer's attributes. A walk is charged
       one step per 16 of them (a 2 M-attribute
       Bindings cost one step per round). */
    size_t width(const void * p, Kind kind) const
    {
        switch (kind) {
        case kEnv:
            return envSizeOf((const Env *) p);
        case kBindings:
            return ((const Bindings *) p)->layerSize();
        case kValue:
            return 0;
        }
        return 0;
    }

    /* Add R(content), every young object reachable from `content` without
       pruning, to Evac(S). */
    void mark(const Value & content)
    {
        stack.clear();
        auto add = [&](const void * p, Kind k) {
            if (!p || !young(p))
                return;
            auto old = marks.add(p, inEvac);
            if (!(old & inEvac)) {
                markedBytes += k == kValue ? sizeof(Value)
                               : k == kEnv ? sizeof(Env) + width(p, k) * sizeof(Value *)
                                           : sizeof(Bindings) + width(p, k) * sizeof(Attr);
                checkMarked();
                if (onMark && k != kEnv)
                    onMark(onMarkCtx, p);
                stack.emplace_back(p, k);
            }
        };
        auto array = [&](Value * const * elems, size_t size) {
            auto old = marks.add(elems, inEvac);
            if (!(old & inEvac)) {
                markedBytes += size * sizeof(Value *);
                checkMarked();
                if (onMark)
                    onMark(onMarkCtx, elems);
                if (size)
                    arrays.emplace_back(elems, size);
            }
        };
        arrays.clear();
        children(content, add, array);
        for (;;) {
            if (!stack.empty()) {
                auto [p, kind] = stack.back();
                stack.pop_back();
                charge(1 + width(p, kind) / 16);
                children(p, kind, add, array);
            } else if (!arrays.empty()) {
                auto & [elems, left] = arrays.back();
                auto e = *elems++;
                if (!--left)
                    arrays.pop_back();
                add(e, kValue);
            } else
                break;
        }
    }

};

/* Per target of a job, whether a live `UnforceSeen` set reported it
   (`reportedTargets`). */
using Reported = std::vector<bool>;

/* Count a job's forces of its targets; returns whether each reached K.

   The map's cap. So that the counts do not grow with the length of a
   traversal (about 50 bytes per distinct target, never
   erased), when counting this job's targets would take the map past
   max(256 Ki, batch size / 256) entries (the force-counts cap), it is
   first rebuilt from this job's targets: the counts of cells this job did
   not force are forgotten, so it never holds more than max(cap, this job's
   targets). Dropping the whole map instead would drop this job's counts as
   well, and if every job outgrew the cap, a cell forced in every job would
   never reach K and would be recomputed in every job. A cell forced only
   in jobs that are not consecutive can still
   lose its count that way; the closure does not depend on the counts. The
   old counts are read out before the old map is freed and the new one
   built, so the two never coexist. */
static std::vector<bool> countTargets(
    std::unordered_map<const Value *, uint32_t> & counts,
    const JobTargets & targets,
    uint64_t batchSize,
    const Reported * skip)
{
    auto & cfg = unforceSettings();
    auto cap = cfg.countsCap ? cfg.countsCap : std::max<uint64_t>(256 << 10, batchSize / 256);
    auto counted = targets.size();
    if (skip)
        for (size_t i = 0; i < targets.size(); ++i)
            counted -= (*skip)[i];
    if (counts.size() + counted > cap) {
        std::vector<uint32_t> prior(targets.size());
        for (size_t i = 0; i < targets.size(); ++i)
            if (auto c = counts.find(targets.cells[i]); c != counts.end())
                prior[i] = c->second;
        std::unordered_map<const Value *, uint32_t>().swap(counts);
        counts.reserve(counted);
        for (size_t i = 0; i < targets.size(); ++i)
            if (prior[i])
                counts.emplace(targets.cells[i], prior[i]);
        stats.countsResets++;
    }
    std::vector<bool> reached(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        /* Reported cells are nominated anyway (`decideJob`): counting them
           would only grow the map. */
        if (skip && (*skip)[i])
            continue;
        if (onThreadStack(targets.cells[i]))
            stats.stackTargets++;
        auto before = counts[targets.cells[i]]++;
        if (before)
            stats.reforced++;
        reached[i] = before + 1 >= cfg.promoteAfter;
    }
    return reached;
}

/* The owner closure: S is the least superset of the nominated set such that
   every identity-bearing young object in Evac(S) has its owner in S or no
   owner (OC of the policy comment), with owners read off the timeline. S
   only grows, so each target is joined (copied, or marked when the job may
   be kept whole) once, when it joins, and the objects that join visits are
   Evac(S): the work is one owner lookup per identity-bearing object, which
   promotion copies anyway. No walk, no budget. */
struct OwnerClosure
{
    const JobTargets & targets;
    std::vector<bool> & promote;
    std::vector<bool> joined;
    /* (cell, target index), sorted by cell. */
    std::vector<std::pair<const Value *, uint32_t>> byCell;
    std::vector<size_t> work;
    size_t added = 0;
    uint32_t last = noOwner;

    OwnerClosure(const JobTargets & targets, std::vector<bool> & promote)
        : targets(targets)
        , promote(promote)
        , joined(targets.size())
    {
        byCell.reserve(targets.size());
        for (size_t i = 0; i < targets.size(); ++i)
            byCell.emplace_back(targets.cells[i], i);
        std::sort(byCell.begin(), byCell.end());
    }

    std::optional<size_t> indexOf(const Value * c) const
    {
        auto i = std::lower_bound(
            byCell.begin(), byCell.end(), c, [](const auto & e, const Value * c) { return std::less<const Value *>{}(e.first, c); });
        if (i == byCell.end() || i->first != c)
            return std::nullopt;
        return i->second;
    }

    /* An identity-bearing object joined Evac(S): its owner must be in S. */
    static void note(void * ctx, const void * p)
    {
        auto & x = *(OwnerClosure *) ctx;
        stats.ownerJobLookups++;
        auto o = ownerOf(p);
        /* Consecutive objects mostly share an owner. */
        if (o == noOwner || o == x.last)
            return;
        x.last = o;
        auto t = x.indexOf(ownerCell(o));
        /* Every owner is a cell logged in this job. */
        assert(t);
        if (t && !x.promote[*t]) {
            x.promote[*t] = true;
            x.added++;
            x.work.push_back(*t);
        }
    }

    /* Join every promoted target not joined yet, and what they bring. */
    template<typename Join>
    void run(Join && join)
    {
        for (size_t i = 0; i < targets.size(); ++i)
            if (promote[i] && !joined[i])
                work.push_back(i);
        while (!work.empty()) {
            auto i = work.back();
            work.pop_back();
            if (joined[i])
                continue;
            joined[i] = true;
            join(i);
        }
    }
};

/* Decide which of a job's targets to promote, and copy them with
   `evacuator` into `copies` (indexed like the targets): the counts in
   `counts` nominate, as do the targets `printed` marks (`UnforceSeen`; null
   when the printers reported nothing in the job), and the owner closure
   adds what identity needs (`OwnerClosure`, see above). This is the fallible
   step before the job's memory is released.
   It writes no target, so the caller turns an allocation failure here into
   a fatal error before anything has changed, and the release that follows
   only assigns, reverts and frees. The caller pauses the region.
   `youngBytes` is the memory the job allocated other than string data.
   Returns a flag per target.

   Evac(S). S only grows, so every target that joins S is copied at once and
   the Evacuator marks Evac(S) as it copies (the owner closure promotes each
   copied object's owner): no copy is ever wasted, and Evac(S) costs no walk
   of its own. A printing or over-the-copy-limit job marks Evac(S) first
   (`Closure::mark`) and copies only if it keeps less than half its memory;
   otherwise it is kept whole (exact). */
std::vector<bool> decideJob(
    EvalState & state,
    size_t mark,
    const JobTargets & targets,
    std::unordered_map<const Value *, uint32_t> & counts,
    const YoungSpace & young,
    size_t youngBytes,
    Evacuator & evacuator,
    std::vector<Value> & copies,
    const Reported * printed = nullptr,
    bool * keepWhole = nullptr)
{
    auto & cfg = unforceSettings();
    std::vector<bool> promote(targets.size());
    std::vector<bool> copied(targets.size());
    auto copy = [&](size_t i) {
        /* Sized at the first copy: a job that copies nothing (it reverts
           or keeps everything) needs none. */
        if (copies.empty())
            copies.resize(targets.size());
        if (!copied[i]) {
            copies[i] = evacuator.copy(*targets.cells[i]);
            copied[i] = true;
        }
    };
    auto copyPromoted = [&] {
        for (size_t i = 0; i < targets.size(); ++i)
            if (promote[i])
                copy(i);
    };

    if (cfg.promoteAfter)
        promote = countTargets(counts, targets, state.settings.unforceBatchSize, printed);

    /* Cells reported to an `UnforceSeen` (holding a container a printer
       printed, or walked by `forceValueDeep` for a printer that prints
       later) are nominated like counted ones, so that what they hold stays
       forced and keeps its identity for the rest of the print. */
    if (printed)
        for (size_t i = 0; i < targets.size(); ++i)
            if (!promote[i] && (*printed)[i]) {
                promote[i] = true;
                stats.seenNominated++;
            }

    /* Objects that client code built in this job were allocated outside the
       region: the closure cannot see them, and re-forcing the cell that
       called the client builds them again. Pure revert keeps identity. */
    if (clientCodeInJob) {
        stats.clientJobs++;
        return std::vector<bool>(targets.size());
    }

    bool any = std::find(promote.begin(), promote.end(), true) != promote.end();
    if (!any) {
        copyPromoted();
        return promote;
    }

    if (unforceDebug().failDecide)
        throw std::bad_alloc();

    /* The owner timeline ran out of budget: keep the job whole, which is
       exact (nothing is reverted). */
    if (ownersOverflowed && keepWhole) {
        *keepWhole = true;
        return promote;
    }

    if (ownersOn) {
        OwnerClosure oc(targets, promote);
        /* As below: a printing job that may be kept whole marks Evac(S)
           first, and copies only if it keeps less than half its memory.

           So does a job larger than the copy limit (a quarter of the
           batch, at least 4 MiB): it marks Evac(S) first, and if Evac(S)
           exceeds the limit, the job is kept whole instead (exact: nothing
           is reverted), because the copies and the Evacuator's table would
           exceed the batch while the region is still mapped. A job no
           larger than the limit cannot copy more, so it copies at once
           (e.g. print-meta-200 copied
           1 101 MiB in one job). */
        auto copyLimit =
            cfg.copyLimit ? cfg.copyLimit : std::max<size_t>(state.settings.unforceBatchSize / 4, ownerBudgetMin);
        bool markFirst = keepWhole && (printed || youngBytes > copyLimit);
        std::optional<Closure<RegionMarks>> marker;
        if (markFirst) {
            marker.emplace(young, RegionMarks(evalRegion.base, evalRegion.cur));
            marker->onMark = &OwnerClosure::note;
            marker->onMarkCtx = &oc;
            marker->markedLimit = copyLimit;
        } else {
            evacuator.onIdentity = &OwnerClosure::note;
            evacuator.onIdentityCtx = &oc;
        }
        auto join = [&](size_t i) {
            if (markFirst)
                marker->mark(*targets.cells[i]);
            else
                copy(i);
        };
        try {
        oc.run(join);
        /* Consumers of promoted targets (a heuristic, see the setting):
           nominate them and close again, until nothing changes. */
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        if (ownerConsumersOn)
            for (auto k : ownerConsumers.slots)
                if (k != PairSet::empty) {
                    auto ci = oc.indexOf(ownerCell(uint32_t(k >> 32))), pi = oc.indexOf(ownerCell(uint32_t(k)));
                    if (ci && pi)
                        pairs.emplace_back(*ci, *pi);
                }
        for (bool more = !pairs.empty(); more;) {
            more = false;
            for (auto [c, p] : pairs) {
                std::optional<size_t> ci = c, pi = p;
                if (ci && pi && promote[*pi] && !promote[*ci]) {
                    promote[*ci] = true;
                    stats.ownerConsumerNominated++;
                    oc.added++;
                    more = true;
                }
            }
            if (more)
                oc.run(join);
        }
        } catch (OverBudget &) {
            /* Evac(S) is larger than the copy limit. */
            evacuator.onIdentity = nullptr;
            stats.ownerCopyLimitJobs++;
            *keepWhole = true;
            return promote;
        }
        evacuator.onIdentity = nullptr;
        stats.markerPromoted += oc.added;
        if (oc.added)
            stats.markerJobs++, stats.ownerJobs++;
        /* Markers' rule for printing jobs (it keeps the printer's «repeated»
           cheaply); a job that marked first only for the copy limit
           copies. */
        if (markFirst && printed && 2 * marker->markedBytes >= youngBytes)
            *keepWhole = true;
        if (!keepWhole || !*keepWhole)
            copyPromoted();
        return promote;
    }

    /* The owner timeline is the sole algorithm. If it is not on here (the
       timeline could not be built, or there is no open region), the job is
       kept whole, which is exact: nothing is promoted, reverted or freed. */
    if (keepWhole)
        *keepWhole = true;
    return promote;
}

/* The force counts only nominate promotions; the closure keeps identity
   whatever they say, so forgetting a count is always sound. A promoted cell
   holds a value from now on and is not logged again, so its count goes.
   Runs in the pre-release step; erasing does not allocate. (The map's cap
   is applied before a job's targets are counted: `countTargets`.) */
void pruneCounts(
    std::unordered_map<const Value *, uint32_t> & counts, const JobTargets & targets, const std::vector<bool> & promoted)
{
    stats.countsPeak = std::max(stats.countsPeak, counts.size());
    for (size_t i = 0; i < targets.size(); ++i)
        if (promoted[i])
            counts.erase(targets.cells[i]);
}

/* An allocation failed at the end of a job. Nothing can be released
   partially, so this is fatal. */
[[noreturn]] void unforceFatal(const char * what) noexcept
{
    struct rlimit rl;
    std::string limit = getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
                            ? std::to_string(rl.rlim_cur >> 20) + " MiB"
                            : "unlimited";
    fprintf(
        stderr,
        "error: unforce: out of memory while %s.\n"
        "The process could not allocate more memory (address-space limit, 'ulimit -v': %s; "
        "otherwise the memory the system will commit).\n"
        "Raise the limit, or disable unforcing with '--option eval-unforce false'.\n",
        what,
        limit.c_str());
    std::abort();
}

} // namespace

#if !NIX_USE_BOEHMGC
void * evalBaseAlloc(size_t n)
{
#  if NIX_UNFORCE_MIMALLOC
    return n <= MI_SMALL_SIZE_MAX ? mi_zalloc_small(n) : mi_zalloc(n);
#  else
    return calloc(n, 1);
#  endif
}
#endif

void evalRegionNoteBigEnv(const void * env, size_t size)
{
    noteBigEnv(env, size);
}

std::string unforceStatsJSON()
{
    if (!stats.jobs)
        return "";
    return fmt(
        R"({"scopes":%d,"logged":%d,"reverted":%d,"stubbed":%d,"promoted":%d,"copiedBytes":%d,"largestScopeBytes":%d,)"
        R"("reforced":%d,"promotedBytes":%d,"freedBytes":%d,"markerPromoted":%d,"markerJobs":%d,)"
        R"("countsPeak":%d,"countsResets":%d,"stackTargets":%d,"clientJobs":%d,)"
        R"("revertedInClientJobs":%d,"seenNominated":%d,"seenKept":%d,"keptJobs":%d,)"
        R"("keptJobBytes":%d,"identity":"%s","ownerLookups":%d,"ownerChanges":%d,"ownerSegsPeak":%d,)"
        R"("ownerJobLookups":%d,"ownerJobs":%d,"ownerConsumerNominated":%d,)"
        R"("ownerOverflowJobs":%d,"ownerConsumerPairsPeak":%d,"ownerCopyLimitJobs":%d})",
        stats.jobs,
        stats.logged,
        stats.reverted,
        stats.stubbed,
        stats.promoted,
        stats.copiedBytes,
        stats.regionPeak,
        stats.reforced,
        stats.copiedBytes,
        stats.freedBytes,
        stats.markerPromoted,
        stats.markerJobs,
        stats.countsPeak,
        stats.countsResets,
        stats.stackTargets,
        stats.clientJobs,
        stats.revertedInClientJobs,
        stats.seenNominated,
        stats.seenKept,
        stats.keptJobs,
        stats.keptJobBytes,
        "owners",
        stats.ownerLookups + stats.ownerChanges,
        stats.ownerChanges,
        stats.ownerSegsPeak,
        stats.ownerJobLookups,
        stats.ownerJobs,
        stats.ownerConsumerNominated,
        stats.ownerOverflowJobs,
        stats.ownerConsumerPairsPeak,
        stats.ownerCopyLimitJobs);
}

static size_t allocatorErrors = 0;

std::string allocatorStatsJSON()
{
#if NIX_UNFORCE_MIMALLOC
    /* mi_process_info covers the whole process, whichever collector owns
       the evaluator's heap; mi_stats_get_json adds mimalloc's own counters
       (arena reservation and commit, mmap and purge calls). */
    size_t elapsed, user, sys, rss, peakRss, commit, peakCommit, faults;
    mi_process_info(&elapsed, &user, &sys, &rss, &peakRss, &commit, &peakCommit, &faults);
    auto mi = mi_stats_get_json(0, nullptr);
    auto res = fmt(
        R"({"mimallocVersion":%d,"currentRss":%d,"peakRss":%d,"currentCommit":%d,"peakCommit":%d,"pageFaults":%d,"errors":%d,"mimalloc":%s})",
        mi_version(),
        rss,
        peakRss,
        commit,
        peakCommit,
        faults,
        allocatorErrors,
        mi ? mi : "null");
    mi_free(mi);
    return res;
#else
    return "";
#endif
}

void initAllocator()
{
#if NIX_UNFORCE_MIMALLOC
    /* Count mimalloc's errors (it reports them itself as well); throwing
       from here would unwind through C. */
    mi_register_error([](int, void *) { allocatorErrors++; }, nullptr);
#endif
}

size_t unforceGeneration()
{
    return rootTask.generation;
}

size_t unforceCurrentScopeBytes()
{
    return evalRegion.cur ? size_t(evalRegion.cur - evalRegion.base) : 0;
}

bool unforceEnabled(const EvalState & state)
{
    /* Not with Boehm GC, whatever the setting (see `eval-unforce`). */
    return !NIX_USE_BOEHMGC && state.settings.unforce && !state.debugRepl;
}

UnforceBatch::UnforceBatch(EvalState & state, bool enabled)
    : state(enabled && unforceEnabled(state) ? &state : nullptr)
{
}

UnforceBatch::~UnforceBatch()
{
    end();
}

void UnforceBatch::openAt(const void * stackMark)
{
    if (state && !mark)
        mark = state->beginUnforceScope(stackMark);
}

void UnforceBatch::open()
{
    /* The visiting runs in our caller's callees. */
    openAt(__builtin_dwarf_cfa());
}

bool UnforceBatch::full() const
{
    if (!mark)
        return false;
    /* Besides what the job allocated, its end holds its log (at the
       capacity it has grown to) and its targets (`JobTargets`, two words
       per entry). The batch bounds those too, so that a job that forces
       many cells while allocating little stays within about one batch of
       memory beyond what it keeps. */
    auto & log = undoLog;
    auto bookkeeping =
        (log.capacity() - *mark) * sizeof(EvalState::UndoEntry) + (log.size() - *mark) * 2 * sizeof(void *);
    /* So does the owner timeline. */
    bookkeeping += unforceOwnerBytes();
    return unforceCurrentScopeBytes() + bookkeeping >= state->settings.unforceBatchSize;
}

UnforceBatch::Element::Element(UnforceBatch & batch)
    : batch(batch)
{
    /* The visiting runs in our caller's callees. */
    batch.openAt(__builtin_dwarf_cfa());
}

UnforceBatch::Element::~Element()
{
    if (batch.full())
        batch.end();
}

void UnforceBatch::end()
{
    if (!mark)
        return;
    state->endUnforceScope(*std::exchange(mark, std::nullopt));
}


/* Per open scope, the stack pointer of its opener (see
   `unforceIsScopeStackSlot`), and this thread's lowest stack address. */
static thread_local std::vector<const char *> scopeStackMarks;
static thread_local const char * stackLow = [] {
    pthread_attr_t attr;
    void * addr = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
    }
    return (const char *) addr;
}();

void unforceNoteClientCode()
{
    if (!scopeStackMarks.empty())
        clientCodeInJob = true;
}

/* Live `UnforceSeen` sets on this thread. */
static thread_local std::vector<UnforceSeen *> seenSets;

UnforceSeen::UnforceSeen(std::set<const void *> & seen)
    : seen(seen)
{
    seenSets.push_back(this);
}

UnforceSeen::~UnforceSeen()
{
    std::erase(seenSets, this);
}

bool unforceSeenExact()
{
    return unforceSettings().keepSeen;
}

void unforceNoteSeen(std::set<const void *> & seen, const Value & cell, const void * key)
{
    if (seenSets.empty() || !evalRegion.base || !unforceSeenExact())
        return;
    /* A cell inside the job's memory is no target. A key inside it is
       found again at the job's end as an entry within that memory
       (`jobKeys`), and a cell that is its own key as an entry of the set,
       so neither is recorded: only a cell outside the job's memory that
       holds a container keyed elsewhere (a Bindings). */
    if (evalRegion.contains(&cell))
        return;
    for (auto s : seenSets)
        if (&s->seen == &seen) {
            if ((const void *) &cell == key)
                s->selfKeyed = true;
            else
                s->cells.push_back(&cell);
            return;
        }
}

/* The entries of `seen` inside the job's memory: the keys the job's
   reports added (no entry points into memory a job freed, since a job
   that frees rewrites its own first). */
static auto jobKeys(std::set<const void *> & seen)
{
    return std::ranges::subrange(
        seen.lower_bound((const void *) evalRegion.base), seen.lower_bound((const void *) evalRegion.cur));
}

/* Whether the live `UnforceSeen` sets reported anything in this job. */
static bool seenReportedAny()
{
    for (auto s : seenSets)
        if (!s->cells.empty() || s->selfKeyed || !jobKeys(s->seen).empty())
            return true;
    return false;
}

/* Per target, whether the live `UnforceSeen` sets reported it in this job:
   as a cell holding a container keyed elsewhere, or as a key of its own. A
   target in a set was added in this job, since it held a thunk when the
   job began, and a reported cell is never reverted (`seenAllKept`). */
static Reported reportedTargets(const JobTargets & targets)
{
    std::vector<const Value *> cells;
    for (auto s : seenSets) {
        if (cells.empty())
            cells.swap(s->cells);
        else {
            cells.insert(cells.end(), s->cells.begin(), s->cells.end());
            std::vector<const Value *>().swap(s->cells);
        }
    }
    std::sort(cells.begin(), cells.end());
    Reported reported(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        auto t = targets.cells[i];
        if (std::binary_search(cells.begin(), cells.end(), t))
            reported[i] = true;
        else
            for (auto s : seenSets)
                if (s->selfKeyed && s->seen.count(t)) {
                    reported[i] = true;
                    break;
                }
    }
    return reported;
}

/* Whether the decision kept everything the live `UnforceSeen` sets
   reported in this job: every reported target is promoted, and every entry
   inside the job's memory was copied. */
static bool seenAllKept(Evacuator & evacuator, const std::vector<bool> & promoted, const Reported * reported)
{
    if (reported)
        for (size_t i = 0; i < promoted.size(); ++i)
            if (!promoted[i] && (*reported)[i])
                return false;
    for (auto s : seenSets)
        for (auto key : jobKeys(s->seen))
            if (!evacuator.forwarded(key))
                return false;
    return true;
}

/* At a job's end, after the decision has copied what it keeps: rewrite the
   job's own entries of the live `UnforceSeen` sets to their copies'
   addresses, moving each node (no allocation). Only called when
   `seenAllKept`; the job otherwise keeps its memory and the entries stay
   valid as they are. The copies lie outside the job's memory, so none is
   met again. */
static void translateSeen(Evacuator & evacuator)
{
    for (auto s : seenSets) {
        auto stop = (const void *) evalRegion.cur;
        for (auto i = s->seen.lower_bound((const void *) evalRegion.base);
             i != s->seen.end() && std::less<const void *>{}(*i, stop);) {
            auto key = *i;
            auto copy = evacuator.forwarded(key);
            assert(copy && !evalRegion.contains(copy));
            auto node = s->seen.extract(i++);
            node.value() = copy;
            s->seen.insert(std::move(node));
            if (s->onRekey)
                s->onRekey(key, copy);
            stats.seenKept++;
        }
    }
}

static void forgetSeenReports()
{
    for (auto s : seenSets) {
        std::vector<const Value *>().swap(s->cells);
        s->selfKeyed = false;
    }
}

bool unforceIsScopeStackSlot(const void * p)
{
    if (scopeStackMarks.empty() || !stackLow)
        return false;
    /* The scope that will restore the slot: the job (the outermost scope). */
    auto mark = scopeStackMarks.front();
    return (const char *) p >= stackLow && (const char *) p < mark;
}

/* Push a young force's entry and change the owner to `o`: `opens` has room
   (reserved by the caller), and `setOwner` never throws. */
[[gnu::always_inline]] inline static void pushYoungOpen(const Value & v, uint32_t o)
{
    *opens.t++ = {&v, uint64_t(curOwner) << 1};
    setOwnerLast(o);
}

/* A young force by a target consumer: record the pair (a heuristic, see the
   setting) once, then push. The pair is policy only: if it cannot be
   recorded, it is not. */
[[gnu::noinline]] static void youngConsumerChange(const Value & v, uint32_t o)
{
    opens.reserve();
    auto pair = (uint64_t(curOwner) << 32) | o;
    if (o != noOwner && pair != lastConsumerPair) {
        try {
            if (ownerConsumers.insert(pair, ownerBudget / 4))
                lastConsumerPair = pair;
        } catch (std::bad_alloc &) {
        }
    }
    pushYoungOpen(v, o);
}

/* `opens` is full: grow it (which may throw, with nothing changed), then
   push. */
[[gnu::noinline]] static void youngOwnerChangeGrow(const Value & v, uint32_t o)
{
    opens.reserve();
    pushYoungOpen(v, o);
}

/* A young force that changes the owner to `o`. Its calls are tail calls,
   so the hook needs no stack frame. */
[[gnu::always_inline]] inline static void youngOwnerChange(const Value & v, uint32_t o)
{
    if (statsObserved) [[unlikely]]
        stats.ownerChanges++;
    if (curOwner != noOwner && ownerConsumersOn) [[unlikely]]
        return youngConsumerChange(v, o);
    if (opens.t == opens.e) [[unlikely]]
        return youngOwnerChangeGrow(v, o);
    pushYoungOpen(v, o);
}

/* A force of a young cell outside the current segment: its owner becomes
   the current one while it runs. */
[[gnu::always_inline]] inline static void youngForce(const Value & v)
{
    auto o = ownerOf(&v);
    /* `ownerLookups` counts these forces; the common one changes the
       owner, so it is `ownerChanges` that the hot path counts. */
    if (o == curOwner) [[unlikely]] {
        if (statsObserved) [[unlikely]]
            stats.ownerLookups++;
        return;
    }
    youngOwnerChange(v, o);
}

/* The start of a logged force: an old cell. Every container is reserved
   before anything is pushed, so a failed allocation changes nothing. */
[[gnu::noinline]] static void logOldForce(EvalState & state, Value & v, const Value & preImage)
{
    if (unforceIsScopeStackSlot(&v))
        return;
    if (undoLog.size() == undoLog.capacity())
        undoLog.reserve(std::max<size_t>(16, 2 * undoLog.capacity()));
    opens.reserve();
    openLogIndices.reserve();
    auto index = undoLog.size();
    /* An owner's name must fit, and differ from the sentinel's. */
    if (ownersOn && index >= segSentinel.owner) [[unlikely]]
        overflowOwners();
    /* inc2c-1: record the CAPTURED pre-image, not a re-read of `v`. The force
       site elected the forcer and CAS'd word0 to the pending word before
       calling here, so `v` is no longer its original thunk/app; the election
       passed the pre-CAS Value down as `preImage`. (The single-argument
       `logPreImageSlow` passes `v` itself, which is still intact for its
       callers.) */
    undoLog.push_back({&v, preImage});
    *opens.t++ = {&v, (uint64_t(curOwner) << 1) | 1};
    *openLogIndices.t++ = index;
    if (ownersOn)
        setOwner(uint32_t(index));
}

/* `logPreImageSlow` past its common cases. Only the old-cell branch records
   contents, so only it needs the captured pre-image. */
[[gnu::noinline]] static void logPreImageRest(EvalState & state, Value & v, const Value & preImage)
{
    if (!evalRegion.contains(&v))
        return logOldForce(state, v, preImage);
    /* Young, but not of the current owner segment. The owner timeline is keyed
       by address, so the pre-image is not needed here. */
    if (ownersOn)
        youngForce(v);
}

void EvalState::logPreImageSlow(Value & v, const Value & preImage)
{
    auto p = (const char *) &v;
    /* A young cell of the open segment keeps the owner, and needs no log
       entry. With owners off, the open segment is all of [base, cur). */
    if (p >= ownerSeg && p < evalRegion.cur) [[likely]]
        return;
    /* Mostly a driver forcing a cell of the segment just below the open
       one, whose owner differs from the current one (adjacent segments
       never share an owner). With owners off, `ownerSegs` holds only its
       sentinel, which no address reaches. */
    if (p >= segStart(ownerSegs.t[-1]) && p < ownerSeg)
        return youngOwnerChange(v, ownerSegs.t[-1].owner);
    return logPreImageRest(*this, v, preImage);
}

/* Single-argument callers (`handleEvalFailed`) have not overwritten `v` yet,
   so `v` is its own pre-image. */
void EvalState::logPreImageSlow(Value & v)
{
    logPreImageSlow(v, v);
}

/* The end of a force whose start pushed a logged entry on `opens` (the
   top). */
[[gnu::noinline]] static void endLoggedOpen()
{
    auto saved = opens.back().saved;
    opens.pop();
    openLogIndices.pop();
    if (ownersOn)
        setOwner(uint32_t(saved >> 1));
}

void EvalState::endLoggedForceSlow(Value & v)
{
    /* Only a force whose start pushed `v` on `opens` has anything to end. */
    if (opens.t[-1].cell != &v) [[likely]]
        return;
    auto saved = opens.t[-1].saved;
    if (saved & 1) [[unlikely]]
        return endLoggedOpen();
    --opens.t;
    /* Only a young force that changed the owner pushes this; owners may
       have run out of budget since. */
    if (ownersOn) [[likely]]
        setOwnerLast(uint32_t(saved >> 1));
}

/* inc2c-2: the process-global single-job arbiter `jobOwner` -- which CAS'd
   nullptr->this and panicked if a second EvalState's job opened while one was
   open -- is DELETED. It existed because the region/owner-timeline state was
   process-wide, so at most one job could own it. inc2c-1 made all that state
   per-worker (thread_local), and 2c-2 binds each worker to its own disjoint
   stripe; concurrent jobs are therefore physically independent and need no
   global arbiter. Admission is now per-worker and lazy: the first force on a
   worker (isolationDepth 0->1) binds its stripe and opens its own timeline. */

size_t EvalState::beginUnforceScope(const void * stackMark)
{
    /* The canonical frame address is the caller's stack pointer at the call:
       frames the caller enters from now on lie below it, its own locals at
       or above. */
    scopeStackMarks.push_back((const char *) (stackMark ? stackMark : __builtin_dwarf_cfa()));
    if (!isolationDepth++) {
        /* First force on this worker: bind its stripe (one per worker, indexed
           by the eval-thread id -- 0 for the main thread -> stripe 0 -> the
           whole arena at eval-cores == 1), bind the stripe count to eval-cores,
           and point `curTask` at this worker's task (Inc3 escape scaffolding).
           Nested forces on the same worker just bump the depth. */
        bindStripeCount(executor->evalCores);
        myStripe = myEvalThreadId;
        curTask = &rootTask;
        openRegion();
        openOwners(*this);
    }
    return undoLog.size();
}

void EvalState::endUnforceScope(size_t mark)
{
    scopeStackMarks.pop_back();
    /* Only the outermost scope is a job. */
    if (--isolationDepth)
        return;
    /* inc2c-2: no `jobOwner` to release -- admission is per-worker now.
       `curTask` is cleared at the very end, after the join (where Inc3 consumes
       `curTask->escaped`); inert in 2c-2. */

    auto & cfg = unforceSettings();
    stats.jobs++;
    rootTask.generation++;
    stats.logged += undoLog.size() - mark;
    dropOpenForces(mark);

    /* Before anything is released: decide what to promote, and copy it.
       The Evacuator, whose table is keyed by young addresses, goes before
       the region does. */
    bool keepWhole = false;
    {
        PauseRegion pause;
        Evacuator evacuator{*this};
        std::optional<JobTargets> jt;
        std::vector<bool> promoted;
        std::vector<Value> copies;
        try {
            jt.emplace(undoLog, mark);
            std::optional<Reported> printed;
            if (seenReportedAny())
                printed = reportedTargets(*jt);
            promoted = decideJob(
                *this,
                mark,
                *jt,
                forceCounts,
                YoungSpace{},
                /* String data is never walked: a long string must not buy
                   the closure steps. */
                size_t(evalRegion.cur - evalRegion.base)
                    - std::min(evalRegion.stringBytes, size_t(evalRegion.cur - evalRegion.base)),
                evacuator,
                copies,
                printed ? &*printed : nullptr,
                &keepWhole);
            /* A printer's «repeated» entry for an object the decision does
               not keep (client code ran in the job, or the closure fell back
               to reverting) would be lost, and the object, rebuilt later,
               printed in full. Keep the whole job instead: nothing is
               promoted, reverted or freed, as without unforcing. The
               decision may also have chosen that (`decideJob`). */
            keepWhole = keepWhole || !seenAllKept(evacuator, promoted, printed ? &*printed : nullptr);
            if (!keepWhole)
                translateSeen(evacuator);
            forgetSeenReports();
            pruneCounts(forceCounts, *jt, promoted);
        } catch (std::bad_alloc &) {
            unforceFatal("deciding which values to keep at the end of an unforce batch, and copying them");
        }
        closeOwners();

        /* The release: assigns, reverts and frees. Nothing is allocated. */
        if (!keepWhole) {
            auto & targets = jt->cells;
            for (size_t i = 0; i < targets.size(); ++i)
                if (promoted[i]) {
                    *targets[i] = copies[i];
                    stats.promoted++;
                }

            for (size_t i = 0; i < targets.size(); ++i) {
                if (promoted[i])
                    continue;
                *targets[i] = jt->preImage(i);
                stats.reverted++;
                if (clientCodeInJob)
                    stats.revertedInClientJobs++;
            }
        }
    }

    if (keepWhole)
        stats.keptJobs++;
    else if (evalRegion.base)
        stats.freedBytes += evalRegion.cur - evalRegion.base;
    clientCodeInJob = false;
    undoLog.resize(mark);
    undoLog.shrink_to_fit();
    /* Task-join: the root task reclaims its nursery segment. */
    rootTask.joinAndReclaim(keepWhole);

    /* inc2c-2: the join is done; clear the Inc3 escape context (inert in 2c-2
       -- `escaped` is never appended to, so nothing to pin). */
    curTask->escaped.clear();
    curTask = nullptr;

    if (cfg.stats)
        printStatsAtExit();
}

/* inc2c-2: the per-worker scope close (Inc3 scaffolding; see Executor::worker).
   Unlike `endUnforceScope` -- the main thread's full promote/revert decision --
   a worker RETAINS its whole stripe at join: revert stays OFF until Inc3, so
   reclamation is retain-only (`joinAndReclaim(keepWhole = true)` ->
   `retainRegion`, never `releaseRegion`/revert). Every surviving cell keeps its
   address, so pointer-equality is trivially preserved. INERT until 2c-3: never
   reached while the eval-cores > 1 + unforce ctor guard stands.

   NOTE for 2c-3: this retains into the still-shared `rootTask` (see its note);
   it must reclaim a PER-WORKER task segment before it can run concurrently, and
   this worker's `stats` must be summed into a process total here. */
void EvalState::endUnforceScopeRetaining()
{
    scopeStackMarks.pop_back();
    if (--isolationDepth)
        return;
    stats.jobs++;
    rootTask.generation++;
    /* A cleanly-finished worker has no running forces open and never consumes
       its undo log (retain-only), so just close the owner timeline, retain the
       whole stripe, and drop the log. 2c-3 adds per-worker task segmentation
       and the cross-worker stats merge here. */
    closeOwners();
    undoLog.clear();
    undoLog.shrink_to_fit();
    rootTask.joinAndReclaim(/*keepWhole=*/true);
    curTask->escaped.clear();
    curTask = nullptr;
}

} // namespace nix
