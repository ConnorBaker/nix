#pragma once

#include "nix/util/source-path.hh"
#include "nix/util/sync.hh"

#include <memory>
#include <set>
#include <unordered_set>

#include <boost/unordered/concurrent_flat_map_fwd.hpp>

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
struct FilteringSourceAccessor : ForwardingSourceAccessor<FilteringSourceAccessor>
{
    ref<SourceAccessor> next;
    CanonPath prefix;
    MakeNotAllowedError makeNotAllowedError;

    FilteringSourceAccessor(const SourcePath & src, MakeNotAllowedError && makeNotAllowedError)
        : next(src.accessor)
        , prefix(src.path)
        , makeNotAllowedError(std::move(makeNotAllowedError))
    {
        displayPrefix.clear();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;

    using ForwardingSourceAccessor<FilteringSourceAccessor>::readFile;

    template<typename Read>
    decltype(auto) withSource(const CanonPath & path, Read && read)
    {
        auto owner = checkAllowed(path);
        return std::invoke(std::forward<Read>(read), *next, prefix / path);
    }

    bool pathExists(const CanonPath & path) override;

    Stat lstat(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    /** Scope a view that only filters access and forwards the bytes.
     * Subclasses that change reads must retain their own traversal. */
    void readDirectoryScoped(const CanonPath & path, ReadDirectoryCallback callback);

    std::string showPath(const CanonPath & path) override;

    void invalidateCache() override
    {
        auto inner = next;
        inner->invalidateCache();
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

private:
    /* Admission retains shared receivers; stack-scoped receivers remain borrowed. */
    std::shared_ptr<SourceAccessor> checkAllowed(const CanonPath & path);
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
 * A wrapping `SourceAccessor` admitting exactly a fixed, prefix-closed
 * set of paths relative to `src.path` (`filteredPaths`), named by the
 * inner tree's name and the admitted set (01 §7.1, §8.2). The root is
 * `CanonPath::root`; entries are not rebased or discarded by this factory.
 * Dumping it without a filter yields the NAR the filter would.
 */
ref<SourceAccessor> makeFixedSetFilteringSourceAccessor(const SourcePath & src, std::set<CanonPath> accepted);

/**
 * A wrapping `SourceAccessor` mix-in where `isAllowed()` caches the result of virtual `isAllowedUncached()`.
 */
struct CachingFilteringSourceAccessor : FilteringSourceAccessor
{
private:
    using Decisions = boost::concurrent_flat_map<CanonPath, bool>;

    /**
     * The current view's decisions. A decision is recorded in the map that
     * was current when it was looked up, so one computed across an
     * invalidation lands in the retired map, never in its replacement.
     */
    SharedSync<std::shared_ptr<Decisions>> decisions;

public:
    CachingFilteringSourceAccessor(const SourcePath & src, MakeNotAllowedError && makeNotAllowedError);

    bool isAllowed(const CanonPath & path) override;

    /**
     * Also forget the decisions, which `isAllowedUncached` recomputes.
     * The empty map is published once the child has been invalidated,
     * as `makeCachingSourceAccessor`'s view does.
     */
    void invalidateCache() override;

    virtual bool isAllowedUncached(const CanonPath & path) = 0;
};

} // namespace nix
