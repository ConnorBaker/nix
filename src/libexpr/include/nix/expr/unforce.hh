#pragma once
///@file

/* Unforcing (unforce.cc): the cold parts. The hot ones (`EvalRegion`,
   `logPreImage`) are in eval.hh. */

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"

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
 * copy that nothing remembers, a build) or show again (a trace, a warning)
 * ran: the open job, if any, keeps its result.
 */
void unforceNoteEffect();

/** Whether a thunk of `expr` is a cell that unforcing reverted. */
bool unforceIsReverted(const Expr * expr);

/** `Value::determinePos` of a thunk of `expr`: a reverted cell's value's position, or `pos`. */
[[gnu::noinline, gnu::cold]] PosIdx unforceRevertedPos(const Expr * expr, PosIdx pos);

/** Whether unforcing has reverted a cell in this process. */
extern bool unforceAnyReverted;

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
