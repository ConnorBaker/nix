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
        std::shared_ptr<Caches> observed;
        {
            auto state = caches.lock();
            if (auto i = (*state)->lstat.find(path); i != (*state)->lstat.end())
                return i->second;
            observed = *state;
        }

        auto owner = weak_from_this().lock();
        auto st = next->maybeLstat(path);
        if (!st)
            return std::nullopt;

        /* Never evict, the evaluator better keep positive lookups cached. */
        {
            auto state = caches.lock();
            observed->lstat.emplace(path, *st);
        }
        return st;
    }

    Stat lstat(const CanonPath & path) override
    {
        std::shared_ptr<Caches> observed;
        {
            auto state = caches.lock();
            if (auto i = (*state)->lstat.find(path); i != (*state)->lstat.end())
                return i->second;
            observed = *state;
        }

        auto owner = weak_from_this().lock();
        auto st = next->lstat(path);
        /* Never evict, the evaluator better keep positive lookups cached. */
        {
            auto state = caches.lock();
            observed->lstat.emplace(path, st);
        }
        return st;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto inner = next;
        return inner->readDirectory(path);
    }

    void readDirectory(
        const CanonPath & dirPath,
        std::function<void(SourceAccessor & subdirAccessor, const CanonPath & subdirRelPath)> callback) override
    {
        auto inner = next;
        return inner->readDirectory(dirPath, std::move(callback));
    }

    std::string readLink(const CanonPath & path) override
    {
        std::shared_ptr<Caches> observed;
        {
            auto state = caches.lock();
            if (auto i = (*state)->readLink.find(path); i != (*state)->readLink.end())
                return i->second;
            observed = *state;
        }

        auto owner = weak_from_this().lock();
        auto target = next->readLink(path);
        /* Never evict, the evaluator better keep positive lookups cached. */
        {
            auto state = caches.lock();
            observed->readLink.emplace(path, target);
        }
        return target;
    }

    void readDirectoryWithEntries(
        const CanonPath & path,
        std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback) override
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
        std::shared_ptr<Caches> observed;
        {
            auto state = caches.lock();
            if (auto i = (*state)->fingerprint.find(path); i != (*state)->fingerprint.end())
                return i->second;
            observed = *state;
        }

        auto owner = weak_from_this().lock();
        auto identity = next->getFingerprint(path);
        {
            auto state = caches.lock();
            /* No identity is not a claim that the node is absent. */
            observed->fingerprint.emplace(path, identity);
        }
        return identity;
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
