/* Unforcing thunks between jobs.

   Thunk update in `forceValue` is the evaluator's only mutation of existing
   cells. While a job (an outermost unforce scope) runs, the evaluator
   allocates in a region of its own (`evalRegion`), and the pre-image of
   every *old* cell (one outside the region) that the job forces is logged.
   At the job's end each logged cell is reverted to its pre-image, or
   promoted: it keeps its value, and with it what the value reaches in the
   region (see "Promotion" below). A cell whose value holds no pointer into
   the region (`youngFree`) keeps it as it is. When nothing is promoted, the
   region is freed: no old cell points into it any more. A job in which an
   effect ran is kept whole instead: nothing is reverted, and its memory
   stays. */

#include "nix/expr/eval.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/attr-set.hh"
#include "nix/util/config.hh"
#include "nix/util/signals.hh"
#include "nix/util/std-hash.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>

#include <boost/context/fiber.hpp>

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

#if defined(__linux__)

static size_t pageSize()
{
    static const size_t page = sysconf(_SC_PAGESIZE);
    return page;
}

static char * pageUp(char * p)
{
    return (char *) (((uintptr_t) p + pageSize() - 1) & ~(uintptr_t) (pageSize() - 1));
}

/* Jobs share a mapping of two regions: [mapping, keptTop) holds what kept
   jobs left, ordinary memory from then on, and the next job's region starts
   at `keptTop` while the mapping has a region's room above it. */
static char * mapping = nullptr;
static char * keptTop = nullptr;

/* Zero [from, to): in place in the first page, and above it by returning
   whole pages to the system, which zero-fills private anonymous memory when
   it is touched again. */
static void zeroMemory(char * from, char * to)
{
    if (to <= from)
        return;
    auto up = std::min(pageUp(from), to);
    memset(from, 0, up - from);
    if (to > up && madvise(up, pageUp(to) - up, MADV_DONTNEED) != 0)
        memset(up, 0, to - up);
}

#endif

/* The sizes of the Envs in the open job's region, which nothing else
   records: a byte per granule, where an Env starts, or 255 for one of 255
   values or more, whose size is in `bigEnvSizes`. Only the pages that a
   job's Envs touch are memory. */
static uint8_t * envSizes = nullptr;
static boost::unordered_flat_map<const void *, size_t> bigEnvSizes;

static size_t granuleOf(const void * p)
{
    return size_t((const char *) p - evalRegion.base) >> 4;
}

static size_t envSizeOf(const Env * e)
{
    auto s = envSizes[granuleOf(e)];
    return s < 255 ? s : bigEnvSizes.at(e);
}

/* Open the job's region, where the system has room for one (only Linux is
   supported): without one, the job allocates as outside any job, and its
   end reverts nothing. */
static void openRegion()
{
#if defined(__linux__)
    auto mappingBytes = 2 * regionBytes;
    if (!envSizes) {
        if (!regionBytes)
            return;
        auto p =
            mmap(nullptr, regionBytes >> 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            return;
        envSizes = (uint8_t *) p;
    }
    if (!mapping || size_t(mapping + mappingBytes - keptTop) < regionBytes) {
        auto p =
            mmap(nullptr, mappingBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            return;
        /* The old mapping keeps the pages of what its jobs kept. */
        if (mapping)
            munmap(pageUp(keptTop), mapping + mappingBytes - pageUp(keptTop));
        mapping = keptTop = (char *) p;
    }
    evalRegion.base = evalRegion.cur = keptTop;
    evalRegion.end = keptTop + regionBytes;
#endif
}

/* End the job's region. `keep`: its memory stays, for the rest of the
   process; otherwise it is freed, and zeroed for the next job, since the
   evaluator expects allocations zeroed. Its Env sizes are not needed any
   more. */
static void closeRegion(bool keep)
{
#if defined(__linux__)
    auto base = evalRegion.base, cur = evalRegion.cur;
    zeroMemory((char *) envSizes, (char *) envSizes + granuleOf(cur));
    decltype(bigEnvSizes)().swap(bigEnvSizes);
    if (keep)
        keptTop = cur;
    else
        zeroMemory(base, cur);
#endif
    auto paused = evalRegion.paused;
    evalRegion = EvalRegion{};
    evalRegion.paused = paused;
}

/* ---------------------------------------------------------------------------
   Re-forcing at the first force's call depth.

   Without unforcing, a thunk is evaluated once, at the call depth of its
   first force. A target reverted at a job's end is evaluated again by
   whatever forces it next, which may be deeper, and could then exceed
   `max-call-depth` where the evaluation without unforcing does not. So a
   reverted target holds its pre-image wrapped in an `ExprReforce` that
   carries the call depth of its first force: forcing it evaluates at that
   depth, and restores the depth it found afterwards.

   That is exact. Evaluation is deterministic, so a re-force only repeats
   work that the evaluation without unforcing did, and the rest of the work
   is done in the same order, by the same chains of calls, at the same
   depths, as every re-force restores the depth it found (on return and on
   throw). A re-force runs at the depth where its first force completed (a
   job that leaves a target failed, or retries a failure, is kept whole), and
   the re-forces nested in it are rebased in turn: it fails nowhere that the
   evaluation without unforcing did not.

   An application's pre-image becomes a thunk whose environment is the
   function and its argument, called at its first force's position (a primop
   may put its call's position in its value). The wrapper also keeps the
   position of the value the cell held (`unforceRevertedPos`), and the native
   stack room its first force had (see "Re-forcing on the native stack").
   --------------------------------------------------------------------------- */

/** `EvalState`'s private call depth, for re-forces. */
struct UnforceDepth
{
    /** Sets the call depth until destroyed. */
    struct Rebase
    {
        EvalState & state;
        size_t saved;

        Rebase(EvalState & state, uint32_t depth)
            : state(state)
            , saved(std::exchange(state.callDepth, depth))
        {
        }

        ~Rebase()
        {
            state.callDepth = saved;
        }
    };
};

struct ReforceKey
{
    /** The pre-image's expression, or null for an application. */
    Expr * expr;
    uint32_t depth;
    /** An application's call position. */
    PosIdx pos;
    /** The position of the value the cells held when they were reverted, if it has one. */
    std::optional<PosIdx> valuePos;

    bool operator==(const ReforceKey &) const = default;
};

/** A reverted cell's pre-image (see above): one per distinct key, never freed. */
struct ExprReforce : Expr, ReforceKey
{
    /** The most native stack room any of the cells had when its first force
        started, in `stackUnit`s (0: unknown; see `stackRoom`). */
    uint32_t room = 0;

    ExprReforce(const ReforceKey & key)
        : ReforceKey(key)
    {
    }

    /** Re-force: on a fresh stack if this one has less room left than `room`. */
    void eval(EvalState & state, Env & env, Value & v) override;

    /** The evaluation itself, at `depth`. */
    void reforce(EvalState & state, Env & env, Value & v)
    {
        UnforceDepth::Rebase rebase(state, depth);
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

struct ReforceHash
{
    using is_transparent = void;

    size_t operator()(const ReforceKey & k) const
    {
        size_t h = 0;
        hash_combine(h, k.expr, k.depth, k.pos, k.valuePos);
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

static ExprReforce * internReforce(const ReforceKey & key, uint32_t room)
{
    auto i = reforces.find(key);
    auto r = i != reforces.end() ? *i : *reforces.insert(new ExprReforce(key)).first;
    unforceAnyReverted = true;
    /* Shared by cells whose first forces had different room left: the most
       of it, so that each has at least its own. */
    r->room = std::max(r->room, room);
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
   pre-image, to be re-forced at its force's depth, position and stack room.
   A pre-image wrapped already (reverted before), or that is neither a thunk
   nor an application, stays as it is. Allocates: outside the region, which
   the caller pauses. */
static Value
reforcePreImage(EvalState & state, const Value & preImage, uint32_t depth, PosIdx pos, uint32_t room, const Value & now)
{
    Value v = preImage;
    if (preImage.isThunk() && !preImage.isBlackhole()) {
        auto t = preImage.thunk();
        if (!unforceIsReverted(t.expr))
            v.mkThunk(t.env, internReforce({t.expr, depth, noPos, ownPos(now)}, room));
    } else if (preImage.isApp()) {
        auto a = preImage.app();
        auto operands = (Value **) state.mem.allocBytes(2 * sizeof(Value *));
        operands[0] = a.left;
        operands[1] = a.right;
        v.mkThunk((Env *) operands, internReforce({nullptr, depth, pos, ownPos(now)}, room));
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

void * evalCallocEnv(size_t size)
{
    size_t n;
    if (__builtin_mul_overflow(size, sizeof(Value *), &n) || __builtin_add_overflow(n, sizeof(Env), &n))
        throw std::bad_alloc();
    if (!evalRegion.active()) [[likely]] {
        auto p = calloc(n, 1);
        if (!p)
            throw std::bad_alloc();
        return p;
    }
    auto p = evalRegion.bump(n);
    if (size >= 255)
        bigEnvSizes.emplace(p, size);
    envSizes[granuleOf(p)] = std::min<size_t>(size, 255);
    return p;
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

/* This thread's stack: its lowest address and size, or null if unknown. */
static thread_local const std::pair<const char *, size_t> threadStack = [] {
    void * addr = nullptr;
    size_t size = 0;
#if defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
    }
#endif
    return std::pair{(const char *) addr, addr ? size : 0};
}();

/* ---------------------------------------------------------------------------
   Re-forcing on the native stack.

   `ExprReforce` gives a re-force the call depth of its first force, but its
   evaluation still nests on the native stack inside whatever forced it, and
   could overflow the stack where the evaluation without unforcing does not
   (`max-call-depth` is there to protect it). So the native stack is treated
   like the call depth. A first force records the room left on the stack
   where its evaluation starts (`stackRoom`); without unforcing, the
   evaluation fit in it. A re-force with less room left than that runs on a
   fresh stack, a *segment* as large as the thread's stack, where it has
   more; otherwise it runs where it is.

   Re-forces also nest in re-forces: a chain of k reverted cells whose
   evaluations are each S deep and each force the previous cell holds k * S
   of native stack when it is re-forced from the last, where the evaluation
   without unforcing, which forced each cell before the next, held S. So the
   stack that nested re-forces hold together (`liveStack`) may grow only
   within the thread's stack size: past it, a nested re-force does not start.
   It unwinds (`ReforceFirst`, an `Interrupted`, so that each thunk it
   unwinds through is forced again later, as after a user's interrupt; a job
   in which that happens is kept whole) to the outermost re-force on a
   segment, which re-forces that cell first, on a segment of its own, and
   then retries, with one more cell forced. So the stack re-forces hold is at
   most the thread's stack and one segment.

   A segment is mapped with a guard page below it, so that an overflow on it
   faults as on the thread's stack, and kept for reuse until a job ends. Cells
   on a segment are slots of frames entered during the job, never logged
   (`isJobStackSlot`).
   --------------------------------------------------------------------------- */

#if defined(__linux__)

namespace {

struct Segment
{
    char * map;
    size_t mapped;
    /* While in use: the stack pointer it was entered from. */
    const char * entry = nullptr;
#  if NIX_ASAN_ENABLED
    /* While in use: AddressSanitizer's fake stack of the stack it was
       entered from, whose frames still run. */
    void * callerFakeStack = nullptr;
#  endif

    /* The usable stack, above the guard page. */
    char * low() const
    {
        return map + pageSize();
    }

    char * high() const
    {
        return map + mapped;
    }

    bool holds(const void * p) const
    {
        return (const char *) p >= low() && (const char *) p < high();
    }
};

/* Segments in use, innermost last, and free ones. */
thread_local std::vector<Segment> activeSegments, freeSegments;

} // namespace

/* The bottom of the stack that `sp` is on: the innermost segment holding
   it, or the thread's stack; null if neither does (another stack, a
   coroutine's, whose room is not known). */
static const char * stackBottom(const char * sp)
{
    for (auto i = activeSegments.rbegin(); i != activeSegments.rend(); ++i)
        if (i->holds(sp))
            return i->low();
    if (sp >= threadStack.first && size_t(sp - threadStack.first) < threadStack.second)
        return threadStack.first;
    return nullptr;
}

/* The unit stack room is recorded in: the stack alignment of the ABIs Nix
   runs on, so a frame boundary is a whole number of them. */
constexpr size_t stackUnit = 16;

/* The stack room below `sp`, in `stackUnit`s, rounded up: 0 if unknown. It
   saturates at 64 GiB, for a larger stack: a first force with more room
   left records that much, so that a re-force always starts with at least as
   much room as its first force had, or at least 64 GiB. */
static uint32_t stackRoom(const char * sp)
{
    auto bottom = stackBottom(sp);
    if (!bottom)
        return 0;
    return uint32_t(std::min<size_t>((size_t(sp - bottom) + stackUnit - 1) / stackUnit, UINT32_MAX));
}

[[gnu::always_inline]] static inline const char * currentStackPointer()
{
    const char * sp;
#  if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#  elif defined(__aarch64__)
    asm volatile("mov %0, sp" : "=r"(sp));
#  else
    sp = (const char *) __builtin_frame_address(0);
#  endif
    return sp;
}

/* The native stack the re-forces running at `sp` hold: the thread's stack
   down to the first segment's entry, each segment down to the next's, and
   the innermost down to `sp`. */
static size_t liveStack(const char * sp)
{
    auto top = threadStack.first + threadStack.second;
    size_t live = 0;
    for (auto & s : activeSegments) {
        live += size_t(top - s.entry);
        top = s.high();
    }
    return live + size_t(top - sp);
}

/* The most stack the re-forces on this thread held as one entered a
   segment, for the unit tests. */
static thread_local size_t liveStackHighWater = 0;

/* For the unit tests: `liveStackHighWater`, and the thread's stack size. */
std::pair<size_t, size_t> unforceTestReforceStack();

std::pair<size_t, size_t> unforceTestReforceStack()
{
    return {liveStackHighWater, threadStack.second};
}

#  if NIX_ASAN_ENABLED
/* Whether `p` is a slot of a running frame that AddressSanitizer placed on
   a fake stack of its own: this stack's, or that of a stack a running
   re-force left for a segment. */
static bool onFakeStack(const void * p)
{
    auto in = [&](void * fake) {
        return fake && __asan_addr_is_in_fake_stack(fake, const_cast<void *>(p), nullptr, nullptr);
    };
    if (in(__asan_get_current_fake_stack()))
        return true;
    for (auto & s : activeSegments)
        if (in(s.callerFakeStack))
            return true;
    return false;
}

/* Clear AddressSanitizer's poison on a segment whose use has ended:
   Boost.Context's entry frame never returns, and frames unwound far below
   the top stay poisoned. On Linux, whole pages of the shadow go back to the
   kernel, which reads them as zeros (unpoisoned). */
static void unpoisonSegment(const Segment & segment)
{
    size_t scale = 0, offset = 0;
    __asan_get_shadow_mapping(&scale, &offset);
    auto shadow = [&](const char * p) { return (uintptr_t(p) >> scale) + offset; };
    auto memory = [&](uintptr_t s) { return (const char *) ((s - offset) << scale); };
    uintptr_t page = pageSize();
    uintptr_t begin = (shadow(segment.low()) + page - 1) & ~(page - 1), end = shadow(segment.high()) & ~(page - 1);
    if (begin < end && madvise((void *) begin, end - begin, MADV_DONTNEED) == 0) {
        ASAN_UNPOISON_MEMORY_REGION(segment.low(), memory(begin) - segment.low());
        ASAN_UNPOISON_MEMORY_REGION(memory(end), segment.high() - memory(end));
        return;
    }
    ASAN_UNPOISON_MEMORY_REGION(segment.low(), segment.high() - segment.low());
}
#  endif

/* Whether `p` is on a stack that is running: the thread's, a segment's,
   or (under AddressSanitizer) a fake stack. */
static bool onStack(const void * p)
{
#  if NIX_ASAN_ENABLED
    if (onFakeStack(p))
        return true;
#  endif
    return stackBottom((const char *) p);
}

/* A segment, or none if the address space (or the kernel's count of
   mappings) cannot hold another. */
static std::optional<Segment> acquireSegment()
{
    if (!freeSegments.empty()) {
        auto s = freeSegments.back();
        freeSegments.pop_back();
        return s;
    }
    if (!threadStack.second)
        return std::nullopt;
    size_t mapped = threadStack.second + pageSize();
    auto p = mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return std::nullopt;
    if (mprotect(p, pageSize(), PROT_NONE) != 0) {
        munmap(p, mapped);
        return std::nullopt;
    }
    return Segment{(char *) p, mapped};
}

/* At a job's end: unmap the free segments. */
static void releaseSegments()
{
    for (auto & s : freeSegments)
        munmap(s.map, s.mapped);
    freeSegments.clear();
}

/* Hands Boost.Context the segment, which it neither allocates nor frees. */
struct SegmentAllocator
{
    Segment segment;

    boost::context::stack_context allocate()
    {
        boost::context::stack_context sc;
        sc.size = segment.high() - segment.low();
        sc.sp = segment.high();
        return sc;
    }

    void deallocate(boost::context::stack_context &) noexcept {}
};

/* Run `body` on a segment, and rethrow here what it throws; or here, if no
   segment can be mapped (where a long enough chain of re-forces can then
   overflow the stack). */
template<typename F>
static void runOnFreshStack(F && body)
{
    auto acquired = acquireSegment();
    if (!acquired)
        return body();
    acquired->entry = currentStackPointer();
    liveStackHighWater = std::max(liveStackHighWater, liveStack(acquired->entry));
    activeSegments.push_back(*acquired);
    auto segment = *acquired;
    std::exception_ptr ex;
#  if NIX_ASAN_ENABLED
    /* AddressSanitizer is told of each switch, or it takes the thread's
       stack for the one it runs on, and leaves the frames an exception
       unwinds on the segment poisoned. */
    void * fakeStack = nullptr;
    const void * callerBottom = nullptr;
    size_t callerSize = 0;
#  endif
    auto onSegment = [&](boost::context::fiber && caller) {
#  if NIX_ASAN_ENABLED
        __sanitizer_finish_switch_fiber(nullptr, &callerBottom, &callerSize);
#  endif
        try {
            body();
        } catch (...) {
            ex = std::current_exception();
        }
#  if NIX_ASAN_ENABLED
        /* Leaving the segment for good: null, so that ASan drops its fake
           stack. */
        __sanitizer_start_switch_fiber(nullptr, callerBottom, callerSize);
#  endif
        return std::move(caller);
    };
    {
#  if NIX_ASAN_ENABLED
        /* Before the fiber is made: Boost.Context enters its entry function
           on the segment as it makes it. */
        __sanitizer_start_switch_fiber(&fakeStack, segment.low(), segment.high() - segment.low());
        activeSegments.back().callerFakeStack = fakeStack;
#  endif
        boost::context::fiber f{std::allocator_arg, SegmentAllocator{segment}, onSegment};
        std::move(f).resume();
#  if NIX_ASAN_ENABLED
        __sanitizer_finish_switch_fiber(fakeStack, nullptr, nullptr);
#  endif
    }
    activeSegments.pop_back();
#  if NIX_ASAN_ENABLED
    unpoisonSegment(segment);
#  endif
    freeSegments.push_back(segment);
    if (ex)
        std::rethrow_exception(ex);
}

namespace {

/* A nested re-force of `cell` that would hold more native stack than the
   bound: the outermost re-force on a segment re-forces it first. */
struct ReforceFirst : Interrupted
{
    Value * cell;

    explicit ReforceFirst(Value * cell)
        : Interrupted("unforce: re-force a cell first")
        , cell(cell)
    {
    }
};

} // namespace

[[gnu::noinline, gnu::cold]] static void reforceOnFreshStack(ExprReforce & r, EvalState & state, Env & env, Value & v)
{
    if (!activeSegments.empty()) {
        /* Nested in a re-force on a segment. Not a stack slot, which the
           unwinding would free. */
        if (liveStack(currentStackPointer()) + size_t(r.room) * stackUnit > threadStack.second && !onStack(&v))
            throw ReforceFirst(&v);
        return runOnFreshStack([&] { r.reforce(state, env, v); });
    }
    /* The outermost: what is to be re-forced first, innermost last. */
    std::vector<Value *> first;
    for (;;) {
        try {
            runOnFreshStack([&] {
                if (first.empty())
                    r.reforce(state, env, v);
                else
                    state.forceValue(*first.back(), noPos);
            });
        } catch (ReforceFirst & e) {
            first.push_back(e.cell);
            continue;
        }
        if (first.empty())
            return;
        first.pop_back();
    }
}

void ExprReforce::eval(EvalState & state, Env & env, Value & v)
{
    if (room) {
        /* The room the evaluation would start with here: below this frame,
           as below `forceValue`'s caller without unforcing. */
        auto sp = currentStackPointer();
        auto bottom = stackBottom(sp);
        if (bottom && size_t(sp - bottom) < size_t(room) * stackUnit) [[unlikely]]
            return reforceOnFreshStack(*this, state, env, v);
    }
    reforce(state, env, v);
}

#else

static uint32_t stackRoom(const char * sp)
{
    return 0;
}

static void releaseSegments() {}

void ExprReforce::eval(EvalState & state, Env & env, Value & v)
{
    reforce(state, env, v);
}

#endif

/* The stack pointer of the open job's opener: the frames entered during the
   job are below it, down to the bottom of the thread's stack, and on
   segments. */
static const char * jobStackMark = nullptr;

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
        keepWhole = true;
        return true;
    }
#  endif
    /* The job was opened on the thread's stack (`beginUnforceScope`), so
       every segment in use was entered during the job. */
    for (auto & s : activeSegments)
        if (s.holds(p))
            return true;
#endif
    return (const char *) p >= threadStack.first && (const char *) p < jobStackMark;
}

/* ---------------------------------------------------------------------------
   The objects in a job's region.

   A pointer into the region says what its object is: a value, an `Env`, a
   `Bindings` layer, a list's element array, a string's context, or its
   `StringData`, which holds no pointers. Bump allocations are 16-byte
   aligned and never share a granule.
   --------------------------------------------------------------------------- */

enum class Obj : uint8_t { value, env, bindings, elems, context, string };

using Context = Value::StringWithContext::Context;

/* The pointers of `v` to the objects it is made of: `edge(p, kind, n)` for
   each (`n`: an element array's length). */
template<typename E>
static void valueEdges(const Value & v, E && edge)
{
    switch (v.internalType()) {
    case tString:
        edge(v.stringData(), Obj::string, 0);
        edge(v.context(), Obj::context, 0);
        break;
    case tPath:
        edge(v.pathData(), Obj::string, 0);
        break;
    case tAttrs:
        edge(v.attrs(), Obj::bindings, 0);
        break;
    case tListSmall:
        edge(v.rawSmallList()[0], Obj::value, 0);
        edge(v.rawSmallList()[1], Obj::value, 0);
        break;
    case tListN:
        edge(v.rawList().elems, Obj::elems, v.rawList().size);
        break;
    case tThunk:
        edge(v.thunk().env, Obj::env, 0);
        break;
    case tApp:
        edge(v.app().left, Obj::value, 0);
        edge(v.app().right, Obj::value, 0);
        break;
    case tPrimOpApp:
        edge(v.primOpApp().left, Obj::value, 0);
        edge(v.primOpApp().right, Obj::value, 0);
        break;
    case tLambda:
        edge(v.lambda().env, Obj::env, 0);
        break;
    case tFailed:
        edge(v.failed().recoveryValue, Obj::value, 0);
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

/* How many pointers `p`, an object of `kind` other than a value, holds
   (`n`: an element array's length). */
static size_t wideEdges(const void * p, Obj kind, size_t n)
{
    switch (kind) {
    case Obj::env:
        return 1 + envSizeOf((const Env *) p);
    case Obj::bindings:
        return 1 + ((const Bindings *) p)->layerSize();
    case Obj::elems:
        return n;
    case Obj::context:
        return ((const Context *) p)->size();
    case Obj::value:
    case Obj::string:
        return 0;
    }
    unreachable();
}

/* Pointer `i` of `p` (see `wideEdges`), passed to `edge` as `valueEdges`
   passes a value's. */
template<typename E>
static void wideEdge(const void * p, Obj kind, size_t i, E && edge)
{
    switch (kind) {
    case Obj::env: {
        auto e = (const Env *) p;
        if (i == 0)
            edge(e->up, Obj::env, 0);
        else
            edge(e->values[i - 1], Obj::value, 0);
        break;
    }
    case Obj::bindings: {
        auto b = (const Bindings *) p;
        if (i == 0)
            edge(b->layerBase(), Obj::bindings, 0);
        else
            edge(b->layerValue(i - 1), Obj::value, 0);
        break;
    }
    case Obj::elems:
        edge(((Value * const *) p)[i], Obj::value, 0);
        break;
    case Obj::context:
        edge(((const Context *) p)->begin()[i], Obj::string, 0);
        break;
    case Obj::value:
    case Obj::string:
        break;
    }
}

/* Whether `v` is a value (not a thunk, an application or a failure) that
   holds no pointer into the job's region. Such a target stays as it is:
   reverting it would free nothing, and its value is what the evaluation
   without unforcing keeps. Its objects are old, so nothing builds them
   again. */
static bool youngFree(const Value & v)
{
    if (v.isThunk() || v.isApp() || v.isFailed() || v.internalType() == tUninitialized)
        return false;
    bool young = false;
    valueEdges(v, [&](const void * p, Obj, size_t) { young = young || evalRegion.contains(p); });
    return !young;
}

/* A bit per granule of [base, cur). */
struct GranuleBits
{
    const char * base;
    size_t words;
    uint64_t * w;

    GranuleBits(const char * base, const char * cur)
        : base(base)
        , words(size_t(cur - base + 1023) >> 10)
        , w((uint64_t *) std::calloc(std::max<size_t>(words, 1), sizeof(uint64_t)))
    {
        if (!w)
            throw std::bad_alloc();
    }

    ~GranuleBits()
    {
        std::free(w);
    }

    GranuleBits(const GranuleBits &) = delete;
    GranuleBits & operator=(const GranuleBits &) = delete;

    size_t granule(const void * p) const
    {
        return size_t((const char *) p - base) >> 4;
    }

    bool test(const void * p) const
    {
        auto g = granule(p);
        return (w[g >> 6] >> (g & 63)) & 1;
    }

    /* Set `p`'s bit; returns whether it was set already. */
    bool testAndSet(const void * p)
    {
        auto g = granule(p);
        auto bit = uint64_t(1) << (g & 63);
        auto old = w[g >> 6];
        w[g >> 6] = old | bit;
        return old & bit;
    }
};

/* A depth-first walk of the young objects reachable from root values, each
   entered once (`Visit::enter`). Values are scanned whole, wide objects
   (`wideEdges`) a pointer at a time, once the values found so far are done,
   so that the worklists hold about one path's worth of entries, however
   wide the objects on it.

   A path can be as long as the job's memory allows, so the worklists hold
   at most as many bytes as a bit map of the job's memory (`room`). An
   object entered while they are full is deferred instead: a bit at its
   first granule, in a bit map of its kind. Only values, Envs and Bindings
   layers are ever deferred (a value is scanned only while its pointers
   fit). Once the worklists are empty, the deferred objects are walked from
   the maps in address order, each map swept on from where its last sweep
   stopped; a map that holds nothing is not swept. */
template<typename Visit>
struct Walk
{
    Visit & visit;
    std::vector<const Value *> values;

    struct Wide
    {
        const void * p;
        size_t next, end;
        Obj kind;
    };

    std::vector<Wide> wides;
    size_t room;

    struct Deferred
    {
        std::optional<GranuleBits> bits;
        size_t count = 0;
        size_t next = 0;
    };

    /* Values, Envs, Bindings layers. */
    Deferred deferred[3];

    Walk(Visit & visit)
        : visit(visit)
        , room(std::max<size_t>(2, (size_t(evalRegion.cur - evalRegion.base) >> 10) * sizeof(uint64_t) / sizeof(Wide)))
    {
    }

    bool full(size_t more = 1) const
    {
        return values.size() + wides.size() + more > room;
    }

    void defer(const void * p, Obj kind)
    {
        auto & d = deferred[kind == Obj::value ? 0 : kind == Obj::env ? 1 : 2];
        assert(kind == Obj::value || kind == Obj::env || kind == Obj::bindings);
        if (!d.bits)
            d.bits.emplace(evalRegion.base, evalRegion.cur);
        if (!d.bits->testAndSet(p))
            d.count++;
    }

    /* Push the next deferred object; false if none. */
    bool undefer()
    {
        for (size_t k = 0; k < 3; ++k) {
            auto & d = deferred[k];
            if (!d.count)
                continue;
            auto & b = *d.bits;
            for (;; d.next = (d.next + 1) % b.words)
                if (auto w = b.w[d.next]) {
                    b.w[d.next] = w & (w - 1);
                    d.count--;
                    auto p = b.base + (((d.next << 6) + std::countr_zero(w)) << 4);
                    if (k == 0)
                        values.push_back((const Value *) p);
                    else {
                        auto kind = k == 1 ? Obj::env : Obj::bindings;
                        wides.push_back({p, 0, wideEdges(p, kind, 0), kind});
                    }
                    return true;
                }
        }
        return false;
    }

    void edge(const void * p, Obj kind, size_t n)
    {
        if (!p || !evalRegion.contains(p) || !visit.enter(p, kind, n))
            return;
        if (kind == Obj::value) {
            if (full())
                defer(p, kind);
            else
                values.push_back((const Value *) p);
        } else if (auto end = wideEdges(p, kind, n)) {
            if (full())
                defer(p, kind);
            else
                wides.push_back({p, 0, end, kind});
        }
    }

    void scan(const Value & v)
    {
        visit.value(v);
        valueEdges(v, [&](const void * p, Obj kind, size_t n) { edge(p, kind, n); });
    }

    /* Walk what `v`, a target's value, reaches. */
    void root(const Value & v)
    {
        scan(v);
        for (;;) {
            if (!values.empty()) {
                auto next = values.back();
                values.pop_back();
                if (full(2))
                    defer(next, Obj::value);
                else
                    scan(*next);
            } else if (!wides.empty()) {
                auto & w = wides.back();
                auto p = w.p;
                auto kind = w.kind;
                auto i = w.next++;
                if (w.next == w.end)
                    wides.pop_back();
                wideEdge(p, kind, i, [&](const void * p, Obj kind, size_t n) { edge(p, kind, n); });
            } else if (!undefer())
                break;
        }
    }
};

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

/* Marking, for `Walk`: each object's first granule into `marked`, and each
   identity-bearing one (a value, a Bindings layer, an element array) to
   `onMark`. A failure that it keeps forgets its young cell. */
template<typename F>
struct Mark
{
    GranuleBits & marked;
    F onMark;
    size_t count = 0;

    bool enter(const void * p, Obj kind, size_t n)
    {
        if (marked.testAndSet(p))
            return false;
        count++;
        if (kind == Obj::value || kind == Obj::bindings || kind == Obj::elems)
            onMark(p);
        return true;
    }

    void value(const Value & v)
    {
        if (v.isFailed())
            forgetYoungCell(v.failed());
    }
};

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
   Bindings layer, an element array) that a promoted value reaches, Evac(S),
   has its owner in S, or none. Then a reverted target, re-forced later,
   rebuilds only cells that nothing kept.

   Two lemmas make that exact. While t is being forced, the young cells it
   owns are visible only to its own evaluation and to the forces of those
   cells (no other cell's evaluation can reach them but through a cell's
   value, and t has none yet). So if a promoted value reaches an object t
   owns, t's value reaches it too: promoting t keeps nothing that its value
   does not keep. The closure is computed by marking Evac(S) from the
   promoted values and adding the owner of each object marked, until no
   owner is added.

   A job promotes the targets that hold a failure (failed thunks stay
   failed: a failure cannot be recomputed, as a fresh force runs under other
   frames and its trace differs), and the owners of young cells that failed
   and that their owner's value reaches; and the targets that an earlier job
   reverted, which are evidently shared between jobs: reverting them again
   would only make each job evaluate them anew.
   --------------------------------------------------------------------------- */

/* The young cells whose force failed in the open job, and whether one could
   not be recorded (then the job is kept whole). */
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

/* Which targets the job promotes (see above); `evac`: whether their values
   reach anything young. */
static std::vector<bool> promoted(const std::vector<UnforceUndoEntry> & log, const Targets & targets, bool & evac)
{
    std::vector<bool> promote(targets.cells.size());
    std::vector<uint32_t> pending;
    /* An owner names a log entry: an entry is dropped only when no segment
       has its label. */
    auto targetOf = [&](uint32_t o) {
        assert(o < log.size());
        return targets.ofEntry[o];
    };
    auto nominate = [&](uint32_t t) {
        if (!promote[t]) {
            promote[t] = true;
            pending.push_back(t);
        }
    };
    for (size_t i = 0; i < log.size(); ++i) {
        auto & p = log[i].preImage;
        if (log[i].target->isFailed() || (p.isThunk() && unforceIsReverted(p.thunk().expr)))
            nominate(targets.ofEntry[i]);
    }

    /* The owners of young failures that their values reach. */
    std::vector<std::pair<const Value *, uint32_t>> failures;
    for (auto x : youngFailures)
        if (auto o = ownerOf(x); x->isFailed() && o != noOwner && !promote[targetOf(o)])
            failures.emplace_back(x, targetOf(o));
    if (!failures.empty()) {
        GranuleBits reached(evalRegion.base, evalRegion.cur);
        auto mark = Mark{reached, [](const void *) {}};
        Walk walk{mark};
        for (auto [x, t] : failures)
            walk.root(*targets.cells[t]);
        for (auto [x, t] : failures)
            if (reached.test(x))
                nominate(t);
    }

    /* The closure. */
    GranuleBits marked(evalRegion.base, evalRegion.cur);
    auto mark = Mark{marked, [&](const void * p) {
                         if (auto o = ownerOf(p); o != noOwner)
                             nominate(targetOf(o));
                     }};
    Walk walk{mark};
    while (!pending.empty()) {
        auto t = pending.back();
        pending.pop_back();
        walk.root(*targets.cells[t]);
    }
    evac = mark.count;
    return promote;
}

void unforceNoteEffect()
{
    /* Running it again could give another result, or show it again. */
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
    keepWhole = !(jobStackMark > threadStack.first && size_t(jobStackMark - threadStack.first) <= threadStack.second);
    openRegion();
    openSeg = evalRegion.base;
}

void EvalState::endUnforceScope()
{
    if (NIX_USE_BOEHMGC || --isolationDepth)
        return;
    endUnforceJob();
}

void EvalState::endUnforceJob()
{
    /* A cell whose failure was retried is kept with the whole job: its
       re-force would run at the depth of its first force, which failed
       (`ExprReforce`). */
    bool keep = keepWhole || youngFailuresLost || !evalRegion.base
                || std::any_of(undoLog.begin(), undoLog.end(), [](auto & e) { return e.preImage.isFailed(); });
    /* Each entry's cell is reverted (`target` stays) to what its entry
       says, decided from the cell's value at the job's end before any is
       written, or keeps its value (no `target`). While promoted values reach
       anything young, the region stays. */
    bool evacuated = false;
    if (!keep) {
        PauseRegion pause;
        try {
            Targets targets(undoLog);
            auto promote = promoted(undoLog, targets, evacuated);
            for (size_t i = 0; i < undoLog.size(); ++i) {
                auto & e = undoLog[i];
                if (youngFree(*e.target) || promote[targets.ofEntry[i]])
                    e.target = nullptr;
                else
                    e.preImage = reforcePreImage(*this, e.preImage, e.depth, e.pos, e.room, *e.target);
            }
        } catch (std::bad_alloc &) {
            keep = true;
        }
    }
    /* Newest first, so that a cell logged more than once ends with its
       oldest entry's: its value before the job, re-forced at the depth of
       its outermost force, which completed last. */
    if (!keep)
        for (auto i = undoLog.rbegin(); i != undoLog.rend(); ++i)
            if (i->target)
                *i->target = i->preImage;
    undoLog.clear();
    undoLog.shrink_to_fit();
    decltype(segments)().swap(segments);
    decltype(opens)().swap(opens);
    decltype(youngFailures)().swap(youngFailures);
    youngFailuresLost = false;
    curOwner = noOwner;
    if (evalRegion.base)
        closeRegion(keep || evacuated);
    /* No re-force is running. */
    releaseSegments();
    keepWhole = false;
    jobOwner = nullptr;
}

void EvalState::logPreImageSlow(Value & v, PosIdx pos)
{
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
    assert(callDepth <= UINT32_MAX && undoLog.size() < noOwner);
    roomFor(undoLog, 1);
    roomFor(segments, opens.size() + 2);
    roomFor(opens, 1);
    auto entry = uint32_t(undoLog.size());
    /* The stack pointer of `forceValue`'s caller, where the force's
       evaluation starts. */
    undoLog.push_back({&v, uint32_t(callDepth), pos, stackRoom((const char *) __builtin_dwarf_cfa()), v});
    openForce(v, entry, entry);
}

void EvalState::endLoggedForceSlow(Value & v, bool completed)
{
    if (opens.empty() || opens.back().cell != &v)
        return;
    auto f = opens.back();
    opens.pop_back();
    /* A logged force that completed with a young-free value drops its
       entry, if it is still the last one and no closed segment has its
       label: the job's end would keep the cell as it is anyway, so the log
       and the timeline do not grow with such forces, however many a job
       makes. What the force allocated in its segment is garbage (its value
       reaches none of it, and a cell older than the force that reaches it
       would have a later entry), and becomes the restored owner's. */
    if (f.entry != noOwner && completed && f.entry + 1 == undoLog.size() && segments.size() == f.segs && youngFree(v)) {
        undoLog.pop_back();
        curOwner = f.saved;
        if (!segments.empty() && segments.back().owner == f.saved) {
            openSeg = segStart(segments.back());
            segments.pop_back();
        }
        return;
    }
    setOwner(f.saved);
}

} // namespace nix
