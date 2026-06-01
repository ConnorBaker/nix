#pragma once
///@file

#include "nix/util/source-accessor.hh"

#include <map>

namespace nix {

/**
 * `Switch`: the longest-prefix-match combinator on accessors. A query
 * path is routed to the *nearest ancestor* mount point and that
 * accessor is **authoritative** — if it doesn't have the path, the
 * lookup commits to that miss; it does NOT fall through to a
 * shorter-prefix mount. The chosen accessor sees the path with the
 * mount prefix stripped (`path.removePrefix(mountPoint)`), exactly as
 * `StripPrefix(mountPoint)` would compute it.
 *
 * `Switch` is the categorical **dual** of `Layer` (= `UnionSourceAccessor`,
 * `union-source-accessor.hh`):
 *
 *   - `Layer` is `Alternative` (`<|>`): ordered children, **retry on a
 *     miss** (fall-through), `readDirectory` merges listings.
 *   - `Switch` is longest-prefix-**commit**: the matching child owns its
 *     subtree; a miss is final; listings come from the single owner.
 *
 * The two N-ary combinators are distinct — neither reduces to the
 * other. A naive `Layer([StripPrefix(p_i)(a_i)])` would differ from a
 * `Switch` exactly when a shorter-prefix child *could* serve a path the
 * nearest child misses (e.g. `{/ → A with /a/b/c, /a/b → B empty}`:
 * `Switch.read(/a/b/c)` is not-found, but the Layer falls through to
 * `A` and finds it). Authoritative-commit is the correct mount
 * semantics — a filesystem mounted at `/mnt` shadows whatever lay
 * beneath it. See `doc/tecnix-survey/PROPOSAL.md` §1.3.
 *
 * The per-mount identity that DOES hold: a single mount's path-action
 * is exactly `StripPrefix(p)(a)` (no shadowing is possible with one
 * mount), including a byte-identical `getFingerprint` — both `Switch`
 * and `StripPrefix` have empty `computeOwnSuffix` and an unset
 * `fingerprint`, so `composeFingerprint` threads the same stripped
 * inner path and agrees. So `Switch = ⊕_{longest-prefix} StripPrefix(p_i)(a_i)`
 * per branch, but it is NOT a `Layer`. The strip is implemented inline
 * here (pure `CanonPath::removePrefix`, so it needs no dependency on
 * the libfetchers `StripPrefixSourceAccessor`); the equivalence is
 * pinned by test.
 *
 * `SwitchSourceAccessor` holds no mount storage itself — it defines the
 * shared resolve + strip-and-dispatch for every read method and defers
 * the mount lookup to the pure-virtual `getMount`. `makeSwitch` builds
 * an immutable instance (fixed `std::map`); `MountedSourceAccessor`
 * (see below) is the mutable face, adding runtime `mount()`.
 */
struct SwitchSourceAccessor : SourceAccessor
{
    /**
     * Return the accessor mounted exactly at `mountPoint`, or `nullptr`
     * if there is no such mount point. (Exact-key lookup, not a prefix
     * search — `resolve` walks up the path calling this per ancestor.)
     */
    virtual std::shared_ptr<SourceAccessor> getMount(CanonPath mountPoint) = 0;

    /**
     * Invoke `fn(mountPoint, accessor)` for every mount whose mount
     * point is a STRICT descendant of `subpath` (i.e. `subpath` is a
     * proper ancestor of `mountPoint`). Used only by `prefetchSubtree`
     * to reach sub-mounts (e.g. Git submodules) that the
     * `subpath`-owning accessor cannot see. Default is a no-op — a
     * `Switch` that can't cheaply enumerate its mounts simply forgoes
     * the sub-mount prefetch (correctness is unaffected; those blobs
     * fall back to per-read on-demand fetching). The concrete immutable
     * / mutable Switches override it.
     */
    virtual void forEachMountUnder(const CanonPath & subpath, fun<void(const CanonPath &, SourceAccessor &)> fn) {}

    /* Re-expose the `std::string readFile(path)` convenience overload
       from the base; declaring the 3-arg form below would otherwise
       hide it. */
    using SourceAccessor::readFile;
    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;

    Stat lstat(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    std::string readLink(const CanonPath & path) override;

    std::string showPath(const CanonPath & path) override;

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;

    /* `depth = 1` mirrors the base default so callers through the
       `Switch`/`Mounted` type can still invoke the 1-arg form. */
    void prefetchSubtree(const CanonPath & subpath, unsigned depth = 1) override;

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;

protected:

    /**
     * Find the nearest parent of `path` that is a mount point. Returns
     * the mounted accessor and the mount point itself (so callers can
     * `path.removePrefix(mountPoint)` to get the stripped subpath). A
     * root mount is required (see `makeSwitch`), so this always
     * terminates with a match.
     */
    std::pair<ref<SourceAccessor>, CanonPath> resolveMount(CanonPath path);
};

/**
 * Construct an **immutable** `Switch` over a fixed set of mounts.
 * Requires a root mount (`CanonPath::root`) so every path resolves.
 *
 * Use this for mount sets known fully at construction time (e.g. a Git
 * superproject + its submodules). For a mount set that grows at
 * runtime, use `makeMountedSourceAccessor` (`mounted-source-accessor.hh`),
 * the mutable face of the same combinator.
 */
ref<SourceAccessor> makeSwitch(std::map<CanonPath, ref<SourceAccessor>> mounts);

} // namespace nix
