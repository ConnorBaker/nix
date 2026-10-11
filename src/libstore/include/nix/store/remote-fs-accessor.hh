#pragma once
///@file

#include "nix/util/source-accessor.hh"
#include "nix/util/ref.hh"
#include "nix/util/nar-cache.hh"
#include "nix/store/store-api.hh"

namespace nix {

class RemoteFSAccessor : public ForwardingSourceAccessor<RemoteFSAccessor>
{
    void anchor() override;

    ref<Store> store;

    /**
     * Map from store path hash part to the NAR cache's key: the object
     * hash, or the NAR hash a description asserted. The indirection
     * allows avoiding opening multiple redundant NAR accessors for the
     * same NAR.
     */
    std::map<std::string, Hash, std::less<>> hashes;

    NarCache narCache;

    bool requireValidPath;

    friend struct BinaryCacheStore;

public:

    /**
     * @return nullptr if the store does not contain any object at that path.
     *
     * @todo This actually doesn't return nullptr, but throws on invalid paths.
     */
    std::shared_ptr<SourceAccessor> accessObject(const StorePath & path);

    RemoteFSAccessor(ref<Store> store, bool requireValidPath = true, std::optional<AbsolutePath> cacheDir = {});

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    using ForwardingSourceAccessor<RemoteFSAccessor>::readFile;

    template<typename Read>
    decltype(auto) withSource(const CanonPath & path, Read && read)
    {
        auto [storePath, restPath] = store->toStorePath(store->storeDir + path.abs());
        if (requireValidPath && !store->isValidPath(storePath))
            throw InvalidPath("path '%1%' is not a valid store path", store->printStorePath(storePath));
        return std::invoke(std::forward<Read>(read), *ref{accessObject(storePath)}, restPath);
    }
};

} // namespace nix
