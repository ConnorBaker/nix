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
   lowering the bump cursor: nothing is marked, walked or moved. A job in
   which an effect ran outside any cell's force is kept whole instead:
   nothing is reverted, and its memory stays. */

#include "nix/expr/eval.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/primops.hh"
#include "nix/util/config.hh"
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

/* A job's room: 64 GiB of address space, 2^32 of the bump allocator's
   16-byte granules (so that a 32-bit offset addresses one), of which only
   the pages it touches are memory. None on 32-bit systems. */
constexpr uint64_t regionWanted = uint64_t(1) << 36;
constexpr size_t regionBytes = SIZE_MAX / 2 < regionWanted ? 0 : size_t(regionWanted);

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

/* The highest the open job's region reached: a loop's step lowers `cur`
   when it frees its memory (see "Steps"). */
static char * jobTop = nullptr;

#if defined(__linux__)

/* Jobs share a mapping of two regions: [mapping, keptTop) holds what kept
   jobs left, ordinary memory from then on, and the next job's region starts
   at `keptTop` while the mapping has a region's room above it. */
static char * mapping = nullptr;
static char * keptTop = nullptr;

/* The end of the open job's room, and the top of the mapping's memory that
   jobs wrote: above it, the system's zero-filled pages. The evaluator
   expects allocations zeroed, so what lies below it is zeroed as the bump
   cursor reaches it (`EvalRegion::more`), a page at a time. */
static char * roomEnd = nullptr;
static char * dirtyTop = nullptr;

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

/* Open the job's region. Only Linux on a 64-bit system has one: elsewhere,
   the job allocates as outside any job, and its end reverts nothing. Failing
   to map one is running out of memory (`std::bad_alloc`): the job does not
   run without one. Not an `Error`, which traversals take for an element's
   failure and print or skip. */
static void openRegion()
{
#if defined(__linux__)
    auto mappingBytes = 2 * regionBytes;
    if (!regionBytes)
        return;
    if (!mapping || unforceTestFailMapping || size_t(mapping + mappingBytes - keptTop) < regionBytes) {
        auto p = mapAnonymous(mappingBytes);
        if (p == MAP_FAILED)
            throw std::bad_alloc();
        /* The old mapping keeps the pages of what its jobs kept. */
        if (mapping)
            munmap(pageUp(keptTop), mapping + mappingBytes - pageUp(keptTop));
        mapping = keptTop = dirtyTop = (char *) p;
    }
    evalRegion.base = evalRegion.cur = jobTop = keptTop;
    roomEnd = keptTop + regionBytes;
    evalRegion.end = dirtyTop > keptTop ? keptTop : roomEnd;
#endif
}

void * EvalRegion::more(size_t n)
{
#if defined(__linux__)
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
        end = to < dirtyTop ? to : roomEnd;
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
   `ExprReforce`, and is evaluated again by whatever forces it next. Evaluation
   is deterministic, so the re-force repeats work that the evaluation without
   unforcing did. It runs at the call depth and on the native stack of the
   force that finds it reverted, as do the cells it builds again, which
   consumers forced before the target was reverted: a chain forced front to
   back without unforcing may be forced back to front. So recomputation may
   fail on `max-call-depth` where the evaluation without unforcing does not,
   instead of giving its value or its error, or overflow the native stack,
   which ends the process; the exactness contract allows both.

   An old target whose force failed keeps its failure (see Promotion). A
   failed young cell inside a reverted owner is built again, and throws the
   same error again when re-forced (or fails on `max-call-depth` first, as
   above). A failure on `max-call-depth` depends on the depth of the force
   that met it, so one met in a job has a recovery value, as an interrupted
   or recoverable one has: every later read retries it at its own depth,
   where it may succeed, also where the evaluation without unforcing fails,
   which the exactness contract allows. A failure that its job retried is
   reverted to the failure, which retries again where it is forced next.

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

/* Out of line: a class whose virtual functions are all inline has its
   vtable emitted in every translation unit (Clang's -Wweak-vtables). */
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
    if (v.type() == nAttrs)
        return v.attrs()->pos;
    if (v.isLambda())
        return v.lambda().fun->pos;
    return std::nullopt;
}

/* What a target that holds `now` at the job's end is reverted to: its
   pre-image, an application to be called again at its force's position. A
   pre-image wrapped already (reverted before), or that is neither a thunk
   nor an application, stays as it is. Allocates: outside the region, which
   the caller pauses. */
static Value reforcePreImage(EvalState & state, const Value & preImage, PosIdx pos, const Value & now)
{
    Value v = preImage;
    if (preImage.isThunk() && !preImage.isBlackhole()) {
        auto t = preImage.thunk();
        if (!unforceIsReverted(t.expr))
            v.mkThunk(t.env, internReforce({t.expr, noPos, ownPos(now)}));
    } else if (preImage.isApp()) {
        auto a = preImage.app();
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

/* Whether `p` is a slot of a stack frame entered during the open job, which
   will have returned when it ends: such a cell is never logged, as the end
   must not write it. */
static bool isJobStackSlot(const void * p)
{
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
    if (v.isThunk() || v.isApp() || v.internalType() == tUninitialized)
        return false;
    bool young = false;
    valueEdges(v, [&](const void * p) { young = young || evalRegion.contains(p); });
    return !young;
}

/* A failure that outlives its job may name one of the job's cells: an
   `InfiniteRecursionError` names the cell whose force found it blackholed,
   which `handleEvalExceptionForThunk` compares with the cells whose forces
   it unwinds, to mark where the recursion is entered. Freed and reused, the
   address could match a later job's cell. Without unforcing it matches no
   cell after the error's first propagation, which has ended with the job,
   so it is forgotten. */
static void forgetYoungCell(const Value::Failed & f)
{
    try {
        std::rethrow_exception(f.exRef->ex);
    } catch (InfiniteRecursionError & e) {
        if (evalRegion.contains(e.v))
            e.v = nullptr;
    } catch (...) {
    }
}

/* ---------------------------------------------------------------------------
   The owner timeline.

   The owner of a young object x is the first old cell on its creator chain
   (x, the cell being forced when x was allocated, the cell being forced
   when that one was allocated, ...): the target whose re-force is the only
   thing that can build x again, or none (the driver, outside any force). It
   is a function of allocation time: one force at a time is the current
   owner, and everything allocated then is its.
   - A logged force of an old cell t makes t the current owner until it
     ends.
   - A force of a young cell c makes c's owner the current one until it
     ends: what c's evaluation builds is built again only by re-forcing the
     target that rebuilds c.
   - Any other force (a stack slot, a cell of the current owner) keeps it.
   The bump cursor is the allocation clock, so owner changes cut the
   region's used part into segments of one owner each, in address order:
   the closed ones (`segments`) and the open one, [openSeg, cur), whose
   owner is `curOwner`. Adjacent segments never have the same owner. Owners
   are named by the log index of their force (`noOwner`: the driver).

   A force that changes the owner pushes itself on `opens`, and its end, in
   `forceValue` or its exception handler, pops itself and restores the
   owner. The hooks make room for every push first, so that a force's end
   never allocates.
   --------------------------------------------------------------------------- */

static constexpr uint32_t noOwner = UINT32_MAX;

struct OwnerSeg
{
    /* Its start, in granules from the region's base. */
    uint32_t off;
    uint32_t owner;
};

static std::vector<OwnerSeg> segments;
static char * openSeg = nullptr;
static uint32_t curOwner = noOwner;

/* A force that changed the owner: a logged one (`entry`, its log index) or
   a young one (`noOwner`). `saved` is the owner to restore at its end;
   `segs`, how many closed segments there were once its own opened. */
struct OpenForce
{
    const Value * cell;
    uint32_t entry;
    uint32_t saved;
    size_t segs;
};

static std::vector<OpenForce> opens;

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
        segments.push_back({uint32_t(granuleOf(openSeg)), curOwner});
        openSeg = evalRegion.cur;
    }
    curOwner = o;
}

/* The owner of the young object at `p`. */
static uint32_t ownerOf(const void * p)
{
    if ((const char *) p >= openSeg)
        return curOwner;
    auto i = std::upper_bound(
        segments.begin(), segments.end(), uint32_t(granuleOf(p)), [](uint32_t off, auto & s) { return off < s.off; });
    assert(i != segments.begin());
    return i[-1].owner;
}

/* Push a force that changes the owner to `o` (`entry`, for a logged one). */
static void openForce(const Value & v, uint32_t entry, uint32_t o)
{
    opens.push_back({&v, entry, curOwner, 0});
    setOwner(o);
    opens.back().segs = segments.size();
}

/* ---------------------------------------------------------------------------
   Promotion.

   Reverting a target t breaks the evaluator's pointer identity (`eqValues`
   short-cuts on `&v1 == &v2`; Nixpkgs relies on it) when a promoted value
   keeps a young cell that re-forcing t builds again: two cells then stand
   for one of the evaluation without unforcing. So the promoted set S is
   closed under ownership: every young identity-bearing object (a cell, a
   Bindings layer, an element array) that a promoted value reaches has its
   owner in S, or none. Then a reverted target, re-forced later, rebuilds
   only cells that nothing kept.

   S is decided from records alone. While t is being forced, the young
   cells it owns are visible only to its own evaluation and to the forces of
   those cells (no other cell's evaluation can reach them but through a
   cell's value, and t has none yet). So a value reaches t's young objects
   only once t's force has ended, and what the forces a promoted target owns
   (its own, and those of its young cells) leave reachable was allocated
   before the last of them ended, below the bump cursor then, and reaches
   only targets whose forces ended before then. Promoting those too, until
   none is added, closes S under ownership without visiting anything. The
   job keeps its memory below the highest cursor at the end of a force that
   S owns, and frees the rest.

   S starts from the nominated targets: those under whose force an effect
   ran (`keep`); those an earlier job reverted, which are evidently shared
   between jobs (reverting them again would only make each job evaluate
   them anew); those a holder keeps; and the owners of the young objects
   holders hold (see unforce.hh). A young object a holder holds that the
   driver owns keeps the job whole. A failed target is not nominated. Its
   failure (`Value::Failed` and its exception) is allocated outside the
   region, so one whose value holds no pointer into it (no young recovery
   value, `youngFree`) keeps its value, as the evaluation without unforcing
   does: later reads throw the first force's error again, with its frames.
   A failed young cell inside a reverted owner is still built again.
   --------------------------------------------------------------------------- */

/* A job's targets, each once, in the order of their first entries, and the
   target of each log entry. */
struct Targets
{
    std::vector<Value *> cells;
    std::vector<uint32_t> ofEntry;

    Targets(const std::vector<UnforceUndoEntry> & log)
    {
        boost::unordered_flat_map<const Value *, uint32_t> index;
        ofEntry.reserve(log.size());
        for (auto & e : log) {
            auto [i, added] = index.try_emplace(e.target, cells.size());
            if (added)
                cells.push_back(e.target);
            ofEntry.push_back(i->second);
        }
    }
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
        young.emplace_back(p, kind);
        youngTop = std::max(youngTop, (const void *) p, std::less<>());
    }
    return true;
}

void UnforceSeen::held(Hold & hold)
{
    for (auto & [p, kind] : young)
        hold(p, kind);
}

void UnforceSeen::ended()
{
    young.clear();
    youngTop = nullptr;
}

/* The young cells whose force failed in the open job, and whether one could
   not be recorded (then the job is kept whole). Not nominations: a job's
   end makes each failure it made forget the job's cells (`forgetYoungCell`),
   as no walk tells which failures outlive it. */
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
   one that the driver owns, keeping the job whole. */
struct OwnerHold : UnforceHolder::Hold
{
    const Targets & targets;
    std::vector<bool> & promote;
    bool driver = false;

    OwnerHold(const Targets & targets, std::vector<bool> & promote)
        : targets(targets)
        , promote(promote)
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
};

} // namespace

/* Which targets the job promotes (see above), and the top of the memory it
   keeps; null to keep it whole. No object is visited. */
static char *
keepPrefix(const std::vector<UnforceUndoEntry> & log, const Targets & targets, std::vector<bool> & promote)
{
    for (size_t i = 0; i < log.size(); ++i) {
        auto & p = log[i].preImage;
        if (log[i].keep || (p.isThunk() && unforceIsReverted(p.thunk().expr))
            || std::ranges::any_of(holders, [&](auto h) { return h->keeps(log[i].target); }))
            promote[targets.ofEntry[i]] = true;
    }
    OwnerHold hold{targets, promote};
    for (auto h : holders)
        h->held(hold);
    if (hold.driver)
        return nullptr;

    std::vector<EntryEnds> per(targets.cells.size(), EntryEnds{0, 0, nullptr});
    for (size_t i = 0; i < log.size(); ++i) {
        auto & e = per[targets.ofEntry[i]];
        e.own = std::max(e.own, ends[i].own);
        e.last = std::max(e.last, ends[i].last);
        e.at = std::max(e.at, ends[i].at);
    }
    uint64_t horizon = 0;
    char * top = evalRegion.base;
    auto add = [&](size_t t) {
        if (per[t].own == UINT64_MAX)
            top = evalRegion.cur;
        horizon = std::max({horizon, per[t].own == UINT64_MAX ? 0 : per[t].own, per[t].last});
        top = std::max(top, per[t].at);
    };
    for (size_t t = 0; t < promote.size(); ++t)
        if (promote[t])
            add(t);
    /* The time closure, in the order the targets' forces ended. */
    std::vector<uint32_t> order(per.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, {}, [&](auto t) { return per[t].own; });
    for (auto t : order) {
        if (per[t].own > horizon)
            break;
        if (!promote[t]) {
            promote[t] = true;
            add(t);
        }
    }
    /* Nothing live lies above the cursor. */
    return std::min(top, evalRegion.cur);
}

/* ---------------------------------------------------------------------------
   Steps.

   A loop's steps (`UnforceSteps`) free their memory inside a job, by
   records, without a walk or a move. A step's memory is what the bump
   cursor passed during it, [start, cur). Nothing allocated before the step
   points into it unless the step wrote it, and the step writes a cell
   outside its memory only by forcing it, or as the cell its value goes to.
   So once the step's call has returned, a pointer into its memory can only
   be in:
   - the step's value, in a cell outside its memory (`UnforceSteps::cell`);
   - a cell logged during the step (an old cell): its value or pre-image;
   - a young cell below `start`, or a slot of a frame outside the step (the
     loop's and its callers'), forced during the step (`stepWrites`);
   - a holder (`youngTop`), a young failure recorded during the step, or
     what native code holds.
   The step's end looks at the own pointers of each (`valueEdges`), not
   further: what they point to outside the step's memory was allocated
   before it, and points into it only if the step wrote it, which is a
   record above. A failed value is taken to point into it. If none does,
   the cursor goes back to `start`, the memory is zeroed, and the owner
   timeline is cut back to `start`.

   This rests on the step writing its own value's cell once, at its end, by
   copy: no evaluation keeps the address of the cell its result goes to
   (`callFunction` evaluates into a local and copies).

   The cell the next step writes lies outside that step's memory: a cell
   allocated before it starts, or the cell that an earlier step read
   (`spare`) once that step's memory was freed, or was empty at its end,
   and no record points to the cell, so that nothing reads it any more. A
   step's memory is empty at its end if it allocated nothing, or if inner
   steps freed all it allocated. Such a step goes through the same checks:
   none can find a pointer into its memory, but each looks for one to that
   cell, which its value may hold (`[ a ]` in an inner `foldl'`).
   --------------------------------------------------------------------------- */

namespace {

struct StepFrame
{
    char * start = nullptr;
    /* The loop's frame: slots at or above it are outside the steps. */
    const char * sp = nullptr;
    /* At the step's start: the sizes of `stepWrites`, the log, the young
       failures and `opens`; the owner; the job. */
    size_t writes = 0, log = 0, failures = 0, opens = 0;
    uint32_t owner = noOwner;
    uint64_t job = 0;
    /* The cell the step writes, the one the step before wrote (this step's
       argument), whether each is the loop's own (`cell` allocated it), and
       a cell of the loop's that nothing reads. */
    Value * cur = nullptr;
    Value * prev = nullptr;
    bool curOwn = false, prevOwn = false;
    Value * spare = nullptr;
};

} // namespace

/* Plain statics, like `segments` and `opens`: steps open only in a job
   (`begin`), and only its owner's thread forces in it (`logPreImageSlow` runs
   while `isolationDepth` is set, which only the job's owner has), so no other
   thread touches them; a thread-local here cost a `__tls_get_addr` call per
   access from libnixexpr.so, several per step and per force. */
static std::vector<StepFrame> stepFrames;
static std::vector<Value *> stepWrites;

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
    return true;
}

Value * UnforceSteps::cellSlow(Value * given)
{
    auto & f = stepFrames.back();
    if (f.job != unforceStats.jobs)
        f.spare = nullptr;
    auto c = given;
    if (!c) {
        if (f.spare) {
            c = f.spare;
            f.spare = nullptr;
        } else
            c = state.allocValue();
    }
    f.cur = c;
    f.curOwn = !given;
    /* The step starts here. */
    f.start = evalRegion.cur;
    f.writes = stepWrites.size();
    f.log = state.undoLog.size();
    f.failures = youngFailures.size();
    f.opens = opens.size();
    f.owner = curOwner;
    f.job = unforceStats.jobs;
    return c;
}

void UnforceSteps::stepSlow(Value * v)
{
    auto & f = stepFrames.back();
    auto top = evalRegion.cur;
    /* Whether the step's argument, the loop's own cell, may be written by
       a later step: nothing it leaves points to it. `escapes` clears it as
       it looks; it is read only when no check below stopped the step, so
       that every check has run in full. */
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
    /* An empty step (the cursor is back at its start: it allocated nothing,
       or inner steps freed all it allocated) goes through the same checks:
       only whether its argument's cell may be reused depends on them. */
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
        /* Free the step's memory. */
        jobTop = std::max(jobTop, top);
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
    }
    if (!kept && spare
        && std::ranges::none_of(*jobHolders, [&](auto h) { return !std::less<>()(h->youngTop, f.prev); }))
        f.spare = f.prev;
    f.prev = f.cur;
    f.prevOwn = f.curOwn;
    dropStepWrites(f);
}

void UnforceSteps::end()
{
    dropStepWrites(stepFrames.back());
    stepFrames.pop_back();
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

/* For the unit tests, which share their process with the C API's. */
void unforceTestResetNativeCode();

void unforceTestResetNativeCode()
{
    cApiCode = false;
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
    if (NIX_USE_BOEHMGC || state.debugRepl)
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
       owns, which only its re-force runs again. Outside any (the driver's),
       or in another evaluator than the job's: the job is kept whole. */
    auto job = jobOwner.load();
    if (!job)
        return;
    if (job == &state && curOwner != noOwner)
        state.undoLog[curOwner].keep = true;
    else
        keepWhole = true;
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
    const Value * savedRoot;
    const char * opener = nullptr;
};

static thread_local std::vector<Traversal> traversals;

void unforceJobsBegin(EvalState & state, const Value * root)
{
    auto unforce = unforceEnabled(state);
    traversals.push_back({state, unforce, state.unforceRoot});
    state.unforceRoot = unforce ? root : nullptr;
}

void unforceJobsEnd()
{
    auto t = traversals.back();
    traversals.pop_back();
    t.state.unforceRoot = t.savedRoot;
    if (t.opener)
        t.state.endUnforceScope(__builtin_dwarf_cfa());
}

void unforceElementStart()
{
    auto & t = traversals.back();
    /* The level's stack pointer, below which its element's frames lie. */
    auto level = (const char *) __builtin_dwarf_cfa();
    if (!t.unforce || (t.opener && t.opener != level))
        return;
    if (t.opener) {
        t.opener = nullptr;
        t.state.endUnforceScope(level);
    }
    /* In a job, of which the traversal is part. */
    if (jobOwner.load())
        return;
    t.state.beginUnforceScope(level);
    t.opener = level;
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
    jobHolders = &holders;
    keepWhole = stepsUnsafe = !(jobStackMark > stack.low && size_t(jobStackMark - stack.low) <= stack.size);
    try {
        openRegion();
        openSeg = evalRegion.base;
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
       allocate is done before anything is written. */
    size_t promoted = 0;
    /* The top of what the job keeps. */
    char * keptTo = nullptr;
    if (!keep) {
        PauseRegion pause;
        try {
            Targets targets(undoLog);
            std::vector<bool> promote(targets.cells.size());
            keptTo = keepPrefix(undoLog, targets, promote);
            /* Null: a holder holds what the driver allocated in the job. */
            if (!keptTo)
                keep = true;
            else {
                /* Each failure the job made forgets the job's cells. */
                for (auto & e : undoLog)
                    if (e.target->isFailed())
                        forgetYoungCell(e.target->failed());
                for (auto x : youngFailures)
                    if (x->isFailed())
                        forgetYoungCell(x->failed());
                for (size_t i = 0; i < undoLog.size(); ++i) {
                    auto & e = undoLog[i];
                    if (youngFree(*e.target) || promote[targets.ofEntry[i]])
                        e.target = nullptr;
                    else
                        e.preImage = reforcePreImage(*this, e.preImage, e.pos, *e.target);
                }
                promoted = std::ranges::count(promote, true);
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
    }
    for (auto h : holders)
        h->ended();
    undoLog.clear();
    undoLog.shrink_to_fit();
    decltype(ends)().swap(ends);
    decltype(segments)().swap(segments);
    decltype(opens)().swap(opens);
    decltype(youngFailures)().swap(youngFailures);
    youngFailuresLost = false;
    curOwner = noOwner;
    if (evalRegion.base) {
        auto top = keep ? evalRegion.cur : keptTo;
        unforceStats.bytesKept += top - evalRegion.base;
        unforceStats.bytesFreed += evalRegion.cur - top;
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
        if (o == curOwner)
            return;
        roomFor(segments, opens.size() + 2);
        roomFor(opens, 1);
        return openForce(v, noOwner, o);
    }
    if (isJobStackSlot(p))
        return;
    assert(undoLog.size() < noOwner);
    roomFor(undoLog, 1);
    roomFor(ends, 1);
    roomFor(segments, opens.size() + 2);
    roomFor(opens, 1);
    auto entry = uint32_t(undoLog.size());
    undoLog.push_back({&v, pos, false, v});
    ends.emplace_back();
    openForce(v, entry, entry);
}

void EvalState::endLoggedForceSlow(Value & v, bool completed)
{
    if (!opens.empty() && opens.back().cell == &v) {
        auto f = opens.back();
        opens.pop_back();
        /* A logged force that completed with a young-free value drops its
           entry, if it is still the last one and no closed segment has its
           label: the job's end would keep the cell as it is anyway, so the
           log and the timeline do not grow with such forces, however many a
           job makes. What the force allocated in its segment is garbage (its
           value reaches none of it, and a cell older than the force that
           reaches it would have a later entry), and becomes the restored
           owner's. */
        if (f.entry != noOwner && completed && f.entry + 1 == undoLog.size() && segments.size() == f.segs
            && youngFree(v)) {
            undoLog.pop_back();
            ends.pop_back();
            curOwner = f.saved;
            if (!segments.empty() && segments.back().owner == f.saved) {
                openSeg = segStart(segments.back());
                segments.pop_back();
            }
        } else {
            /* The clock of these ends, and what each target owned last. */
            auto t = ++forceClock;
            if (curOwner != noOwner) {
                ends[curOwner].last = t;
                ends[curOwner].at = std::max(ends[curOwner].at, evalRegion.cur);
            }
            if (f.entry != noOwner)
                ends[f.entry].own = t;
            setOwner(f.saved);
        }
    }
}

} // namespace nix
