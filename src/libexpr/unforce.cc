/* Unforcing thunks between jobs.

   Thunk update in `forceValue` is the evaluator's only mutation of existing
   cells. While a job (an outermost unforce scope) runs, the evaluator
   allocates in a region of its own (`evalRegion`), and the pre-image of
   every *old* cell (one outside the region) that the job forces is logged.
   At the job's end each logged cell is reverted to its pre-image, or
   promoted: it keeps its value, and with it what the value reaches in the
   region (see "Promotion" below). A cell whose value holds no pointer into
   the region (`youngFree`) keeps it as it is. The job keeps its memory below
   a cut decided from records made as forces ended, and frees the rest by
   lowering the bump cursor: nothing is marked, walked or moved. Below the
   cut, the whole pages of what no kept value reaches go back to the system. */

#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/primops.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/store/store-api.hh"
#include "nix/util/config.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/std-hash.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <numeric>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

#if NIX_ASAN_ENABLED
#  include <sanitizer/asan_interface.h>
#  include <sanitizer/common_interface_defs.h>
#endif

namespace nix {

EvalRegion evalRegion;

static size_t pageSize()
{
#if defined(__linux__)
    static const size_t page = sysconf(_SC_PAGESIZE);
    return page;
#else
    /* Only a region asks, and only Linux has one (`openRegion`). */
    unreachable();
#endif
}

static char * pageUp(char * p)
{
    return (char *) (((uintptr_t) p + pageSize() - 1) & ~(uintptr_t) (pageSize() - 1));
}

/* Give [lo, hi), memory of the open job that nothing will read, back to the
   system: its whole pages, which read as zeros if touched again, whose size
   it returns. Under AddressSanitizer all of it is poisoned instead, so that a
   read of any of it fails. */
static size_t giveBack(char * lo, char * hi)
{
#if defined(__linux__)
    auto a = pageUp(lo), b = (char *) ((uintptr_t) hi & ~(uintptr_t) (pageSize() - 1));
#  if NIX_ASAN_ENABLED
    if (lo < hi)
        ASAN_POISON_MEMORY_REGION(lo, hi - lo);
#  else
    if (a < b)
        madvise(a, b - a, MADV_DONTNEED);
#  endif
    return a < b ? b - a : 0;
#else
    return 0;
#endif
}

/* The highest the open job's region reached: a loop's step lowers `cur`
   when it frees its memory (see "Steps"). */
static char * jobTop = nullptr;

#if defined(__linux__)

/* Jobs share a mapping of two regions: [mapping, keptTop) holds what kept
   jobs left, ordinary memory from then on, and the next job's region starts
   at `keptTop` while the mapping has a region's room above it. */
static char * mapping = nullptr;
static char * keptTop = nullptr;

/* A job's room: half the mapping, of which only the pages it touches are
   memory. */
static size_t regionBytes = 0;

/* The end of the open job's room, and the top of the mapping's memory that
   jobs wrote: above it, the system's zero-filled pages. The evaluator
   expects allocations zeroed, so what lies below it is zeroed as the bump
   cursor reaches it (`EvalRegion::more`), a page at a time. */
static char * roomEnd = nullptr;
static char * dirtyTop = nullptr;

/* The end of the mapping's part that a core dump holds: every page a job
   wrote lies below it, and the rest of the mapping, which no job touched, is
   left out (`MADV_DONTDUMP`), else a core would carry all of it. It moves
   ahead of `end`, to twice the span below where the cursor reached, so that
   it moves as often as the span doubles. If moving it fails, the rest of
   the mapping is put back in cores as a whole, as before it was left out. */
static char * dumpTop = nullptr;

#endif

static size_t granuleOf(const void * p)
{
    return size_t((const char *) p - evalRegion.base) >> 4;
}

/* For the unit tests: while set, a job maps its region afresh, and every
   mapping fails, as where the system has no room for one. Never set by
   evaluation. */
extern bool unforceTestFailMapping;
bool unforceTestFailMapping = false;

#if defined(__linux__)
/* `bytes` of address space, of which only the pages written become memory. */
static void * mapAnonymous(size_t bytes)
{
    if (unforceTestFailMapping)
        return MAP_FAILED;
    return mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}
#endif

/* Whether a job has a region (`openRegion`): only on Linux, with a 64-bit
   address space. */
static constexpr bool jobsHaveRegion()
{
#if defined(__linux__)
    return sizeof(void *) >= 8;
#else
    return false;
#endif
}

/* mimalloc's, when it is the process's allocator (the `nix` binary links
   it): gives the memory it holds freed back to the system. Else null. */
extern "C" [[gnu::weak]] void mi_collect(bool force) noexcept;

/* Open the job's region. Only Linux on a 64-bit system has one: elsewhere,
   the job allocates as outside any job, and its end reverts nothing. The
   mapping is as large as the system gives: the largest power of two a
   `size_t` holds, halved until `mmap` accepts it, so that no size or
   address layout is assumed. Failing to map a page is running out of
   memory (`std::bad_alloc`): the job does not run without a region. Not an
   `Error`, which traversals take for an element's failure and print or
   skip. */
static void openRegion()
{
#if defined(__linux__)
    if (!jobsHaveRegion())
        return;
    /* mimalloc gives freed memory back only seconds later, longer than
       many evaluations run: it goes back here, before the job runs. */
    if (mi_collect)
        mi_collect(true);
    if (!mapping || unforceTestFailMapping || size_t(mapping + 2 * regionBytes - keptTop) < regionBytes) {
        auto bytes = ~(SIZE_MAX >> 1);
        auto p = MAP_FAILED;
        while (bytes >= pageSize() && (p = mapAnonymous(bytes)) == MAP_FAILED)
            bytes >>= 1;
        if (p == MAP_FAILED)
            throw std::bad_alloc();
        /* The old mapping keeps the pages of what its jobs kept. */
        if (mapping)
            munmap(pageUp(keptTop), mapping + 2 * regionBytes - pageUp(keptTop));
        mapping = keptTop = dirtyTop = dumpTop = (char *) p;
        if (madvise(p, bytes, MADV_DONTDUMP) != 0)
            dumpTop = mapping + bytes;
        regionBytes = bytes / 2;
    }
    evalRegion.base = evalRegion.cur = jobTop = keptTop;
    /* Under AddressSanitizer every frame that allocates poisons what it may. */
    unforceFrameSpan = NIX_ASAN_ENABLED ? 1 : pageSize();
    unforceStepSpan = pageSize();
    roomEnd = keptTop + regionBytes;
    evalRegion.end = dirtyTop > keptTop ? keptTop : std::min(dumpTop, roomEnd);
#endif
}

void * EvalRegion::more(size_t n)
{
#if defined(__linux__)
    /* `bump` relies on it: the mapping, its page bounds, and cursors, which
       move by multiples of 16. */
    assert(!((uintptr_t) cur & 15) && !((uintptr_t) end & 15) && !((uintptr_t) roomEnd & 15));
    if (size_t(roomEnd - cur) >= n) {
        auto to = pageUp(cur + n);
        if (end < dirtyTop) {
            auto zeroed = size_t(std::min(to, dirtyTop) - end);
#  if NIX_ASAN_ENABLED
            /* Zeroed while poisoned (`closeRegion`); `bump` unpoisons what
               it hands out. */
            ASAN_UNPOISON_MEMORY_REGION(end, zeroed);
#  endif
            memset(end, 0, zeroed);
#  if NIX_ASAN_ENABLED
            ASAN_POISON_MEMORY_REGION(end, zeroed);
#  endif
        }
        if (to > dumpTop) {
            auto top = std::min(pageUp(to + (to - mapping)), pageUp(roomEnd));
            if (madvise(dumpTop, top - dumpTop, MADV_DODUMP) != 0) {
                top = mapping + 2 * regionBytes;
                madvise(dumpTop, top - dumpTop, MADV_DODUMP);
            }
            dumpTop = top;
        }
        end = to < dirtyTop ? to : std::min(dumpTop, roomEnd);
        return bump(n);
    }
#endif
    throw std::bad_alloc();
}

/* End the job's region: its memory below `keepTo` stays, for the rest of
   the process; above it, the next job's region reuses it. What earlier jobs
   wrote above where this one reached goes back to the system. */
static void closeRegion(char * keepTo)
{
#if defined(__linux__)
    auto top = std::max(jobTop, evalRegion.cur);
#  if NIX_ASAN_ENABLED
    /* What the job's end freed is poisoned until handed out again
       (`EvalRegion::bump`) or unmapped, so that a read through a pointer
       the end missed fails at once instead of finding the old bytes. */
    if (top > keepTo)
        ASAN_POISON_MEMORY_REGION(keepTo, top - keepTo);
#  endif
    dirtyTop = std::max(dirtyTop, top);
    if (pageUp(top) < dirtyTop && madvise(pageUp(top), pageUp(dirtyTop) - pageUp(top), MADV_DONTNEED) == 0)
        dirtyTop = pageUp(top);
    keptTop = keepTo;
    jobTop = nullptr;
#endif
    auto paused = evalRegion.paused;
    evalRegion = EvalRegion{};
    evalRegion.paused = paused;
}

/* ---------------------------------------------------------------------------
   Re-forcing.

   A target reverted at a job's end holds its pre-image wrapped in an
   `ExprReforce`, evaluated again by whatever forces it next: evaluation is
   deterministic, so the re-force repeats work the evaluation without
   unforcing did. It runs at the depth and on the native stack of the force
   that finds it, as do the cells it builds again, which consumers forced
   before the target was reverted: a chain forced front to back without
   unforcing may be forced back to front. So it may fail on `max-call-depth`,
   or overflow the native stack (which ends the process), where the evaluation
   without unforcing does not; the exactness contract allows both.

   An old target whose force failed keeps its failure (see Promotion); a
   failed young cell inside a reverted owner is built again and throws again.
   A `max-call-depth` failure depends on its force's depth, so one met in a
   job has a recovery value, as an interrupted or recoverable one has: every
   later read retries it at its own depth, where it may succeed (also where
   the evaluation without unforcing fails, which the exactness contract
   allows). A failure its job retried is reverted to the failure, which
   retries where it is forced next.

   An application's pre-image becomes a thunk whose environment is the
   function and its argument, called at its first force's position (a primop
   may put its call's position in its value). The wrapper also keeps the
   position of the value the cell held (`unforceRevertedPos`).
   --------------------------------------------------------------------------- */

struct ReforceKey
{
    /** The pre-image's expression, or null for an application. */
    Expr * expr;
    /** An application's call position. */
    PosIdx pos;
    /** The position of the value the cells held when they were reverted, if it has one. */
    std::optional<PosIdx> valuePos;

    bool operator==(const ReforceKey &) const = default;
};

/** A reverted cell's pre-image (see above): one per distinct key, never freed. */
struct ExprReforce : Expr, ReforceKey
{
    ExprReforce(const ReforceKey & key)
        : ReforceKey(key)
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override;

    PosIdx getPos() const override
    {
        return expr ? expr->getPos() : pos;
    }

    void show(const SymbolTable & symbols, std::ostream & str) const override
    {
        if (expr)
            expr->show(symbols, str);
    }

    void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override {}
};

static void noteRebuilt(const Value & cell);

/* Out of line: a class whose virtual functions are all inline has its
   vtable emitted in every translation unit (Clang's -Wweak-vtables). `v` is
   the reverted cell: only `forceValue` evaluates a thunk, into its cell. */
void ExprReforce::eval(EvalState & state, Env & env, Value & v)
{
    if (expr)
        expr->eval(state, env, v);
    else {
        /* Not an `Env`: the application's operands (`reforcePreImage`).
           The cell holds the application while it is called, as it did. */
        auto operands = reinterpret_cast<Value * const *>(&env);
        v.mkApp(operands[0], operands[1]);
        state.callFunction(*operands[0], *operands[1], v, pos);
    }
    noteRebuilt(v);
}

struct ReforceHash
{
    using is_transparent = void;

    size_t operator()(const ReforceKey & k) const
    {
        size_t h = 0;
        hash_combine(h, k.expr, k.pos, k.valuePos);
        return h;
    }

    size_t operator()(const ExprReforce * r) const
    {
        return (*this)(static_cast<const ReforceKey &>(*r));
    }
};

struct ReforceEq
{
    using is_transparent = void;

    static const ReforceKey & key(const ReforceKey & k)
    {
        return k;
    }

    static const ReforceKey & key(const ExprReforce * r)
    {
        return *r;
    }

    bool operator()(const auto & a, const auto & b) const
    {
        return key(a) == key(b);
    }
};

/* Never freed: reverted cells refer to them for the rest of the process. */
static boost::unordered_flat_set<ExprReforce *, ReforceHash, ReforceEq> reforces;

bool unforceAnyReverted = false;

UnforceStats unforceStats;

static ExprReforce * internReforce(const ReforceKey & key)
{
    auto i = reforces.find(key);
    auto r = i != reforces.end() ? *i : *reforces.insert(new ExprReforce(key)).first;
    unforceAnyReverted = true;
    return r;
}

/* What `Value::determinePos` gives for `v` whatever its fallback, if it
   gives something else. */
static std::optional<PosIdx> ownPos(const Value & v)
{
    if (v.isApp())
        return ownPos(*v.app().left);
    if (v.isAttrApp())
        return ownPos(*v.attrApp().fun);
    if (v.type() == nAttrs)
        return v.attrs()->pos;
    if (v.isLambda())
        return v.lambda().fun->pos;
    return std::nullopt;
}

/* Whether a cell reverted to `preImage` holds a thunk that
   `ExprReforce::eval` evaluates (`reforcePreImage`). */
static bool reforceable(const Value & preImage)
{
    return (preImage.isThunk() && !preImage.isBlackhole()) || preImage.isApp() || preImage.isAttrApp();
}

/* What a target that holds `now` at the job's end is reverted to: its
   pre-image, an application to be called again at its force's position. A
   pre-image wrapped already (reverted before), or that is neither a thunk
   nor an application, stays as it is. Allocates: outside the region, which
   the caller pauses. */
static Value reforcePreImage(EvalState & state, const Value & preImage, PosIdx pos, const Value & now)
{
    Value v = preImage;
    if (!reforceable(preImage))
        return v;
    if (preImage.isThunk()) {
        auto t = preImage.thunk();
        if (!unforceIsReverted(t.expr))
            v.mkThunk(t.env, internReforce({t.expr, noPos, ownPos(now)}));
    } else {
        /* An attribute application is called again as the application
           `mapAttrs` made before it had a cell for `fun name`. */
        auto a = preImage.isApp() ? preImage.app() : [&] {
            auto x = preImage.attrApp();
            auto f = state.allocValue();
            f->mkApp(x.fun, Value::toPtr(state.symbols[x.attr->name]));
            return Value::FunctionApplicationThunk{.left = f, .right = x.attr->value};
        }();
        auto operands = (Value **) state.mem.allocBytes(2 * sizeof(Value *));
        operands[0] = a.left;
        operands[1] = a.right;
        v.mkThunk((Env *) operands, internReforce({nullptr, pos, ownPos(now)}));
    }
    return v;
}

bool unforceIsReverted(const Expr * expr)
{
    return dynamic_cast<const ExprReforce *>(expr);
}

PosIdx unforceRevertedPos(const Expr * expr, PosIdx pos)
{
    if (auto r = dynamic_cast<const ExprReforce *>(expr); r && r->valuePos)
        return *r->valuePos;
    return pos;
}

#if !NIX_USE_BOEHMGC
void * evalCalloc(size_t n)
{
    if (evalRegion.active()) [[unlikely]]
        return evalRegion.bump(n);
    return calloc(n, 1);
}
#endif

void unforcePauseBegin()
{
    ++evalRegion.paused;
}

void unforcePauseEnd()
{
    --evalRegion.paused;
}

/* The EvalState whose job is open. The region is process-wide, so at most
   one job may be open in the process: another EvalState's job would take the
   region over, and the first job's end would see none of what it
   allocated. */
static std::atomic<const EvalState *> jobOwner = nullptr;

/* Whether the open job is kept whole (see the top of this file). */
static bool keepWhole = false;

/* Whether no step of the open job may free (see "Steps"): it forced a cell
   whose frame cannot be told (a fake stack's), or ran a plugin's lookup
   path hook. */
static bool stepsUnsafe = false;

/* This thread's stack: its lowest address and size, or null if unknown.
   Constant-initialized and filled on first use, so that no access runs a
   dynamic initializer; read once per job, at its opening. */
struct ThreadStack
{
    const char * low = nullptr;
    size_t size = 0;
    bool known = false;
};

static thread_local ThreadStack threadStackCache;

static const ThreadStack & threadStack()
{
    auto & t = threadStackCache;
    if (!t.known) {
#if defined(__linux__)
        pthread_attr_t attr;
        void * addr = nullptr;
        size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            pthread_attr_getstack(&attr, &addr, &size);
            pthread_attr_destroy(&attr);
        }
        t.low = (const char *) addr;
        t.size = addr ? size : 0;
#endif
        t.known = true;
    }
    return t;
}

#if defined(__linux__) && NIX_ASAN_ENABLED
/* Whether `p` is a slot of a running frame that AddressSanitizer placed on
   its fake stack. */
static bool onFakeStack(const void * p)
{
    auto fake = __asan_get_current_fake_stack();
    return fake && __asan_addr_is_in_fake_stack(fake, const_cast<void *>(p), nullptr, nullptr);
}
#endif

/* The stack pointer of the open job's opener: the frames entered during the
   job are below it, down to the bottom of the thread's stack (`jobStackLow`,
   the opener's thread's, read at the opening: the force path reads no
   thread-local). */
static const char * jobStackMark = nullptr;
static const char * jobStackLow = nullptr;
static const char * jobStackHigh = nullptr;

/* Whether `p` is a slot of a stack frame entered during the open job, which
   will have returned when it ends: such a cell is never logged, as the end
   must not write it. */
static bool isJobStackSlot(const void * p)
{
    /* Forced on another stack (a coroutine's, as a binary cache store runs
       a path filter on), whose slots look like cells: keep the job whole,
       so that its end writes none of them once that stack is gone. */
    auto here = (const char *) __builtin_dwarf_cfa();
    if (here < jobStackLow || here >= jobStackHigh) {
        keepWhole = stepsUnsafe = true;
        return true;
    }
#if defined(__linux__)
#  if NIX_ASAN_ENABLED
    /* On one of AddressSanitizer's fake stacks, whose addresses tell nothing
       of where its frame is: keep the job whole, so that its end writes no
       slot of a frame still running, nor of one that returned. */
    if (onFakeStack(p)) {
        keepWhole = stepsUnsafe = true;
        return true;
    }
#  endif
#endif
    return (const char *) p >= jobStackLow && (const char *) p < jobStackMark;
}

/* The pointers of `v` to the objects it is made of: `edge(p)` for each. */
template<typename E>
static void valueEdges(const Value & v, E && edge)
{
    switch (v.internalType()) {
    case tString:
        edge(v.stringData());
        edge(v.context());
        break;
    case tPath:
        edge(v.pathData());
        break;
    case tAttrs:
        edge(v.attrs());
        break;
    case tListSmall:
        edge(v.rawSmallList()[0]);
        edge(v.rawSmallList()[1]);
        break;
    case tListN:
        edge(v.rawList().elems);
        break;
    case tThunk:
        edge(v.thunk().env);
        break;
    case tApp:
        edge(v.app().left);
        edge(v.app().right);
        break;
    case tAttrApp:
        edge(v.attrApp().fun);
        edge(v.attrApp().attr);
        break;
    case tPrimOpApp:
        edge(v.primOpApp().left);
        edge(v.primOpApp().right);
        break;
    case tLambda:
        edge(v.lambda().env);
        break;
    case tFailed:
        edge(v.failed().recoveryValue);
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

/* Whether `v` is a value (not a thunk or an application) that holds no
   pointer into the job's region. A failure's record lies outside it, and
   its recovery value is an edge. Such a target stays as it is:
   reverting it would free nothing, and its value is what the evaluation
   without unforcing keeps. Its objects are old, so nothing builds them
   again. */
static bool youngFree(const Value & v)
{
    if (v.isThunk() || v.isApp() || v.isAttrApp() || v.internalType() == tUninitialized)
        return false;
    bool young = false;
    valueEdges(v, [&](const void * p) { young = young || evalRegion.contains(p); });
    return !young;
}

/* ---------------------------------------------------------------------------
   The owner timeline.

   The owner of a young object x is the first old cell on its creator chain
   (x, the cell being forced when x was allocated, the cell being forced when
   that one was, ...): the target whose re-force alone can build x again, or
   none (the driver). It is a function of allocation time: one force at a
   time is the current owner, and everything allocated then is its.
   - A logged force of an old cell t makes t the current owner until it ends.
   - A force of a young cell c makes c's owner the current one until it ends
     (only re-forcing the target that rebuilds c builds what c's evaluation
     builds).
   - Any other force (a stack slot, a cell of the current owner) keeps it.

   The bump cursor is the allocation clock, so owner changes cut the region's
   used part into segments of one owner each, in address order: the closed
   ones (`segments`) and the open one, [openSeg, cur), owned by `curOwner`;
   adjacent segments never have the same owner. Owners are named by the log
   index of their force (`noOwner`: the driver). A force that changes the
   owner pushes itself on `opens`, and its end pops itself and restores the
   owner; the hooks make room for every push first, so an end never
   allocates.
   --------------------------------------------------------------------------- */

static constexpr uint32_t noOwner = UINT32_MAX;

/* For the unit tests: the length at which a job's undo log is full, which
   otherwise owner names bound. Never set by evaluation. */
extern uint32_t unforceTestLogLimit;
uint32_t unforceTestLogLimit = noOwner;

struct OwnerSeg
{
    /* Its start, in granules from the region's base. */
    uint64_t off;
    uint32_t owner;
};

static std::vector<OwnerSeg> segments;
char * unforceOpenSeg = nullptr;
static char *& openSeg = unforceOpenSeg;
static uint32_t curOwner = noOwner;
const Value * unforceTopOpen = nullptr;

/* A force that changed the owner: a logged one (`entry`, its log index) or
   a young one (`noOwner`). `saved` is the owner to restore at its end;
   `segs`, how many closed segments there were once its own opened; `at`,
   the cursor at its start; `events`, `ownerEvents` once it opened. */
struct OpenForce
{
    const Value * cell;
    uint32_t entry;
    uint32_t saved;
    size_t segs;
    char * at;
    uint64_t events;
};

static std::vector<OpenForce> opens;

/* Ticks at every force that changes the owner, and at every force of a
   young cell of the current owner's below its open segment or of a slot on
   the job's stack: the writes outside the open segment that no record
   shows. */
static uint64_t ownerEvents = 0;

/* Per log entry, when its force ended (`own`; the clock counts the ends of
   forces that changed the owner), and when and where (the bump cursor) the
   last force it owned ended (`last`, `at`). */
struct EntryEnds
{
    uint64_t own = UINT64_MAX;
    uint64_t last = 0;
    char * at = nullptr;
};

static std::vector<EntryEnds> ends;
static uint64_t forceClock = 0;

/* The clock at each boundary the open job continued past, and the ends (an
   entry, the clock) of the forces of an entry's young cells in a later
   element than its own force's, which `last` leaves out (see "Elements"). */
static std::vector<uint64_t> joinClocks;
static std::vector<std::pair<uint32_t, uint64_t>> crossForces;

/* Room for `more` pushes on `v`, growing it geometrically. */
template<typename T>
static void roomFor(std::vector<T> & v, size_t more)
{
    if (v.capacity() - v.size() < more)
        v.reserve(std::max(v.size() + more, 2 * v.capacity()));
}

static char * segStart(const OwnerSeg & s)
{
    return evalRegion.base + (size_t(s.off) << 4);
}

/* Make `o`, which is not the current owner, the current owner. Each force
   running pushes at most one segment here at its start and one at its end,
   for which its start makes room. */
static void setOwner(uint32_t o)
{
    if (evalRegion.cur == openSeg) {
        /* Nothing was allocated under the current owner since its segment
           opened: reopen the segment below if it is `o`'s. */
        if (!segments.empty() && segments.back().owner == o) {
            openSeg = segStart(segments.back());
            segments.pop_back();
        }
    } else {
        segments.push_back({granuleOf(openSeg), curOwner});
        openSeg = evalRegion.cur;
    }
    curOwner = o;
}

/* The closed segment the last lookup found, tried first. */
static size_t ownerHint = 0;

/* The owner of the young object at `p`. */
static uint32_t ownerOf(const void * p)
{
    if ((const char *) p >= openSeg)
        return curOwner;
    uint64_t g = granuleOf(p);
    auto & h = ownerHint;
    if (h < segments.size() && segments[h].off <= g && (h + 1 == segments.size() || g < segments[h + 1].off))
        return segments[h].owner;
    auto i = std::upper_bound(segments.begin(), segments.end(), g, [](uint64_t off, auto & s) { return off < s.off; });
    assert(i != segments.begin());
    h = i - segments.begin() - 1;
    return i[-1].owner;
}

/* Push a force that changes the owner to `o` (`entry`, for a logged one). */
static void openForce(const Value & v, uint32_t entry, uint32_t o)
{
    opens.push_back({&v, entry, curOwner, 0, evalRegion.cur, ++ownerEvents});
    unforceTopOpen = &v;
    setOwner(o);
    opens.back().segs = segments.size();
}

/* ---------------------------------------------------------------------------
   Promotion.

   Reverting a target t breaks pointer identity (`eqValues` short-cuts on
   `&v1 == &v2`; Nixpkgs relies on it) when a promoted value keeps a young
   cell that re-forcing t builds again: two cells would stand for one. So the
   promoted set S is closed under ownership: every young identity-bearing
   object (a cell, a Bindings layer, an element array) a promoted value
   reaches has its owner in S, or none, and a reverted target, re-forced
   later, rebuilds only cells that nothing kept.

   S is decided from records alone. While t is forced, its young cells are
   visible only to its own evaluation and to their forces (t has no value
   yet), so a value reaches them only once t's force has ended. What the
   forces a promoted target owns (its own, and its young cells') leave
   reachable was allocated before the last of them ended, below the cursor
   then, and reaches only targets whose forces ended before then. Promoting
   those too, until none is added, closes S without visiting anything. The job
   keeps its memory below the highest cursor at the end of a force that S
   owns, and frees the rest.

   S starts from the nominated targets: those under whose force an effect ran
   (`keep`); those an earlier job reverted (evidently shared between jobs,
   which reverting again would make each evaluate anew); those a holder keeps;
   the owners of the young objects holders hold (see unforce.hh); and those
   whose young cells a later element of the job forced (see "Elements"). A
   young object a holder holds that the driver owns keeps the job whole. A
   failed target is not nominated: its failure is allocated outside the
   region, so one with no young recovery value (`youngFree`) keeps its value,
   as without unforcing, and later reads throw its first error again, with its
   frames. With `unforce-always-revert`, a target an earlier job reverted, or
   one whose young cells a later element forced, is not nominated: every job
   reverts it again.

   A target whose value is a string keeps it outside S, by a copy
   (`copyStringOut`): neither the first three nominations nor the time closure
   put it in S, so it raises neither the horizon nor the cut. A string reaches
   no cell, Bindings or list and is compared by content, so no identity rests
   on where its bytes are. Its young objects leave its force only through its
   value, through a holder (which names it as their owner, keeping it in S),
   or through a table that keeps values across forces, of which there is none
   (the import cache gives each file a cell of its own). So once its force has
   ended nothing reaches them; a force of one of them after its own (its
   `last` past its `own`) keeps it in S. Another target sharing its value is
   decided on its own, and a young cell that does was written below its
   owner's `at`: under the cut if that owner is in S, unreachable if not. The
   parts of its value that the end frees (its bytes, its context array and the
   array's elements) are copied out of the region; the cell keeps its value at
   its own address.

   Below the cut, a target neither in S nor kept by copy owns memory that
   nothing kept reaches (the argument above holds for objects of every kind: a
   value reaches another owner's objects only through that owner's cells,
   written once its force has ended). The job's end gives the whole pages of
   such targets' runs back to the system (`releaseDead`), never reused: the
   next job starts at the cut. The driver's runs stay, as a holder may reach
   them. A string kept by copy may hold another target's bytes (its value
   taken from one whose force enclosed a promoted one): it is copied out of
   those runs too.

   A seen set's entry for a set (`UnforceSeen`) stands only for the set's
   address. One that is the value of the target whose force built it does not
   nominate the target: if the target is reverted, nothing kept reaches the
   set (what does was made after the target's force ended and would promote it
   by time). So the set forgets the address, which the job may free, and
   records the cell, reverted to a thunk that `ExprReforce::eval` evaluates
   (`rebuilds`): the re-force puts the set it builds in the entry's place
   before anything reads the cell. Other entries (a list's cell, a set inside
   a value, one the driver owns) nominate as before.
   --------------------------------------------------------------------------- */

/* A job's targets, each once, in the order of their first entries, and the
   target of each log entry. `rebuilds`: whether a target, reverted to its
   first entry's pre-image, holds a thunk that `ExprReforce::eval`
   evaluates. */
struct Targets
{
    std::vector<Value *> cells;
    std::vector<uint32_t> ofEntry;
    std::vector<bool> rebuilds;

    Targets(const std::vector<UnforceUndoEntry> & log)
    {
        boost::unordered_flat_map<const Value *, uint32_t> index;
        ofEntry.reserve(log.size());
        for (auto & e : log) {
            auto [i, added] = index.try_emplace(e.target, cells.size());
            if (added) {
                cells.push_back(e.target);
                rebuilds.push_back(reforceable(e.preImage));
            }
            ofEntry.push_back(i->second);
        }
    }
};

/* A seen set's entry that a job's end forgets (see above), and its cell's
   record, allocated before the end writes anything. */
struct SeenEntry
{
    UnforceSeen * set;
    const void * p;
    uint32_t target;
    UnforceSeen::Owner owner;
};

/* The holders alive (see unforce.hh), roots of a job's end besides the
   promoted values. Per thread: a holder's thread is the one that runs the
   jobs it sees. `jobHolders`: the open job's thread's, set at its opening,
   so that a step's end reads no thread-local. */
static thread_local std::vector<UnforceHolder *> holders;
static std::vector<UnforceHolder *> * jobHolders = nullptr;

UnforceHolder::UnforceHolder()
{
    if (!NIX_USE_BOEHMGC)
        holders.push_back(this);
}

UnforceHolder::~UnforceHolder()
{
    if (!NIX_USE_BOEHMGC)
        std::erase(holders, this);
}

UnforceHolder::Hold::~Hold() = default;

namespace {

struct HeldRoot : UnforceHolder
{
    Value * v;

    explicit HeldRoot(Value * v)
        : v(v)
    {
        youngTop = v;
    }

    void held(Hold & hold) override
    {
        if (evalRegion.contains(v))
            hold(v, Kind::value);
    }

    void ended() override
    {
        youngTop = nullptr;
    }
};

} // namespace

RootValue unforceHeldRoot(Value * v)
{
    auto root = std::make_shared<HeldRoot>(v);
    return RootValue(root, &root->v);
}

bool UnforceSeen::see(const void * p, Kind kind)
{
    if (!insert(p).second)
        return false;
    if (evalRegion.contains(p)) {
        /* Only the end of a job not kept whole reads `young` (`held`), and a
           job kept whole stays so until its end. */
        if (!keepWhole)
            young.emplace_back(p, kind);
        youngTop = std::max(youngTop, (const void *) p, std::less<>());
    }
    return true;
}

void UnforceSeen::held(Hold & hold)
{
    for (auto & [p, kind] : young)
        if (kind == Kind::bindings)
            hold.identity(p, *this);
        else
            hold(p, kind);
}

void UnforceSeen::ended()
{
    young.clear();
    youngTop = nullptr;
}

void UnforceSeen::forget(const void * p, Owner owner)
{
    erase(p);
    owners.insert(std::move(owner));
}

void UnforceSeen::rebuilt(const Value & cell)
{
    if (auto i = owners.find(&cell); i != owners.end()) {
        if (cell.type() == nAttrs)
            see(cell.attrs(), Kind::bindings);
        owners.erase(i);
    }
}

static void noteRebuilt(const Value & cell)
{
    for (auto h : holders)
        h->rebuilt(cell);
}

/* The young cells whose force failed in the open job, and whether one could
   not be recorded (then the job is kept whole). Not nominations: a job's
   end releases the records of those whose cells it frees. */
static std::vector<const Value *> youngFailures;
static bool youngFailuresLost = false;

void unforceNoteFailure(const Value & v) noexcept
{
    if (!evalRegion.contains(&v))
        return;
    try {
        youngFailures.push_back(&v);
    } catch (std::bad_alloc &) {
        youngFailuresLost = true;
    }
}

namespace {

/* A holder's young pointers, each nominating its owner's target, or, for
   one that the driver owns, keeping the job whole; a seen set's entry that
   is its owner's value, a candidate to forget instead (see Promotion). */
struct OwnerHold : UnforceHolder::Hold
{
    const Targets & targets;
    std::vector<bool> & promote;
    std::vector<SeenEntry> & forget;
    bool driver = false;

    OwnerHold(const Targets & targets, std::vector<bool> & promote, std::vector<SeenEntry> & forget)
        : targets(targets)
        , promote(promote)
        , forget(forget)
    {
    }

    void operator()(const void * p, UnforceHolder::Kind) override
    {
        if (!evalRegion.contains(p))
            return;
        if (auto o = ownerOf(p); o == noOwner)
            driver = true;
        else
            promote[targets.ofEntry[o]] = true;
    }

    /* The record names the cell by address, so it must outlive the set: it
       does for every seen set today, whose cells are older than the frames
       of the call that owns the set. */
    void identity(const void * p, UnforceSeen & set) override
    {
        if (evalRegion.contains(p))
            if (auto o = ownerOf(p); o != noOwner) {
                auto t = targets.ofEntry[o];
                auto cell = targets.cells[t];
                if (cell->type() == nAttrs && cell->attrs() == p && targets.rebuilds[t]) {
                    forget.push_back({&set, p, t, {}});
                    return;
                }
            }
        (*this)(p, UnforceHolder::Kind::bindings);
    }
};

} // namespace

/* Copy string `v`'s bytes and context out of the job's region, the parts
   that the job's end frees (`freed`), to memory allocated as outside any job
   (the caller pauses the region). `fromBuilder` allocates a context array's
   elements right after it, with no force between, so neither the cut nor an
   owner's run separates them. */
static void copyStringOut(EvalState & state, Value & v, auto && freed)
{
    using Context = Value::StringWithContext::Context;
    auto s = v.stringData();
    auto c = v.context();
    if (freed(s))
        s = &StringData::make(state.mem, s->view());
    if (freed(c)) {
        auto c2 = new (state.mem.allocBytes<const StringData *, sizeof(Context)>(c->size())) Context(c->size());
        std::ranges::transform(*c, const_cast<const StringData **>(c2->begin()), [&](auto e) {
            return &StringData::make(state.mem, e->view());
        });
        c = c2;
    }
    v.mkStringNoCopy(*s, c);
}

/* ---------------------------------------------------------------------------
   Elements. A job that a traversal continues past an element's boundary
   (`unforceElementStart`) runs several elements. As one stretch of time, its
   latest promoted value could reach anything an earlier element allocated, as
   a read of a cell already forced leaves no record. So at each such boundary
   every cell logged since the one before whose value points into the region
   is trapped: it holds a thunk of `trapExpr`, whose environment is the cell,
   and its value waits in `traps`. Forcing the cell gives the value back, the
   same bits, and is logged as a *read* of it that ends in the reading
   element; forcing a copy of the trap forces the cell and copies its value,
   as a copy of the value would hold. A holder that holds the job's memory at
   the boundary could let go of it before the job's end, after a later element
   read through it: the job ends there instead. A seen set lets go of nothing
   before the job's end (only `ended` clears it) and is read only by address,
   so the end sees what it holds if it still lives. So an element reaches what
   an earlier one allocated only through a read, a holder that the end asks,
   or a value the end keeps, and the argument above holds in each element on
   its own, with a horizon of its own. A promoted target raises the horizon of
   the element its force ended in to its forces' last end there, and that of
   each later element where a force of one of its young cells ended to that
   end; every entry, a read too, that ended in an element by its horizon
   promotes its target. A read allocates nothing: it raises no horizon and no
   cut. The job's end puts every trapped value back before it decides
   anything; a read's entry is never written back (the cell ends with its
   first entry's pre-image). A boundary where this cannot be done ends the job
   instead (see `UnforceNest::trap`).
   --------------------------------------------------------------------------- */

struct ExprTrap : Expr
{
    void eval(EvalState & state, Env & env, Value & v) override;

    void show(const SymbolTable & symbols, std::ostream & str) const override {}

    void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override {}
};

static ExprTrap trapExpr;
static boost::unordered_flat_map<const Value *, Value> traps;

/* The undo log's length at the last boundary. */
static size_t trapFrom = 0;

void ExprTrap::eval(EvalState & state, Env & env, Value & v)
{
    auto cell = reinterpret_cast<Value *>(&env);
    if (&v == cell) {
        auto i = traps.find(cell);
        assert(i != traps.end());
        v = i->second;
        traps.erase(i);
    } else {
        state.forceValue(*cell, noPos);
        v = *cell;
    }
}

const Value * unforceTrapped(const Expr * expr, const Env * env)
{
    if (expr != &trapExpr)
        return nullptr;
    auto cell = reinterpret_cast<const Value *>(env);
    auto i = traps.find(cell);
    return i != traps.end() ? &i->second : cell;
}

/* Whether `e` logged a read of its cell's trap (a copy's force is a force). */
static bool isRead(const UnforceUndoEntry & e)
{
    return e.preImage.isThunk() && e.preImage.thunk().expr == &trapExpr
           && e.preImage.thunk().env == reinterpret_cast<Env *>(e.target);
}

/* Which targets keep their values (`promote`: S, and the strings kept by
   copy; see above), and the top of the memory the job keeps; null to keep
   it whole. No object is visited. `forget`: the seen sets' entries whose
   cells are reverted if not promoted. */
static char * keepPrefix(
    EvalState & state,
    const std::vector<UnforceUndoEntry> & log,
    const Targets & targets,
    std::vector<bool> & promote,
    bool alwaysRevert,
    std::vector<SeenEntry> & forget)
{
    std::vector<EntryEnds> per(targets.cells.size(), EntryEnds{0, 0, nullptr});
    for (size_t i = 0; i < log.size(); ++i) {
        auto & e = per[targets.ofEntry[i]];
        e.own = std::max(e.own, ends[i].own);
        e.last = std::max(e.last, ends[i].last);
        e.at = std::max(e.at, ends[i].at);
    }
    /* Whether target `t` keeps its value by copy instead of in S. */
    auto byCopy = [&](size_t t) {
        if (targets.cells[t]->internalType() != tString)
            return false;
        /* Never so, by the argument above; kept as a guard. */
        if (per[t].last != per[t].own) [[unlikely]]
            return false;
        return true;
    };
    std::vector<bool> copy(promote.size());
    /* A target whose young cells a later element of the job forced is
       needed there: an end at the boundary between would have reverted it,
       and that element forced it again (see "Elements"). */
    for (auto & f : crossForces)
        if (auto t = targets.ofEntry[f.first]; !alwaysRevert && !targets.cells[t]->isFailed())
            promote[t] = true;
    for (size_t i = 0; i < log.size(); ++i) {
        auto & p = log[i].preImage;
        auto t = targets.ofEntry[i];
        if ((log[i].keep && !log[i].target->isFailed())
            || (!alwaysRevert && p.isThunk() && unforceIsReverted(p.thunk().expr))
            || std::ranges::any_of(holders, [&](auto h) { return h->keeps(log[i].target); }))
            (byCopy(t) ? copy : promote)[t] = true;
    }
    OwnerHold hold{targets, promote, forget};
    for (auto h : holders)
        h->held(hold);
    if (hold.driver)
        return nullptr;

    /* The time closure, each element on its own (see "Elements"): `h[e]`
       is element e's horizon; its entries, by their ends, are byEnd[first[e],
       first[e + 1]), of which those before next[e] are closed in. */
    auto elemOf = [&](uint64_t c) { return size_t(std::ranges::lower_bound(joinClocks, c) - joinClocks.begin()); };
    auto m = joinClocks.size();
    std::vector<uint32_t> byEnd(log.size());
    std::iota(byEnd.begin(), byEnd.end(), 0);
    std::ranges::sort(byEnd, {}, [&](auto i) { return ends[i].own; });
    std::vector<size_t> first(m + 2, byEnd.size());
    first[0] = 0;
    for (size_t e = 1; e <= m; ++e)
        first[e] = std::ranges::upper_bound(byEnd, joinClocks[e - 1], {}, [&](auto i) { return ends[i].own; })
                   - byEnd.begin();
    auto next = first;
    std::vector<uint64_t> h(m + 1);
    std::vector<size_t> raised;
    auto raise = [&](uint64_t c) {
        if (auto e = elemOf(c); c > h[e]) {
            h[e] = c;
            raised.push_back(e);
        }
    };
    /* Each target's entries, from its first. */
    std::vector<uint32_t> head(promote.size(), noOwner), link(log.size(), noOwner);
    for (auto i = log.size(); i-- > 0;) {
        link[i] = head[targets.ofEntry[i]];
        head[targets.ofEntry[i]] = i;
    }
    std::ranges::sort(crossForces);
    char * top = evalRegion.base;
    auto add = [&](size_t t) {
        for (auto i = head[t]; i != noOwner; i = link[i])
            if (!isRead(log[i])) {
                raise(std::max(ends[i].own, ends[i].last));
                top = ends[i].own == UINT64_MAX ? evalRegion.cur : std::max(top, ends[i].at);
                for (auto & f : std::ranges::equal_range(crossForces, i, {}, &std::pair<uint32_t, uint64_t>::first))
                    raise(f.second);
            }
    };
    for (size_t t = 0; t < promote.size(); ++t)
        if (promote[t])
            add(t);
    while (!raised.empty()) {
        auto e = raised.back();
        raised.pop_back();
        for (; next[e] < first[e + 1] && ends[byEnd[next[e]]].own <= h[e]; ++next[e])
            if (auto t = targets.ofEntry[byEnd[next[e]]]; !promote[t] && byCopy(t))
                copy[t] = true;
            else if (!promote[t]) {
                promote[t] = true;
                add(t);
            }
    }
    /* Nothing live lies above the cursor. */
    auto keep = std::min(top, evalRegion.cur);
    /* What the job's end frees: what lies above the cut, and the runs below
       it of the targets that keep nothing (`releaseDead`). */
    auto freed = [&](const void * p) {
        if (!evalRegion.contains(p))
            return false;
        if ((const char *) p >= keep)
            return true;
        auto o = ownerOf(p);
        return o != noOwner && !promote[targets.ofEntry[o]] && !copy[targets.ofEntry[o]];
    };
    for (size_t t = 0; t < promote.size(); ++t)
        if (copy[t] && !promote[t]) {
            copyStringOut(state, *targets.cells[t], freed);
            promote[t] = true;
        }
    return keep;
}

/* Give back the runs below the cut `keptTo` of the targets that keep nothing
   (see Promotion): adjacent ones as one run, so that a page they share goes
   too. Returns how much memory went back. */
static size_t releaseDead(const Targets & targets, const std::vector<bool> & promote, char * keptTo)
{
    char *lo = nullptr, *hi = nullptr;
    size_t given = 0;
    auto run = [&](char * s, char * e, uint32_t o) {
        e = std::min(e, keptTo);
        if (s < e && o != noOwner && !promote[targets.ofEntry[o]]) {
            if (hi != s) {
                given += giveBack(lo, hi);
                lo = s;
            }
            hi = e;
        } else {
            given += giveBack(lo, hi);
            lo = hi = nullptr;
        }
    };
    for (size_t k = 0; k < segments.size() && segStart(segments[k]) < keptTo; ++k)
        run(segStart(segments[k]), k + 1 < segments.size() ? segStart(segments[k + 1]) : openSeg, segments[k].owner);
    run(openSeg, evalRegion.cur, curOwner);
    return given + giveBack(lo, hi);
}

/* ---------------------------------------------------------------------------
   Steps.

   A loop's steps (`UnforceSteps`) free their memory inside a job, by records,
   without a walk or a move. They end in batches: a batch's steps run on until
   what it holds (its memory, its writes and its log entries) spans a page, as
   a boolean frame must to give anything back, or a step's cell was given (the
   loop's last), or the job is gone. A batch's memory, [start, cur), starts
   before its first step's cell, so every cell its steps write a value to lies
   in it, but a given one. Nothing allocated before the batch points into it
   unless the batch wrote it, and its steps write a cell outside it only by
   forcing it, or as the given cell. So once its last step's call has
   returned, a pointer into its memory can only be in:
   - the last step's value;
   - a cell logged during the batch (an old cell): its value or pre-image;
   - a young cell below `start`, or a slot of a frame outside the steps (the
     loop's and its callers'), forced during the batch (`stepWrites`);
   - a holder (`youngTop`), a young failure recorded during the batch, or what
     native code holds.

   The records accumulate over the batch: writes are dropped only at its end,
   and a log entry only by its force's end with a young-free value
   (`endOwnerForce`), whose cell then points to nothing young. The batch's end
   looks at the own pointers of each (`valueEdges`), not further: what they
   point to outside its memory was allocated before it, and points into it
   only if the batch wrote it, which is a record above. A failed value is
   taken to point into it. If none does, the cursor goes back to `start`, the
   memory is zeroed, and the owner timeline is cut back to `start`.

   This rests on each step writing its own value's cell once, at its end, by
   copy: no evaluation keeps the address of the cell its result goes to
   (`callFunction` evaluates into a local and copies). Between steps the loop
   reads only that cell; a step's memory may hold the cells earlier steps of
   its batch wrote, which only the step after each reads.

   A freed batch's last value moves out of it: to the cell the batch's first
   step read (`prev`) if that is the loop's own and nothing reads it any more
   (no record and no holder points to it), else to a new cell at `start`. So a
   loop's cells do not pile up.
   --------------------------------------------------------------------------- */

using StepFrame = UnforceStepFrame;

/* Plain globals (declared in unforce.hh for the inline steps), not
   thread-locals, like `segments` and `opens`: steps open only in a job
   (`begin`), and only its owner's thread forces in it (`logPreImageSlow` runs
   while `isolationDepth` is set, which only the job's owner has), so no other
   thread touches them; a thread-local here cost a `__tls_get_addr` call per
   access from libnixexpr.so, several per step and per force. */
std::vector<StepFrame> unforceStepFrames;
std::vector<Value *> unforceStepWrites;
size_t unforceStepSpan = SIZE_MAX;
static auto & stepFrames = unforceStepFrames;
static auto & stepWrites = unforceStepWrites;
unsigned unforceStepsOpen = 0;

/* Whether `p` lies outside the memory and the frames of step frame `f`. */
static bool outsideStep(const void * p, const StepFrame & f)
{
    return evalRegion.contains(p) ? (const char *) p < f.start : (const char *) p >= f.sp;
}

/* A force of `v` starts while a step runs: record it if `v` lies outside
   the step's memory and frames, and is no old cell (which is logged). */
static void noteStepWrite(Value & v)
{
    auto & f = stepFrames.back();
    auto p = (const char *) &v;
    if (evalRegion.contains(p) ? p < f.start : p >= f.sp && p >= jobStackLow && p < jobStackMark) {
        roomFor(stepWrites, 1);
        stepWrites.push_back(&v);
    }
}

/* Drop the writes recorded since `f`'s step started that lie inside the
   frame around it, or all of them if none. */
static void dropStepWrites(const StepFrame & f)
{
    auto up = stepFrames.size() > 1 ? &stepFrames.end()[-2] : nullptr;
    stepWrites.erase(
        std::remove_if(
            stepWrites.begin() + f.writes, stepWrites.end(), [&](auto w) { return !up || !outsideStep(w, *up); }),
        stepWrites.end());
}

bool UnforceSteps::begin()
{
    if (jobOwner.load() != &state || evalRegion.paused)
        return false;
    roomFor(stepFrames, 1);
    StepFrame f;
    f.sp = (const char *) __builtin_dwarf_cfa();
    f.start = evalRegion.cur;
    f.writes = stepWrites.size();
    f.job = unforceStats.jobs;
    stepFrames.push_back(f);
    unforceStepsOpen++;
    return true;
}

Value * UnforceSteps::cellSlow(Value * given)
{
    auto & f = stepFrames.back();
    /* A batch starts here, before its first cell (`cell`). A batch of
       another job's ends unchecked, and a cell of another job's is not the
       loop's to write. */
    if (f.batch)
        dropStepWrites(f);
    f.prevOwn = f.prevOwn && f.job == unforceStats.jobs;
    f.start = evalRegion.cur;
    f.writes = stepWrites.size();
    f.log = state.undoLog.size();
    f.failures = youngFailures.size();
    f.opens = opens.size();
    f.owner = curOwner;
    f.job = unforceStats.jobs;
    f.batch = true;
    f.own = !given;
    return given ? given : state.allocValue();
}

Value * UnforceSteps::stepSlow(Value * v)
{
    /* The batch ends (`step`). */
    auto & f = stepFrames.back();
    auto top = evalRegion.cur;
    f.batch = false;
    /* Whether the batch's argument, the loop's own cell, may take its
       value: nothing it leaves points to it. `escapes` clears it as it
       looks; it is read only when no check below stopped the batch, so that
       every check has run in full. */
    bool spare = f.prevOwn;
    auto escapes = [&](const Value & x) {
        if (x.isFailed())
            return true;
        bool into = false;
        valueEdges(x, [&](const void * p) {
            into = into || ((const char *) p >= f.start && (const char *) p < top);
            spare = spare && p != f.prev;
        });
        return into;
    };
    bool kept = f.job != unforceStats.jobs || jobOwner.load() != &state || evalRegion.paused || youngFailuresLost
                || youngFailures.size() != f.failures || opens.size() != f.opens || curOwner != f.owner || stepsUnsafe
                || unforceNativeCode(state)
                || std::ranges::any_of(*jobHolders, [&](auto h) { return !std::less<>()(h->youngTop, f.start); })
                || escapes(*v)
                || std::any_of(
                    state.undoLog.begin() + f.log,
                    state.undoLog.end(),
                    [&](auto & e) { return escapes(*e.target) || escapes(e.preImage); })
                || std::any_of(stepWrites.begin() + f.writes, stepWrites.end(), [&](auto w) { return escapes(*w); });
    if (!kept && top > f.start) {
        /* Free the batch's memory. */
        auto value = *v;
        jobTop = std::max(jobTop, top);
#if NIX_ASAN_ENABLED
        /* Frames' ends may have poisoned some of it (`giveBack`). */
        ASAN_UNPOISON_MEMORY_REGION(f.start, top - f.start);
#endif
        memset(f.start, 0, top - f.start);
#if NIX_ASAN_ENABLED
        ASAN_POISON_MEMORY_REGION(f.start, top - f.start);
#endif
        evalRegion.cur = f.start;
        if (openSeg > f.start) {
            while (!segments.empty() && segStart(segments.back()) >= f.start)
                segments.pop_back();
            openSeg = f.start;
            if (!segments.empty() && segments.back().owner == curOwner) {
                openSeg = segStart(segments.back());
                segments.pop_back();
            }
        }
        unforceStats.stepEnds++;
        unforceStats.stepBytesFreed += top - f.start;
        if (f.own) {
            assert((char *) v >= f.start && (char *) v < top);
            v = spare && std::ranges::none_of(*jobHolders, [&](auto h) { return !std::less<>()(h->youngTop, f.prev); })
                    ? f.prev
                    : state.allocValue();
            *v = value;
        }
    }
    f.prev = v;
    f.prevOwn = f.own;
    dropStepWrites(f);
    return v;
}

void UnforceSteps::end()
{
    dropStepWrites(stepFrames.back());
    stepFrames.pop_back();
    unforceStepsOpen--;
}

/* ---------------------------------------------------------------------------
   Boolean frames.

   An in-place boolean evaluation in a job (`UnforceBoolFrame`) is a frame:
   it starts with the bump cursor at `s` under owner O, and ends with a
   Boolean, which points to nothing. What O allocated in it, O's runs in
   [s, cur), is then dead unless a pointer into it was stored outside it,
   and its whole pages go back to the system. Pointers are stored as an
   object is made, as a force writes its cell, and in C++ state that a
   holder covers. A force in the frame writes a young cell or a slot of the
   job's stack, whose value is looked at as the force ends (`frameWrite`),
   whoever owns it; or an old cell, which is logged, so forced as its own
   owner from its pre-image (a thunk or an application), which reaches O's
   memory in the frame only through a cell that a force wrote first, or
   through a pointer of its own, which blocks the frame. So the frame gives
   nothing back if a young cell older than it or a slot was written with a
   pointer at or above its start, or a pre-image logged in it points into
   the region; if a force in it failed (a failed cell gets its recovery
   value under O after its force's end, and the job's end reads failed
   young cells); if a holder may hold its memory; or if its job is gone, the
   region paused, native code possible, the job kept whole (as when its log
   is full, and cells are forced unlogged, under O) or its steps unsafe. The
   owner at its end is O: forces push and pop owners in pairs, and only a
   failed force's end pops one out of turn.

   The records are the innermost frame's, merged into the enclosing one's as
   it ends. A young write blocks the frames that start above the cell and at
   or below its highest pointer, found among the open frames' starts, which
   grow along the stack. Neither depends on who owns what at the write,
   which a dropped entry may change later.
   --------------------------------------------------------------------------- */

UnforceFrameRecord unforceFrame = {(const char *) UINTPTR_MAX, nullptr};
std::vector<UnforceFrameMark> unforceFrames;
size_t unforceFrameSpan = SIZE_MAX;
uint64_t unforceFailures = 0;

/* The highest pointer of `v` into the job's region, or null. */
static const char * maxYoung(const Value & v)
{
    const char * p = nullptr;
    valueEdges(v, [&](const void * q) {
        if (evalRegion.contains(q))
            p = std::max(p, (const char *) q);
    });
    return p;
}

/* Cell `v` was written in a frame, at its force's end, or is logged with `v`
   its pre-image (`preImage`). Out of line, as `valueEdges`'s locals would
   cost the force path a stack frame. */
[[gnu::noinline, gnu::cold]] static void frameWrite(const Value & v, bool preImage)
{
    auto & r = unforceFrame;
    auto c = (const char *) &v;
    bool young = evalRegion.contains(c);
    if (young ? c >= r.s : !preImage && (c < jobStackLow || c >= jobStackMark))
        return;
    auto p = maxYoung(v);
    if (!p)
        return;
    if (!young)
        /* Every open frame. */
        r.lowC = (const char *) 1;
    else if (p >= r.s)
        r.lowC = std::min(r.lowC, c);
    else {
        auto & st = unforceFrames;
        auto byS = [](const char * x, const UnforceFrameMark & f) { return x < f.s; };
        int64_t hi = std::upper_bound(st.begin(), st.end(), p, byS) - st.begin() - 1;
        int64_t lo = std::upper_bound(st.begin(), st.begin() + (hi + 1), c, byS) - st.begin();
        if (lo <= hi)
            st[hi].markLo = std::min(st[hi].markLo, lo);
    }
}

/* A force ended in a frame, or failed. */
static void frameForceEnd(const Value & v, bool completed)
{
    if (!completed)
        unforceFailures++;
    else if (unforceFrame.s)
        frameWrite(v, false);
}

/* The innermost frame ended with its Boolean, spanning `unforceFrameSpan`. */
void unforceBoolFrameEnd(EvalState & state, uint64_t job, uint64_t fails)
{
    auto s = (char *) unforceFrame.s;
    if (job != unforceStats.jobs || jobOwner.load() != &state || evalRegion.paused || keepWhole || stepsUnsafe
        || fails != unforceFailures || unforceFrame.lowC < s
        || unforceFrames.back().markLo < int64_t(unforceFrames.size())
        || std::ranges::any_of(
            *jobHolders, [&](auto h) { return !std::less<>()(h->youngTop, s); })
        || unforceNativeCode(state))
        return;
    giveBack(std::max(openSeg, s), evalRegion.cur);
    for (auto k = segments.size(); k-- > 0;) {
        auto end = k + 1 < segments.size() ? segStart(segments[k + 1]) : openSeg;
        if (end <= s)
            break;
        if (segments[k].owner == curOwner)
            giveBack(std::max(segStart(segments[k]), s), end);
    }
}

/* ---------------------------------------------------------------------------
   Native code (see unforce.hh). Nix's own primops are recorded by name and
   function before plugins load: an evaluator whose primops differ in any
   entry may run a plugin's code. Nix's own `extraPrimOps` entries and lookup
   path hooks are recorded where Nix adds them: any other is a plugin's. A
   function is identified by its target's type and, for a plain function
   pointer, the pointer.
   --------------------------------------------------------------------------- */

struct NativeEntry
{
    std::string name;
    const std::type_info * type;
    const void * fn;

    bool operator==(const NativeEntry & other) const
    {
        return name == other.name && *type == *other.type && fn == other.fn;
    }
};

template<typename Sig>
static NativeEntry nativeEntry(std::string name, const fun<Sig> & f)
{
    auto & g = f.get_fn();
    auto p = g.template target<Sig *>();
    return {std::move(name), &g.target_type(), p ? reinterpret_cast<const void *>(*p) : nullptr};
}

static std::vector<NativeEntry> nativeEntries(const std::vector<PrimOp> & primOps)
{
    std::vector<NativeEntry> r;
    for (auto & p : primOps)
        r.push_back(nativeEntry(p.name, p.impl));
    return r;
}

/* Written before any evaluator is built. */
static std::optional<std::vector<NativeEntry>> ownPrimOps;
static std::vector<NativeEntry> ownExtraPrimOps, ownLookupPathHooks;
static std::atomic<bool> cApiCode = false;

void unforceNotePluginsLoading()
{
    ownPrimOps = nativeEntries(RegisterPrimOp::primOps());
}

void unforceNoteOwnExtraPrimOp(const PrimOp & primOp)
{
    ownExtraPrimOps.push_back(nativeEntry(primOp.name, primOp.impl));
}

void unforceNoteOwnLookupPathHooks(const EvalSettings::LookupPathHooks & hooks)
{
    for (auto & [scheme, hook] : hooks)
        ownLookupPathHooks.push_back(nativeEntry(scheme, hook));
}

void unforceNoteCApiCode()
{
    cApiCode = true;
}

/* The memory that ended jobs keep (less what they gave back), the most
   memory in use at a traversal's boundary so far, and the most that one
   element took in its job: from `elementStart`, where it started, to the
   job's top. */
static uint64_t keptMemory = 0, highWater = 0, mostOne = 0;
static char * elementStart = nullptr;

/* For the unit tests, which share their process with the C API's and with
   each other. */
void unforceTestReset();

void unforceTestReset()
{
    cApiCode = false;
    keptMemory = highWater = mostOne = 0;
}

void unforceNoteCApiState(EvalState & state)
{
    const char * none = nullptr;
    state.unforceNativeCodeReason.compare_exchange_strong(none, "C API host");
}

void unforceDecideNativeCode(EvalState & state)
{
    auto & settings = state.settings;
    /* `importNative` is in the evaluator if the setting was on when it was
       built, whatever the setting says later. */
    if (settings.enableNativeCode)
        state.unforceNativeCodeReason = "allow-unsafe-native-code-during-evaluation";
    else if (ownPrimOps && nativeEntries(RegisterPrimOp::primOps()) != *ownPrimOps)
        state.unforceNativeCodeReason = "plugin primop";
    else if (std::ranges::any_of(nativeEntries(settings.extraPrimOps), [](auto & e) {
                 return !std::ranges::contains(ownExtraPrimOps, e);
             }))
        state.unforceNativeCodeReason = "plugin extraPrimOps entry";
}

const char * unforceNativeCode(const EvalState & state)
{
    if (auto r = state.unforceNativeCodeReason.load())
        return r;
    return cApiCode ? "C API client code" : nullptr;
}

bool unforceEnabled(const EvalState & state)
{
    if (NIX_USE_BOEHMGC || !jobsHaveRegion() || state.debugRepl)
        return false;
    if (auto reason = unforceNativeCode(state)) {
        static std::atomic<const char *> reported = nullptr;
        if (reported.exchange(reason) != reason)
            debug("unforcing is off: native code may run (%s)", reason);
        return false;
    }
    return true;
}

bool unforceNoteLookupPathHook(const EvalSettings & settings, const std::string & scheme)
{
    /* Hooks are read at each lookup, so one added after the evaluator was
       built counts too. */
    if (auto * hook = get(settings.lookupPathHooks, scheme);
        hook && std::ranges::contains(ownLookupPathHooks, nativeEntry(scheme, *hook)))
        return false;
    /* What the hook allocates, or keeps of what it reaches in the job, would
       be freed with the job's region. */
    if (jobOwner.load())
        keepWhole = stepsUnsafe = true;
    return true;
}

/* The evaluator's builtins: per base environment slot and `builtins`
   attribute, its cell and, if it holds a primop, the primop and its
   function. */
static std::vector<const void *> builtinsFingerprint(EvalState & state)
{
    std::vector<const void *> r;
    auto entry = [&](const Value * v) {
        r.push_back(v);
        if (v && v->isPrimOp()) {
            auto e = nativeEntry("", v->primOp()->impl);
            r.insert(r.end(), {v->primOp(), e.type, e.fn});
        } else
            r.insert(r.end(), {nullptr, nullptr, nullptr});
    };
    for (auto & [symbol, displ] : state.staticBaseEnv->vars)
        entry(state.baseEnv.values[displ]);
    if (auto & builtins = state.getBuiltins(); builtins.type() == nAttrs)
        for (auto & attr : *builtins.attrs())
            entry(attr.value);
    return r;
}

UnforceBuiltinsCheck::UnforceBuiltinsCheck(EvalState & state)
    : state(state)
    , before(builtinsFingerprint(state))
{
}

UnforceBuiltinsCheck::~UnforceBuiltinsCheck()
{
    try {
        if (builtinsFingerprint(state) == before)
            return;
    } catch (...) {
        /* Could not look: as if they changed. */
    }
    const char * none = nullptr;
    state.unforceNativeCodeReason.compare_exchange_strong(none, "plugin primop installed at runtime");
}

void unforceNoteEffect(EvalState & state)
{
    /* Running in the current owner's force, or in that of a young cell it
       owns, which only its re-force runs again. In another evaluator than
       the job's, the job is kept whole. */
    auto job = jobOwner.load();
    if (job && job != &state)
        keepWhole = true;
    else if (job && curOwner != noOwner)
        state.undoLog[curOwner].keep = true;
}

/* `path` is read as it is: outside the REPL, the evaluator's caches keep the
   resolution of its symlinks (`lstat`, `readlink`), and accessors follow none. */
UnforceChange unforceReadChange(EvalState & state, const SourcePath & path)
{
    auto job = jobOwner.load();
    if (!job || (job == &state && curOwner == noOwner))
        return UnforceChange::none;
    auto & store = *state.store;
    auto s = path.path.abs();
    auto allow = state.rootFS.dynamic_pointer_cast<AllowListSourceAccessor>();
    if (path.accessor == state.rootFS && allow && !allow->isAllowed(path.path))
        return UnforceChange::byEvaluation;
    /* The store directory's listing too: the evaluation adds to it. */
    if (path.accessor != state.rootFS || !(store.isInStore(s) || s == store.storeDir))
        return UnforceChange::byEdit;
    auto p = store.maybeParseStorePath(s.substr(0, s.find('/', store.storeDir.size() + 1)));
    if (!p)
        return UnforceChange::byEvaluation;
    if (auto mount = state.storeFS->getMount(CanonPath(store.printStorePath(*p)))) {
        auto dir = mount->getPhysicalPath(CanonPath::root);
        return !dir || store.isInStore(dir->string()) ? UnforceChange::none : UnforceChange::byEdit;
    }
    /* What the store cannot answer is as if not valid yet. */
    try {
        auto held = state.heldStorePaths.find(*p);
        if (held == state.heldStorePaths.end()) {
            store.addTempRoot(*p);
            held = state.heldStorePaths.emplace(*p, false).first;
        }
        if ((held->second = held->second || store.isValidPath(*p)))
            return UnforceChange::none;
    } catch (Error &) {
    }
    /* Impure, `rootFS` reads the host's file system first. A store that
       keeps its objects elsewhere (a dummy, chroot or remote store) never
       writes there: what the host has there changes only by an edit. */
    auto local = dynamic_cast<LocalFSStore *>(&store);
    if (!state.settings.pureEval && !(local && local->getRealStoreDir().string() == store.storeDir)
        && getFSSourceAccessor()->pathExists(CanonPath(store.printStorePath(*p))))
        return UnforceChange::byEdit;
    return UnforceChange::byEvaluation;
}

void UnforceFileChanged::anchor() {}

SourcePath UnforceRead::at(SourcePath path)
{
    resolved = path.path.abs();
    linked = path != *given || path.accessor != state.rootFS;
    change = unforceReadChange(state, *given);
    if (linked)
        change = std::max(change, unforceReadChange(state, path));
    return path;
}

std::optional<std::string> UnforceRead::key(bool changes)
{
    /* Recorded in this evaluator's job, outside the REPL (which keeps every
       impure operation's cell). */
    bool record = changes && change == UnforceChange::byEdit && jobOwner.load() == &state && !state.fileCachesReset;
    if (changes && change != UnforceChange::none && !record)
        unforceNoteEffect(state);
    if (!given || (!record && state.readDigests.empty()))
        return std::nullopt;
    auto key = std::string(op) + ' ' + std::to_string(given->accessor->number) + ' ' + given->path.abs();
    if (!record && !state.readDigests.contains(key))
        return std::nullopt;
    return key;
}

void UnforceRead::check(fun<void(Sink &)> feed, bool changes)
{
    if (auto k = key(changes)) {
        HashSink sink(HashAlgorithm::SHA256);
        sink << resolved;
        feed(sink);
        auto digest = sink.finish().hash;
        if (state.readDigests.emplace(std::move(*k), digest).first->second != digest)
            changed();
    }
}

SourcePath unforceResolve(EvalState & state, std::string_view op, const SourcePath & path, SymlinkResolution mode)
{
    /* A resolution through `rootFS` that followed no link follows none
       later either (its cache keeps what `lstat` found), so no read of the
       path was recorded or will be: it needs none (copies run at every
       coercion of a path). */
    if (path.accessor == state.rootFS)
        if (auto resolved = path.resolveSymlinks(mode); resolved == path)
            return resolved;
    UnforceRead read(state, op, path);
    auto resolved = read.at(read([&] { return path.resolveSymlinks(mode); }));
    read.check([](Sink &) {}, read.linked);
    return resolved;
}

void UnforceRead::changed()
{
    throw UnforceFileChanged((unsigned int) 102, "file '%s' changed during evaluation (%s)", given->to_string(), op);
}

/* ---------------------------------------------------------------------------
   Traversals (`UnforceJobs`), the innermost last. `opener` is the frame of
   the level whose element runs in the job that the traversal opened, if
   one is open.
   --------------------------------------------------------------------------- */

struct Traversal
{
    EvalState & state;
    bool unforce;
    const char * opener = nullptr;
};

static thread_local std::vector<Traversal> traversals;

/* ---------------------------------------------------------------------------
   Nested levels (`unforceLevelElement`). In a traversal's job, an element of
   a level nested in its element (a set the front end recurses into) starts
   a job of its own, ending the one before it (a *switch*). That end keeps
   what the levels still open read across it: their containers, which
   `UnforceNest` holds (their loops hold copies of `Attr *`s or elements). A
   declared loop (`UnforceJobs::element`) reads through held roots, and
   forces again what a switch reverted. A loop that names no container and
   declares nothing may read anything the job allocated: no switch.

   A level is told by its frame, its loop's kind (that of the loop whose
   element opened the job) and its call depth: one step past its parent's
   for printers and `getDerivations`, equal for declared loops. A printer
   that an element's evaluation reaches (`builtins.toJSON`) is at least three
   steps past the innermost level (the element's own, a call, its own), so
   it is no level; a primop calling a printer without a call would be. A
   level whose loop returned is dropped when an element starts in a frame
   above it, at its depth or less (a sibling's, for loops that step the
   depth), or when a job ends above it. A job's end settles the levels open:
   what they hold then lies below every later job's memory, so later ends
   skip them.
   --------------------------------------------------------------------------- */

struct UnforceNest : UnforceHolder
{
    struct Level
    {
        const char * frame;
        const Value * container;
        size_t depth;
        bool settled = false;
    };

    /* Settled levels first. */
    std::vector<Level> levels;
    /* The loop whose element opened the job. */
    UnforceLoop base = UnforceLoop::other;

    void held(Hold & hold) override;

    void ended() override
    {
        for (auto l = levels.rbegin(); l != levels.rend() && !l->settled; ++l)
            l->settled = true;
    }

    /* The levels whose frames lie below `frame` have returned. */
    void prune(const char * frame)
    {
        while (!levels.empty() && levels.back().frame < frame)
            levels.pop_back();
    }

    void open(EvalState & state, const char * frame, UnforceLoop loop)
    {
        base = loop;
        levels.clear();
        levels.push_back({frame, nullptr, state.callDepth});
    }

    void start(EvalState & state, const char * frame, const Value * container, UnforceLoop loop);

    bool trap(EvalState & state);
};

void UnforceNest::held(Hold & hold)
{
    for (auto l = levels.rbegin(); l != levels.rend() && !l->settled; ++l)
        if (auto v = l->container) {
            hold(v, Kind::value);
            if (v->type() == nAttrs)
                hold(v->attrs(), Kind::bindings);
            else if (v->internalType() == tListN)
                hold(v->rawList().elems, Kind::value);
            else if (v->internalType() == tListSmall)
                for (auto e : v->rawSmallList())
                    if (e)
                        hold(e, Kind::value);
        }
}

/* A holder is the thread's that makes it. */
static thread_local UnforceNest nest;

/* An element of the level of `loop` at `frame`, which iterates `container`,
   starts in a job. */
void UnforceNest::start(EvalState & state, const char * frame, const Value * container, UnforceLoop loop)
{
    if (loop != base)
        return;
    size_t step = loop != UnforceLoop::nests;
    prune(frame);
    /* Another level's frame, at an address that a returned one had. */
    if (!levels.empty() && levels.back().frame == frame
        && (levels.back().container != container || levels.back().depth != state.callDepth))
        levels.pop_back();
    if (step)
        while (levels.size() > 1 && levels.back().frame != frame && levels.back().depth >= state.callDepth)
            levels.pop_back();
    if (levels.empty())
        return;
    if (levels.back().frame != frame) {
        /* The level of a function or a functor iterates what a call
           returned, which nothing holds. */
        if (state.callDepth != levels.back().depth + step
            || (container
                && (container->type() == nAttrs ? container->attrs()->get(state.s.functor) != nullptr
                                                : container->type() != nList)))
            return;
        levels.push_back({frame, container, state.callDepth});
    }
    /* Only the levels' loops run (no force, step or frame is open), by the
       level rule above: a level it mistook would switch nothing. */
    if (jobOwner.load() != &state || !opens.empty() || !stepFrames.empty() || unforceFrame.s || evalRegion.paused
        || keepWhole || (evalRegion.cur == evalRegion.base && state.undoLog.empty()))
        return;
    /* A slot logged in a frame between the job's opener and the outermost
       level's, of a level still running or of a frame that returned and
       whose address another reuses, which the end can tell apart from
       neither: the job is kept whole. */
    if (std::ranges::any_of(state.undoLog, [&](auto & e) {
            return (const char *) e.target >= jobStackMark && (const char *) e.target < levels.front().frame;
        }))
        keepWhole = true;
    state.endUnforceScope(frame);
    try {
        state.beginUnforceScope(frame);
    } catch (...) {
        /* No job is open, for the traversal that opened the one that ended
           to end. */
        for (auto & t : traversals)
            t.opener = nullptr;
        levels.clear();
        throw;
    }
}

/* The open job continues past a boundary: trap the cells logged since the
   last one (see "Elements"). False, having trapped nothing, if a force, a
   step or a frame is open, the region is paused, native code may run (it
   could read a trap unforced), a cell to trap is a slot on the stack (a copy
   of its trap could outlive the frame), a holder other than a seen set holds
   the job's memory, or memory runs out: the job ends here then. */
bool UnforceNest::trap(EvalState & state)
{
    auto & log = state.undoLog;
    auto & stack = threadStack();
    auto onStack = [&](auto & e) {
        auto p = (const char *) e.target;
        return p >= stack.low && p < stack.low + stack.size;
    };
    /* Seen sets are exempt (see "Elements"). `DerivationsSeen::found` is read
       through (its derivations' attrs, young memory of earlier elements), but
       only by `getDerivations`' callers once the traversal's jobs have ended:
       a read of them across elements would need to be logged as a read. */
    auto holds = [](auto h) { return !dynamic_cast<UnforceSeen *>(h) && evalRegion.contains(h->youngTop); };
    if (!opens.empty() || !stepFrames.empty() || unforceFrame.s || evalRegion.paused || unforceNativeCode(state)
        || std::any_of(log.begin() + trapFrom, log.end(), onStack) || std::ranges::any_of(*jobHolders, holds))
        return false;
    try {
        traps.reserve(traps.size() + log.size() - trapFrom);
        roomFor(joinClocks, 1);
    } catch (std::bad_alloc &) {
        return false;
    }
    /* An emplace may still allocate (erasures lower a flat map's load
       limit): a throw leaves every trapped cell's value in `traps`, which the
       job's end, as the throw unwinds the traversal, puts back. */
    for (auto i = log.begin() + trapFrom; i != log.end(); ++i)
        if (auto c = i->target; !c->isThunk() && !c->isApp() && !c->isAttrApp() && !c->isFailed() && !youngFree(*c)) {
            traps.emplace(c, *c);
            c->mkThunk(reinterpret_cast<Env *>(c), &trapExpr);
        }
    trapFrom = log.size();
    joinClocks.push_back(forceClock);
    unforceAnyReverted = true;
    return true;
}

void unforceNestedElementStart(EvalState & state, const Value * v, UnforceLoop loop)
{
    nest.start(state, (const char *) __builtin_dwarf_cfa(), v, loop);
}

void unforceJobsBegin(EvalState & state)
{
    traversals.push_back({state, unforceEnabled(state)});
}

void unforceJobsEnd()
{
    auto t = traversals.back();
    traversals.pop_back();
    if (t.opener) {
        nest.levels.clear();
        t.state.endUnforceScope(__builtin_dwarf_cfa());
    }
}

void unforceElementStart(UnforceLoop loop)
{
    auto & t = traversals.back();
    /* The level's stack pointer, below which its element's frames lie. */
    auto level = (const char *) __builtin_dwarf_cfa();
    if (!t.unforce || (t.opener && t.opener != level))
        return;
    if (t.opener) {
        /* The next element runs in the open job too, unless memory in use
           (what ended jobs keep, and the open job's top) rose past the most
           at any earlier boundary, or the open job holds more than any one
           element took (so it holds at most what the largest element took,
           and the element that crossed it): the job's end then frees what
           its elements left for later ones to reuse. Such a job is decided
           at its end as any job. A job kept whole, or one a nested level's
           switch opened, ends here. */
        auto top = std::max(jobTop, evalRegion.cur);
        auto inUse = keptMemory + uint64_t(top - evalRegion.base);
        bool rose = inUse > highWater || uint64_t(top - evalRegion.base) > mostOne;
        highWater = std::max(highWater, inUse);
        mostOne = std::max(mostOne, uint64_t(top - elementStart));
        if (!rose && !keepWhole && jobStackMark == level && nest.trap(t.state)) {
            elementStart = evalRegion.cur;
            nest.open(t.state, level, loop);
            return;
        }
        t.opener = nullptr;
        t.state.endUnforceScope(level);
    }
    /* In a job, of which the traversal is part: a nested level of the
       traversal whose element opened it, if its loop declared it nests. */
    if (jobOwner.load()) {
        if (loop == UnforceLoop::nests)
            nest.start(t.state, level, nullptr, loop);
        return;
    }
    t.state.beginUnforceScope(level);
    t.opener = level;
    nest.open(t.state, level, loop);
}

void unforceKeepJob()
{
    if (jobOwner.load())
        keepWhole = true;
}

void EvalState::beginUnforceScope(const void * stackMark)
{
    if (NIX_USE_BOEHMGC || isolationDepth++)
        return;
    const EvalState * none = nullptr;
    if (!jobOwner.compare_exchange_strong(none, this))
        panic("unforce: a job was opened while another EvalState's job is open");
    /* The canonical frame address is the caller's stack pointer at the call:
       frames the caller enters from now on lie below it. */
    jobStackMark = (const char *) (stackMark ? stackMark : __builtin_dwarf_cfa());
    /* On a stack other than the thread's (a coroutine's), or one that is
       not known, the frames entered during the job cannot be told apart. */
    auto & stack = threadStack();
    jobStackLow = stack.low;
    jobStackHigh = stack.low + stack.size;
    jobHolders = &holders;
    keepWhole = stepsUnsafe = !(jobStackMark > stack.low && size_t(jobStackMark - stack.low) <= stack.size);
    /* Opened in a boolean frame (never, by the level rule): searches for the
       frames a write marks would mix two jobs' starts. */
    stepsUnsafe = stepsUnsafe || unforceFrame.s;
    try {
        openRegion();
        openSeg = elementStart = evalRegion.base;
    } catch (...) {
        /* Memory for the job could not be mapped: no job is open. */
        if (evalRegion.base)
            closeRegion(evalRegion.base);
        keepWhole = stepsUnsafe = false;
        isolationDepth = 0;
        jobOwner = nullptr;
        throw;
    }
}

void EvalState::endUnforceScope(const void * ender)
{
    if (NIX_USE_BOEHMGC || --isolationDepth)
        return;
    endUnforceJob((const char *) (ender ? ender : __builtin_dwarf_cfa()));
}

void EvalState::endUnforceJob(const char * ender)
{
    /* A job ends between a traversal's elements, or as an exception leaves
       the traversal: no force, loop step or boolean frame of it is open,
       whose end would write what this end reverts or frees. */
    assert(opens.empty() && stepFrames.empty() && !unforceFrame.s);
    /* Trapped values first: the end decides from values (see "Elements"). */
    for (auto & [cell, v] : traps)
        *const_cast<Value *>(cell) = v;
    traps.clear();
    nest.prune(ender);
    /* A job during which native code came to be able to run is kept whole,
       and so is one that logged a slot of a frame that has returned since
       (between the job's opener and `ender`), which must be neither read
       nor written, and one that could not record a young failure. */
    bool keep = keepWhole || youngFailuresLost || !evalRegion.base || unforceNativeCode(*this)
                || std::any_of(undoLog.begin(), undoLog.end(), [&](auto & e) {
                       return (const char *) e.target >= jobStackMark && (const char *) e.target < ender;
                   });
    /* No walk tells which of the failures the job made outlive it: one whose
       recovery value (an interrupted, a recoverable or a max-call-depth
       failure has one) is young keeps the job whole. */
    auto youngRecovery = [](const Value * v) {
        return v->isFailed() && evalRegion.contains(v->failed().recoveryValue);
    };
    keep = keep || std::ranges::any_of(undoLog, [&](auto & e) { return youngRecovery(e.target); })
           || std::ranges::any_of(youngFailures, youngRecovery);
    /* Each entry's cell is reverted (`target` stays) to what its entry
       says, decided from the cell's value at the job's end before any is
       written, or keeps its value (no `target`). Everything that may fail to
       allocate is done before anything is written, the seen sets' records
       included (the entries the end forgets go with the reverts), but for a
       string kept by copy (`copyStringOut`), whose cell then holds the same
       content outside the region: harmless if the job is kept whole after
       all. */
    size_t promoted = 0, given = 0;
    /* The top of what the job keeps. */
    char * keptTo = nullptr;
    std::vector<SeenEntry> forget;
    std::vector<Value::Failed *> dead;
    if (!keep) {
        PauseRegion pause;
        try {
            Targets targets(undoLog);
            std::vector<bool> promote(targets.cells.size());
            keptTo = keepPrefix(*this, undoLog, targets, promote, settings.unforceAlwaysRevert, forget);
            /* Null: a holder holds what the driver allocated in the job. */
            if (!keptTo)
                keep = true;
            else {
                /* The seen sets' entries whose cells the job reverts, each
                   with its cell's record. */
                std::erase_if(forget, [&](auto & e) { return promote[e.target]; });
                for (auto & e : forget) {
                    std::set<const Value *> owner{targets.cells[e.target]};
                    e.owner = owner.extract(owner.begin());
                }
                /* The failures of the young cells the end frees (above the
                   cut, or in a run `releaseDead` gives back) go with them, as
                   no copy of a failed cell's value outlives the cell (a read
                   rethrows its error): their records, and the error with its
                   last one. A cell may have failed more than once. */
                for (auto x : youngFailures)
                    if (x->isFailed()) {
                        auto p = (const char *) x;
                        if (p >= keptTo || (ownerOf(p) != noOwner && !promote[targets.ofEntry[ownerOf(p)]]))
                            dead.push_back(&x->failed());
                    }
                std::ranges::sort(dead);
                dead.erase(std::ranges::unique(dead).begin(), dead.end());
                for (size_t i = 0; i < undoLog.size(); ++i) {
                    auto & e = undoLog[i];
                    if (youngFree(*e.target) || promote[targets.ofEntry[i]] || isRead(e))
                        e.target = nullptr;
                    else
                        e.preImage = reforcePreImage(*this, e.preImage, e.pos, *e.target);
                }
                promoted = std::ranges::count(promote, true);
                /* Last: the reads of young objects above are done, and nothing
                   after it keeps the job whole. */
                given = releaseDead(targets, promote, keptTo);
            }
        } catch (std::bad_alloc &) {
            keep = true;
        }
    }
    unforceStats.jobs++;
    unforceStats.jobsKeptWhole += keep;
    if (!keep) {
        /* Newest first, so that a cell logged more than once ends with its
           oldest entry's: its value before the job, and the position of its
           outermost force, which completed last. */
        for (auto i = undoLog.rbegin(); i != undoLog.rend(); ++i)
            if (i->target) {
                *i->target = i->preImage;
                unforceStats.cellsReverted++;
            }
        unforceStats.cellsPromoted += promoted;
        for (auto & e : forget)
            e.set->forget(e.p, std::move(e.owner));
        for (auto f : dead) {
            delete f->exRef;
            delete f;
        }
    }
    for (auto h : holders)
        h->ended();
    undoLog.clear();
    undoLog.shrink_to_fit();
    decltype(ends)().swap(ends);
    decltype(segments)().swap(segments);
    decltype(opens)().swap(opens);
    unforceTopOpen = nullptr;
    decltype(youngFailures)().swap(youngFailures);
    joinClocks.clear();
    crossForces.clear();
    trapFrom = 0;
    youngFailuresLost = false;
    curOwner = noOwner;
    if (evalRegion.base) {
        auto top = keep ? evalRegion.cur : keptTo;
        unforceStats.bytesKept += top - evalRegion.base;
        unforceStats.bytesFreed += evalRegion.cur - top;
        keptMemory += top - evalRegion.base - given;
        closeRegion(top);
    }
    keepWhole = stepsUnsafe = false;
    jobOwner = nullptr;
}

void EvalState::logPreImageSlow(Value & v, PosIdx pos)
{
    if (!stepFrames.empty()) [[unlikely]]
        noteStepWrite(v);
    auto p = (const char *) &v;
    /* A young cell is freed with the region, or kept with it, and is its
       owner's while it is forced. */
    if (evalRegion.contains(p)) {
        if (p >= openSeg)
            return;
        auto o = ownerOf(p);
        if (o == curOwner) {
            ownerEvents++;
            return;
        }
        roomFor(segments, opens.size() + 2);
        roomFor(opens, 1);
        roomFor(crossForces, opens.size() + 1);
        return openForce(v, noOwner, o);
    }
    if (isJobStackSlot(p)) {
        ownerEvents++;
        return;
    }
    /* A log as long as owner names allow keeps the job whole, and no step or
       frame of it frees: the cells it forces from then on are not logged,
       and are forced under the current owner. */
    if (undoLog.size() >= unforceTestLogLimit) [[unlikely]] {
        keepWhole = stepsUnsafe = true;
        return;
    }
    roomFor(undoLog, 1);
    roomFor(ends, 1);
    roomFor(segments, opens.size() + 2);
    roomFor(opens, 1);
    auto entry = uint32_t(undoLog.size());
    if (unforceFrame.s) [[unlikely]]
        frameWrite(v, true);
    undoLog.push_back({&v, pos, false, v});
    ends.emplace_back();
    openForce(v, entry, entry);
}

/* Only tail calls: the end of a force that changed no owner needs no stack
   frame. */
void EvalState::endLoggedForceSlow(Value & v, bool completed)
{
    if (!opens.empty() && opens.back().cell == &v)
        return endOwnerForce(v, completed);
    if (!completed || unforceFrame.s) [[unlikely]]
        return frameForceEnd(v, completed);
}

/* Whether a holder holds memory in [at, cur): the drops in `endOwnerForce`
   rest on none keeping a pointer into a force's memory past its end. */
[[maybe_unused]] static bool heldFrom(const char * at)
{
    return std::ranges::any_of(*jobHolders, [&](auto h) {
        auto p = (const char *) h->youngTop;
        return !std::less<>()(p, at) && std::less<>()(p, evalRegion.cur);
    });
}

void EvalState::endOwnerForce(Value & v, bool completed)
{
    auto f = opens.back();
    opens.pop_back();
    unforceTopOpen = opens.empty() ? nullptr : opens.back().cell;
    /* A logged force that completed with a young-free value drops its
       entry, if it is still the last one and no closed segment has its
       label: the job's end would keep the cell as it is anyway, so the
       log and the timeline do not grow with such forces, however many a
       job makes. What the force allocated in its segment is garbage (its
       value reaches none of it, and a cell older than the force that
       reaches it would have a later entry), and becomes the restored
       owner's. */
    if (f.entry != noOwner && completed && f.entry + 1 == undoLog.size() && segments.size() == f.segs && youngFree(v)) {
        assert(!heldFrom(f.at));
        undoLog.pop_back();
        ends.pop_back();
        curOwner = f.saved;
        if (!segments.empty() && segments.back().owner == f.saved) {
            openSeg = segStart(segments.back());
            segments.pop_back();
        }
    } else if (
        f.entry == noOwner && completed && f.events == ownerEvents && segments.size() == f.segs && openSeg == f.at
        && (curOwner == noOwner || joinClocks.empty() || ends[curOwner].own > joinClocks.back()) && youngFree(v)) {
        /* The same for a young force (of a cell of another owner's) that
           wrote nothing outside the segment it opened at its start but its
           cell, with a young-free value: what it allocated is garbage too.
           Its owner's ends stay: what its forces left is what was before. */
        assert(!heldFrom(f.at));
        curOwner = f.saved;
        if (!segments.empty() && segments.back().owner == f.saved) {
            openSeg = segStart(segments.back());
            segments.pop_back();
        }
    } else {
        /* The clock of these ends, and what each target owned last. */
        auto t = ++forceClock;
        if (curOwner != noOwner) {
            auto & r = ends[curOwner];
            if (!joinClocks.empty() && r.own <= joinClocks.back())
                crossForces.push_back({curOwner, t});
            else
                r.last = t;
            r.at = std::max(r.at, evalRegion.cur);
        }
        if (f.entry != noOwner)
            ends[f.entry].own = t;
        setOwner(f.saved);
    }
    frameForceEnd(v, completed);
}

} // namespace nix
