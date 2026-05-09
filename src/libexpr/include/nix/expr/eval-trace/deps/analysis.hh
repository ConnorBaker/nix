#pragma once
/// deps/analysis.hh — Pure analysis helpers over Dep / Dep::Key ranges.
///
/// These were methods on `TraceStore` but touch no store state — they
/// are pure functions of the input range and (for extractGitIdentityHash)
/// a target repo id. Exposing them as free functions lets the upcoming
/// Recorder and Verifier call them without depending on TraceStore.
///
/// Step 5 of rearchitecture-proposal.md §14.

#include "nix/expr/eval-trace/deps/types.hh"

#include <optional>
#include <vector>

namespace nix::eval_trace {

/// Scope discipline: these free fns take raw `std::vector<Dep>` / `Dep::Key`
/// ranges that do NOT carry `OriginScope<CurrentTrace|HistoricalCandidate>`
/// tagging. They are SOUND for any dep range because they branch only on
/// `dep.key` structural fields (kind, governingRepoId) — never on
/// `dep.hash`. Passing a raw dep vector is intentional; origin scoping
/// applies to hash-resolution paths (see `resolveDepHash`), not to
/// analysis over key material. Callers in Recorder / Verifier should
/// extract the underlying `std::vector<Dep>` from their tagged wrapper
/// (`OriginDep<O>::value()`) before passing it in.

/// Extract the governing repo id from a trace's deps. Returns the
/// governingRepoId of the GitRevisionIdentity dep (which IS the repo
/// itself by construction — see recordGitIdentityObservation). Returns
/// nullopt if no GitRevisionIdentity dep is present or the
/// GitIdentity dep has no governingRepoId stamped on it.
[[nodiscard]] std::optional<RepoRootId> extractGoverningRepoId(const std::vector<Dep> & deps);
[[nodiscard]] std::optional<RepoRootId> extractGoverningRepoId(const std::vector<Dep::Key> & keys);

/// Check if all deps in a trace are recoverable via git identity:
/// every file-content dep must carry the same `governingRepoId` as
/// the target, and no volatile / TraceContext deps present.
[[nodiscard]] bool allDepsGitRecoverable(const std::vector<Dep> & deps, RepoRootId targetRepoId);
[[nodiscard]] bool allDepsGitRecoverable(const std::vector<Dep::Key> & keys, RepoRootId targetRepoId);

/// Extract the git identity hash from a trace's deps for the History
/// index. Used at recording time (deps are current). Returns nullopt
/// if not eligible. Returns `StoredGitIdentityHash` to distinguish
/// from `CurrentGitIdentityHash` (computed from the live workdir);
/// BUG-1 — passing a stored hash where a current hash is expected is
/// a compile error.
[[nodiscard]] std::optional<StoredGitIdentityHash> extractGitIdentityHash(const std::vector<Dep> & deps);

} // namespace nix::eval_trace
