#include "nix/util/source-accessor.hh"
#include "nix/util/finally.hh"
#include "nix/util/sync.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <type_traits>

namespace nix {

namespace {

class CachingSourceAccessor : public ForwardingSourceAccessor<CachingSourceAccessor>
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

    /* Keep the observation's result: no fingerprint is a cacheable fact,
       while no stat supplies no fact to cache. */
    template<typename Map, typename Observe>
    std::invoke_result_t<Observe> cached(Map Caches::* table, const CanonPath & path, Observe && observe)
    {
        using Value = typename Map::mapped_type;
        using Result = std::invoke_result_t<Observe>;
        std::shared_ptr<Caches> observed;
        {
            auto state = caches.lock();
            auto & entries = (**state).*table;
            if (auto i = entries.find(path); i != entries.end())
                return Result{i->second};
            observed = *state;
        }

        auto owner = weak_from_this().lock();
        auto value = observe();
        auto fact = [&]() -> const Value * {
            if constexpr (std::is_same_v<Result, Value>)
                return &value;
            else {
                static_assert(std::is_same_v<Result, std::optional<Value>>);
                return value ? &*value : nullptr;
            }
        }();
        if (fact) {
            auto state = caches.lock();
            ((*observed).*table).emplace(path, *fact);
        }
        return value;
    }

    void anchor() override {};

public:
    CachingSourceAccessor(ref<SourceAccessor> next_)
        : next(std::move(next_))
    {
        displayPrefix.clear();
    }

    template<typename Read>
    decltype(auto) withSource(const CanonPath & path, Read && read)
    {
        return std::invoke(std::forward<Read>(read), *next.get_ptr(), path);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        return cached(&Caches::lstat, path, [&] { return next->maybeLstat(path); });
    }

    Stat lstat(const CanonPath & path) override
    {
        return cached(&Caches::lstat, path, [&] { return next->lstat(path); });
    }

    std::string readLink(const CanonPath & path) override
    {
        return cached(&Caches::readLink, path, [&] { return next->readLink(path); });
    }

    void readDirectoryWithEntries(const CanonPath & path, ReadDirectoryCallback callback) override
    {
        next.get_ptr()->readDirectoryWithEntries(path, std::move(callback));
    }

    std::string showPath(const CanonPath & path) override
    {
        return next.get_ptr()->showPath(path);
    }

    template<typename Forward>
    void forget(Forward && forward)
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
        forward();
    }

    void invalidateCache() override
    {
        forget([&] { next->invalidateCache(); });
    }

    void releaseCaches() override
    {
        forget([&] { next->releaseCaches(); });
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return next.get_ptr()->getPhysicalPath(path);
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        return cached(&Caches::fingerprint, path, [&] { return next->getFingerprint(path); });
    }

    std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath & path) override
    {
        return next.get_ptr()->getKnownEntry(path);
    }
};

} // namespace

ref<SourceAccessor> makeCachingSourceAccessor(ref<SourceAccessor> next)
{
    return make_ref<CachingSourceAccessor>(std::move(next));
}

} // namespace nix
