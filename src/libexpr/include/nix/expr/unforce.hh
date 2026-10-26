#pragma once
///@file

/* Unforcing (unforce.cc): the cold parts. The hot ones (`EvalRegion`,
   `logPreImage`) are in eval.hh. */

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"

#include <set>
#include <vector>

namespace nix {

[[gnu::noinline]] void unforcePauseBegin();
[[gnu::noinline]] void unforcePauseEnd();

/**
 * While this lives, evaluator memory is allocated outside the open job's
 * region: for what outlives the job outside any thunk it forces (a cache).
 */
struct PauseRegion
{
    [[gnu::always_inline]] PauseRegion()
    {
        unforcePauseBegin();
    }

    [[gnu::always_inline]] ~PauseRegion()
    {
        unforcePauseEnd();
    }

    PauseRegion(const PauseRegion &) = delete;
    PauseRegion & operator=(const PauseRegion &) = delete;
};

/** `v`'s force just failed in an open scope. */
[[gnu::noinline, gnu::cold]] void unforceNoteFailure(const Value & v) noexcept;

/**
 * An effect that running again could give another result (a download, a
 * copy that nothing remembers, a build) ran: the open job, if any, keeps
 * its result.
 */
void unforceNoteEffect();

/**
 * A trace or a warning is about to be shown, which running again would show
 * again: the open job, if any, keeps the value of the cell whose re-force
 * would show it again.
 */
void unforceNoteDiagnostic(EvalState & state);

/** The caller keeps something it built in the open job outside any cell: the job, if any, keeps all its memory. */
void unforceKeepJob();

/**
 * `allocRootValue` of `v`, in the open job's memory: while the root lives,
 * the job's end keeps what it points to (an `UnforceHolder`), and it points
 * to where that moves.
 */
RootValue unforceHeldRoot(Value * v);

/**
 * C++ state that points into the open job's memory from outside any cell (a
 * printer's set of the containers it has printed, the derivations that
 * `getDerivations` found). While it is registered (from its construction to
 * its destruction), a job's end keeps what it points to, as it keeps what a
 * promoted value reaches, and rewrites its pointers to where that moves.
 */
class UnforceHolder
{
public:
    enum class Kind { value, bindings };

    struct Hold
    {
        virtual ~Hold();
        /** Keep `p`, a `Value *` or a `Bindings *`; returns where it is from now on. */
        virtual const void * operator()(const void * p, Kind kind) = 0;
    };

    UnforceHolder();
    virtual ~UnforceHolder();
    UnforceHolder(const UnforceHolder &) = delete;
    UnforceHolder & operator=(const UnforceHolder &) = delete;

    /**
     * At a job's end: pass each pointer into the job's memory that this
     * holds to `hold`, and store what it returns. Called once, and once more
     * if anything moves; then `ended`. Allocates nothing.
     */
    virtual void held(Hold & hold) = 0;

    /** A job ended: nothing this holds is in a job's memory any more. */
    virtual void ended() = 0;

    /**
     * At a job's end: whether `cell`, older than the job, which forced it,
     * keeps its value, and with it what the value reaches.
     */
    virtual bool keeps(const Value * cell) const
    {
        return false;
    }
};

/**
 * A set of the containers a traversal has seen, by address (to print
 * «repeated», or to drop duplicates), that stays exact across jobs: its
 * entries in the open job's memory are held.
 */
class UnforceSeen : public std::set<const void *>, public UnforceHolder
{
    std::vector<std::pair<const void *, Kind>> young;

public:
    /** Add `p`, of `kind`; returns whether it was not there yet. */
    bool see(const void * p, Kind kind);
    void held(Hold & hold) override;
    void ended() override;
};

[[gnu::noinline]] void unforceJobsBegin(EvalState & state, const Value * root);
[[gnu::noinline]] void unforceJobsEnd();
[[gnu::noinline]] void unforceElementStart();

/**
 * Unforce jobs for a traversal of independent elements (the derivations of
 * a package set, the attributes that a printer prints): each element runs
 * in a job of its own, which ends when the next element starts or when this
 * ends, unless the traversal itself runs in a job. The traversal is a loop
 * in the caller's frame that calls `element` before each element, or a
 * recursive walk of `root` whose levels call `unforceElement`: only the
 * outermost level's elements are jobs. Nothing a job allocated may outlive
 * its element but through the cells it forced or a holder
 * (`UnforceHolder`). This holds nothing, and covers one call of the
 * traversal: a level is told by its frame.
 */
class UnforceJobs
{
public:
    [[gnu::always_inline]] explicit UnforceJobs(EvalState & state, const Value * root = nullptr)
    {
        unforceJobsBegin(state, root);
    }

    [[gnu::always_inline]] ~UnforceJobs()
    {
        unforceJobsEnd();
    }

    UnforceJobs(const UnforceJobs &) = delete;
    UnforceJobs & operator=(const UnforceJobs &) = delete;

    /** An element of the caller's loop starts. */
    [[gnu::always_inline]] void element()
    {
        unforceElementStart();
    }
};

/** An element of `v` starts, in a level of a traversal (`UnforceJobs`). */
[[gnu::always_inline]] inline void unforceElement(EvalState & state, const Value & v)
{
    if (state.unforceRoot == &v) [[unlikely]]
        unforceElementStart();
}

/** Whether a thunk of `expr` is a cell that unforcing reverted. */
bool unforceIsReverted(const Expr * expr);

/** `Value::determinePos` of a thunk of `expr`: a reverted cell's value's position, or `pos`. */
[[gnu::noinline, gnu::cold]] PosIdx unforceRevertedPos(const Expr * expr, PosIdx pos);

/** Whether unforcing has reverted a cell in this process. */
extern bool unforceAnyReverted;

/** What unforcing did in this process (`NIX_SHOW_STATS`). */
struct UnforceStats
{
    uint64_t jobs = 0, jobsKeptWhole = 0, cellsReverted = 0, cellsPromoted = 0, bytesKept = 0, bytesFreed = 0;
};

extern UnforceStats unforceStats;

/**
 * Whether front ends open unforce jobs in `state`: not with Boehm GC, nor
 * with the debugger, nor where native code may run (`unforceNativeCode`).
 */
bool unforceEnabled(const EvalState & state);

/* Native code (a plugin's primop, `builtins.importNative`, a C API client)
   may keep any `Value *` it reaches and read it in a later job without
   forcing it, so unforcing is off wherever it may run. A plugin's lookup
   path hook is client code instead (`unforceNoteLookupPathHook`). A
   plugin's command needs no check: `nix` chooses its command before plugins
   load (`initialFlagsProcessed`), so a command a plugin adds never runs. */

/** Plugins are about to be loaded: record the primops Nix itself registered. */
void unforceNotePluginsLoading();

/** Nix itself adds `primOp` to some settings' `extraPrimOps`. Other entries are a plugin's. */
void unforceNoteOwnExtraPrimOp(const PrimOp & primOp);

/** Nix itself gives some settings `hooks`. Other hooks are a plugin's. */
void unforceNoteOwnLookupPathHooks(const EvalSettings::LookupPathHooks & hooks);

/** C API client code (a primop, an external value's callbacks) may now run in any evaluator. */
void unforceNoteCApiCode();

/** `state` was built by a C API host, whose code may run in it. */
void unforceNoteCApiState(EvalState & state);

/** Decide whether native code may run in `state`, just built. */
void unforceDecideNativeCode(EvalState & state);

/**
 * Why native code may run in `state`, or null; once non-null, it stays so. It
 * may if its primops or `extraPrimOps` are not exactly Nix's own (by name and
 * function), if it was built with `allow-unsafe-native-code-during-evaluation`,
 * or once C API client code appears.
 */
const char * unforceNativeCode(const EvalState & state);

/**
 * `settings`' lookup path hook for `scheme` is about to run. Returns whether it
 * is a plugin's, which the caller then runs under a `PauseRegion` and an
 * `UnforceBuiltinsCheck`; the open job, if any, is kept whole.
 */
bool unforceNoteLookupPathHook(const EvalSettings & settings, const std::string & scheme);

/**
 * Lives around a plugin's lookup path hook. If the hook changed the
 * evaluator's builtins (a base environment slot, a `builtins` attribute, a
 * primop or its function), native code may run in it from then on.
 */
class UnforceBuiltinsCheck
{
    EvalState & state;
    std::vector<const void *> before;

public:
    explicit UnforceBuiltinsCheck(EvalState & state);
    ~UnforceBuiltinsCheck();
};

} // namespace nix
