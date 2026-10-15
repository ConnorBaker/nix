/* PROTOTYPE: unforcing thunks between jobs.

   Thunk update in `forceValue` is the evaluator's only mutation of existing
   cells. While a job (an outermost unforce scope) runs, the pre-image of
   every *old* cell it forces is logged; at the end of the job each logged
   cell is either reverted to its pre-image, replaced by a derivation stub,
   or promoted (its forced value copied out of the job region). Afterwards no
   old cell points into the job region, which is then released wholesale. */

#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
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

EvalRegion evalRegion;
const Value * unforceOpenTop = nullptr;
BumpArena evalBaseArena;

/* The address range freed by the last region release. */
static std::pair<uintptr_t, uintptr_t> lastReleased{0, 0};
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

/* Whether unforcing happens at all is the `eval-unforce` setting; these
   NIX_UNFORCE_* variables are for experiments and debugging, and default to
   the measured-best choices. */
struct Settings
{
    /* "": log every forced cell, revert all, let Boehm reclaim.
       "values": Values in a job region. "all": everything in it. */
    std::string region = getEnv("NIX_UNFORCE_REGION").value_or("all");
    bool tripwire = getEnv("NIX_UNFORCE_TRIPWIRE").value_or("0") != "0";
    bool stubs = getEnv("NIX_UNFORCE_STUB").value_or("0") != "0";
    /* Promote a cell forced in this many jobs (0: never). Needs "all". */
    uint32_t promoteAfter = std::stoul(getEnv("NIX_UNFORCE_PROMOTE").value_or("2"));
    bool stats = getEnv("NIX_UNFORCE_STATS").value_or("0") != "0";
    /* Keep one region mapped across jobs and zero on allocation, instead of
       mapping fresh (kernel-zeroed) pages for every job. */
    bool reuse = getEnv("NIX_UNFORCE_REUSE").value_or("0") != "0";
    /* "bump": one flat region per outermost scope. "mimalloc": a mimalloc
       heap per scope, in an exclusive arena; every scope frees its memory
       when it ends, and only the outermost one promotes. */
    std::string backend = getEnv("NIX_UNFORCE_BACKEND").value_or("bump");
    /* How a job's promotion set is closed under "producer-of" (see
       `decideJob`): "on" (promote the producers), "off" (counts only, the
       old behaviour; for negative controls). */
    std::string markers = getEnv("NIX_UNFORCE_MARKERS").value_or("on");
    /* The closure's step budget per job (`decideJob`): a fixed allowance
       plus a number of steps per KiB the job allocated. Dev overrides. The
       comparison's realistic workloads take at most 57 steps per KiB in a
       job (`nix search`), and 1 M steps is under 0.2 G instructions. */
    uint64_t walkBudgetBase = std::stoull(getEnv("NIX_UNFORCE_WALK_BUDGET_BASE").value_or("1000000"));
    uint64_t walkBudgetPerKiB = std::stoull(getEnv("NIX_UNFORCE_WALK_BUDGET_PER_KIB").value_or("256"));
    /* What a job whose closure runs out of budget does: "promote" every
       target, or "revert" every target. Both keep identity. */
    std::string walkFallback = getEnv("NIX_UNFORCE_WALK_FALLBACK").value_or("promote");
    /* Most entries the force counts may hold (`countTargets`); 0: the
       default, max(256 Ki, eval-unforce-batch-size / 256). Dev override. */
    uint64_t countsCap = std::stoull(getEnv("NIX_UNFORCE_COUNTS_CAP").value_or("0"));
    /* Whether printers' `seen` sets are kept exact across jobs
       (`UnforceSeen`). Dev override, for measurements and as a negative
       control: "0" restores forgetting them. */
    bool keepSeen = getEnv("NIX_UNFORCE_SEEN").value_or("1") != "0";
    /* How the closure finds a young object's producer (bump backend):
       "owners" (the owner timeline: exact, no walk) or "markers" (the
       interval-pruned walks). The mimalloc backend always uses markers. */
    std::string identity = getEnv("NIX_UNFORCE_IDENTITY").value_or("owners");
    /* Dev: with "owners", also compute the markers closure and report
       targets that the owner closure promotes and markers does not (there
       must be none: markers' candidates include every true owner). */
    bool ownerCheck = getEnv("NIX_UNFORCE_OWNER_CHECK").value_or("0") != "0";
    /* With "owners": also nominate the consumers of promoted targets (a
       target that forced a young cell another target owns), as markers'
       candidate test does as a side effect. A heuristic for CPU (a consumer
       of a hot producer is usually forced again); the closure keeps
       identity either way. "0" turns it off. */
    bool ownerConsumers = getEnv("NIX_UNFORCE_OWNER_CONSUMERS").value_or("1") != "0";
};

const Settings & unforceSettings()
{
    static const Settings s;
    return s;
}

/* The tripwire keeps every released region reserved, so it uses smaller
   ones: 64 GiB regions would exhaust a 48-bit address space after about
   4000 jobs. */
size_t regionSize()
{
    return unforceSettings().tripwire ? size_t(4) << 30 : size_t(64) << 30;
}

struct Stats
{
    size_t jobs = 0, logged = 0, reverted = 0, stubbed = 0, promoted = 0, copiedBytes = 0, regionPeak = 0, relogged = 0;
    /* Targets logged in a job that an earlier job had forced and reverted. */
    size_t reforced = 0;
    /* Job memory released, summed over jobs. */
    size_t freedBytes = 0;
    /* Force markers: promotions added by the producer closure, closure
       rounds, budget steps the closure was charged, jobs whose closure
       changed anything, and the bookkeeping: bytes the markers add to log
       entries, and the largest transient bookkeeping of one job close
       (estimated). */
    size_t markerPromoted = 0, markerRounds = 0, markerSteps = 0, markerJobs = 0;
    /* The closure's budget: jobs that ran out of it (and so promoted, or
       reverted, every target), the most steps one job took, and the most
       steps per KiB of job memory one job took. */
    size_t budgetJobs = 0, maxJobSteps = 0, maxStepsPerKiB = 0;
    /* The force counts: the most entries they held, and how often they
       were rebuilt from one job's targets for outgrowing their cap. */
    size_t countsPeak = 0, countsResets = 0;
    /* Counted targets on the thread's stack (stack slots the job restores),
       whose addresses a later job may reuse for another cell (P5). */
    size_t stackTargets = 0;
    size_t markerLogBytes = 0, markerPeakBytes = 0;
    /* Jobs that promoted nothing because client code ran in them, and the
       cells reverted at their ends. */
    size_t clientJobs = 0, revertedInClientJobs = 0;
    /* Cells reverted by nested scopes of the mimalloc backend, which never
       promote (so these are reverted whatever the promotion threshold). */
    size_t innerScopeReverts = 0;
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
       and consumers nominated. `ownerCheckExtra`: targets the owner closure
       promoted that the markers closure did not (NIX_UNFORCE_OWNER_CHECK;
       must stay 0), and `ownerCheckFewer`, the converse (the markers walk's
       false positives). */
    size_t ownerLookups = 0, ownerChanges = 0, ownerSegsPeak = 0, ownerJobLookups = 0, ownerJobs = 0,
           ownerConsumerNominated = 0, ownerCheckExtra = 0, ownerCheckFewer = 0;
} stats;

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

bool nested()
{
    return unforceSettings().backend == "mimalloc" && !unforceSettings().region.empty();
}

/* With NIX_UNFORCE_STATS: print a summary of all scopes when the process
   exits. */
static void printStatsAtExit()
{
    [[maybe_unused]] static bool registered =
        (std::atexit([] {
             std::cerr << fmt(
                 "unforce: %s %d, logged %d, reverted %d, stubbed %d, promoted %d (relogged %d), copied %d MiB, "
                 "largest scope %d MiB; reforced %d, freed %d MiB; markers %s: promoted %d, "
                 "rounds %d, steps %d, jobs %d, log %d KiB, peak %d KiB, over budget %d (%s), max job steps %d, "
                 "max steps per KiB %d; counts peak %d, counts resets %d, stack targets %d; client-code jobs %d, reverted in client-code jobs %d, "
                 "inner-scope reverts %d; seen nominated %d, kept %d; jobs kept whole %d (%d KiB)\n",
                 nested() ? "scopes" : "jobs",
                 stats.jobs,
                 stats.logged,
                 stats.reverted,
                 stats.stubbed,
                 stats.promoted,
                 stats.relogged,
                 stats.copiedBytes >> 20,
                 stats.regionPeak >> 20,
                 stats.reforced,
                 stats.freedBytes >> 20,
                 unforceSettings().markers,
                 stats.markerPromoted,
                 stats.markerRounds,
                 stats.markerSteps,
                 stats.markerJobs,
                 stats.markerLogBytes >> 10,
                 stats.markerPeakBytes >> 10,
                 stats.budgetJobs,
                 unforceSettings().walkFallback,
                 stats.maxJobSteps,
                 stats.maxStepsPerKiB,
                 stats.countsPeak,
                 stats.countsResets,
                 stats.stackTargets,
                 stats.clientJobs,
                 stats.revertedInClientJobs,
                 stats.innerScopeReverts,
                 stats.seenNominated,
                 stats.seenKept,
                 stats.keptJobs,
                 stats.keptJobBytes >> 10);
         }),
         true);
}

/* One scope of the mimalloc backend. */
struct Frame
{
    size_t logMark;
    void * heap;
    size_t bytes = 0;
    /* Value cells are carved from chunks of the heap in allocation order,
       so that a cell's address gives its allocation time (`CellClock`). */
    std::vector<char *> chunks;
    char * cellCur = nullptr;
    char * cellEnd = nullptr;
};

/* Bytes per cell chunk of the mimalloc backend, and cells per chunk. */
constexpr size_t cellChunkBytes = 32 << 10;
constexpr size_t cellsPerChunk = cellChunkBytes / 16;

std::vector<Frame> frames;

#if NIX_UNFORCE_MIMALLOC
mi_arena_id_t arenaId = nullptr;
uintptr_t arenaLo = 0, arenaHi = 0;
mi_theap_t * currentTheap = nullptr;

bool inArena(const void * p)
{
    return (uintptr_t) p >= arenaLo && (uintptr_t) p < arenaHi;
}

bool inHeap(void * heap, const void * p)
{
    return inArena(p) && mi_heap_contains((mi_heap_t *) heap, p);
}

void setCurrentHeap(void * heap)
{
    evalRegion.heap = heap;
    evalRegion.allTypes = heap != nullptr;
    currentTheap = heap ? mi_heap_theap((mi_heap_t *) heap) : nullptr;
}
#endif

/* The allocation clock of the innermost scope: the bump cursor, or with
   the mimalloc backend the time of the next cell (see `CellClock`). It only
   grows while a scope is open. */
static uint64_t unforceClock()
{
#if NIX_UNFORCE_MIMALLOC
    if (nested()) {
        if (frames.empty() || frames.back().chunks.empty())
            return 0;
        auto & f = frames.back();
        return (f.chunks.size() - 1) * cellsPerChunk + (f.cellCur - f.chunks.back()) / 16;
    }
#endif
    return (uintptr_t) evalRegion.cur;
}

/* Log indices of the logged forces that are running, innermost last. */
static thread_local std::vector<size_t> activeForces;

/* ---------------------------------------------------------------------------
   The owner timeline (bump backend, NIX_UNFORCE_IDENTITY=owners).

   owner(x) of a young object x (MARKERS.md §1.1, the policy comment below)
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
   Owner changes cut the job's bump range [base, cur) into segments
   [start, next start) with one owner each, appended in address order,
   because the bump cursor is the allocation clock. A young cell of the
   current segment [seg, cur) has the current owner, so its force changes
   nothing, and `logPreImage` does not leave the inline path for it. Only a
   young cell of an earlier segment looks its owner up (binary search).
   Zero-length segments are overwritten and equal neighbours merged, so a
   force that allocates nothing leaves no trace.

   The hooks keep nothing across the evaluation (STACK-PARITY.md, R1): a
   force that pushes something (a logged force, or a young force that
   changed the owner) pushes `v` on `opens`, and `endLoggedForce` leaves the
   inline path only when `unforceOpenTop` is `v`.

   At the job's end owner(x) is exact for every copied object, so the
   closure needs no walk: promote S0, look up the owner of every
   identity-bearing object the Evacuator copies, promote it if it is a
   reverted target, until nothing changes (`closeByOwners`).
   --------------------------------------------------------------------------- */
struct OwnerSeg
{
    uintptr_t start;
    const Value * owner;
};

/* Process-wide, like `evalRegion` (one job per process: `jobOwner`). Not
   thread_local: libnixexpr's thread_local variables use the general dynamic
   TLS model, a __tls_get_addr call per access, and a force by the driver of
   a young cell of a target changes the owner twice (about 300 instructions
   with these thread_local; rss-indep-3000 does it 90 M times). */
static std::vector<OwnerSeg> ownerSegs;
/* Forces that pushed something, innermost last: a logged force (`logged`;
   the owner changed iff `changed`), or a young force that changed the
   owner. `saved` is the owner to restore. */
struct OpenForce
{
    const Value * cell;
    const Value * saved;
    bool logged, changed;
    /* A logged force's log index. */
    size_t logIndex;
};

static std::vector<OpenForce> opens;
static const Value * curOwner = nullptr;
static bool ownersOn = false;
static bool ownerConsumersOn = false;
/* (consumer, producer): the target that was the owner when a young cell of
   another target was forced (`NIX_UNFORCE_OWNER_CONSUMERS`). */
static std::vector<std::pair<const Value *, const Value *>> ownerConsumers;


static void setOwner(const Value * o)
{
    auto c = (uintptr_t) evalRegion.cur;
    if (ownerSegs.back().start == c) {
        /* Nothing was allocated under the previous owner since it began. */
        ownerSegs.back().owner = o;
        if (ownerSegs.size() > 1 && ownerSegs[ownerSegs.size() - 2].owner == o)
            ownerSegs.pop_back();
    } else if (ownerSegs.back().owner != o) {
        ownerSegs.push_back({c, o});
        stats.ownerSegsPeak = std::max(stats.ownerSegsPeak, ownerSegs.size());
    }
    curOwner = o;
    evalRegion.seg = (char *) ownerSegs.back().start;
}

/* The owner of the young object at `p` (an address in [base, cur)). */
static size_t ownerHint = 0;

static const Value * ownerOf(const void * p)
{
    auto a = (uintptr_t) p;
    /* Lookups come in runs (the Evacuator copies depth first). */
    if (auto h = ownerHint; h < ownerSegs.size() && ownerSegs[h].start <= a
                             && (h + 1 == ownerSegs.size() || a < ownerSegs[h + 1].start))
        return ownerSegs[h].owner;
    auto i = std::upper_bound(
        ownerSegs.begin(), ownerSegs.end(), a, [](uintptr_t a, const OwnerSeg & s) { return a < s.start; });
    assert(i != ownerSegs.begin());
    ownerHint = i - ownerSegs.begin() - 1;
    return std::prev(i)->owner;
}

/* A job opens: everything is the driver's until a force says otherwise. */
static void openOwners()
{
    auto & cfg = unforceSettings();
    ownersOn = cfg.identity == "owners" && cfg.markers != "off" && evalRegion.cur && !nested();
    ownerConsumersOn = cfg.ownerConsumers;
    ownerSegs.clear();
    ownerConsumers.clear();
    curOwner = nullptr;
    if (ownersOn)
        ownerSegs.push_back({(uintptr_t) evalRegion.base, nullptr});
    evalRegion.seg = evalRegion.base;
}

static void setOpenTop()
{
    unforceOpenTop = opens.empty() ? nullptr : opens.back().cell;
}

static void pushOpen(const Value * cell, bool logged, bool changed, const Value * owner, size_t logIndex = 0)
{
    opens.push_back({cell, curOwner, logged, changed, logIndex});
    if (changed)
        setOwner(owner);
    unforceOpenTop = cell;
}

static void closeOwners()
{
    ownersOn = false;
    ownerConsumers.clear();
    ownerSegs.clear();
    opens.clear();
    unforceOpenTop = nullptr;
    curOwner = nullptr;
}


/* Forget running forces logged at or after `mark` (none should be). */
static void dropActiveForces(size_t mark)
{
    while (!activeForces.empty() && activeForces.back() >= mark)
        activeForces.pop_back();
    /* Their `opens` entries (logged ones are pushed whatever the identity
       mode, so that `endLoggedForce` finds them). */
    while (!opens.empty() && (!opens.back().logged || opens.back().logIndex >= mark))
        opens.pop_back();
    setOpenTop();
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
#  if NIX_UNFORCE_MIMALLOC
    /* Push the committed part of each area of each scope heap. */
    for (auto & f : frames)
        mi_heap_visit_blocks(
            (mi_heap_t *) f.heap,
            false,
            [](const mi_heap_t *, const mi_heap_area_t * area, void * block, size_t, void *) -> bool {
                if (!block && area && area->committed)
                    GC_push_all(area->blocks, (char *) area->blocks + area->committed);
                return true;
            },
            nullptr);
#  endif
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

/* With NIX_UNFORCE_REUSE, the region the next job reuses. */
static char * regionSpare = nullptr;

static size_t pageSize()
{
    static const size_t page = sysconf(_SC_PAGESIZE);
    return page;
}

static char * pageUp(char * p)
{
    return (char *) (((uintptr_t) p + pageSize() - 1) & ~(uintptr_t) (pageSize() - 1));
}

/* Kept jobs (`retainRegion`) share one mapping: [keptStart, keptFloor) holds
   their objects, which are ordinary memory from then on, and the jobs after
   them allocate above it, up to keptEnd, so a kept job costs no page or
   mapping of its own (review of markers v4, S4). */
static char * keptStart = nullptr;
static char * keptFloor = nullptr;
static char * keptEnd = nullptr;

/* The side table of Env sizes (`EvalRegion::envSizes`) of the bump
   region's mapping `envSizeMapping`, and the sizes too large for it. */
static uint8_t * envSizeTable = nullptr;
static const char * envSizeMapping = nullptr;
static std::unordered_map<const void *, size_t> bigEnvSizes;

static size_t envSizeTableBytes()
{
    return regionSize() / 16;
}

static void noteBigEnv(const void * env, size_t size)
{
    bigEnvSizes[env] = size;
}

/* The size of a young Env: with the mimalloc backend from its prefix,
   otherwise from the side table. */
static size_t envSizeOf(const Env * e, bool heap)
{
    if (heap)
        return *(const size_t *) ((const char *) e - 16);
    auto s = envSizeTable[((const char *) e - envSizeMapping) >> 4];
    return s < 255 ? s : bigEnvSizes.at(e);
}

static void unmapEnvSizeTable()
{
    if (envSizeTable)
        munmap(envSizeTable, envSizeTableBytes());
    envSizeTable = nullptr;
    envSizeMapping = nullptr;
}

/* Point the region at the side table of `mapping`, mapping one if it has
   none yet. */
static void useEnvSizeTable(const char * mapping)
{
    if (envSizeMapping != mapping) {
        unmapEnvSizeTable();
        envSizeTable = (uint8_t *) mapArena(envSizeTableBytes());
        envSizeMapping = mapping;
    }
    evalRegion.envSizes = envSizeTable;
    evalRegion.envSizesBase = envSizeMapping;
}

/* At a job's end its Envs are copied or kept as old objects, or freed:
   none of their sizes is read again. */
static void dropEnvSizes(const char * base, const char * cur)
{
    std::unordered_map<const void *, size_t>().swap(bigEnvSizes);
    if (!envSizeTable || cur <= base)
        return;
    auto from = (uintptr_t) (envSizeTable + ((base - envSizeMapping) >> 4)) & ~(uintptr_t) (pageSize() - 1);
    auto to = (char *) envSizeTable + ((cur - envSizeMapping + 15) >> 4);
    /* Entries below `base` belong to kept jobs, those above `cur` to none
       yet: the whole pages can go. */
    madvise((void *) from, pageUp(to) - (char *) from, MADV_DONTNEED);
}

/* Stop allocating above the kept jobs: their mapping keeps only its prefix. */
static void leaveKeptMapping()
{
    if (!keptStart)
        return;
    auto tail = pageUp(keptFloor);
    if (tail < keptEnd)
        munmap(tail, keptEnd - tail);
    keptStart = keptFloor = keptEnd = nullptr;
}

void openRegion(bool allTypes)
{
    hookCollector();
    if (keptStart) {
        /* Continue above the kept jobs while at least half a region is left
           (with the tripwire, released memory is not reused). */
        if (!unforceSettings().tripwire && size_t(keptEnd - keptFloor) >= regionSize() / 2) {
            evalRegion.base = evalRegion.cur = keptFloor;
            evalRegion.end = keptEnd;
            evalRegion.allTypes = allTypes;
            useEnvSizeTable(keptStart);
            /* Released above the floor as zero pages, and the floor's own
               page zeroed above it (`releaseRegion`). */
            evalRegion.dirty = false;
            return;
        }
        leaveKeptMapping();
    }
    auto & spare = regionSpare;
    bool dirty = false;
    if (unforceSettings().reuse && spare)
        dirty = true;
    else
        spare = (char *) mapArena(regionSize());
    evalRegion.base = evalRegion.cur = spare;
    evalRegion.end = evalRegion.base + regionSize();
    evalRegion.allTypes = allTypes;
    evalRegion.dirty = dirty;
    useEnvSizeTable(spare);
    if (!unforceSettings().reuse)
        spare = nullptr;
}

/* With NIX_UNFORCE_VERIFY, check the invariant that makes releasing a
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
                        if (getenv("NIX_UNFORCE_VERIFY_DUMP")) {
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
    if (getenv("NIX_UNFORCE_VERIFY"))
        verifyNoEscapes();
    auto base = evalRegion.base;
    auto cur = evalRegion.cur;
    stats.regionPeak = std::max(stats.regionPeak, size_t(cur - base));
    lastReleased = {(uintptr_t) base, (uintptr_t) cur};
    evalRegion = EvalRegion{};
    dropEnvSizes(base, cur);
    if (keptStart && base == keptFloor) {
        /* Above kept jobs: free this job's pages, and zero what it used of
           the floor's page, which the kept objects share. */
        auto up = pageUp(base);
        memset(base, 0, std::min(cur, up) - base);
        if (cur > up)
            madvise(up, pageUp(cur) - up, MADV_DONTNEED);
        return;
    }
    if (unforceSettings().reuse)
        return;
    if (unforceSettings().tripwire) {
        mprotect(base, regionSize(), PROT_NONE);
        madvise(base, regionSize(), MADV_DONTNEED);
    } else
        munmap(base, regionSize());
    if (envSizeMapping == base)
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
    if (keptStart && base == keptFloor)
        keptFloor = cur;
    else {
        leaveKeptMapping();
        keptStart = base;
        keptFloor = cur;
        keptEnd = evalRegion.end;
    }
    if (regionSpare == base)
        regionSpare = nullptr;
    lastReleased = {0, 0};
    evalRegion = EvalRegion{};
}

bool isScalar(const Value & v)
{
    switch (v.internalType()) {
    case tInt:
    case tBool:
    case tNull:
    case tFloat:
    case tString:
    case tPath:
        return true;
    case tUninitialized:
    case tFailed:
    case tExternal:
    case tPrimOp:
    case tAttrs:
    case tListSmall:
    case tPrimOpApp:
    case tApp:
    case tThunk:
    case tLambda:
    case tListN:
    case tNumberOfInternalTypes:
        return false;
    }
    return false;
}

/* Recognise a derivation without forcing anything, so that no new log
   entries appear while a job is being closed. */
bool isDerivationNoForce(EvalState & state, const Value & v)
{
    if (v.internalType() != tAttrs)
        return false;
    auto type = v.attrs()->get(state.s.type);
    return type && type->value->internalType() == tString && type->value->string_view() == "derivation";
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

    /* The scope being closed, for the mimalloc backend; its copies go to
       wherever allocation currently goes (the enclosing scope's heap). */
    void * youngHeap = nullptr;

    /* If set, called for every young object as it is copied. */
    std::function<void(const void *)> onForward;

    /* If set, called with every identity-bearing young object as it is
       copied (cells, Bindings layers, element arrays): the owner closure. */
    void (*onIdentity)(void *, const void *) = nullptr;
    void * onIdentityCtx = nullptr;

    void note(const void * p, void * n)
    {
        forward.emplace(p, n);
        if (onForward)
            onForward(p);
    }

    bool young(const void * p) const
    {
#if NIX_UNFORCE_MIMALLOC
        if (youngHeap)
            return inHeap(youngHeap, p);
#endif
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
        auto size = envSizeOf(e, youngHeap != nullptr);
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

std::unordered_map<const Value *, std::pair<std::string, const Value *>> selectRegistry;
std::unordered_map<const Value *, std::string> sourceRegistry;

/* Builds derivation stubs: forced scalars are kept (copied out of the
   region), nested derivations (outputs) become stubs too, and every other
   attribute becomes `getAttr name source`, where `source` recomputes the
   whole derivation from a recipe outside the region. Callers pause the
   region. */
struct Stubber
{
    EvalState & state;
    Evacuator & evacuator;
    std::unordered_map<const Bindings *, Value *> memo;
    std::unordered_map<uint32_t, Value *> selectors;
    Value * getAttr = nullptr;

    Value * selector(Symbol name)
    {
        auto & sel = selectors[name.getId()];
        if (!sel) {
            if (!getAttr)
                getAttr = &state.getBuiltin("getAttr");
            auto nameV = state.allocValue();
            nameV->mkString(state.symbols[name], state.mem);
            sel = state.allocValue();
            sel->mkPrimOpApp(getAttr, nameV);
        }
        return sel;
    }

    Value * select(Value * source, Symbol name)
    {
        auto res = state.allocValue();
        res->mkApp(selector(name), source);
        if (getenv("NIX_UNFORCE_DEBUG"))
            selectRegistry[res] = {std::string(state.symbols[name]), source};
        if (getenv("NIX_UNFORCE_DEBUG")) {
            std::set<const Value *> seen{res};
            size_t depth = 0;
            for (auto v = source; v; ++depth) {
                if (!seen.insert(v).second) {
                    std::cerr << fmt(
                        "unforce debug: cycle selecting '%s' after %d steps\n", state.symbols[name], depth);
                    break;
                }
                if (v->internalType() == tApp)
                    v = v->app().right;
                else
                    break;
            }
            if (depth > 8)
                std::cerr << fmt("unforce debug: selector chain depth %d for '%s'\n", depth, state.symbols[name]);
        }
        return res;
    }

    Value * stub(const Value & drv, Value * source, unsigned depth)
    {
        if (auto i = memo.find(drv.attrs()); i != memo.end())
            return i->second;
        auto res = state.allocValue();
        memo.emplace(drv.attrs(), res);
        auto attrs = state.buildBindings(drv.attrs()->size());
        for (auto & a : *drv.attrs()) {
            auto & v = *a.value;
            Value * n;
            if (isScalar(v))
                n = evacuator.value(a.value);
            else if (depth < 2 && isDerivationNoForce(state, v))
                n = stub(v, select(source, a.name), depth + 1);
            else
                n = select(source, a.name);
            attrs.insert(a.name, n, a.pos);
        }
        res->mkAttrs(attrs.alreadySorted());
        return res;
    }
};

/* ---------------------------------------------------------------------------
   Force markers: closing a job's promotion set under "producer-of".

   Promotion breaks the evaluator's pointer identity (`eqValues` short-cuts on
   `&v1 == &v2`; Nixpkgs relies on it for `hostPlatform == buildPlatform`)
   when a promoted cell keeps a copy of a cell that a reverted target rebuilds
   when it is re-forced: two cells then stand for one cell of an evaluation
   without unforcing. Force counts cannot rule this out (an earlier promotion
   creates new old thunks with count 0, so a consumer can reach the threshold
   before its producer), so the counts only nominate promotions; this closure
   makes them safe.

   Definitions, for one job (outermost scope). Time is the job's allocation
   clock (`unforceClock`): the bump cursor, or the cell clock of the mimalloc
   backend's chunks; a young object's time a(x) is its address (bump), or its
   chunk position (mimalloc cells; other mimalloc objects have no time).
   - Targets T: old cells forced in the job (the undo log). Every logged force
     of t has an interval [s, e) of times: s when it was logged, e when it
     returned or threw (`endLoggedForce`). Forcing is LIFO, so the intervals
     form a forest (`parent`), and innermost(a) is the deepest interval
     containing time a.
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
   ∈ Evac(S) has owner(X) ∈ S or no owner. (Until 2026-10-15 OC covered
   cells only, and a promoted cell could keep a copy of a reverted
   producer's Bindings that held only old cells: base prints the second
   occurrence as «repeated», markers printed it in full; review
   notes/reviews/identity-comparison.md §2.2, B1.)

   OC is sufficient: a reverted target t re-forced later builds only cells
   that t owns, which are fresh: none of them survives, because a survivor is
   in Evac(S) and is owned by a member of S, and t ∉ S. A surviving copy is
   never rebuilt, because its owner is promoted (it is a value and is not
   re-forced), and every cell on a creator chain below an owner is built only
   by re-running the owner. So each cell of the evaluation without unforcing
   has at most one counterpart alive at any time, and the history-independence
   argument for pure revert (notes/reviews/identity-reliance-review.md §3.2)
   applies to the heap at the next job's start. OC is also necessary in
   general: a cell X ∈ Evac(S) owned by a reverted t is rebuilt as X' when t
   is re-forced, and `X == X'` is observable whenever X holds a function.

   Lemma V (no upward visibility). While t is being forced, cells owned by t
   are accessible only to t's own evaluation and to forces of young cells
   owned by t. Reason: a target u forced inside t starts from its pre-image,
   which refers only to old memory (old cells are written only by their own
   logged forces, A1/A3 of the review); u gains access to young objects only
   by allocating them (owned by u or below) or by reading the contents of
   completed forces, and t's content does not exist until t ends. A young
   cell forced inside t but created before t started (not owned by t)
   likewise runs on its own closure. Hence no cell owned by t is allocated
   while a target nested in t is running.

   Lemma C (closed region). If t owns X and X is reachable at job end from a
   promoted r ≠ t, then X ∈ R(c(t)), along a path whose interior consists of
   objects owned by t. Reason: before t ends, no object not owned by t points
   at an object owned by t (Lemma V; containers are immutable, a cell is
   written only by its own force, whose evaluation is closed over its creator's
   data). After t ends, t's evaluation has no frame left, and objects owned by
   t are found only through c(t): forcing a young thunk owned by t (found in
   c(t), or through the environment of a lambda in c(t)) puts what it builds
   into that thunk's own content, which is owned by t and stays reachable
   from c(t) through the thunk's cell; another evaluation that reads c(t)
   copies pointers that are reachable from c(t) already.

   Candidates. Let X ∈ Evac(S) be owned by t ∉ S. If t was running when X was
   allocated, then innermost(a(X)) is t's interval (Lemma V: no nested target
   was running); otherwise t had ended, a(X) ≥ e. Either way a(X) ≥ s, and
   by Lemma C every object on the path from c(t) satisfies the same, so
   allowed(t, a) := a ≥ s ∧ (a ≥ e ∨ innermost(a) is t's interval) holds
   along it. So t is a candidate producer iff a walk of R(c(t)) that never
   enters an object with ¬allowed(t, a(x)) reaches an identity-bearing object
   of Evac(S) (Lemma C holds for every object t owns). For a
   target with several intervals (forced again after an error) only a ≥ s
   of the first interval is used. Objects without a time are never pruned:
   that only adds candidates (with no times at all the walk is plain
   reachability, option C's candidate set, which Lemma C also covers).

   This is where the markers differ from allocation-interval attribution
   (option E of notes/research/identity-reliance.md, rejected as unsound):
   E names innermost(a(X)) as X's producer. That misses producers whose young
   thunk is forced inside a later target's interval (the "precedes" and
   "follows" demos in tests/functional/eval-unforce-identity/: the producer
   ended before X was allocated). Here the interval only *rules out* targets
   (those that started after X, or that were inside a nested target when X was
   allocated), and reachability from the target's own content, which the
   heap does preserve (Lemma C), supplies the producers that ended earlier.
   Reachability alone (option C) over-promotes every consumer; the interval
   test removes consumers that only read data allocated before they started.

   Rule. Promote the candidates (NIX_UNFORCE_MARKERS=on): S is the least
   superset of the count-nominated set S0 that satisfies OC under the
   candidate test, computed by rounds until no reverted target is a
   candidate; the promoted set is copied with one Evacuator, so the shared
   objects keep one copy. The decision, copies included, runs before the
   job's memory is released and writes no target (`decideJob`): with the
   default budget fallback every target is copied as it joins S and the
   copies mark Evac(S); with the "revert" fallback Evac(S) is marked by
   walking and the copies follow the decision. The release then only
   assigns, reverts and frees. The alternative,
   refusing every member of S0 that reaches a candidate's cell (the
   greatest subset of S0 satisfying OC), retains less per job but can starve:
   a consumer that forces a producer's young thunks in the job where the
   producer is nominated is a candidate (the walk cannot tell whether that
   young thunk was created before the consumer started), and in a Nixpkgs
   traversal that recurs in every job, so the fixpoint would never be
   promoted. The counts (K) stay a heuristic for which cells to consider;
   correctness depends only on the closure.

   Premises: A1–A4 of the review (every write to old memory is logged;
   old memory allocated during a job, i.e. fileEvalCache cells and C-API
   objects, is inert; no escapes; deterministic evaluation), and exceptions
   carry no evaluator values. Derivation stubs (NIX_UNFORCE_STUB) change
   identity by construction and are not covered. With the mimalloc backend
   only the outermost scope promotes (nested scopes revert everything), so
   the relogging of promoted cells into an enclosing scope cannot break OC.
   --------------------------------------------------------------------------- */

/* Times of young objects: the address in the bump region; for the mimalloc
   backend, a cell's position in its scope's chunks (no time otherwise). */
struct CellClock
{
    bool bump = true;
    /* (chunk start, time of its first cell), sorted by address. */
    std::vector<std::pair<uintptr_t, uint64_t>> chunks;

    std::optional<uint64_t> operator()(const void * p) const
    {
        auto a = (uintptr_t) p;
        if (bump)
            return a;
        auto i =
            std::upper_bound(chunks.begin(), chunks.end(), a, [](uintptr_t a, const auto & c) { return a < c.first; });
        if (i == chunks.begin())
            return std::nullopt;
        --i;
        if (a >= i->first + cellChunkBytes)
            return std::nullopt;
        return i->second + (a - i->first) / 16;
    }
};

/* A job's targets, in the order of their first log entries (= the order
   their first forces started). A cell may have been forced more than once
   (after a recoverable error); its oldest pre-image is the one from before
   the job.

   Memory. Built at every job's end, beside the log, so it is kept small:
   the pre-images stay in the log (which lives until the release), and the
   repeated cells are found by sorting the log indices by cell, not with a
   hash map (review of markers v4, B1-mem: the map and copied pre-images
   took about 70 bytes per target, more than the log itself). */
struct JobTargets
{
    const decltype(EvalState::undoLog) & log;
    std::vector<Value *> cells;
    /* The log index of each target's first entry, and whether it has more. */
    std::vector<size_t> first;
    std::vector<bool> repeated;

    JobTargets(const decltype(EvalState::undoLog) & log, size_t mark)
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

/* The force intervals of one job (log entries [mark, size)), compiled for
   the candidate test: per target, its first interval [s, e), whether it has
   more, and the intervals of the first one's direct children, which are
   disjoint and sorted. */
struct ForceMarkers
{
    struct Span
    {
        uint64_t s, e;
        bool single;
        uint32_t c0, c1;
    };

    std::vector<Span> spans;
    std::vector<std::pair<uint64_t, uint64_t>> childIntervals;

    ForceMarkers(const decltype(EvalState::undoLog) & log, size_t mark, uint64_t jobEnd, const JobTargets & targets)
    {
        /* A force still open at job end (none should be) ends with the job. */
        auto endOf = [&](size_t i) {
            auto e = log[i].end;
            return e == EvalState::UndoEntry::openForce ? jobEnd : e;
        };
        auto n = log.size() - mark;
        std::vector<uint32_t> begin(n + 1);
        for (auto j = mark; j < log.size(); ++j)
            if (log[j].parent >= int64_t(mark))
                begin[log[j].parent - mark + 1]++;
        for (size_t i = 0; i < n; ++i)
            begin[i + 1] += begin[i];
        childIntervals.resize(begin[n]);
        auto fill = begin;
        /* Log order is start order, so each list comes out sorted. */
        for (auto j = mark; j < log.size(); ++j)
            if (log[j].parent >= int64_t(mark))
                childIntervals[fill[log[j].parent - mark]++] = {log[j].start, endOf(j)};
        for (size_t i = 0; i < targets.size(); ++i) {
            auto f = targets.first[i];
            spans.push_back({log[f].start, endOf(f), !targets.repeated[i], begin[f - mark], begin[f - mark + 1]});
        }
    }

    /* allowed(t, a) of the comment above: whether target `t` (its span)
       can own an object allocated at time `a`. Inside t's own interval,
       innermost(a) is t unless a lies in one of its children's intervals
       (the intervals form a forest). With several intervals, only a >= s
       of the first is used. */
    bool allowed(const Span & t, uint64_t a) const
    {
        if (a < t.s)
            return false;
        if (!t.single || a >= t.e)
            return true;
        auto b = childIntervals.begin() + t.c0, e = childIntervals.begin() + t.c1;
        auto i = std::upper_bound(b, e, a, [](uint64_t a, const auto & c) { return a < c.first; });
        return i == b || !(a < std::prev(i)->second);
    }
};

/* Which objects belong to the job: its bump region, or (mimalloc backend)
   its scope heap. */
struct YoungSpace
{
    void * heap = nullptr;

    bool operator()(const void * p) const
    {
#if NIX_UNFORCE_MIMALLOC
        if (heap)
            return inHeap(heap, p);
#endif
        return evalRegion.contains(p);
    }
};

/* Per young object, four flags of the closure (`Closure`): in Evac(S),
   visited by the current walk, known to reach no Evac(S) cell from the
   walks still to come (a memo), and reached by some walk of this job. */
enum Flag : uint8_t { inEvac = 1, visited = 2, memo = 4, reached = 8 };

/* The flags of the bump region's objects: four bits per 16-byte granule
   (bump allocations are 16-byte aligned), side by side so that one load
   reads them all, in pieces covering 64 KiB of the region that are
   allocated when first touched. At most 3.1 % of the job's memory. */
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

    uint8_t get(const void * p) const
    {
        auto g = ((uintptr_t) p - base) >> granuleShift;
        auto & piece = pieces[g >> (pieceShift - granuleShift)];
        return piece ? (piece->nibbles[(g >> 4) % words] >> ((g & 15) * 4)) & 15 : 0;
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

    /* For a walk: if `p` is neither visited nor memo'd, mark it visited
       and return true. One lookup. */
    bool claim(const void * p)
    {
        auto g = ((uintptr_t) p - base) >> granuleShift;
        auto & piece = pieces[g >> (pieceShift - granuleShift)];
        if (!piece) [[unlikely]] {
            piece = std::make_unique<Piece>();
            allocated++;
        }
        auto & w = piece->nibbles[(g >> 4) % words];
        auto shift = (g & 15) * 4;
        if ((w >> shift) & (visited | memo))
            return false;
        w |= uint64_t(visited) << shift;
        return true;
    }

    void remove(const void * p, uint8_t flags)
    {
        auto g = ((uintptr_t) p - base) >> granuleShift;
        if (auto & piece = pieces[g >> (pieceShift - granuleShift)])
            piece->nibbles[(g >> 4) % words] &= ~(uint64_t(flags) << ((g & 15) * 4));
    }

    /* Returns its cost in closure steps (`Closure::charge`), scaled so
       that a step stands for about as much work as popping an object:
       64 words cleared, or 64 empty pieces skipped. */
    uint64_t removeAll(uint8_t flags)
    {
        auto mask = ~(uint64_t(flags) * 0x1111111111111111);
        for (auto & piece : pieces)
            if (piece)
                for (auto & w : piece->nibbles)
                    w &= mask;
        return 1 + pieces.size() / 64 + allocated * (words / 64);
    }

    size_t bytes() const
    {
        return pieces.capacity() * sizeof(pieces[0]) + allocated * sizeof(Piece);
    }
};

/* The same for the mimalloc backend, whose objects are not in one range. */
struct HeapMarks
{
    std::unordered_map<const void *, uint8_t> flags;

    uint8_t get(const void * p) const
    {
        auto i = flags.find(p);
        return i == flags.end() ? 0 : i->second;
    }

    uint8_t add(const void * p, uint8_t f)
    {
        auto & b = flags[p];
        auto old = b;
        b |= f;
        return old;
    }

    bool claim(const void * p)
    {
        auto & b = flags[p];
        if (b & (visited | memo))
            return false;
        b |= visited;
        return true;
    }

    void remove(const void * p, uint8_t f)
    {
        if (auto i = flags.find(p); i != flags.end())
            i->second &= ~f;
    }

    uint64_t removeAll(uint8_t f)
    {
        for (auto & [p, b] : flags)
            b &= ~f;
        return 1 + flags.size() / 8;
    }

    size_t bytes() const
    {
        return flags.size() * 32 + flags.bucket_count() * sizeof(void *);
    }
};

/* The closure of a job ran out of its step budget. */
struct OverBudget
{};

/* The producer closure of one job: walks of young object graphs along the
   edges the Evacuator copies, without copying.

   Cost. A job may take `budget` steps. A step is charged for every object
   a walk pops (plus one per 16 attributes of a Bindings layer or slots of
   an env whose children it scans), every element array it claims (plus
   one per 16 elements it scans), every target a round tests, and the
   bitmap clears of `dropMemos` in proportion to their size (review of v2, S1: a round tests every
   reverted target, so before these were charged a chain found one target
   per round, beside 300 000 targets whose walks pop nothing, cost 45 G
   instructions without ever reaching the budget). So the budget bounds the
   whole decision, not only its walks. These keep the count down:
   - Evac(S) is marked once per object, by the copies themselves or by a
     marking walk that stops at marked objects: linear in Evac(S).
   - A list's element array is visited once per walk however many list
     values share it.
   - Memo. While Evac(S) does not change, a walk from target w that ends
     without a hit has shown, for every object x it visited (pruned ones
     included), that no identity-bearing object of Evac(S) is reachable
     from x through objects with times >= θ, where θ = s(w), or e(w) if it
     pruned an object inside its own interval (a child's); without such
     prunes every object of time >= s(w) it met was allowed. A later walk u
     never enters an object of time < s(u) (allowed(u, a) implies
     a >= s(u)), so if s(u) >= θ, u can skip x: it would find no hit below
     x. A round walks the targets in start order, so a memo with θ = s(w)
     holds for every later walk of the round; one with θ = e(w) is held back
     until a walk starts at or after e(w). Every round starts with no memos,
     since it starts again from the earliest target. A walk that hits
     leaves no memo (its target joins S).
     Memos waiting for their θ hold at most `pendingCap` objects; beyond
     that a memo is not kept, which only costs steps.
   - `reached`. Every object visited by a walk that ends without a hit is
     marked `reached` (and stays so for the job). The objects a reverted
     target's walk can visit do not change while the job is decided (the
     graph is fixed), and all of them are `reached` once it has been
     walked: by its own walk, or by the earlier hit-free walk whose memo let
     it skip them (a memo covers only objects that walk explored or
     pruned). So when S grows and none of the objects newly added to
     Evac(S) is `reached`, no reverted target already walked can have
     become a candidate, and every memo still holds (a memo of x fails only
     if a new Evac(S) object is reachable from x through objects its walk
     explored). Only when a new object is `reached` are the memos dropped
     and all targets walked again. */
template<typename Marks>
struct Closure
{
    enum Kind : uint8_t { kValue, kEnv, kBindings };

    const YoungSpace & young;
    const CellClock & clock;
    Marks marks;
    std::vector<std::pair<const void *, Kind>> stack;
    /* `mark`'s element arrays whose elements are still to be added, and
       how many: taken one element at a time once `stack` is empty, so that
       marking a list of n young elements does not push n entries at once
       (review of markers v4, B1-mem: a 4 M-element list took 64 MiB of
       stack). */
    std::vector<std::pair<Value * const *, size_t>> arrays;
    /* Objects visited by the current walk. */
    std::vector<const void *> touched;
    /* Memos waiting for a walk that starts at or after their θ (a heap,
       smallest θ first), and the objects they hold. */
    std::vector<std::pair<uint64_t, std::vector<const void *>>> pending;
    static constexpr auto laterTheta = [](const auto & a, const auto & b) { return a.first > b.first; };
    size_t pendingSize = 0;
    static constexpr size_t pendingCap = size_t(1) << 20;
    /* Whether `mark` added an object some walk reached. */
    bool reachedNew = false;
    /* For the check (NIX_UNFORCE_CLOSURE_CHECK): no memos, and every
       promotion starts another round. */
    bool memos = true;
    /* Steps taken in this job, and how many it may take. */
    uint64_t steps = 0, budget = UINT64_MAX;
    /* If set, `mark` calls it with every identity-bearing object it adds
       (the owner closure, when it marks before copying). */
    void (*onMark)(void *, const void *) = nullptr;
    void * onMarkCtx = nullptr;
    /* What `mark` added to Evac(S), in bytes (string data, which it does
       not walk, excluded). */
    size_t markedBytes = 0;
    size_t peak = 0;

    void step()
    {
        if (++steps > budget) [[unlikely]]
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
            auto size = envSizeOf(e, young.heap != nullptr);
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
       one step per 16 of them (review of markers v3, S1': a 2 M-attribute
       Bindings cost one step per round). */
    size_t width(const void * p, Kind kind) const
    {
        switch (kind) {
        case kEnv:
            return envSizeOf((const Env *) p, young.heap != nullptr);
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
                reachedNew |= old & reached;
                markedBytes += k == kValue ? sizeof(Value)
                               : k == kEnv ? sizeof(Env) + width(p, k) * sizeof(Value *)
                                           : sizeof(Bindings) + width(p, k) * sizeof(Attr);
                if (onMark && k != kEnv)
                    onMark(onMarkCtx, p);
                stack.emplace_back(p, k);
            }
        };
        auto array = [&](Value * const * elems, size_t size) {
            auto old = marks.add(elems, inEvac);
            if (!(old & inEvac)) {
                reachedNew |= old & reached;
                markedBytes += size * sizeof(Value *);
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

    /* Whether a walk from target t's content, pruned by `allowed`, reaches
       an identity-bearing object of Evac(S): t is a candidate producer. */
    bool candidate(const Value & content, const ForceMarkers & markers, const ForceMarkers::Span & t)
    {
        /* Testing the target at all, even if its walk pops nothing. */
        step();

        /* Memos of earlier walks that hold from here on. */
        while (!pending.empty() && pending.front().first <= t.s) {
            std::pop_heap(pending.begin(), pending.end(), laterTheta);
            for (auto p : pending.back().second)
                marks.add(p, memo);
            pendingSize -= pending.back().second.size();
            pending.pop_back();
        }

        stack.clear();
        touched.clear();
        auto push = [&](const void * p, Kind k) {
            if (!p || !young(p) || !marks.claim(p))
                return;
            touched.push_back(p);
            stack.emplace_back(p, k);
        };
        bool hit = false, ownPruned = false;
        /* Whether `p` (not visited yet) is pruned for t. */
        auto pruned = [&](const void * p) {
            auto a = clock(p);
            if (!a || markers.allowed(t, *a))
                return false;
            ownPruned |= *a >= t.s;
            return true;
        };
        /* An element array is checked where it is reached: its elements
           are pushed only if it is neither pruned nor a hit. */
        auto array = [&](Value * const * elems, size_t size) {
            if (hit || !marks.claim(elems))
                return;
            charge(1 + size / 16);
            touched.push_back(elems);
            if (pruned(elems))
                return;
            if (marks.get(elems) & inEvac) {
                hit = true;
                return;
            }
            for (size_t i = 0; i < size; ++i)
                push(elems[i], kValue);
        };
        children(content, push, array);
        while (!hit && !stack.empty()) {
            auto [p, kind] = stack.back();
            stack.pop_back();
            step();
            if (pruned(p))
                continue;
            /* Identity is observable on cells (`eqValues`) and on Bindings
               (the printers' «repeated» tracking keys on the Bindings);
               envs are never compared. */
            if (kind != kEnv && marks.get(p) & inEvac) {
                hit = true;
                break;
            }
            charge(width(p, kind) / 16);
            children(p, kind, push, array);
        }

        /* `reached` records the walks that leave their target reverted
           (see `Closure`); a target that hit joins S. */
        for (auto p : touched) {
            marks.remove(p, visited);
            if (!hit)
                marks.add(p, reached);
        }
        notePeak();
        if (!hit && memos) {
            auto theta = ownPruned ? t.e : t.s;
            if (theta <= t.s)
                for (auto p : touched)
                    marks.add(p, memo);
            else if (pendingSize + touched.size() <= pendingCap) {
                pendingSize += touched.size();
                pending.emplace_back(theta, std::move(touched));
                std::push_heap(pending.begin(), pending.end(), laterTheta);
            }
        }
        return hit;
    }

    /* A round ended and S grew: the memos no longer hold. */
    void dropMemos()
    {
        pending.clear();
        pendingSize = 0;
        charge(marks.removeAll(memo));
    }

    void notePeak()
    {
        auto bytes = marks.bytes() + stack.capacity() * sizeof(stack[0]) + arrays.capacity() * sizeof(arrays[0])
                     + touched.capacity() * sizeof(void *) + pendingSize * sizeof(void *)
                     + pending.capacity() * sizeof(pending[0]);
        peak = std::max(peak, bytes);
    }
};

/* Dev: compare every closure with a plain recomputation (slow). */
static const bool checkClosure = getEnv("NIX_UNFORCE_CLOSURE_CHECK").value_or("0") != "0";

/* For tests of the fatal path: fail the decision's allocations. */
static const bool failDecide = getEnv("NIX_UNFORCE_FAIL_DECIDE").value_or("0") != "0";

/* The closure's rounds. Each round walks the reverted targets in start
   order and promotes a candidate as soon as it is found: `join(i)` adds
   target i's R(c) to Evac(S) (`mark`, or a copy that marks what it
   copies), so the targets after it are tested against the larger Evac(S).
   If what join added meets no object a reverted target's walk reached
   (`reachedNew` false; see `Closure`), the memos stay valid and the targets
   already tested stay non-candidates; otherwise the memos are dropped and
   another round tests everything again. The candidate test is monotone in
   S, so this reaches the least fixpoint. Returns the promotions added. */
template<typename Marks, typename Join>
size_t closePromotions(
    Closure<Marks> & closure,
    const JobTargets & targets,
    const ForceMarkers & markers,
    std::vector<bool> & promote,
    Join && join)
{
    for (size_t i = 0; i < targets.size(); ++i)
        if (promote[i])
            join(i);
    size_t added = 0;
    for (bool again = true; again;) {
        again = false;
        stats.markerRounds++;
        /* A memo holds only for walks that start after its own; a round
           starts again from the earliest target. */
        closure.dropMemos();
        for (size_t i = 0; i < targets.size(); ++i) {
            if (promote[i] || !closure.candidate(*targets.cells[i], markers, markers.spans[i]))
                continue;
            promote[i] = true;
            added++;
            closure.reachedNew = false;
            join(i);
            if (closure.reachedNew || !closure.memos) {
                closure.dropMemos();
                again = true;
            }
        }
    }
    return added;
}

/* Per target of a job, whether a live `UnforceSeen` set reported it
   (`reportedTargets`). */
using Reported = std::vector<bool>;

/* Count a job's forces of its targets; returns whether each reached K.

   The map's cap. So that the counts do not grow with the length of a
   traversal (epoch v3 review S3: about 50 bytes per distinct target, never
   erased), when counting this job's targets would take the map past
   max(256 Ki, batch size / 256) entries (NIX_UNFORCE_COUNTS_CAP), it is
   first rebuilt from this job's targets: the counts of cells this job did
   not force are forgotten, so it never holds more than max(cap, this job's
   targets). Dropping the whole map instead would drop this job's counts as
   well, and if every job outgrew the cap, a cell forced in every job would
   never reach K and would be recomputed in every job (review of markers
   v2, S2). A cell forced only in jobs that are not consecutive can still
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
    const Value * last = nullptr;

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
        if (!o || o == x.last)
            return;
        x.last = o;
        auto t = x.indexOf(o);
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
   when the printers reported nothing in the job), and the
   producer closure adds what identity needs (see above). This is the fallible step before the job's memory is released.
   It writes no target, so the caller turns an allocation failure here into
   a fatal error before anything has changed, and the release that follows
   only assigns, reverts and frees. The caller pauses the region.
   `youngBytes` is the memory the job allocated other than string data, which sets the closure's
   step budget. Returns a flag per target.

   The budget. The closure is a walk per reverted target per round, and
   targets can share young structure that every one of them may reach, so
   even with the memo it can take steps quadratic in the job's size (review
   S1: walk-2000-100000 took 400 M steps before the memo; see
   tmp/markers-v2's nested adversary for one the memo does not help). A job
   that runs out of budget falls back to a promotion set that needs no walk
   and keeps identity on its own:
   - "promote" (the default): S = T. Every young cell's owner is a target
     or nothing, so OC holds. This is what K = 1 does for every job: the
     job keeps what it forced, as without unforcing, and its CPU cost is
     the copy, linear in what is kept.
   - "revert": S = {} (pure revert). Nothing is kept, so a hot producer is
     rebuilt in later jobs; a workload that kept exceeding the budget
     would recompute it every job.
   Neither is partial: the fallback replaces the whole decision.

   Evac(S). With the "promote" fallback S only grows, so every target that
   joins S is copied at once and the Evacuator marks Evac(S) as it copies:
   no copy is ever wasted, and Evac(S) costs no walk of its own. With the
   "revert" fallback nothing may be copied before the decision is final, so
   Evac(S) is marked by a walk (`Closure::mark`) and the copies follow. */
std::vector<bool> decideJob(
    EvalState & state,
    size_t mark,
    const JobTargets & targets,
    bool canCopy,
    std::unordered_map<const Value *, uint32_t> & counts,
    const YoungSpace & young,
    const CellClock & clock,
    uint64_t jobEnd,
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

    if (canCopy && cfg.promoteAfter)
        promote = countTargets(counts, targets, state.settings.unforceBatchSize, printed);

    /* Cells reported to an `UnforceSeen` (holding a container a printer
       printed, or walked by `forceValueDeep` for a printer that prints
       later) are nominated like counted ones, so that what they hold stays
       forced and keeps its identity for the rest of the print. With
       NIX_UNFORCE_MARKERS=off they are promoted without the closure, as
       counted ones are. */
    if (printed && canCopy)
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
    if (!any || cfg.markers == "off" || !canCopy) {
        copyPromoted();
        return promote;
    }

    if (failDecide)
        throw std::bad_alloc();

    if (ownersOn && !young.heap) {
        auto nominated = promote;
        OwnerClosure oc(targets, promote);
        /* As below: a printing job that may be kept whole marks Evac(S)
           first, and copies only if it keeps less than half its memory. */
        bool markFirst = keepWhole && printed;
        std::optional<Closure<RegionMarks>> marker;
        if (markFirst) {
            marker.emplace(young, clock, RegionMarks(evalRegion.base, evalRegion.cur));
            marker->onMark = &OwnerClosure::note;
            marker->onMarkCtx = &oc;
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
        oc.run(join);
        auto closed = promote;
        /* Consumers of promoted targets (a heuristic, see the setting):
           nominate them and close again, until nothing changes. */
        if (ownerConsumersOn) {
            std::sort(ownerConsumers.begin(), ownerConsumers.end());
            ownerConsumers.erase(std::unique(ownerConsumers.begin(), ownerConsumers.end()), ownerConsumers.end());
        }
        for (bool more = ownerConsumersOn && !ownerConsumers.empty(); more;) {
            more = false;
            for (auto [c, p] : ownerConsumers) {
                auto ci = oc.indexOf(c), pi = oc.indexOf(p);
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
        evacuator.onIdentity = nullptr;
        stats.markerPromoted += oc.added;
        if (oc.added)
            stats.markerJobs++, stats.ownerJobs++;
        if (cfg.ownerCheck) {
            ForceMarkers fm(state.undoLog, mark, jobEnd, targets);
            Closure<RegionMarks> plain{young, clock, RegionMarks(evalRegion.base, evalRegion.cur)};
            plain.memos = false;
            auto o = nominated;
            closePromotions(plain, targets, fm, o, [&](size_t i) { plain.mark(*targets.cells[i]); });
            /* The closure proper, before consumer nominations. */
            for (size_t i = 0; i < targets.size(); ++i) {
                if (closed[i] && !o[i]) {
                    stats.ownerCheckExtra++;
                    std::cerr << fmt(
                        "unforce owner check: job %d target %d: owner closure promotes, markers does not\n",
                        stats.jobs,
                        i);
                }
                if (!closed[i] && o[i])
                    stats.ownerCheckFewer++;
            }
        }
        if (markFirst && 2 * marker->markedBytes >= youngBytes)
            *keepWhole = true;
        if (!keepWhole || !*keepWhole)
            copyPromoted();
        return promote;
    }

    /* When every target is promoted (a printing job often nominates them
       all), the closure has nothing to decide: only Evac(S) is needed. */
    bool allPromoted = std::find(promote.begin(), promote.end(), false) == promote.end();
    std::optional<ForceMarkers> markers;
    if (!allPromoted)
        markers.emplace(state.undoLog, mark, jobEnd, targets);
    auto budget = cfg.walkBudgetBase + cfg.walkBudgetPerKiB * (youngBytes >> 10);
    bool fallbackPromote = cfg.walkFallback != "revert";
    uint64_t steps = 0;
    size_t peak = 0;
    auto nominated = promote;
    /* A job that printers reported cells in (`UnforceSeen`) may keep most
       of what it allocated: a job forcing one large printed value keeps all
       of it. Copying that would double the job's memory at its end, so
       Evac(S) is marked first, and if it is at least half the job's memory
       (string data aside), or the closure runs out of budget, the job keeps
       all its memory instead (`keepWhole`): no copy, and no more than
       without unforcing (review of markers v4, B1-mem). */
    bool markFirst = keepWhole && printed;
    bool copyAsWeGo = fallbackPromote && !markFirst;
    size_t markedBytes = 0;
    auto decide = [&](auto & closure) {
        closure.budget = budget;
        if (copyAsWeGo)
            evacuator.onForward = [&](const void * p) { closure.reachedNew |= closure.marks.add(p, inEvac) & reached; };
        auto closed = promote;
        try {
            auto close = [&](auto && join) -> size_t {
                if (allPromoted) {
                    for (size_t i = 0; i < targets.size(); ++i)
                        join(i);
                    return 0;
                }
                return closePromotions(closure, targets, *markers, closed, join);
            };
            auto added = copyAsWeGo ? close(copy) : close([&](size_t i) { closure.mark(*targets.cells[i]); });
            promote = std::move(closed);
            stats.markerPromoted += added;
            if (added)
                stats.markerJobs++;
        } catch (OverBudget &) {
            stats.budgetJobs++;
            if (markFirst)
                *keepWhole = true;
            promote.assign(targets.size(), fallbackPromote && !markFirst);
        }
        evacuator.onForward = nullptr;
        closure.notePeak();
        steps = closure.steps;
        peak = closure.peak;
        markedBytes = closure.markedBytes;
    };
    if (young.heap) {
        Closure<HeapMarks> closure{young, clock};
        decide(closure);
    } else {
        Closure<RegionMarks> closure{young, clock, RegionMarks(evalRegion.base, evalRegion.cur)};
        decide(closure);
    }
    if (checkClosure && !young.heap && markers) {
        /* Recompute the closure plainly (marking walks, no memos, a round
           after every promotion) and compare. */
        Closure<RegionMarks> plain{young, clock, RegionMarks(evalRegion.base, evalRegion.cur)};
        plain.memos = false;
        auto o = nominated;
        bool over = false;
        try {
            closePromotions(plain, targets, *markers, o, [&](size_t i) { plain.mark(*targets.cells[i]); });
        } catch (OverBudget &) {
            over = true;
        }
        if (!over && o != promote)
            for (size_t i = 0; i < targets.size(); ++i)
                if (o[i] != promote[i])
                    std::cerr << fmt(
                        "unforce check: job %d target %d (start %d, end %d, %s): plain closure %s, decided %s\n",
                        stats.jobs,
                        i,
                        markers->spans[i].s,
                        markers->spans[i].e,
                        markers->spans[i].single ? "single" : "repeated",
                        o[i] ? "promotes" : "reverts",
                        promote[i] ? "promotes" : "reverts");
    }
    if (markFirst && 2 * markedBytes >= youngBytes)
        *keepWhole = true;
    if (!keepWhole || !*keepWhole)
        copyPromoted();

    stats.markerSteps += steps;
    stats.maxJobSteps = std::max<size_t>(stats.maxJobSteps, steps);
    stats.maxStepsPerKiB = std::max<size_t>(stats.maxStepsPerKiB, steps / std::max<size_t>(1, youngBytes >> 10));
    stats.markerPeakBytes = std::max(
        stats.markerPeakBytes,
        peak
            + (markers ? markers->spans.capacity() * sizeof(markers->spans[0]) + markers->childIntervals.capacity() * 16
                       : 0));
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
   partially, so this is fatal (REDESIGN-memory L7). */
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

void unforceDebugChain(EvalState & state, const Value * v)
{
    std::set<const Value *> seen;
    for (int k = 0; k < 40 && v; ++k) {
        bool again = !seen.insert(v).second;
        auto t = v->internalType();
        std::string what = t == tApp ? "app" : t == tThunk ? "thunk" : t == tAttrs ? "attrs" : fmt("type %d", (int) t);
        if (auto i = sourceRegistry.find(v); i != sourceRegistry.end())
            what += " [" + i->second + "]";
        if (auto i = selectRegistry.find(v); i != selectRegistry.end()) {
            std::cerr << fmt(
                "  %d: %x select '%s' %s%s\n", k, (uintptr_t) v, i->second.first, what, again ? " (REPEAT)" : "");
            if (again)
                return;
            v = i->second.second;
            continue;
        }
        std::cerr << fmt("  %d: %x %s%s\n", k, (uintptr_t) v, what, again ? " (REPEAT)" : "");
        if (again || t != tApp)
            return;
        v = v->app().right;
    }
}

void * evalRegionHeapAlloc(size_t n)
{
#if NIX_UNFORCE_MIMALLOC
    /* Most evaluator objects are small (a Value is 16 bytes); mimalloc's
       small path skips its size-class dispatch. */
    auto p = n <= MI_SMALL_SIZE_MAX ? mi_theap_zalloc_small(currentTheap, n) : mi_theap_zalloc(currentTheap, n);
    if (!p)
        throw std::bad_alloc();
    frames.back().bytes += n;
    return p;
#else
    unreachable();
#endif
}

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

void * evalRegionHeapAllocCell()
{
#if NIX_UNFORCE_MIMALLOC
    auto & f = frames.back();
    if (f.cellCur == f.cellEnd) [[unlikely]] {
        auto chunk = (char *) mi_theap_zalloc(currentTheap, cellChunkBytes);
        if (!chunk)
            throw std::bad_alloc();
        f.chunks.push_back(chunk);
        f.cellCur = chunk;
        f.cellEnd = chunk + cellChunkBytes;
        f.bytes += cellChunkBytes;
    }
    auto p = f.cellCur;
    f.cellCur += 16;
    return p;
#else
    unreachable();
#endif
}

void evalRegionNoteBigEnv(const void * env, size_t size)
{
    noteBigEnv(env, size);
}

bool evalRegionHeapContains(const void * p)
{
#if NIX_UNFORCE_MIMALLOC
    return inHeap(evalRegion.heap, p);
#else
    unreachable();
#endif
}

bool unforceMayHaveFreed(const void * p)
{
    if (unforceSettings().region.empty())
        return true;
#if NIX_UNFORCE_MIMALLOC
    /* Destroyed heaps' pages go back to the arena for any later heap. */
    if (nested())
        return inArena(p);
#endif
    return (uintptr_t) p >= lastReleased.first && (uintptr_t) p < lastReleased.second;
}

std::string unforceStatsJSON()
{
    if (!stats.jobs)
        return "";
    return fmt(
        R"({"scopes":%d,"logged":%d,"reverted":%d,"stubbed":%d,"promoted":%d,"relogged":%d,"copiedBytes":%d,"largestScopeBytes":%d,)"
        R"("reforced":%d,"promotedBytes":%d,"freedBytes":%d,"markers":"%s","markerPromoted":%d,)"
        R"("markerRounds":%d,"markerSteps":%d,"markerJobs":%d,"markerLogBytes":%d,"markerPeakBytes":%d,)"
        R"("markerBudgetJobs":%d,"markerFallback":"%s","markerMaxJobSteps":%d,"markerMaxStepsPerKiB":%d,)"
        R"("countsPeak":%d,"countsResets":%d,"stackTargets":%d,"clientJobs":%d,)"
        R"("revertedInClientJobs":%d,"innerScopeReverts":%d,"seenNominated":%d,"seenKept":%d,"keptJobs":%d,)"
        R"("keptJobBytes":%d,"identity":"%s","ownerLookups":%d,"ownerChanges":%d,"ownerSegsPeak":%d,)"
        R"("ownerJobLookups":%d,"ownerJobs":%d,"ownerConsumerNominated":%d,"ownerCheckExtra":%d,"ownerCheckFewer":%d})",
        stats.jobs,
        stats.logged,
        stats.reverted,
        stats.stubbed,
        stats.promoted,
        stats.relogged,
        stats.copiedBytes,
        stats.regionPeak,
        stats.reforced,
        stats.copiedBytes,
        stats.freedBytes,
        unforceSettings().markers,
        stats.markerPromoted,
        stats.markerRounds,
        stats.markerSteps,
        stats.markerJobs,
        stats.markerLogBytes,
        stats.markerPeakBytes,
        stats.budgetJobs,
        unforceSettings().walkFallback,
        stats.maxJobSteps,
        stats.maxStepsPerKiB,
        stats.countsPeak,
        stats.countsResets,
        stats.stackTargets,
        stats.clientJobs,
        stats.revertedInClientJobs,
        stats.innerScopeReverts,
        stats.seenNominated,
        stats.seenKept,
        stats.keptJobs,
        stats.keptJobBytes,
        unforceSettings().identity,
        stats.ownerLookups,
        stats.ownerChanges,
        stats.ownerSegsPeak,
        stats.ownerJobLookups,
        stats.ownerJobs,
        stats.ownerConsumerNominated,
        stats.ownerCheckExtra,
        stats.ownerCheckFewer);
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

static size_t generation = 0;

size_t unforceGeneration()
{
    return generation;
}

size_t unforceCurrentScopeBytes()
{
    if (!frames.empty())
        return frames.back().bytes;
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
    if (state && !mark) {
        /* Only a scope that is not nested in another one is a job, except
           with the mimalloc backend, where every scope is. */
        job = nested() || state->isolationDepth == 0;
        mark = state->beginUnforceScope(stackMark);
    }
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
       memory beyond what it keeps (review of markers v4, B1-mem). */
    auto & log = state->undoLog;
    auto bookkeeping =
        (log.capacity() - *mark) * sizeof(EvalState::UndoEntry) + (log.size() - *mark) * 2 * sizeof(void *);
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
    if (onEnd)
        onEnd(job);
}

#if NIX_UNFORCE_MIMALLOC
/* With NIX_UNFORCE_VERIFY: before a scope heap is destroyed, check that no
   evaluator object outside it points into it. The enclosing scopes' heaps
   and the base arena hold evaluator objects only, so every word of them is
   checked against the dying heap. */
static void verifyHeapScope(void * dying)
{
    struct Scan
    {
        void * dying;
        size_t blocks = 0, bytes = 0, hits = 0;

        void words(const void * start, size_t size)
        {
            auto w = (const uintptr_t *) start;
            for (size_t i = 0; i < size / sizeof(uintptr_t); ++i)
                if (auto p = (const void *) (w[i] & ~uintptr_t(7)); inHeap(dying, p))
                    if (hits++ < 5)
                        std::cerr << fmt("unforce verify: word at %x points into the dying scope\n", (uintptr_t) &w[i]);
        }
    } scan{dying};

    for (auto & f : frames)
        mi_heap_visit_blocks(
            (mi_heap_t *) f.heap,
            true,
            [](const mi_heap_t *, const mi_heap_area_t *, void * block, size_t size, void * arg) -> bool {
                auto & scan = *(Scan *) arg;
                if (block) {
                    scan.blocks++;
                    scan.bytes += size;
                    scan.words(block, size);
                }
                return true;
            },
            &scan);
    if (evalUseBaseArena)
        for (auto [start, end] : evalBaseArena.chunks) {
            if (evalBaseArena.cur >= start && evalBaseArena.cur <= end)
                end = evalBaseArena.cur;
            scan.bytes += end - start;
            scan.words(start, end - start);
        }
    std::cerr << fmt(
        "unforce verify: scope %d: scanned %d enclosing-scope blocks and the base arena (%d MiB), %d pointers into it\n",
        stats.jobs,
        scan.blocks,
        scan.bytes >> 20,
        scan.hits);
}

static void beginHeapScope(size_t logMark)
{
    hookCollector();
    if (!arenaId) {
        /* The arena bounds what all open scopes hold at once. It costs every
           scope too: each heap other than the main one allocates and zeroes
           page bitmaps for every arena it uses, 62 of them with a bit per
           64 KiB slice (0.5 MiB for 4 GiB; mimalloc src/arena.c,
           mi_arena_pages_alloc). Arenas above 16 GiB are split into children
           (mimalloc d4881d33). mimalloc 3.5.1 also leaked those bitmaps once
           they exceeded 512 KiB, i.e. for arenas above 4 GiB (fixed in
           3.5.2, 6c45ba09). */
        static const size_t arenaBytes = std::stoul(getEnv("NIX_UNFORCE_ARENA_GB").value_or("4")) << 30;
        if (mi_reserve_os_memory_ex(arenaBytes, false, false, true, &arenaId) != 0)
            throw Error("could not reserve the mimalloc arena for unforce scopes");
        size_t size = 0;
        arenaLo = (uintptr_t) mi_arena_area(arenaId, &size);
        arenaHi = arenaLo + size;
    }
    auto heap = mi_heap_new_in_arena(arenaId);
    if (!heap)
        throw std::bad_alloc();
    frames.push_back(Frame{.logMark = logMark, .heap = heap});
    setCurrentHeap(heap);
}

/* Close the innermost heap scope: every cell older than it that it forced
   is reverted, or, for the outermost scope, possibly promoted (copied out of
   all scopes; see `decideJob`). Then the scope's heap is destroyed
   wholesale. */
static void endHeapScope(EvalState & state, size_t mark)
{
    auto & cfg = unforceSettings();
    auto & log = state.undoLog;
    auto jobEnd = unforceClock();
    Frame f = std::move(frames.back());
    frames.pop_back();
    Frame * parent = frames.empty() ? nullptr : &frames.back();
    stats.jobs++;
    generation++;
    stats.logged += log.size() - mark;
    dropActiveForces(mark);

    /* Copies go to wherever allocation goes once this scope is closed. */
    setCurrentHeap(parent ? parent->heap : nullptr);
    std::optional<JobTargets> jt;
    std::vector<bool> promoted;
    std::vector<Value> copies;
    try {
        jt.emplace(log, mark);
        if (!parent) {
            PauseRegion pause;
            Evacuator evacuator{state};
            evacuator.youngHeap = f.heap;
            CellClock clock{.bump = false};
            for (size_t i = 0; i < f.chunks.size(); ++i)
                clock.chunks.emplace_back((uintptr_t) f.chunks[i], i * cellsPerChunk);
            std::sort(clock.chunks.begin(), clock.chunks.end());
            promoted = decideJob(
                state,
                mark,
                *jt,
                true,
                state.forceCounts,
                YoungSpace{f.heap},
                clock,
                jobEnd,
                f.bytes,
                evacuator,
                copies);
            pruneCounts(state.forceCounts, *jt, promoted);
        }
    } catch (std::bad_alloc &) {
        unforceFatal("deciding which values to keep at the end of an unforce scope, and copying them");
    }

    if (parent) {
        /* Only the outermost scope promotes, as with the bump backend: a
           nested scope reverts everything it forced, which keeps identity
           (pure revert), whereas a cell promoted into the enclosing scope
           would be relogged there and could be reverted after a cell holding
           a copy of its data was promoted. */
        for (size_t i = 0; i < jt->size(); ++i) {
            *jt->cells[i] = jt->preImage(i);
            stats.reverted++;
            stats.innerScopeReverts++;
        }
    } else
        /* The release allocates nothing. */
        for (size_t i = 0; i < jt->size(); ++i)
            if (promoted[i]) {
                *jt->cells[i] = copies[i];
                stats.promoted++;
            } else {
                *jt->cells[i] = jt->preImage(i);
                stats.reverted++;
                if (clientCodeInJob)
                    stats.revertedInClientJobs++;
            }

    log.resize(mark);
    if (!parent) {
        log.shrink_to_fit();
        clientCodeInJob = false;
    }
    stats.freedBytes += f.bytes;
    stats.regionPeak = std::max(stats.regionPeak, f.bytes);
    if (getenv("NIX_UNFORCE_VERIFY"))
        verifyHeapScope(f.heap);
    mi_heap_destroy((mi_heap_t *) f.heap);

    if (cfg.stats)
        printStatsAtExit();
}
#endif

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
    return !nested() && unforceSettings().keepSeen;
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
    /* The scope that will restore the slot: every scope with the mimalloc
       backend, only the job (the outermost scope) with the bump one. */
    auto mark = nested() ? scopeStackMarks.back() : scopeStackMarks.front();
    return (const char *) p >= stackLow && (const char *) p < mark;
}

/* A force of a young cell outside the current segment: its owner becomes
   the current one while it runs. */
static void youngForce(const Value & v)
{
    stats.ownerLookups++;
    auto o = ownerOf(&v);
    if (o == curOwner)
        return;
    stats.ownerChanges++;
    /* A consumer forcing many cells of one producer records the pair once
       per run of them (pwith-100-200000: 20 M forces, 100 producers). */
    if (curOwner && o && ownerConsumersOn
        && (ownerConsumers.empty() || ownerConsumers.back() != std::pair{curOwner, o}))
        ownerConsumers.emplace_back(curOwner, o);
    pushOpen(&v, false, true, o);
}

void EvalState::logPreImageSlow(Value & v)
{
    if (evalRegion.contains(&v)) {
        /* Young, but not of the current owner segment. */
        if (ownersOn)
            youngForce(v);
        return;
    }
    if (unforceIsScopeStackSlot(&v))
        return;
    UndoEntry e{&v, v};
    e.start = unforceClock();
    e.parent = activeForces.empty() ? -1 : int64_t(activeForces.back());
    undoLog.push_back(e);
    stats.markerLogBytes += sizeof(UndoEntry) - offsetof(UndoEntry, start);
    activeForces.push_back(undoLog.size() - 1);
    pushOpen(&v, true, ownersOn, &v, undoLog.size() - 1);
}

void EvalState::endLoggedForceSlow(Value & v)
{
    /* Called when `v` is the innermost open force (`unforceOpenTop`). */
    if (opens.empty() || opens.back().cell != &v)
        return;
    auto open = opens.back();
    opens.pop_back();
    if (open.changed && ownersOn)
        setOwner(open.saved);
    setOpenTop();
    if (!open.logged)
        return;
    /* Every logged force ends here, innermost first, so the innermost open
       one is `v`'s if `v`'s start was logged. One that a job's end dropped
       (`dropActiveForces`) is not found. */
    if (activeForces.empty())
        return;
    auto i = activeForces.back();
    if (i >= undoLog.size()) {
        activeForces.pop_back();
        return;
    }
    if (undoLog[i].target != &v)
        return;
    activeForces.pop_back();
    undoLog[i].end = unforceClock();
}

/* The EvalState whose job is open. The per-job state above is per thread,
   but the region (`evalRegion`, the mimalloc frames, `lastReleased`) is
   process-wide, so at most one job may be open in the process, and every
   scope opened while it is must belong to its EvalState: another
   EvalState's job would take over the region, and the first job's
   decision would then see none of what it allocated (review of markers
   v2, N2). Nothing opens jobs that way today (only the CLI's traversals
   open jobs, on one thread with one EvalState); this checks it. */
static std::atomic<const EvalState *> jobOwner = nullptr;

size_t EvalState::beginUnforceScope(const void * stackMark)
{
    if (!isolationDepth) {
        const EvalState * none = nullptr;
        if (!jobOwner.compare_exchange_strong(none, this))
            panic("unforce: a job was opened while another EvalState's job is open");
    } else
        assert(jobOwner.load() == this);
    /* The canonical frame address is the caller's stack pointer at the call:
       frames the caller enters from now on lie below it, its own locals at
       or above. */
    scopeStackMarks.push_back((const char *) (stackMark ? stackMark : __builtin_dwarf_cfa()));
#if NIX_UNFORCE_MIMALLOC
    if (nested()) {
        isolationDepth++;
        beginHeapScope(undoLog.size());
        return undoLog.size();
    }
#endif
    if (!isolationDepth++ && !unforceSettings().region.empty()) {
        openRegion(unforceSettings().region == "all");
        openOwners();
    }
    return undoLog.size();
}

void EvalState::endUnforceScope(size_t mark)
{
    scopeStackMarks.pop_back();
#if NIX_UNFORCE_MIMALLOC
    if (nested()) {
        isolationDepth--;
        endHeapScope(*this, mark);
        if (!isolationDepth)
            jobOwner = nullptr;
        return;
    }
#endif
    /* Only the outermost scope is a job. */
    if (--isolationDepth)
        return;
    jobOwner = nullptr;

    auto & cfg = unforceSettings();
    stats.jobs++;
    generation++;
    stats.logged += undoLog.size() - mark;
    auto jobEnd = unforceClock();
    dropActiveForces(mark);

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
                evalRegion.allTypes,
                forceCounts,
                YoungSpace{},
                CellClock{},
                jobEnd,
                /* String data is never walked: a long string must not buy
                   the closure steps (review of markers v4, S1''). */
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
               promoted, reverted or freed, as without unforcing (review of
               markers v3, S4). The decision may also have chosen that
               (`decideJob`). */
            keepWhole = keepWhole || !seenAllKept(evacuator, promoted, printed ? &*printed : nullptr);
            if (!keepWhole)
                translateSeen(evacuator);
            forgetSeenReports();
            pruneCounts(forceCounts, *jt, promoted);
        } catch (std::bad_alloc &) {
            unforceFatal("deciding which values to keep at the end of an unforce batch, and copying them");
        }
        closeOwners();

        /* The release: assigns, reverts and frees. Only derivation stubs
           (NIX_UNFORCE_STUB, a dev mode) allocate here. */
        if (!keepWhole) {
            try {
                Stubber stubber{*this, evacuator};
                auto & targets = jt->cells;
                for (size_t i = 0; i < targets.size(); ++i)
                    if (promoted[i]) {
                        *targets[i] = copies[i];
                        stats.promoted++;
                    }

                for (size_t i = 0; i < targets.size(); ++i) {
                    auto target = targets[i];
                    if (promoted[i])
                        continue;
                    auto & pre = jt->preImage(i);
                    if (cfg.stubs && evalRegion.cur && isDerivationNoForce(*this, *target)) {
                        auto source = allocValue();
                        *source = pre;
                        if (getenv("NIX_UNFORCE_DEBUG")) {
                            auto n = target->attrs()->get(s.name);
                            sourceRegistry[source] =
                                fmt("source for %s (job %d, pre-image %s)",
                                    n && n->value->internalType() == tString ? std::string(n->value->string_view()) : "?",
                                    stats.jobs,
                                    pre.internalType() == tApp     ? "app"
                                    : pre.internalType() == tThunk ? "thunk"
                                                                   : "other");
                        }
                        /* Memoise within one stub only (outputs refer to each other).
                           Another cell holding the same derivation, such as an alias
                           whose recipe is `self.foo`, must not share this stub: its
                           fallbacks would select from a recipe that evaluates to the
                           stub itself. */
                        stubber.memo.clear();
                        *target = *stubber.stub(*target, source, 0);
                        evacuator.drain();
                        stats.stubbed++;
                        continue;
                    }
                    *target = pre;
                    stats.reverted++;
                    if (clientCodeInJob)
                        stats.revertedInClientJobs++;
                }
            } catch (std::bad_alloc &) {
                unforceFatal("building derivation stubs at the end of an unforce batch");
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
    if (evalRegion.base) {
        if (keepWhole)
            retainRegion();
        else
            releaseRegion();
    }

    if (cfg.stats)
        printStatsAtExit();
}

} // namespace nix
