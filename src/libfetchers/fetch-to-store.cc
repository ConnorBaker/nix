#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/archive.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/store/store-api.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <mutex>
#include <utility>

namespace nix {

struct SrcToStore
{
    /* A fingerprint names the current view. Accessor identity does not:
       grafts and allow lists can grow without replacing their accessor.
       The address also depends on the logical store directory and output
       name, and directory folds depend on the name-decoding policy.
       This memo retains neither sources nor stores, and an address
       makes no claim about its presence in the requested store. */
    using Key = std::tuple<std::string, CanonPath, ContentAddressMethod::Raw, std::string, std::string, bool>;
    using Result = std::pair<StorePath, Hash>;
    boost::concurrent_flat_map<Key, Result> cache;
};

ref<SrcToStore> fetchers::Settings::createSrcToStore()
{
    return make_ref<SrcToStore>();
}

fetchers::Cache::Key
makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod method, const CanonPath & path)
{
    fetchers::Attrs attrs{
        {"fingerprint", std::string(fingerprint)}, {"method", std::string{method.render()}}, {"path", path.abs()}};
    if (method == ContentAddressMethod::Raw::Git)
        attrs.insert_or_assign("version", uint64_t(2));
    if (method.getFileSerialisationMethod() == FileSerialisationMethod::NixArchive)
        attrs.insert_or_assign("caseHack", uint64_t(isCaseHackEnabled()));
    return fetchers::Cache::Key{"sourcePathToHash", std::move(attrs)};
}

/* A git row: the node's entry, mode and hash. */
static fetchers::Attrs entryRow(const merkle::TreeEntry & entry)
{
    return {{"hash", entry.hash.to_string(HashFormat::SRI, true)}, {"mode", uint64_t(entry.mode)}};
}

static merkle::TreeEntry rowEntry(const fetchers::Attrs & row)
{
    auto mode = merkle::decodeMode(fetchers::getIntAttr(row, "mode"));
    if (!mode)
        throw Error("memo row holds an unknown mode %o", fetchers::getIntAttr(row, "mode"));
    return {.mode = *mode, .hash = Hash::parseSRI(fetchers::getStrAttr(row, "hash"))};
}

static fetchers::Cache::Key makeTreeAddressCacheKey(const Hash & narHash)
{
    return fetchers::Cache::Key{"treeAddress", {{"narHash", narHash.to_string(HashFormat::SRI, true)}}};
}

std::shared_ptr<fetchers::Cache> cacheIfAvailable(const fetchers::Settings & settings)
{
    try {
        return settings.getCache().get_ptr();
    } catch (Error & e) {
        static std::once_flag once;
        std::call_once(once, [&] { debug("fetcher cache unavailable, memos disabled: %s", e.msg()); });
        return nullptr;
    }
}

std::optional<Hash> lookupTreeAddress(const fetchers::Settings & settings, const Hash & narHash)
{
    if (auto cache = cacheIfAvailable(settings))
        if (auto res = cache->lookup(makeTreeAddressCacheKey(narHash)))
            return Hash::parseSRI(fetchers::getStrAttr(*res, "hash"));
    return std::nullopt;
}

void recordTreeAddress(const fetchers::Settings & settings, const Hash & narHash, const Hash & treeHash)
{
    if (auto cache = cacheIfAvailable(settings))
        cache->upsert(makeTreeAddressCacheKey(narHash), {{"hash", treeHash.to_string(HashFormat::SRI, true)}});
}

StorePath gitTreePath(const StoreDirConfig & store, std::string_view name, const Hash & treeHash)
{
    return store.makeFixedOutputPathFromCA(
        name, ContentAddressWithReferences::fromParts(ContentAddressMethod::Raw::Git, treeHash, {}));
}

SourcePath filteredTree(const SourcePath & tree, PathFilter & filter)
{
    return SourcePath(makeFixedSetFilteringSourceAccessor(tree, filteredPaths(*tree.accessor, tree.path, filter)));
}

static void recordRootEntry(
    fetchers::Cache & cache, const fetchers::Cache::Key & key, const SourcePath & root, const Hash & objectHash)
{
    if (auto mode = root.lstat().directObjectMode())
        cache.upsert(key, entryRow({.mode = *mode, .hash = objectHash}));
}

void recordRootEntry(const fetchers::Settings & settings, const SourcePath & root, const Hash & objectHash)
{
    auto identity = root.accessor->getFingerprint(root.path);
    if (auto cache = identity ? cacheIfAvailable(settings) : nullptr)
        recordRootEntry(
            *cache,
            makeSourcePathToHashCacheKey(identity->second, ContentAddressMethod::Raw::Git, identity->first),
            root,
            objectHash);
}

Hash narHashOf(const fetchers::Settings & settings, Store & store, const SourcePath & tree, const Hash & treeHash)
{
    /* The dry run under `NixArchive` is the memoised walk: the in-process
       memo by the path, the `sourcePathToHash` row by the tree's name.  The
       name passed shapes only the store path, not the hash. */
    auto narHash =
        fetchToStore2(settings, store, tree, FetchMode::DryRun, "source", ContentAddressMethod::Raw::NixArchive).second;

    /* The pair is recorded only for a named tree, since then both hashes
       came from the memo of that name; an unnamed tree could change between
       the two walks, and the row would outlive the mistake. */
    if (tree.accessor->getFingerprint(tree.path))
        recordTreeAddress(settings, narHash, treeHash);
    return narHash;
}

Error inputHashMismatch(std::string_view kind, std::string_view input, const Hash & expected, const Hash & got)
{
    return Error(
        (unsigned int) 102,
        "%s mismatch in input '%s', expected '%s' but got '%s'",
        kind,
        input,
        expected.to_string(HashFormat::SRI, true),
        got.to_string(HashFormat::SRI, true));
}

void assertNarHash(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & tree,
    const Hash & treeHash,
    const Hash & asserted,
    std::string_view inputDescription)
{
    /* The memo answers first: a pair recorded from one named tree's two
       memoised hashes needs no walk. */
    if (auto paired = lookupTreeAddress(settings, asserted); paired && *paired == treeHash)
        return;
    auto narHash = narHashOf(settings, store, tree, treeHash);
    if (narHash != asserted)
        throw inputHashMismatch("NAR hash", inputDescription, asserted, narHash);
}

/* Subtree naming shared by the memo visitor and copy route (01 §9.9–9.10):
   the visitor answers named rows or records descended entries (mode and hash).
   While a filter is in force a directory's name is neither used nor
   recorded, since the filter decides its entries; a symlink is read
   without a lookup, which would cost the same. */
namespace {

struct MemoNamer
{
    const fetchers::Settings & settings;
    const SourcePath & root;
    const std::optional<std::pair<CanonPath, std::string>> & rootName;
    bool filtered;
    bool wholeViewFingerprint =
        rootName && root.accessor->fingerprint && rootName->second == *root.accessor->fingerprint;
    std::shared_ptr<fetchers::Cache> cache{};

    /* Only named facts need synchronization. Once initialized, `cache`
       never changes; unnamed worktree observations take no memo lock. */
    std::mutex mutex{};

    /* The walk's paths are relative to `root` and carry the on-disk names;
       the accessor is asked about `root.path / to`. */
    std::optional<fetchers::Cache::Key> keyOf(const CanonPath & to, const CanonPath & sourcePath)
    {
        /* A name for the entire view does not identify unchanged children
           across revisions. Memoise the requested subtree, without eagerly
           constructing a persistent row for every descendant. Object-level
           names (e.g. SHA-1 Git entries) still reuse their subtree rows. */
        if (wholeViewFingerprint && !to.isRoot())
            return std::nullopt;
        auto keyFor = [&](const auto & name) -> std::optional<fetchers::Cache::Key> {
            if (!name)
                return std::nullopt;
            const auto & [subpath, fingerprint] = *name;
            std::lock_guard lock(mutex);
            if (!cache && !(cache = cacheIfAvailable(settings)))
                return std::nullopt;
            return makeSourcePathToHashCacheKey(fingerprint, ContentAddressMethod::Raw::Git, subpath);
        };
        /* Reuse this operation's root observation, including no name.
           Descendants have their own names, independent of the root's. */
        return to.isRoot() ? keyFor(rootName) : keyFor(root.accessor->getFingerprint(sourcePath));
    }

    /* The memo's answer for the node at `to`: its entry when the memo has
       a row under the node's name, under the memo's rule (a symlink is
       read without a lookup; a directory under a filter is never
       answered). The key found is returned through `key`, for recording.
       Also the copy route's `TreeNamer` (01 §9.10, the second route): the
       copy trusts what the naming trusted. */
    /* Whether the memo is consulted for a node of this kind at all: a
       symlink is read without a lookup, and a directory under a filter is
       never answered, since the filter decides its entries. */
    std::optional<merkle::TreeEntry>
    lookup(const CanonPath & to, const SourceAccessor::Stat & st, std::optional<fetchers::Cache::Key> * key = nullptr)
    {
        if (st.type == SourceAccessor::tSymlink || (st.type == SourceAccessor::tDirectory && filtered))
            return std::nullopt;
        auto lookupAt = [this, &to, key](const CanonPath & sourcePath) -> std::optional<merkle::TreeEntry> {
            if (auto entry = root.accessor->getKnownEntry(sourcePath))
                return entry;
            auto k = keyOf(to, sourcePath);
            if (!k)
                return std::nullopt;
            auto entry = cache->lookup(*k).transform(rowEntry);
            if (key)
                *key = std::move(k);
            return entry;
        };
        /* At the accessor root the walk already owns the source path. */
        return root.path.isRoot() ? lookupAt(to) : lookupAt(root.path / to);
    }
};

struct MemoVisitor : HashingVisitor, MemoNamer
{
    /* The rows this walk has named but not yet written.  They go in one
       commit rather than one apiece (`01` section 9.9, the memo's rule says
       what a row means, not when it is written; `05` section 6 measures the
       per-row commit).  The bound keeps a walk of a large tree from holding
       them all: a row is about two hundred bytes, so this is a few
       megabytes.  A walk that throws writes nothing it had not flushed,
       which costs the next walk exactly the work this one did. */
    static constexpr size_t maxPendingWrites = 16384;
    std::vector<std::pair<fetchers::Cache::Key, fetchers::Attrs>> writes;

    MemoVisitor(
        const fetchers::Settings & settings,
        const SourcePath & root,
        bool filtered,
        const std::optional<std::pair<CanonPath, std::string>> & rootName,
        PathFilter & filter)
        : HashingVisitor(workerCountFor(*root.accessor, filter))
        , MemoNamer{settings, root, rootName, filtered}
    {
    }

    HashedNode node(
        SourceAccessor &,
        const CanonPath &,
        const CanonPath & to,
        const SourceAccessor::Stat & st,
        function_ref<HashedNode()> descend) override
    {
        std::optional<fetchers::Cache::Key> key;
        /* Symlinks are named only when recorded; filtered directories
           are neither answered nor recorded. A hit has no node action. */
        auto entry = lookup(to, st, &key);
        /* This private visitor returns only the entry; it never publishes a NAR size. */
        auto result = entry ? HashedNode{.entry = *entry, .narSize = std::nullopt} : descend();
        if (!entry) {
            result.narSize.reset();
            if (st.type == SourceAccessor::tSymlink)
                key = keyOf(to, root.path / to);
            if (key) {
                std::lock_guard lock(mutex);
                writes.emplace_back(std::move(*key), entryRow(result.entry));
                if (writes.size() >= maxPendingWrites)
                    flush();
            }
        }
        return result;
    }

    merkle::TreeEntry hashTree(PathFilter & filter)
    {
        auto entry = traverse(*root.accessor, root.path, filter, *this).entry;
        flush();
        return entry;
    }

    /* Writes are excluded by mutex during the walk, or by joining all
       visits before the final flush. */
    void flush()
    {
        if (writes.empty())
            return;
        cache->upsert(writes);
        writes.clear();
    }
};

} // namespace

StorePath fetchToStore(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name,
    std::optional<ContentAddressMethod> method,
    PathFilter * filter,
    RepairFlag repair)
{
    return fetchToStore2(settings, store, path, mode, name, method, filter, repair).first;
}

std::pair<StorePath, Hash> fetchToStore2(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & srcPath,
    FetchMode mode,
    std::string_view name,
    std::optional<ContentAddressMethod> methodRequested,
    PathFilter * filter,
    RepairFlag repair)
{
    auto method = methodRequested.value_or(ContentAddressMethod::Raw::Git);

    SourcePath path = srcPath;

    /* A filtered tree is named by the inner tree's name and the admitted
       set (01 §8.2), so both memos below apply to it; when the inner tree
       has no name the walk would be wasted, and the filter is applied while
       dumping instead. */
    if (filter && path.accessor->getFingerprint(path.path)) {
        path = filteredTree(path, *filter);
        filter = nullptr;
    }

    const auto rootName = filter ? std::nullopt : path.accessor->getFingerprint(path.path);
    std::optional<SrcToStore::Key> srcToStoreKey;
    if (rootName && !repair)
        srcToStoreKey.emplace(
            rootName->second,
            rootName->first,
            method.raw,
            store.storeDir,
            name,
            (method.getFileSerialisationMethod() == FileSerialisationMethod::NixArchive) && isCaseHackEnabled());

    /* Naming and destination presence are separate facts. Once an address
       is known, another naming cache cannot make that destination present. */
    std::optional<SrcToStore::Result> named;
    if (srcToStoreKey)
        named = getConcurrent(settings.srcToStore->cache, *srcToStoreKey);

    auto fromHash = [&](const Hash & hash) {
        SrcToStore::Result result{
            store.makeFixedOutputPathFromCA(name, ContentAddressWithReferences::fromParts(method, hash, {})), hash};
        if (srcToStoreKey)
            settings.srcToStore->cache.emplace(*srcToStoreKey, result);
        return result;
    };

    if (!named && !filter && !repair && method == ContentAddressMethod::Raw::Git)
        if (auto entry = path.accessor->getKnownEntry(path.path))
            named = fromHash(merkle::objectHash(*entry));

    std::optional<fetchers::Cache::Key> cacheKey;
    if (!named && rootName && !repair) {
        cacheKey = makeSourcePathToHashCacheKey(rootName->second, method, rootName->first);
        if (auto cache = cacheIfAvailable(settings))
            if (auto res = cache->lookup(*cacheKey))
                named = fromHash(
                    method == ContentAddressMethod::Raw::Git ? merkle::objectHash(rowEntry(*res))
                                                             : Hash::parseSRI(fetchers::getStrAttr(*res, "hash")));
    }

    if (named) {
        /* Root before checking validity: a name does not establish presence
           in this store, nor protect it against collection. */
        if (mode != FetchMode::DryRun)
            store.addTempRoot(named->first);
        if (mode == FetchMode::DryRun || store.isValidPath(named->first)) {
            if (cacheKey)
                debug(
                    "source path '%s' cache hit in '%s' (hash '%s')",
                    path,
                    store.printStorePath(named->first),
                    named->second.to_string(HashFormat::SRI, true));
            return std::move(*named);
        }
        if (cacheKey)
            debug("source path '%s' not in store", path);
        named.reset();
    }
    if (!cacheKey && rootName && !repair)
        cacheKey = makeSourcePathToHashCacheKey(rootName->second, method, rootName->first);
    if (!rootName || repair) {
        static auto barf = getEnv("_NIX_TEST_BARF_ON_UNCACHEABLE") == "1";
        if (barf && !filter)
            throw Error("source path '%s' is uncacheable (filter=%d)", path, (bool) filter);
        debug("source path '%s' is uncacheable", path);
    }

    Activity act(
        *logger,
        lvlChatty,
        actFetchToStore,
        fmt(mode == FetchMode::DryRun ? "hashing '%s'" : "copying '%s' to the store", path),
        std::to_array<Logger::Field>({path.to_string(), (uint64_t) (mode == FetchMode::DryRun)}));

    auto filter2 = filter ? *filter : defaultPathFilter;

    auto [storePath, hash] =
        mode == FetchMode::DryRun ? [&]() {
            if (method == ContentAddressMethod::Raw::Git) {
                /* Repair reads the tree itself, bypassing known entries
                   and subtree memo rows as well as the root caches. */
                auto entry = repair ? objectHashOf(*path.accessor, path.path, filter2).root
                                    : MemoVisitor{settings, path, filter != nullptr, rootName, filter2}.hashTree(filter2);
                auto hash = merkle::objectHash(entry);
                auto storePath = gitTreePath(store, name, hash);
                debug(
                    "named '%s' as '%s' (tree hash '%s')",
                    path,
                    store.printStorePath(storePath),
                    hash.to_string(HashFormat::SRI, true));
                return std::make_pair(std::move(storePath), hash);
            }
            // FIXME: we may have already computed this above.
            auto [storePath, hash] = store.computeStorePath(name, path, method, HashAlgorithm::SHA256, {}, filter2);
            debug(
                "hashed '%s' to '%s' (hash '%s')",
                path,
                store.printStorePath(storePath),
                hash.to_string(HashFormat::SRI, true));
            return std::make_pair(std::move(storePath), hash);
        }()
                                  : [&]() {
                                        // FIXME: ideally addToStore() would return the hash
                                        // right away (like computeStorePath()).
                                        MemoNamer memo{settings, path, rootName, filter != nullptr};
                                        auto storePath = store.addToStore(
                                            name,
                                            path,
                                            method,
                                            HashAlgorithm::SHA256,
                                            {},
                                            filter2,
                                            repair,
                                            [&](const CanonPath & to, const SourceAccessor::Stat & st) {
                                                return repair ? std::nullopt : memo.lookup(to, st);
                                            },
                                            /*callbacksPermitParallelism=*/true);
                                        auto info = store.queryPathInfo(storePath);
                                        assert(info->references.empty());
                                        if (!info->ca || info->ca->method != method)
                                            throw Error("path '%s' lacks a CA field", store.printStorePath(storePath));
                                        auto hash = info->ca->hash;
                                        printMsg(
                                            lvlChatty,
                                            "copied source '%s' -> '%s' (hash '%s')",
                                            path,
                                            store.printStorePath(storePath),
                                            hash.to_string(HashFormat::SRI, true));
                                        return std::make_pair(std::move(storePath), hash);
                                    }();

    act.result(resFetchToStore, store.printStorePath(storePath));

    /* The row: under the git method the root's entry, from the object hash
       (the dry run's visitor wrote it already; the copy route has the hash
       alone), the hash itself under the NAR method. */
    if (cacheKey) {
        if (method == ContentAddressMethod::Raw::Git) {
            if (mode == FetchMode::Copy)
                if (auto cache = cacheIfAvailable(settings))
                    recordRootEntry(*cache, *cacheKey, path, hash);
        } else if (auto cache = cacheIfAvailable(settings))
            cache->upsert(*cacheKey, {{"hash", hash.to_string(HashFormat::SRI, true)}});
    }

    if (srcToStoreKey)
        settings.srcToStore->cache.emplace(*srcToStoreKey, SrcToStore::Result{storePath, hash});

    return {std::move(storePath), hash};
}

} // namespace nix
