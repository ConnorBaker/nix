#pragma once

#include "nix/util/source-path.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <memory>
#include <set>
#include <unordered_set>

namespace nix {

/**
 * A function that returns an exception of type
 * `RestrictedPathError` explaining that access to `path` is
 * forbidden.
 */
typedef fun<RestrictedPathError(const CanonPath & path)> MakeNotAllowedError;

/**
 * An abstract wrapping `SourceAccessor` that performs access
 * control. Subclasses should override `isAllowed()` to implement an
 * access control policy. The error message is customized at construction.
 */
struct FilteringSourceAccessor : SourceAccessor
{
    ref<SourceAccessor> next;
    MakeNotAllowedError makeNotAllowedError;

    FilteringSourceAccessor(ref<SourceAccessor> next, MakeNotAllowedError && makeNotAllowedError)
        : next(std::move(next))
        , makeNotAllowedError(std::move(makeNotAllowedError))
    {
        displayPrefix.clear();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;

    using SourceAccessor::readFile;

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;

    /* No `pathExists` override: inherits `SourceAccessor::pathExists`
       which routes through `maybeLstat`. Derived classes that
       synthesise stats (e.g. `DirectorySynthesizerSourceAccessor`)
       therefore see their synthesis surface through `pathExists`
       without needing an explicit override. */

    Stat lstat(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    std::string readLink(const CanonPath & path) override;

    std::string showPath(const CanonPath & path) override;

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;

    void invalidateCache() override
    {
        next->invalidateCache();
    }

    void prefetchSubtree(const CanonPath & subpath, unsigned depth) override
    {
        /* Only worth prefetching if the wrapper currently allows
           reads under `subpath` — otherwise we'd fetch blobs that
           checkAccess will then forbid. The filter still runs on
           individual children; this is just a heuristic narrowing
           to avoid wasted fetches on disjoint subtrees. */
        if (isAllowed(subpath))
            next->prefetchSubtree(subpath, depth);
    }

    /**
     * Call `makeNotAllowedError` to throw a `RestrictedPathError`
     * exception if `isAllowed()` returns `false` for `path`.
     */
    void checkAccess(const CanonPath & path);

    /**
     * Return `true` iff access to path is allowed.
     */
    virtual bool isAllowed(const CanonPath & path) = 0;
};

/**
 * A wrapping `SourceAccessor` that checks paths against a set of
 * allowed prefixes.
 */
struct AllowListSourceAccessor : public FilteringSourceAccessor
{
    /**
     * Grant access to the specified prefix.
     */
    virtual void allowPrefix(CanonPath prefix) = 0;

    static ref<AllowListSourceAccessor> create(
        ref<SourceAccessor> next,
        const std::set<CanonPath> & allowedPrefixes,
        const std::unordered_set<CanonPath> & allowedPaths,
        MakeNotAllowedError && makeNotAllowedError);

    using FilteringSourceAccessor::FilteringSourceAccessor;
};

/**
 * A wrapping `SourceAccessor` mix-in where `isAllowed()` caches the result of virtual `isAllowedUncached()`.
 */
struct CachingFilteringSourceAccessor : FilteringSourceAccessor
{
    /* Concurrent under eval-cores>1 — repeated decisions on the same
       path are content-determined, so a benign duplicate compute
       between threads is preferable to lock contention. */
    boost::concurrent_flat_map<CanonPath, bool> cache;

    using FilteringSourceAccessor::FilteringSourceAccessor;

    bool isAllowed(const CanonPath & path) override;

    virtual bool isAllowedUncached(const CanonPath & path) = 0;
};

/**
 * A function that returns a `FileNotFound` exception for filtered-out paths.
 * Sibling of `MakeNotAllowedError`; use this when the wrapper's "filtered"
 * semantic is "the path doesn't exist in this view" (L4: Restrict admits S
 * plus ancestors of S, denying everything else with FileNotFound).
 *
 * `FilteringSourceAccessor::checkAccess` calls a `MakeNotAllowedError`-shaped
 * callback (it throws what the callback returns). Use `adaptToNotAllowed`
 * below to thread a `MakeNotFoundError` through that contract: it produces
 * a `[[noreturn]]` lambda that throws `FileNotFound` directly so the dummy
 * `RestrictedPathError` return value is never reached.
 */
typedef fun<FileNotFound(const CanonPath & path)> MakeNotFoundError;

/**
 * Adapter: produces a `MakeNotAllowedError`-shaped callback whose body
 * throws `FileNotFound` (built via `mkNotFound`) and never returns. Use this
 * to bind a `MakeNotFoundError` callback into the `FilteringSourceAccessor`
 * constructor without changing `checkAccess`'s contract.
 */
inline MakeNotAllowedError adaptToNotAllowed(MakeNotFoundError mkNotFound)
{
    return [mkNotFound = std::move(mkNotFound)](const CanonPath & p) -> RestrictedPathError { throw mkNotFound(p); };
}

/**
 * A `MakeNotAllowedError` for operators that **always admit** — `Translate`
 * (`isAllowed` ≡ true) and the `AdmitAll` `PathSetOp`s (`DirectorySynthesizer`,
 * `SoundnessGuard`). Their `checkAccess` can never reach the throw, so the
 * builder is dead; this sentinel makes that explicit (and `unreachable()`s if
 * an admission-shape change ever makes it live, turning a silent
 * latent-bug into a loud one). It is the default error builder on those
 * operators' factories so callers needn't thread one through.
 */
inline MakeNotAllowedError neverDenied()
{
    return [](const CanonPath &) -> RestrictedPathError { unreachable(); };
}

/**
 * Path-set predicate helpers used by `PathSetOp` subclasses
 * (`RestrictSourceAccessor`, `MaskSourceAccessor`, `SoundnessGuardSourceAccessor`).
 *
 * Definitions (S is a `std::set<CanonPath>`, p is a query path):
 *   - ancestorOfMember(S, p): ∃ s ∈ S . s.isWithin(p) ∧ s ≠ p — i.e. p
 *     is a strict ancestor of some member of S (root included).
 *   - memberOrDescendantOfMember(S, p): p ∈ S ∨ ∃ w ∈ S . p.isWithin(w) ∧ p ≠ w
 *     — Mask's "deny p plus everything beneath any whiteout" semantic.
 *   - inClosure(S, p): p ∈ S ∨ ancestorOfMember(S, p) ∨ ∃ s ∈ S . p.isWithin(s) ∧ p ≠ s
 *     — full closure (admit set ∪ ancestors ∪ descendants), used by fingerprint
 *     bypass per §4 (L7d, L10).
 */
namespace pathSet {
bool ancestorOfMember(const std::set<CanonPath> & s, const CanonPath & p);
bool memberOrDescendantOfMember(const std::set<CanonPath> & s, const CanonPath & p);
bool inClosure(const std::set<CanonPath> & s, const CanonPath & p);
} // namespace pathSet

/**
 * Algebraic shape of a `PathSetOp`'s factory under composition.
 *
 * Every `PathSetOp` operator is a **semilattice morphism** on path
 * sets — when two of the same kind stack, they fuse to a single
 * wrapper whose paths are the merge of the two. This enum names the
 * lattice flavour so the shared `makePathSetOp` factory can apply
 * the right merge operator.
 *
 *   - `Meet`: meet-semilattice on `(sets, ∩)`. Identity element is
 *     **universe** (encoded as the empty-set sentinel; per recipe
 *     convention `paths == ∅` means "admit everything"). Used by
 *     `Restrict`. If the intersection of two nested admit sets
 *     happens to be empty, the fused operator collapses to identity
 *     (returns `inner->next` directly).
 *   - `Join`: join-semilattice on `(sets, ∪)`. Identity element is
 *     **∅** (the empty set itself; "no paths to deny / no paths to
 *     synthesise / no paths to bypass"). Used by `Mask`,
 *     `SoundnessGuard`, and `DirectorySynthesizer`. The union of
 *     two non-empty sets is always non-empty, so no
 *     post-merge identity check is needed.
 */
enum class PathSetSemilattice {
    Meet,
    Join,
};

/**
 * Layer-1 admission shape of a `PathSetOp`.
 *
 * Captures the operator's `isAllowed` predicate as a finite
 * enumeration, so the shared `PathSetOp::isAllowed` body can switch
 * on it without needing per-operator method overrides. Adding a new
 * shape forces every `switch` in the codebase to handle it (no
 * `default:` is used).
 *
 *   - `AdmitAll`: always admit. Used by `Translate`-shaped operators
 *     where reads pass through unconditionally and Layer-1 is the
 *     identity. This includes `DirectorySynthesizer` and
 *     `SoundnessGuard` — neither gates reads on `paths`.
 *   - `AdmitClosure`: admit `path ∈ S ∪ ancestors-of-S ∪ {root}`.
 *     The closure-admit shape used by `Restrict` (a Prism whose
 *     accept domain is exactly this closure).
 *   - `DenyClosure`: admit `path ∉ S ∪ descendants-of-S`. The Prism
 *     dual used by `Mask` (deny-closure semantics).
 */
enum class PathSetAdmission {
    AdmitAll,
    AdmitClosure,
    DenyClosure,
};

/**
 * Layer-2 fingerprint shape of a `PathSetOp`.
 *
 * Captures the operator's `computeOwnSuffix` predicate as a finite
 * enumeration. Adding a new shape forces every `switch` to handle
 * it (no `default:` is used).
 *
 *   - `Identity`: `computeOwnSuffix` always returns the empty
 *     string — the operator is Layer-2 transparent. Used by
 *     `DirectorySynthesizer` (D11): synthesis is content-identity
 *     neutral, the tree's hash depends on `next`'s real bytes only.
 *   - `BypassInClosure`: bypass via `nullopt` for paths in
 *     `closure(S)` (S, ancestors, descendants). Used by `Restrict`,
 *     `Mask`, and `SoundnessGuard`. The closure is the algebraic
 *     boundary where the operator's own bytes / admission decision
 *     can affect the merged content's identity.
 */
enum class PathSetFingerprint {
    Identity,
    BypassInClosure,
};

/**
 * CRTP base for the path-set-parameterised operators
 * (`RestrictSourceAccessor`, `MaskSourceAccessor`,
 * `SoundnessGuardSourceAccessor`, `DirectorySynthesizerSourceAccessor`).
 *
 * Provides:
 *   - shared `paths` storage as `std::shared_ptr<const std::set<CanonPath>>`
 *     (immutable, COW-shareable, lockfree-readable);
 *   - `isAllowed` and `computeOwnSuffix` dispatched via exhaustive
 *     `switch` on `Derived::kAdmission` / `Derived::kFingerprint`.
 *     Empty `paths` is the identity element of the operator's
 *     monoid — both methods short-circuit transparent forward.
 *
 * Each `Derived` declares three `static constexpr` shape constants:
 *
 *   - `PathSetAdmission kAdmission`: drives `isAllowed` (Layer-1).
 *   - `PathSetFingerprint kFingerprint`: drives `computeOwnSuffix`
 *     (Layer-2).
 *   - `PathSetSemilattice kSemilattice`: drives the shared
 *     `makePathSetOp<Derived>` factory's fusion behaviour.
 *
 * Adding a new admission/fingerprint/semilattice shape forces every
 * `switch` site to handle it (no `default:` is used). The shape
 * constants are constexpr, so the inliner devirtualises through
 * `final` and the dispatch is compiled to direct branches.
 *
 * Derived classes may also override read/dir methods to add
 * synthesised behaviour on top of the base admission gating —
 * `DirectorySynthesizerSourceAccessor` is the canonical example
 * (override `maybeLstat`/`readDirectory` to add ancestor synthesis,
 * while still admitting everything via `kAdmission = AdmitAll`).
 */
template<typename Derived>
struct PathSetOp : FilteringSourceAccessor
{
    std::shared_ptr<const std::set<CanonPath>> paths;

    PathSetOp(ref<SourceAccessor> next, std::shared_ptr<const std::set<CanonPath>> ps, MakeNotAllowedError && err)
        : FilteringSourceAccessor(std::move(next), std::move(err))
        , paths(std::move(ps))
    {
    }

    bool isAllowed(const CanonPath & p) override
    {
        switch (Derived::kAdmission) {
        case PathSetAdmission::AdmitAll:
            return true;
        case PathSetAdmission::AdmitClosure:
            return paths->empty() || p.isRoot() || paths->contains(p) || pathSet::ancestorOfMember(*paths, p);
        case PathSetAdmission::DenyClosure:
            return paths->empty() || !pathSet::memberOrDescendantOfMember(*paths, p);
        }
        unreachable();
    }

    std::optional<std::string> computeOwnSuffix(const CanonPath & p) override
    {
        if (paths->empty())
            return std::string{};
        switch (Derived::kFingerprint) {
        case PathSetFingerprint::Identity:
            return std::string{};
        case PathSetFingerprint::BypassInClosure:
            return pathSet::inClosure(*paths, p) ? std::nullopt : std::optional<std::string>{std::string{}};
        }
        unreachable();
    }
};

/**
 * Shared factory for `PathSetOp` morphisms. Implements the algebra
 * once: identity short-circuit on the empty path-set, semilattice
 * fusion when stacking two of the same operator, fresh wrap
 * otherwise. Each `Derived` declares its lattice flavour as
 * `static constexpr PathSetSemilattice kSemilattice = …;` and
 * `makePathSetOp<Derived>` picks the right merge via a switch
 * (exhaustive — adding a new lattice flavour forces every dispatch
 * site to handle it).
 *
 * Inlined in the header so the `Derived` constructor is visible to
 * the `make_ref<Derived>(...)` calls below.
 */
template<typename Derived>
ref<SourceAccessor> makePathSetOp(
    ref<SourceAccessor> base, std::shared_ptr<const std::set<CanonPath>> paths, MakeNotAllowedError makeNotAllowedError)
{
    /* Identity element of the morphism. Both lattice flavours
       short-circuit empty-set to no-wrap; the recipe convention
       differs (Meet treats ∅ as universe; Join treats ∅ as ∅) but
       the operational consequence is the same: no admission gating,
       no Layer-2 bypass, transparent forward. */
    if (!paths || paths->empty())
        return base;

    /* Semilattice fusion: when stacking two of the same operator,
       merge their path-sets per the lattice flavour. */
    if (auto inner = base.dynamic_pointer_cast<Derived>()) {
        std::set<CanonPath> merged;
        switch (Derived::kSemilattice) {
        case PathSetSemilattice::Meet:
            /* Intersection. Per recipe convention, `paths == ∅`
               means universe — so an empty intersection collapses
               the entire wrapper pair to identity (return inner's
               next directly, dropping both Self wrappers). */
            for (auto & p : *paths)
                if (inner->paths->contains(p))
                    merged.insert(p);
            if (merged.empty())
                return inner->next;
            break;
        case PathSetSemilattice::Join:
            /* Union. Always non-empty when both inputs are non-empty
               (by the empty-short-circuit above), so no post-merge
               identity check is needed. */
            merged = *paths;
            merged.insert(inner->paths->begin(), inner->paths->end());
            break;
        }
        auto fused = std::make_shared<const std::set<CanonPath>>(std::move(merged));
        return make_ref<Derived>(inner->next, fused, std::move(makeNotAllowedError));
    }

    return make_ref<Derived>(std::move(base), std::move(paths), std::move(makeNotAllowedError));
}

} // namespace nix
