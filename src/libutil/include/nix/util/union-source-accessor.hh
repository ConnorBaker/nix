#pragma once
///@file

#include "nix/util/source-accessor.hh"

namespace nix {

/**
 * `Layer` (= `UnionSourceAccessor`): the `Alternative` (`<|>`)
 * combinator on accessors. Earlier accessors take precedence; reads
 * fall through to later accessors on a miss, and `readDirectory`
 * merges the children's listings (earlier wins on a name clash).
 *
 * This is the categorical **dual** of `SwitchSourceAccessor`
 * (`switch-source-accessor.hh`): Layer retries on a miss (fall-through),
 * whereas Switch commits to the longest-prefix-matching child
 * authoritatively. The two N-ary combinators are distinct; neither
 * reduces to the other. See `doc/tecnix-survey/PROPOSAL.md` §6.7.
 *
 * The struct is exposed here (rather than hidden in the `.cc`) so
 * verification tests can `dynamic_pointer_cast<UnionSourceAccessor>`
 * the result of `makeLayer` / `makeUnionSourceAccessor` and inspect the
 * resulting `accessors` vector — e.g. that `makeLayer` flattened nested
 * unions and short-circuited singletons. The factory free functions
 * (`makeUnionSourceAccessor`, `makeLayer`) stay declared in
 * `source-accessor.hh` alongside the other `make*` factories.
 */
struct UnionSourceAccessor : SourceAccessor
{
    std::vector<ref<SourceAccessor>> accessors;

    UnionSourceAccessor(std::vector<ref<SourceAccessor>> _accessors)
        : accessors(std::move(_accessors))
    {
        displayPrefix.clear();
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (st) {
                accessor->readFile(path, sink, sizeCallback);
                return;
            }
        }
        throw FileNotFound("path '%s' does not exist", showPath(path));
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (st)
                return st;
        }
        return std::nullopt;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        DirEntries result;
        bool exists = false;
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (!st)
                continue;
            exists = true;
            for (auto & entry : accessor->readDirectory(path))
                // Don't override entries from previous accessors.
                result.insert(entry);
        }
        if (!exists)
            throw FileNotFound("path '%s' does not exist", showPath(path));
        return result;
    }

    std::string readLink(const CanonPath & path) override
    {
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (st)
                return accessor->readLink(path);
        }
        throw FileNotFound("path '%s' does not exist", showPath(path));
    }

    std::string showPath(const CanonPath & path) override
    {
        /* Mirror the read resolution: show the path via the FIRST child
           that actually has it (that's the accessor a read would resolve
           to), so the displayed path matches where the bytes come from.
           Fall back to the first child for a universal miss (so an
           error message about a missing path still renders), and to the
           base default if there are no children. This is the Union
           analogue of `SwitchSourceAccessor::showPath`, which shows the
           matched mount's display rather than always the first.
           `showPath` must be total and must not throw (it formats error
           messages), so `maybeLstat` — not `lstat` — drives the choice. */
        for (auto & accessor : accessors)
            if (accessor->maybeLstat(path))
                return accessor->showPath(path);
        if (!accessors.empty())
            return accessors.front()->showPath(path);
        return SourceAccessor::showPath(path);
    }

    void invalidateCache() override
    {
        for (auto & accessor : accessors)
            accessor->invalidateCache();
    }

    void prefetchSubtree(const CanonPath & subpath, unsigned depth) override
    {
        /* Forward to the first accessor that knows about this path —
           that's the one whose reads we're about to do. */
        for (auto & accessor : accessors) {
            if (accessor->maybeLstat(subpath)) {
                accessor->prefetchSubtree(subpath, depth);
                return;
            }
        }
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        for (auto & accessor : accessors) {
            auto p = accessor->getPhysicalPath(path);
            if (p)
                return p;
        }
        return std::nullopt;
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        /* Union doesn't compose: it dispatches. Unlike Mounted /
           Filtering, Union has no own suffix to add to identity —
           there's no "concern" the Union itself contributes. So
           `composeFingerprint`'s "inner-claims-then-suffix" shape
           doesn't apply here.
         *
           The right rule is: return the first child whose
           `getFingerprint` actually has something to say. A child
           that lstats the path but doesn't carry content identity
           (`PosixSourceAccessor` over a real on-disk path that
           happens to live where a storeFS mount also serves) has
           nothing useful to contribute to the cache key, so we keep
           looking.
         *
           If no child has identity, fall back to the Union's own
           `fingerprint` field — that's the wrapper-fallback for
           workdir-style inputs where the input layer wrote the
           input-level fingerprint onto the Union itself. */
        for (auto & accessor : accessors) {
            auto [returnedPath, fp] = accessor->getFingerprint(path);
            if (fp)
                return {returnedPath, fp};
        }
        return {path, fingerprint};
    }
};

} // namespace nix
