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

/**
 * `v`'s force just failed in an open scope: recorded, so that the job's end
 * makes the failure forget the job's cells.
 */
[[gnu::noinline, gnu::cold]] void unforceNoteFailure(const Value & v) noexcept;

/**
 * An effect that running again could give another result (a download, a
 * copy that nothing remembers, a build) is about to run: the open job, if
 * any, keeps the value of the cell whose re-force would run it again.
 * Nothing runs the driver's evaluation (no cell's force) again.
 */
void unforceNoteEffect(EvalState & state);

/** How a read can change in an evaluation (`unforceReadChange`); ordered, as `std::max` combines two. */
enum class UnforceChange { none, byEdit, byEvaluation };

/**
 * How reading `path` (its symlinks resolved as the reader does) in a job's cell can change (else `none`): not at all
 * if the allow list lets it be read and it lies in a store path that is valid (then held by a temporary root) or
 * mounted from a snapshot or a store object; by the evaluation's own builds, copies and allowances if it is not
 * allowed yet or lies elsewhere in the store, unless the host has it where the store keeps its objects elsewhere;
 * else (outside the store, in a work tree mounted there, or on the host) by an edit.
 */
UnforceChange unforceReadChange(EvalState & state, const SourcePath & path);

/** An operation whose result is not checked keeps its cell if it reads what may change. */
inline void unforceNoteRead(EvalState & state, const SourcePath & path)
{
    if (unforceReadChange(state, path) != UnforceChange::none)
        unforceNoteEffect(state);
}

/* A file read in this evaluation changed before it was read again. Not an
   `Error`, which printers and front ends may report and go on from. */
MakeError(UnforceFileChanged, BaseError);

/**
 * A read whose result is checked (`readFile`, `hashFile`, `readDir`, `pathExists`, the bytes `scopedImport` parses, a
 * copy without a filter). In a job of a one-shot evaluation, the first result of a read that only an edit can change
 * is recorded with where the path led and keeps no cell: every later read by the operation of the path as given
 * (before its symlinks are resolved) must lead there and give it again, or the evaluation stops with
 * `UnforceFileChanged` (exit status 102). So a re-force reads what the first force read. A read that the evaluation
 * itself may change keeps its cell, as does any in the REPL.
 */
struct UnforceRead
{
    EvalState & state;
    std::string_view op;
    /** The path as given, once known: the key. */
    std::optional<SourcePath> given;
    /** Where it led (`at`), recorded with the result. */
    std::string resolved;
    /** Whether where it leads may change: a link led it there, or its accessor caches no `lstat` (only `rootFS` does). */
    bool linked = false;
    UnforceChange change = UnforceChange::none;

    UnforceRead(EvalState & state, std::string_view op, std::optional<SourcePath> given = std::nullopt)
        : state(state)
        , op(op)
        , given(std::move(given))
    {
    }

    /** The path as given led to `path`: the read changes as the more restricted of the two. */
    SourcePath at(SourcePath path);

    /** A step of the read (its resolution too): one that fails where a result was recorded changed. */
    auto operator()(auto && read)
    {
        try {
            return read();
        } catch (Error &) {
            if (key(false))
                changed();
            throw;
        }
    }

    /** `v` realised (`EvalState::realisePath`), its symlinks resolved as `mode` says. */
    SourcePath realise(Value & v, std::optional<SymlinkResolution> mode = SymlinkResolution::Full)
    {
        return at((*this)([&] {
            return state.realisePath(noPos, v, mode, EvalState::CopyLazyPaths::PreserveLazy, &given);
        }));
    }

    /** `feed` writes what the read gave; `changes`: whether that can change. */
    void check(fun<void(Sink &)> feed, bool changes = true);

private:
    std::optional<std::string> key(bool changes);
    [[noreturn]] void changed();
};

/**
 * `path` resolved as `mode` says, for an operation (`op`) remembered by where the path led (a copy without a filter,
 * `storePath`): an `UnforceRead` of the resolution, recorded only where that may change (`UnforceRead::linked`).
 */
SourcePath unforceResolve(EvalState & state, std::string_view op, const SourcePath & path, SymlinkResolution mode);

/**
 * Lives around an operation that reads the file system, the store or the
 * network: the cell keeps its value if it fails (its error might not repeat),
 * or where the file caches are reset (`EvalState::fileCachesReset`). What the
 * evaluator caches once it succeeds (imports, copies without a filter, lookup
 * paths, fetched inputs, `lstat`) keeps nothing more.
 */
struct UnforceImpure
{
    EvalState & state;
    int failing = std::uncaught_exceptions();

    explicit UnforceImpure(EvalState & state)
        : state(state)
    {
        if (state.fileCachesReset)
            unforceNoteEffect(state);
    }

    ~UnforceImpure()
    {
        if (std::uncaught_exceptions() > failing)
            unforceNoteEffect(state);
    }
};

/** The caller keeps something it built in the open job outside any cell: the job, if any, keeps all its memory. */
void unforceKeepJob();

/**
 * `allocRootValue` of `v`, in the open job's memory: while the root lives,
 * the job's end keeps what it points to (an `UnforceHolder`).
 */
RootValue unforceHeldRoot(Value * v);

class UnforceSeen;

/**
 * C++ state that points into the open job's memory from outside any cell (a
 * printer's set of the containers it has printed, the derivations that
 * `getDerivations` found). While it is registered (from its construction to
 * its destruction), a job's end keeps what it points to, as it keeps what a
 * promoted value reaches, but for what a seen set only compares by address
 * (`Hold::identity`).
 */
class UnforceHolder
{
public:
    enum class Kind { value, bindings };

    struct Hold
    {
        virtual ~Hold();
        /** Keep `p`, a `Value *` or a `Bindings *`. */
        virtual void operator()(const void * p, Kind kind) = 0;
        /**
         * `p`, a `Bindings *` that `set` only compares by address: kept, or
         * forgotten by `set` until a re-force builds what stands for it
         * (unforce.cc, Promotion).
         */
        virtual void identity(const void * p, UnforceSeen & set) = 0;
    };

    UnforceHolder();
    virtual ~UnforceHolder();
    UnforceHolder(const UnforceHolder &) = delete;
    UnforceHolder & operator=(const UnforceHolder &) = delete;

    /**
     * At a job's end: pass each pointer into the job's memory that this
     * holds to `hold`; then `ended`. Allocates nothing.
     */
    virtual void held(Hold & hold) = 0;

    /** A job ended: nothing this holds is in a job's memory any more. */
    virtual void ended() = 0;

    /**
     * The highest address in the open job's memory that this holds, or
     * null: a loop's step does not free memory at or below it.
     */
    const void * youngTop = nullptr;

    /**
     * At a job's end: whether `cell`, older than the job, which forced it,
     * keeps its value, and with it what the value reaches.
     */
    virtual bool keeps(const Value * cell) const
    {
        return false;
    }

    /** `cell`, which a job's end reverted, was just forced again. */
    virtual void rebuilt(const Value & cell) {}
};

/**
 * A set of the containers a traversal has seen, by address (to print
 * «repeated», or to drop duplicates), that stays exact across jobs: its
 * entries in the open job's memory are held, but for a set that is the
 * value of a cell a job's end reverts, which it forgets until the cell's
 * re-force puts the set it builds in its place.
 */
class UnforceSeen : public std::set<const void *>, public UnforceHolder
{
    std::vector<std::pair<const void *, Kind>> young;
    /* The reverted cells whose re-force gives an entry back. */
    std::set<const Value *> owners;

public:
    /** A record of `owners`, made before a job's end writes anything. */
    using Owner = std::set<const Value *>::node_type;

    /** Add `p`, of `kind`; returns whether it was not there yet. */
    bool see(const void * p, Kind kind);
    void held(Hold & hold) override;
    void ended() override;
    void rebuilt(const Value & cell) override;
    /** Forget `p` until the re-force of `owner`'s cell. Allocates nothing. */
    void forget(const void * p, Owner owner);
};

/**
 * The loop whose element starts: an `UnforceJobs` loop or a printer's
 * (`other`), an `UnforceJobs` loop that declared it nests, the strict JSON
 * printer's, or `getDerivations`'. A level nested in a traversal's job is
 * one of its levels only if its loop is of the kind of the one whose element
 * opened the job (see `unforceLevelElement`).
 */
enum class UnforceLoop : unsigned char { other, nests, json, drvs };

[[gnu::noinline]] void unforceJobsBegin(EvalState & state, const Value * root);
[[gnu::noinline]] void unforceJobsEnd();
[[gnu::noinline]] void unforceElementStart(UnforceLoop loop = UnforceLoop::other);
[[gnu::noinline]] void unforceNestedElementStart(EvalState & state, const Value * v, UnforceLoop loop);

/**
 * Unforce jobs for a traversal of independent elements (the derivations of
 * a package set, the attributes that a printer prints): each element runs
 * in a job, which ends when the next element starts if memory in use rose
 * or the job holds more than any one element took (else that element runs
 * in it too) or when this ends, unless the traversal itself runs in a job.
 * The traversal is a loop
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

    /**
     * An element of the caller's loop starts. `nests`: the loop reads nothing
     * a job allocated across its elements but through held roots, cells it
     * forces again, and copies, so that it may be a nested level.
     */
    [[gnu::always_inline]] void element(bool nests = false)
    {
        unforceElementStart(nests ? UnforceLoop::nests : UnforceLoop::other);
    }
};

/** An element of `v` starts, in a level of a traversal (`UnforceJobs`). */
[[gnu::always_inline]] inline void unforceElement(EvalState & state, const Value & v)
{
    if (state.unforceRoot == &v) [[unlikely]]
        unforceElementStart();
}

/**
 * `unforceElement` for a `loop` whose levels may nest (unforce.cc, "Nested
 * levels"): a level iterates `v`'s attributes or list (or a copy's), reads
 * nothing else across its elements but copies, and never `v` after its
 * first element, which a switch may have reverted.
 */
[[gnu::always_inline]] inline void unforceLevelElement(EvalState & state, const Value & v, UnforceLoop loop)
{
    if (state.unforceRoot == &v) [[unlikely]]
        unforceElementStart(loop);
    else if (loop != UnforceLoop::other && evalRegion.cur) [[unlikely]]
        unforceNestedElementStart(state, &v, loop);
}

/**
 * The steps of a primop's loop that hands a value on from step to step
 * (`foldl'`'s accumulator). Between steps the loop holds nothing that its
 * steps allocated but the cell it hands on: the one `cell` gave, or the one
 * `step` returns. In a job, the steps end in batches, and a batch's end
 * frees its memory when no record shows a pointer into it (see unforce.cc).
 */
class UnforceSteps
{
    EvalState & state;
    bool open;

    [[gnu::noinline]] bool begin();
    [[gnu::noinline]] Value * cellSlow(Value * given);
    [[gnu::noinline]] Value * stepSlow(Value * v);
    [[gnu::noinline]] void end();

public:
    [[gnu::always_inline]] explicit UnforceSteps(EvalState & state)
        : state(state)
        , open(!NIX_USE_BOEHMGC && evalRegion.base && begin())
    {
    }

    [[gnu::always_inline]] ~UnforceSteps()
    {
        if (open) [[unlikely]]
            end();
    }

    UnforceSteps(const UnforceSteps &) = delete;
    UnforceSteps & operator=(const UnforceSteps &) = delete;

    /**
     * The cell a step writes its value to: `given`, or else a new one. The
     * step starts once it is returned.
     */
    [[gnu::always_inline]] Value * cell(Value * given)
    {
        return open ? cellSlow(given) : given ? given : state.allocValue();
    }

    /**
     * A step ended, its value in `v`, the cell `cell` returned. Returns the
     * cell that holds the value from now on.
     */
    [[gnu::always_inline]] Value * step(Value * v)
    {
        return open ? stepSlow(v) : v;
    }
};

/** The value that a thunk of `expr` and `env` stands for if it is a trapped cell (unforce.cc, "Elements"), else null. */
const Value * unforceTrapped(const Expr * expr, const Env * env);

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
    uint64_t stepEnds = 0, stepBytesFreed = 0;
};

extern UnforceStats unforceStats;

/* Boolean frames (unforce.cc). `unforceFrame`: the innermost open frame's
   start, and the lowest cell older than it written with a pointer at or
   above it. `unforceFrames`: each open frame's start, and the lowest index
   of the open frames a write marked. `unforceFrameSpan`: what a frame must
   span to give anything back. `unforceFailures` ticks at every failed force
   and every frame an exception leaves. */
struct UnforceFrameMark
{
    const char * s;
    int64_t markLo;
};

extern std::vector<UnforceFrameMark> unforceFrames;
extern size_t unforceFrameSpan;
extern uint64_t unforceFailures;

[[gnu::noinline, gnu::cold]] void unforceBoolFrameEnd(EvalState & state, uint64_t job, uint64_t fails);

/**
 * An in-place boolean evaluation (`EvalState::evalBool`: `if`, `assert`,
 * `&&`, `||`, `->`, `!`) in a job: a frame, whose end may give back what the
 * current owner allocated in it.
 */
class UnforceBoolFrame
{
    bool on;
    uint64_t job = 0, fails = 0;
    UnforceFrameRecord saved;

    /* Back to the enclosing frame's record, with this one's merged in. */
    [[gnu::always_inline]] void close()
    {
        unforceFrame = {std::min(saved.lowC, unforceFrame.lowC), saved.s};
        auto m = unforceFrames.back().markLo;
        unforceFrames.pop_back();
        if (m < int64_t(unforceFrames.size()))
            unforceFrames.back().markLo = std::min(unforceFrames.back().markLo, m);
    }

public:
    [[gnu::always_inline]] UnforceBoolFrame(bool on, const char * cur)
        : on(on)
    {
        if (on) {
            unforceFrames.push_back({cur, INT64_MAX});
            job = unforceStats.jobs;
            fails = unforceFailures;
            saved = std::exchange(unforceFrame, {(const char *) UINTPTR_MAX, cur});
        }
    }

    /** The evaluation in `state` gave a Boolean, the bump cursor at `cur`. */
    [[gnu::always_inline]] void done(EvalState & state, const char * cur)
    {
        if (on) {
            on = false;
            if (size_t(cur - unforceFrame.s) >= unforceFrameSpan)
                unforceBoolFrameEnd(state, job, fails);
            close();
        }
    }

    [[gnu::always_inline]] ~UnforceBoolFrame()
    {
        if (on) {
            unforceFailures++;
            close();
        }
    }

    UnforceBoolFrame(const UnforceBoolFrame &) = delete;
    UnforceBoolFrame & operator=(const UnforceBoolFrame &) = delete;
};

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
