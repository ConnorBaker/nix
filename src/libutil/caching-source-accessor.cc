#include "nix/util/source-accessor.hh"
#include "nix/util/finally.hh"
#include "nix/util/sync.hh"

#include <boost/unordered/unordered_flat_map.hpp>

namespace nix {

namespace {

class CachingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> next;

    struct Caches
    {
        boost::unordered_flat_map<CanonPath, Stat> lstat;
        boost::unordered_flat_map<CanonPath, std::string> readLink;
        boost::unordered_flat_map<CanonPath, std::optional<std::pair<CanonPath, std::string>>> fingerprint;
    };

    /* A miss retains the observed state while calling the child. If that
       call invalidates this view, its answer only populates the retired
       state, never the new view's cache. No child runs under the lock. */
    Sync<std::shared_ptr<Caches>> caches{std::make_shared<Caches>()};

    /* An observation may supply a cacheable fact. The value can itself be
       optional: no fingerprint is a fact, while no stat is not. */
    template<typename Map, typename Observe>
    std::optional<typename Map::mapped_type>
    cached(Map Caches::* table, const CanonPath & path, Observe && observe)
    {
        std::shared_ptr<Caches> observed;
        {
            auto state = caches.lock();
            auto & entries = (**state).*table;
            if (auto i = entries.find(path); i != entries.end())
                return std::optional<typename Map::mapped_type>{std::in_place, i->second};
            observed = *state;
        }

        auto owner = weak_from_this().lock();
        auto fact = observe();
        if (fact) {
            auto state = caches.lock();
            ((*observed).*table).emplace(path, *fact);
        }
        return fact;
    }

    void anchor() override {};

public:
    CachingSourceAccessor(ref<SourceAccessor> next_)
        : next(std::move(next_))
    {
        displayPrefix.clear();
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto inner = next;
        inner->readFile(path, sink, sizeCallback);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        return cached(&Caches::lstat, path, [&] { return next->maybeLstat(path); });
    }

    Stat lstat(const CanonPath & path) override
    {
        return *cached(&Caches::lstat, path, [&] { return std::make_optional(next->lstat(path)); });
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto inner = next;
        return inner->readDirectory(path);
    }

    std::string readLink(const CanonPath & path) override
    {
        return *cached(&Caches::readLink, path, [&] { return std::make_optional(next->readLink(path)); });
    }

    void readDirectoryWithEntries(
        const CanonPath & path,
        std::function<void(SourceAccessor &, const CanonPath &, DirectoryListing &&)> callback) override
    {
        auto inner = next;
        inner->readDirectoryWithEntries(path, std::move(callback));
    }

    std::string showPath(const CanonPath & path) override
    {
        auto inner = next;
        return inner->showPath(path);
    }

    void invalidateCache() override
    {
        auto owner = weak_from_this().lock();
        auto replacement = std::make_shared<Caches>();
        /* Observations made while the child invalidates still belong to
           the old view. Publish the empty cache on completion, including
           exceptional completion, after those child caches are dropped. */
        Finally publish{[&] {
            auto state = caches.lock();
            state->swap(replacement);
        }};
        next->invalidateCache();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        auto inner = next;
        return inner->getPhysicalPath(path);
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        return *cached(&Caches::fingerprint, path, [&] { return std::make_optional(next->getFingerprint(path)); });
    }

    std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath & path) override
    {
        auto inner = next;
        return inner->getKnownEntry(path);
    }
};

} // namespace

ref<SourceAccessor> makeCachingSourceAccessor(ref<SourceAccessor> next)
{
    return make_ref<CachingSourceAccessor>(std::move(next));
}

} // namespace nix
