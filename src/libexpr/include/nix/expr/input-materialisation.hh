#pragma once
///@file
///
/// `InputMaterialisation` — backing object for the deferred
/// `narHash` thunk produced by `mountInput`.
///
/// **Why this class exists.** Today `mountInput` runs a synchronous
/// `fetchToStore2(... DryRun ...)` walk so it can stamp `input.attrs["narHash"]`
/// with a concrete SRI string. Many consumers of a flake input read only
/// `outPath` (or `outPath` plus `rev`) and never observe `narHash`; for
/// them, the dryRun walk is wasted work, and under blobless (partial) clone, it's the
/// difference between "fetch one tree" and "fetch every blob in the repo".
///
/// `InputMaterialisation` lets `mountInput` install a
/// `LazyAttr` thunk into `narHash` instead. Forcing the thunk drives
/// `force()` here, which performs at most one walk regardless of how
/// many virtual-narHash thunks share the same materialisation.
///
/// **Coalescing axis (vs `MaterialisationScheduler`).** The scheduler
/// coalesces walks across **distinct registration sites that share a
/// `SourceContentId`** — that's the cargo-workspace property. This
/// class coalesces walks **within a single registration**: many virtual
/// thunks of one input, one walk. These are different axes; the two
/// classes are deliberately decoupled (no inheritance, no shared
/// surface beyond `force()` returning `(StorePath, Hash)`).
///
/// **Honest scope of Item 2 alone.** `mountInput` still has to call
/// `force()` synchronously today to obtain the `storePath` (which it
/// must `allowPath()` and `mount` before returning). Item 2 in
/// isolation deduplicates walks across multiple consumers of `narHash`
/// for the same input but does **not** defer the walk — the wall-clock
/// win lands fully when Item 1 introduces virtual storePaths that
/// don't require `narHash` at mount time. See DEFERRED-WORK.md
/// §"R-new (Item 2-specific)" for the full risk register.

#include "nix/util/hash.hh"
#include "nix/util/ref.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/sync.hh"
#include "nix/store/path.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/fetchers/attrs.hh"

#include <future>
#include <optional>
#include <string>

namespace nix {

class Store;

namespace fetchers {
struct Settings;
}

/**
 * Backs the `narHash` thunk produced for a flake input by `mountInput`.
 *
 * Lifetime: each call to `mountInput` creates one of these for the
 * input's accessor; the resulting `ref<>` is owned both by the
 * `LazyAttr` closure (so it survives until the thunk is dropped) and
 * by `EvalState::inputMaterialisations_` (so callers can `peekNarHash()`
 * outside a value-force context). When `EvalState` is destroyed, both
 * holders drop the ref and the materialisation is collected.
 *
 * Phase transitions (taken under `state_.lock()`):
 *
 *     Pending --(first force, becomes winner)--> Running
 *     Running --(walk succeeds)--> Done   [terminal]
 *     Running --(walk throws)--> Failed   [terminal — re-raises on subsequent forces]
 *
 * Concurrent forcers see the future stored in `State::future` and
 * block on it; only the winner runs the walk. This mirrors
 * `MaterialisationScheduler::narHashOf` down to the winner-election
 * shape but keys on a single in-flight walk per instance — there is
 * no contentId-keyed sharing at this layer.
 *
 * Inherits `gc_cleanup` so Boehm GC walks `accessor` correctly when
 * the closure capture extends its lifetime past `mountInput`'s stack
 * frame; see `LazyFetcherAttr` for the parallel pattern in
 * `fetchTree.cc`.
 */
class InputMaterialisation : public gc_cleanup
{
public:
    enum class Phase { Pending, Running, Done, Failed };

    struct State
    {
        Phase phase = Phase::Pending;
        std::optional<StorePath> storePath; ///< Set in Done; the store path produced by the walk.
        std::optional<Hash> narHash;        ///< Set in Done; the NAR hash from the walk.
        std::shared_future<Hash> future;    ///< Live in Running; consumed by losers.
        std::exception_ptr error;           ///< Set in Failed; rethrown on every subsequent force().
    };

private:
    const fetchers::Settings & fetchSettings;
    ref<Store> store;
    ref<SourceAccessor> accessor;
    std::string name;                    ///< Input::getName() at construction; stable for the life of the mat.
    std::optional<Hash> expectedNarHash; ///< If set, force() throws on mismatch (mirrors paths.cc:117-123).
    Sync<State> state_;

public:
    InputMaterialisation(
        const fetchers::Settings & fetchSettings,
        ref<Store> store,
        ref<SourceAccessor> accessor,
        std::string name,
        std::optional<Hash> expectedNarHash);

    /**
     * Run the walk if not already done. Idempotent and exception-safe:
     * a failure in one call latches to Failed and every subsequent
     * call rethrows the same `error`. Concurrent callers share one
     * walk via `State::future`. Returns `(storePath, narHash)`; only
     * `narHash` is part of the virtual thunk's payload, but `storePath`
     * is computed by the same `fetchToStore2` invocation so we expose
     * it.
     */
    std::pair<StorePath, Hash> force();

    /**
     * Read the `narHash` without forcing. Returns:
     * - `nullopt` if Pending or Running;
     * - the resolved `Hash` if Done;
     * - throws the latched error if Failed (so `peekNarHash()` never
     *   silently swallows a known failure).
     *
     * Used by the eager-fallback path in `mountInput` when a caller
     * has already forced and we want to install a literal string
     * instead of a thunk.
     */
    std::optional<Hash> peekNarHash() const;
};

/**
 * Factory: build a `LazyAttr` whose body forces `mat`.
 *
 * The closure captures `mat` by `ref<>` so the materialisation
 * survives `mountInput`'s stack frame; `EvalState::inputMaterialisations_`
 * also retains it so callers can `peekNarHash()` from outside a
 * value-force context.
 *
 * The result is wrapped in `memo<>` so subsequent forces don't
 * re-enter `mat->force()`'s lock acquisition on the hot path. (Once
 * Done, `force()` itself short-circuits, but the memo avoids even
 * that one read-lock.)
 */
fetchers::LazyAttr makeVirtualNarHashAttr(ref<InputMaterialisation> mat);

} // namespace nix
