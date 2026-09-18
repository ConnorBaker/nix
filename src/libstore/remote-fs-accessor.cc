#include "nix/store/remote-fs-accessor.hh"

namespace nix {

void RemoteFSAccessor::anchor() {}

RemoteFSAccessor::RemoteFSAccessor(ref<Store> store, bool requireValidPath, std::optional<AbsolutePath> cacheDir)
    : store(store)
    , narCache(cacheDir)
    , requireValidPath(requireValidPath)
{
}

std::pair<ref<SourceAccessor>, CanonPath> RemoteFSAccessor::fetch(const CanonPath & path)
{
    auto [storePath, restPath] = store->toStorePath(store->storeDir + path.abs());
    if (requireValidPath && !store->isValidPath(storePath))
        throw InvalidPath("path '%1%' is not a valid store path", store->printStorePath(storePath));
    return {ref{accessObject(storePath)}, restPath};
}

std::shared_ptr<SourceAccessor> RemoteFSAccessor::accessObject(const StorePath & storePath)
{
    // Check if we already have the cache key for this store path
    if (auto * key = get(hashes, storePath.hashPart()))
        return narCache.getOrInsert(*key, [&](Sink & sink) { store->narFromPath(storePath, sink); });

    // Query the path info for the key: the object hash when the
    // description carries one, else the NAR hash it asserted.  Either
    // names the NAR's content; two keys for one NAR cost a second copy in
    // the cache and nothing else.
    auto info = store->queryPathInfo(storePath);
    auto key =
        info->objectHash ? info->objectHash->hash
        : info->assertedNarHash
            ? *info->assertedNarHash
            : throw Error("path '%s' has neither an object hash nor a NAR hash", store->printStorePath(storePath));

    // Cache the mapping from store path to key
    hashes.emplace(storePath.hashPart(), key);

    // Get or create the NAR accessor
    return narCache.getOrInsert(key, [&](Sink & sink) { store->narFromPath(storePath, sink); });
}

std::optional<SourceAccessor::Stat> RemoteFSAccessor::maybeLstat(const CanonPath & path)
{
    if (path.isRoot())
        return Stat{.type = tDirectory};
    /* FIXME: Correctly handle invalid names (return nullopt) and don't fail on
       non-existent paths. */
    auto res = fetch(path);
    return res.first->maybeLstat(res.second);
}

SourceAccessor::DirEntries RemoteFSAccessor::readDirectory(const CanonPath & path)
{
    auto res = fetch(path);
    return res.first->readDirectory(res.second);
}

void RemoteFSAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    auto res = fetch(path);
    res.first->readFile(res.second, sink, sizeCallback);
}

std::string RemoteFSAccessor::readLink(const CanonPath & path)
{
    auto res = fetch(path);
    return res.first->readLink(res.second);
}

} // namespace nix
