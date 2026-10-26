/* Unforcing thunks between jobs.

   Thunk update in `forceValue` is the evaluator's only mutation of existing
   cells. While a job (an outermost unforce scope) runs, the evaluator
   allocates in a region of its own (`evalRegion`), and the pre-image of
   every *old* cell (one outside the region) that the job forces is logged.
   At the job's end each logged cell is reverted to its pre-image, unless its
   value holds no pointer into the region (`youngFree`), and the region is
   freed: no old cell points into it any more. A job that leaves a logged
   cell failed, or in which an effect ran, is kept whole instead: nothing is
   reverted, and its memory stays. */

#include "nix/expr/eval.hh"
#include "nix/expr/unforce.hh"
#include "nix/util/config.hh"
#include "nix/util/signals.hh"
#include "nix/util/std-hash.hh"

#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <atomic>
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

#endif

/* Open the job's region, where the system has room for one (only Linux is
   supported): without one, the job allocates as outside any job, and its
   end reverts nothing. */
static void openRegion()
{
#if defined(__linux__)
    auto mappingBytes = 2 * regionBytes;
    if (!mapping || size_t(mapping + mappingBytes - keptTop) < regionBytes) {
        if (!regionBytes)
            return;
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
   evaluator expects allocations zeroed: in place in the first page, and
   above it by returning whole pages to the system, which zero-fills private
   anonymous memory when it is touched again. */
static void closeRegion(bool keep)
{
#if defined(__linux__)
    auto base = evalRegion.base, cur = evalRegion.cur;
    if (keep)
        keptTop = cur;
    else if (cur > base) {
        auto up = std::min(pageUp(base), cur);
        memset(base, 0, up - base);
        if (cur > up && madvise(up, pageUp(cur) - up, MADV_DONTNEED) != 0)
            memset(up, 0, cur - up);
    }
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

/* Whether `v` is a value (not a thunk, an application or a failure) that
   holds no pointer into the job's region. Such a target stays as it is:
   reverting it would free nothing, and its value is what the evaluation
   without unforcing keeps. */
static bool youngFree(const Value & v)
{
    auto young = [](const void * p) { return evalRegion.contains(p); };
    switch (v.internalType()) {
    case tString:
        return !young(v.stringData()) && !young(v.context());
    case tPath:
        return !young(v.pathData());
    case tAttrs:
        return !young(v.attrs());
    case tListSmall:
        return !young(v.rawSmallList()[0]) && !young(v.rawSmallList()[1]);
    case tListN:
        return !young(v.rawList().elems);
    case tPrimOpApp:
        return !young(v.primOpApp().left) && !young(v.primOpApp().right);
    case tLambda:
        return !young(v.lambda().env);
    case tInt:
    case tBool:
    case tNull:
    case tFloat:
    case tExternal:
    case tPrimOp:
        return true;
    case tThunk:
    case tApp:
    case tFailed:
    case tUninitialized:
    case tNumberOfInternalTypes:
        return false;
    }
    unreachable();
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
}

void EvalState::endUnforceScope()
{
    if (NIX_USE_BOEHMGC || --isolationDepth)
        return;
    endUnforceJob();
}

void EvalState::endUnforceJob()
{
    /* Failed thunks stay failed: a failure cannot be recomputed (a fresh
       force runs under other frames, so its trace differs), so it is never
       reverted. Nor is a cell whose failure was retried (`ExprReforce`). */
    bool keep = keepWhole || !evalRegion.base || std::any_of(undoLog.begin(), undoLog.end(), [](auto & e) {
                    return e.target->isFailed() || e.preImage.isFailed();
                });
    /* What each entry reverts its cell to, decided from the cell's value at
       the job's end before any is written: no target, for a cell that keeps
       its value. */
    if (!keep) {
        PauseRegion pause;
        try {
            for (auto & e : undoLog)
                if (youngFree(*e.target))
                    e.target = nullptr;
                else
                    e.preImage = reforcePreImage(*this, e.preImage, e.depth, e.pos, e.room, *e.target);
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
    if (evalRegion.base)
        closeRegion(keep);
    /* No re-force is running. */
    releaseSegments();
    keepWhole = false;
    jobOwner = nullptr;
}

void EvalState::logPreImageSlow(Value & v, PosIdx pos)
{
    /* A young cell is freed with the region, or kept with it. */
    if (evalRegion.contains(&v) || isJobStackSlot(&v))
        return;
    assert(callDepth <= UINT32_MAX);
    /* The stack pointer of `forceValue`'s caller, where the force's
       evaluation starts. */
    undoLog.push_back({&v, uint32_t(callDepth), pos, stackRoom((const char *) __builtin_dwarf_cfa()), v});
}

void EvalState::endLoggedForceSlow(Value & v)
{
    /* A force that completed with a young-free value drops its entry, if it
       is still the last one: the job's end keeps the cell as it is anyway.
       So the log does not grow with such forces, however many a job makes.
       (An older entry of the cell, kept by a force nested in this one,
       agrees: see `endUnforceJob`.) */
    if (!undoLog.empty() && undoLog.back().target == &v && youngFree(v))
        undoLog.pop_back();
}

} // namespace nix
