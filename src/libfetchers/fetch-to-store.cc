#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/archive.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/store/local-store.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <map>
#include <mutex>

namespace nix {

struct SrcToStore
{
    boost::concurrent_flat_map<
        std::tuple<SourcePath, ContentAddressMethod::Raw, std::string>,
        std::tuple<StorePath, Hash, FetchMode>>
        cache;
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

void recordRootEntry(const fetchers::Settings & settings, const SourcePath & root, const Hash & objectHash)
{
    auto [subpath, fingerprint] = root.accessor->getFingerprint(root.path);
    auto cache = fingerprint ? cacheIfAvailable(settings) : nullptr;
    if (!cache)
        return;
    auto st = root.lstat();
    std::optional<merkle::Mode> mode;
    if (st.type == SourceAccessor::tDirectory)
        mode = merkle::Mode::Directory;
    else if (st.type == SourceAccessor::tRegular && !st.isExecutable)
        mode = merkle::Mode::Regular;
    if (mode)
        cache->upsert(
            makeSourcePathToHashCacheKey(*fingerprint, ContentAddressMethod::Raw::Git, subpath),
            entryRow({.mode = *mode, .hash = objectHash}));
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
    if (tree.accessor->getFingerprint(tree.path).second)
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

/* The subtree memo of 01 §9.9 as a visitor: `known` answers a named node
   from its row, the actions record the entry (mode and hash; doc/lazy-store/01-specification.md, the memo's rule).
   While a filter is in force a directory's name is neither used nor
   recorded, since the filter decides its entries; a symlink is read
   without a lookup, which would cost the same. */
namespace {

struct MemoVisitor : HashingVisitor
{
    const fetchers::Settings & settings;
    const SourcePath & root;
    bool filtered;
    std::shared_ptr<fetchers::Cache> cache;

    /* A name is computed once per node: the key found at the lookup is
       kept until the node is recorded (a filtered or grafted tree's name
       is over its structure, and is not free). */
    std::map<CanonPath, std::optional<fetchers::Cache::Key>> pending;

    MemoVisitor(const fetchers::Settings & settings, const SourcePath & root, bool filtered)
        : settings(settings)
        , root(root)
        , filtered(filtered)
    {
    }

    /* The walk's paths are relative to `root` and carry the on-disk names;
       the accessor is asked about `root.path / to`. */
    std::optional<fetchers::Cache::Key> keyOf(const CanonPath & to)
    {
        auto [subpath, fingerprint] = root.accessor->getFingerprint(root.path / to);
        if (!fingerprint)
            return std::nullopt;
        if (!cache && !(cache = cacheIfAvailable(settings)))
            return std::nullopt;
        return makeSourcePathToHashCacheKey(*fingerprint, ContentAddressMethod::Raw::Git, subpath);
    }

    /* The memo's answer for the node at `to`: its entry when the memo has
       a row under the node's name, under the memo's rule (a symlink is
       read without a lookup; a directory under a filter is never
       answered).  The key found is returned through `key`, for `record`.
       Also the copy route's `TreeNamer` (01 §9.10, the second route): the
       copy trusts what the naming trusted. */
    /* Whether the memo is consulted for a node of this kind at all: a
       symlink is read without a lookup, and a directory under a filter is
       never answered, since the filter decides its entries. */
    bool consulted(const SourceAccessor::Stat & st) const
    {
        return !(st.type == SourceAccessor::tSymlink || (st.type == SourceAccessor::tDirectory && filtered));
    }

    std::optional<merkle::TreeEntry>
    lookup(const CanonPath & to, const SourceAccessor::Stat & st, std::optional<fetchers::Cache::Key> * key = nullptr)
    {
        if (!consulted(st))
            return std::nullopt;
        auto k = keyOf(to);
        std::optional<merkle::TreeEntry> entry;
        if (k)
            if (auto res = cache->lookup(*k))
                entry = rowEntry(*res);
        if (key)
            *key = std::move(k);
        return entry;
    }

    std::optional<HashedNode> known(const CanonPath & to, const SourceAccessor::Stat & st) override
    {
        /* A node the memo is not consulted for leaves no pending key, so
           that `record` computes its key afresh (a symlink's row is
           written; a filtered directory's is not, by `directory`). */
        if (!consulted(st))
            return std::nullopt;
        std::optional<fetchers::Cache::Key> key;
        if (auto entry = lookup(to, st, &key))
            /* A directory's NAR size is unknown: nothing beneath it is
               seen.  A file's is the stat's. */
            return HashedNode{
                .entry = *entry,
                .narSize = st.type == SourceAccessor::tRegular && st.fileSize
                               ? std::optional(merkle::nar::regular(*st.fileSize, st.isExecutable))
                               : std::nullopt,
            };
        pending.insert_or_assign(to, std::move(key));
        return std::nullopt;
    }

    HashedNode record(const CanonPath & to, HashedNode node)
    {
        std::optional<fetchers::Cache::Key> key;
        if (auto it = pending.find(to); it != pending.end()) {
            key = std::move(it->second);
            pending.erase(it);
        } else
            key = keyOf(to);
        if (key)
            cache->upsert(*key, entryRow(node.entry));
        return node;
    }

    HashedNode regular(const CanonPath & to, fun<void(CreateRegularFileSink &)> read) override
    {
        return record(to, HashingVisitor::regular(to, std::move(read)));
    }

    HashedNode symlink(const CanonPath & to, const std::string & target) override
    {
        return record(to, HashingVisitor::symlink(to, target));
    }

    HashedNode directory(const CanonPath & to, Children children) override
    {
        auto node = HashingVisitor::directory(to, std::move(children));
        return filtered ? node : record(to, std::move(node));
    }
};

} // namespace

static merkle::TreeEntry
gitTreeHashMemoised(const fetchers::Settings & settings, const SourcePath & root, PathFilter & filter, bool filtered)
{
    MemoVisitor memo{settings, root, filtered};
    return objectHashOf(*root.accessor, root.path, filter, memo).root;
}

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
    auto srcToStoreKey = std::make_tuple(srcPath, method.raw, std::string(name));

    /* The in-memory cache is keyed by the path alone, so it applies only to
       unfiltered requests. */
    bool filtered = filter != nullptr;

    if (!filtered) {
        auto dstPathCached = getConcurrent(settings.srcToStore->cache, srcToStoreKey);
        if (dstPathCached && (mode == FetchMode::DryRun || std::get<2>(*dstPathCached) == FetchMode::Copy))
            return std::make_pair(std::get<0>(*dstPathCached), std::get<1>(*dstPathCached));
    }

    SourcePath path = srcPath;

    /* A filtered tree is named by the inner tree's name and the admitted
       set (01 §8.2), so the memo below applies to it; when the inner tree
       has no name the walk would be wasted, and the filter is applied while
       dumping instead. */
    if (filter && path.accessor->getFingerprint(path.path).second) {
        path = filteredTree(path, *filter);
        filter = nullptr;
    }

    std::optional<fetchers::Cache::Key> cacheKey;

    auto [subpath, fingerprint] = filter ? std::pair<CanonPath, std::optional<std::string>>{path.path, std::nullopt}
                                         : path.accessor->getFingerprint(path.path);

    if (fingerprint) {
        cacheKey = makeSourcePathToHashCacheKey(*fingerprint, method, subpath);
        if (auto cache = cacheIfAvailable(settings); cache)
            if (auto res = cache->lookup(*cacheKey)) {
                /* Under the git method the row is the root's entry and the
                   address its object hash; under the NAR method the hash. */
                auto hash = method == ContentAddressMethod::Raw::Git
                                ? merkle::objectHash(rowEntry(*res))
                                : Hash::parseSRI(fetchers::getStrAttr(*res, "hash"));
                auto storePath =
                    store.makeFixedOutputPathFromCA(name, ContentAddressWithReferences::fromParts(method, hash, {}));

                /* Add a temproot before the call to isValidPath to prevent accidental GC in case the
                   input is cached. Note that this must be done before to avoid races. */
                if (mode != FetchMode::DryRun)
                    store.addTempRoot(storePath);

                if (mode == FetchMode::DryRun || store.isValidPath(storePath)) {
                    debug(
                        "source path '%s' cache hit in '%s' (hash '%s')",
                        path,
                        store.printStorePath(storePath),
                        hash.to_string(HashFormat::SRI, true));
                    settings.srcToStore->cache.insert_or_assign(srcToStoreKey, std::make_tuple(storePath, hash, mode));
                    return {storePath, hash};
                }
                debug("source path '%s' not in store", path);
            }
    } else {
        static auto barf = getEnv("_NIX_TEST_BARF_ON_UNCACHEABLE").value_or("") == "1";
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
        mode == FetchMode::DryRun
            ? [&]() {
                  if (method == ContentAddressMethod::Raw::Git) {
                      /* The compositional name needs no serialisation: the
                         tree's hash, memoised subtree by subtree, forms the path. */
                      auto hash = merkle::objectHash(gitTreeHashMemoised(settings, path, filter2, filter != nullptr));
                      auto storePath = gitTreePath(store, name, hash);
                      debug(
                          "named '%s' as '%s' (tree hash '%s')",
                          path,
                          store.printStorePath(storePath),
                          hash.to_string(HashFormat::SRI, true));
                      return std::make_pair(storePath, hash);
                  }
                  // FIXME: we may have already computed this above.
                  auto [storePath, hash] =
                      store.computeStorePath(name, path, method, HashAlgorithm::SHA256, {}, filter2);
                  debug(
                      "hashed '%s' to '%s' (hash '%s')",
                      path,
                      store.printStorePath(storePath),
                      hash.to_string(HashFormat::SRI, true));
                  return std::make_pair(storePath, hash);
              }()
            : [&]() {
                  // FIXME: ideally addToStore() would return the hash
                  // right away (like computeStorePath()).
                  auto storePath = [&] {
                      /* The second route of 01 §9.10, and the one place it is
                         decided: under the git method, a local store that
                         materialises from its own objects copies what the
                         naming names and the store holds by `linkat` alone,
                         reading from the accessor only what the store lacks;
                         the namer is the naming's own memo lookup.  Every
                         other case is the one route, a NAR dump into
                         `addToStoreFromDump`. */
                      if (method == ContentAddressMethod::Raw::Git)
                          if (auto local = dynamic_cast<LocalStore *>(&store);
                              local && local->materialisesFromObjects()) {
                              MemoVisitor memo{settings, path, filter != nullptr};
                              return local->materialise(
                                  name,
                                  path,
                                  filter2,
                                  repair,
                                  [&](const CanonPath & to, const SourceAccessor::Stat & st) {
                                      return memo.lookup(to, st);
                                  });
                          }
                      return store.addToStore(name, path, method, HashAlgorithm::SHA256, {}, filter2, repair);
                  }();
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
                  return std::make_pair(storePath, hash);
              }();

    act.result(resFetchToStore, store.printStorePath(storePath));

    /* The row: under the git method the root's entry, from the object hash
       (the dry run's visitor wrote it already; the copy route has the hash
       alone), the hash itself under the NAR method. */
    if (cacheKey) {
        if (method == ContentAddressMethod::Raw::Git)
            recordRootEntry(settings, path, hash);
        else if (auto cache = cacheIfAvailable(settings))
            cache->upsert(*cacheKey, {{"hash", hash.to_string(HashFormat::SRI, true)}});
    }

    if (!filtered)
        settings.srcToStore->cache.insert_or_assign(srcToStoreKey, std::make_tuple(storePath, hash, mode));

    return {storePath, hash};
}

} // namespace nix
