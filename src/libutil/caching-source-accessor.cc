#include "nix/util/source-accessor.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

namespace {

class CachingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> next;

    boost::concurrent_flat_map<CanonPath, Stat> lstatCache;
    boost::concurrent_flat_map<CanonPath, std::string> readLinkCache;

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
        if (auto res = getConcurrent(lstatCache, path))
            return *res;

        auto owner = weak_from_this().lock();
        auto st = next->maybeLstat(path);
        if (!st)
            return std::nullopt;

        /* Never evict, the evaluator better keep positive lookups cached. */
        lstatCache.emplace(path, *st);
        return st;
    }

    Stat lstat(const CanonPath & path) override
    {
        if (auto res = getConcurrent(lstatCache, path))
            return *res;

        auto owner = weak_from_this().lock();
        auto st = next->lstat(path);
        /* Never evict, the evaluator better keep positive lookups cached. */
        lstatCache.emplace(path, st);
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
        if (auto res = getConcurrent(readLinkCache, path))
            return *res;

        auto owner = weak_from_this().lock();
        auto target = next->readLink(path);
        /* Never evict, the evaluator better keep positive lookups cached. */
        readLinkCache.emplace(path, target);
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
        lstatCache.clear();
        readLinkCache.clear();
        auto inner = next;
        inner->invalidateCache();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        auto inner = next;
        return inner->getPhysicalPath(path);
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        auto inner = next;
        return inner->getFingerprint(path);
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
