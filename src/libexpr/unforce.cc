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

#include <algorithm>
#include <atomic>
#include <cstring>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

#if NIX_ASAN_ENABLED
#  include <sanitizer/asan_interface.h>
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

/* The stack pointer of the open job's opener: the frames entered during the
   job are below it, down to the bottom of the thread's stack. */
static const char * jobStackMark = nullptr;

/* Whether `p` is a slot of a stack frame entered during the open job, which
   will have returned when it ends: such a cell is never logged, as the end
   must not write it. */
static bool isJobStackSlot(const void * p)
{
#if NIX_ASAN_ENABLED
    /* On one of AddressSanitizer's fake stacks, whose addresses tell nothing
       of where its frame is: keep the job whole, so that its end writes no
       slot of a frame still running, nor of one that returned. */
    if (auto fake = __asan_get_current_fake_stack();
        fake && __asan_addr_is_in_fake_stack(fake, const_cast<void *>(p), nullptr, nullptr)) {
        keepWhole = true;
        return true;
    }
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
       reverted. */
    bool keep = keepWhole || !evalRegion.base
                || std::any_of(undoLog.begin(), undoLog.end(), [](auto & e) { return e.target->isFailed(); });
    /* Newest first, so that a cell logged more than once ends with its
       oldest pre-image, its value before the job. A cell whose value stays
       keeps it at all of its entries: one that is reverted holds a thunk or
       an application, never young-free, at the older ones. */
    if (!keep)
        for (auto i = undoLog.rbegin(); i != undoLog.rend(); ++i)
            if (!youngFree(*i->target))
                *i->target = i->preImage;
    undoLog.clear();
    undoLog.shrink_to_fit();
    if (evalRegion.base)
        closeRegion(keep);
    keepWhole = false;
    jobOwner = nullptr;
}

void EvalState::logPreImageSlow(Value & v)
{
    /* A young cell is freed with the region, or kept with it. */
    if (evalRegion.contains(&v) || isJobStackSlot(&v))
        return;
    undoLog.push_back({&v, v});
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
