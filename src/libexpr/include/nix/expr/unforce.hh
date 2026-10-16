#pragma once
///@file

/* PROTOTYPE (unforcing). Declarations for the cold, out-of-line unforcing
   surface: the job-region pause guard, the «seen»-set and traversal-batch
   helpers, and the free functions that only cold call sites (unforce.cc and
   its consumers) use. The hot inlined pieces that `eval-inline.hh` needs --
   `EvalRegion`, `evalRegion`, `evalBaseAlloc`, `BumpArena`/`evalBaseArena`,
   and the `logPreImage`/`endLoggedForce` hooks -- stay in `eval.hh`. */

#include "nix/expr/eval.hh"

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nix {

struct PauseRegion
{
    bool saved = std::exchange(evalRegion.paused, true);

    ~PauseRegion()
    {
        evalRegion.paused = saved;
    }
};

/**
 * PROTOTYPE. Whether an address recorded before the last unforce scope ended
 * may since have been freed (and so may be reused): true inside the last
 * released region, and always when unforcing without regions (Boehm reclaims
 * unforced values anywhere).
 */
bool unforceMayHaveFreed(const void * p);

/**
 * PROTOTYPE. Keeps a printer's address-keyed `seen` set (its «repeated»
 * tracking) as exact across the end of unforce jobs as without unforcing,
 * while this object lives: a container printed in one job and reached
 * again in a later one must still print as «repeated».
 *
 * Every container the printer marks seen in a job is reported with
 * `unforceNoteSeen`. At the job's end, the cells holding them nominate
 * themselves for promotion (like the force counts do), so the containers
 * are kept, and the set's entries for the job's own objects are rewritten
 * to the copies' addresses (`onRekey` hears of each). If any of them would
 * not be kept (client code ran in the job, or the closure fell back to
 * reverting), the job keeps all its memory instead: nothing is promoted,
 * reverted or freed, and the entries stay as they are.
 */
struct UnforceSeen
{
    std::set<const void *> & seen;
    /* Reported in the open job: cells outside the job's memory that hold
       a container keyed elsewhere (a Bindings); targets among them are
       nominated. The job's own keys are the set's entries inside its
       memory, and a cell that is its own key is an entry of the set, so
       neither is recorded (a vector of them would grow with the job). */
    std::vector<const Value *> cells;
    /* Whether a cell outside the job's memory was reported as its own key
       in the open job. */
    bool selfKeyed = false;
    /** Called when an entry `from` is rewritten to `to` at a job's end. */
    std::function<void(const void * from, const void * to)> onRekey;

    explicit UnforceSeen(std::set<const void *> & seen);
    ~UnforceSeen();
    UnforceSeen(const UnforceSeen &) = delete;
    UnforceSeen & operator=(const UnforceSeen &) = delete;
};

/**
 * PROTOTYPE. `key` (a Bindings, or a list's cell) of the container that
 * `cell` holds was just added to `seen`. Does nothing unless an
 * `UnforceSeen` for `seen` is alive and a job is open.
 */
[[gnu::noinline, gnu::cold]] void unforceNoteSeen(std::set<const void *> & seen, const Value & cell, const void * key);

/**
 * PROTOTYPE. Whether the open unforce jobs keep `UnforceSeen` sets exact
 * (the bump backend), so that a printer need not forget what a freeing
 * job added.
 */
bool unforceSeenExact();

/**
 * PROTOTYPE. Counts unforce scopes that have freed memory. A pointer into
 * evaluator memory cached outside a scope (e.g. by an eval-cache cursor) is
 * stale if this has changed since it was cached and `unforceMayHaveFreed`
 * holds for it.
 */
size_t unforceGeneration();

/**
 * PROTOTYPE. Whether `p` is on this thread's stack below the stack pointer
 * that the innermost unforce scope's opener had: a slot of a frame entered
 * since, which will have returned when the scope ends, so it must not be
 * restored then. (Stacks other than the thread's, e.g. coroutines', are not
 * recognised.)
 */
bool unforceIsScopeStackSlot(const void * p);

/**
 * PROTOTYPE. Client code (a C API primop) is about to run. If an unforce
 * scope is open, the job promotes nothing: what the client builds is
 * allocated outside the job's region, where the producer closure cannot see
 * it, and a re-forced call builds it anew.
 */
void unforceNoteClientCode();

/** PROTOTYPE. Bytes allocated so far in the innermost unforce scope. */
size_t unforceCurrentScopeBytes();

/** PROTOTYPE. NIX_SHOW_STATS sections as JSON text, or empty. */
std::string unforceStatsJSON();
std::string allocatorStatsJSON();

/**
 * PROTOTYPE. Whether traversals unforce what they visit: the `eval-unforce`
 * setting, unless the debugger is active (its traces refer to environments
 * that unforcing frees).
 */
bool unforceEnabled(const EvalState & state);

/**
 * PROTOTYPE. Unforcing for a traversal that visits each element once
 * (printing or serialising a value, searching, checking): what visiting its
 * elements forced is unforced, and what it allocated freed, when their batch
 * ends. Consecutive elements share a batch until it has allocated
 * `eval-unforce-batch-size` bytes; the destructor ends the last one.
 *
 * The traversal must keep nothing allocated while visiting an element beyond
 * the batch, except what it has converted to its own output (text, JSON,
 * store paths). Visited cells may be thunks again afterwards.
 */
class UnforceBatch
{
    EvalState * state;
    std::optional<size_t> mark;
    bool job = false;

public:
    /** Called when a batch ends, with whether it freed anything (only a
        batch that is not nested in another one does). */
    std::function<void(bool freed)> onEnd;

    UnforceBatch(EvalState & state, bool enabled = true);
    ~UnforceBatch();
    UnforceBatch(const UnforceBatch &) = delete;

    bool enabled() const
    {
        return state;
    }

    bool isOpen() const
    {
        return mark.has_value();
    }

    /** Visit one element: opens a batch if none is open, and ends it when
        the element is done if the batch is full. */
    struct Element
    {
        UnforceBatch & batch;
        /* Not inlined, so that it can find its caller's stack pointer. */
        [[gnu::noinline]] explicit Element(UnforceBatch & batch);
        ~Element();
    };

    /** Open a batch if none is open, for a caller that ends batches itself
        (see `full`) instead of using `Element`. Not inlined, so that it can
        find its caller's stack pointer. */
    [[gnu::noinline]] void open();

    /** Whether the open batch has allocated `eval-unforce-batch-size`. */
    bool full() const;

    /** End the open batch, if any. */
    void end();

private:
    void openAt(const void * stackMark);
};

} // namespace nix
