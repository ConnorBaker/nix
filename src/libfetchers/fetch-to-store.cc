#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/filtered-shape.hh"
#include "nix/fetchers/projection.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/fingerprint.hh"
#include "nix/util/hash.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

/* Per-process in-memory memo of content-keyed walk results.
 *
 * Two maps, one per cache shape:
 *   - filteredNarHashMemo: (fp, method, subpath, shapeHash) → narHash.
 *     Populated by the filtered-shape branch.
 *   - sourceNarHashMemo: (fp, method, subpath) → narHash. Populated
 *     by the unfiltered branch.
 *
 * The cargo-workspace optimisation: 200 packages calling
 * fetchToStore2 with the same source + same filter would otherwise
 * pay a SQLite roundtrip per call. With this memo, 199 of them are
 * hash-table hits. SQLite still backs cross-process sharing.
 *
 * Process-lifetime; never invalidated. Soundness: the keys are
 * content-determined, so equal keys imply equal narHashes by the
 * projection law. The map can grow unbounded over a long-lived
 * process, but each entry is small (~100 bytes) and the realistic
 * worst case is "one entry per unique source × filter shape" which
 * scales with the user's source diversity, not eval count. */
static boost::concurrent_flat_map<std::string, Hash> filteredNarHashMemo;
static boost::concurrent_flat_map<std::string, Hash> sourceNarHashMemo;

static std::string makeMemoKey(
    std::string_view fingerprint, std::string_view method, std::string_view subpath, std::string_view shapeHash = {})
{
    /* NUL-separated; none of the fields legitimately contain NULs. */
    std::string k;
    k += fingerprint;
    k.push_back('\0');
    k += method;
    k.push_back('\0');
    k += subpath;
    k.push_back('\0');
    k += shapeHash;
    return k;
}

/* Sibling of `SourceAccessor::dumpPath` that runs the user filter on
 * each child entry (root is always included, matching `dumpPath`'s and
 * `addPath`'s existing semantics) and accumulates a content-free shape
 * digest plus the accepted-path set.
 *
 * The shape digest covers what NAR canonicalisation observes and the
 * filter could affect: paths, types, executable bit on regular files,
 * symlink targets. File contents are intentionally excluded — the
 * source fingerprint already covers content identity. Directory
 * boundaries are framed with begin/end markers so two filtered
 * subsets that accept different boundary configurations cannot
 * collide. */

namespace {

void writeShape(HashSink & sink, std::string_view tag, std::string_view payload = {})
{
    /* Length-prefix tags and payloads so distinct structure produces
       distinct bytes. */
    auto writeU64 = [&](uint64_t n) {
        char buf[8];
        for (int i = 0; i < 8; ++i)
            buf[i] = (n >> (8 * i)) & 0xff;
        sink(std::string_view(buf, sizeof(buf)));
    };
    writeU64(tag.size());
    sink(tag);
    writeU64(payload.size());
    sink(payload);
}

void walkShape(
    SourceAccessor & accessor,
    const CanonPath & path,
    const CanonPath & relative,
    PathFilter & filter,
    HashSink & sink,
    boost::unordered_flat_set<CanonPath> & accepted)
{
    auto st = accessor.lstat(path);
    writeShape(sink, "path", relative.abs());

    switch (st.type) {
    case SourceAccessor::tRegular:
        writeShape(sink, st.isExecutable ? "exec" : "regular");
        break;
    case SourceAccessor::tSymlink:
        writeShape(sink, "symlink", accessor.readLink(path));
        break;
    case SourceAccessor::tDirectory: {
        writeShape(sink, "dir");
        for (auto & [name, _] : accessor.readDirectory(path)) {
            auto childAbs = path / name;
            CanonPath childRel = relative / name;
            /* The user-facing PathFilter contract (master's
               `addPath`) is to call with the absolute path string
               the user-supplied lambda would see — i.e. the path on
               the source accessor, not a relative-to-root rebase.
               Otherwise pure-eval `restrictEval` rejects the call
               because `/config.nix` isn't in the allowlist. */
            if (!filter(childAbs.abs()))
                continue;
            /* Track accepted paths in *absolute* form so the
               accepted-set filter we hand to addToStore later
               compares apples to apples (it's also called with
               absolute paths). */
            accepted.insert(childAbs);
            walkShape(accessor, childAbs, childRel, filter, sink, accepted);
        }
        writeShape(sink, "end");
        break;
    }
    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
        throw Error("filtered-shape walker: unsupported file type at '%s'", relative);
    }
}

} // anonymous namespace

FilteredShape collectFilteredShape(SourceAccessor & accessor, const CanonPath & root, PathFilter & filter)
{
    HashSink sink(HashAlgorithm::SHA256);
    boost::unordered_flat_set<CanonPath> accepted;
    /* Accepted paths are tracked in absolute form (path on the
       source accessor) to match the PathFilter call contract. */
    accepted.insert(root);
    walkShape(accessor, root, CanonPath::root, filter, sink, accepted);
    auto shape = FilteredShape{sink.finish().hash, std::move(accepted)};
    /* The filtered-shape walk is the per-eval *structure* cost (the
       filtered-`builtins.path` warm row, §8.5): the user filter runs
       over every entry to derive the accepted set + shapeHash, even
       when blob bytes are deferred. Logging it
       makes "why is my filtered source slow?" answerable — it shows the
       walk happened and how large the accepted shape is. */
    debug(
        "virtualise: walked filtered shape at '%s' — %d accepted paths, shapeHash %s",
        root,
        shape.accepted.size(),
        shape.shapeHash.to_string(HashFormat::SRI, true));
    return shape;
}

fetchers::Cache::Key
makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod method, const CanonPath & path)
{
    /* Delegate to the `SourcePathToHash` projection so the key shape has
       a single source of truth (this helper also has external callers
       in path.cc / fetchers.cc that pre-seed the cache). */
    return fetchers::SourcePathToHash::key(
        {.sourceFingerprint = std::string(fingerprint), .method = std::string{method.render()}, .subpath = path.abs()});
}

StorePath fetchToStore(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name,
    ContentAddressMethod method,
    PathFilter * filter,
    RepairFlag repair,
    const StorePathSet & refs)
{
    return fetchToStore2(settings, store, path, mode, name, method, filter, repair, refs).first;
}

/**
 * If `fingerprint` names a Git tree (`tree:<sha>...`) at root with the
 * canonical NAR ingestion method and no extra wrapper concerns, query
 * the cross-pipeline `treeHashToNarHash` projection — populated by the
 * tarball pipeline — for a cached NAR hash. This is the explicit bridge
 * called out in the proposal: a tarball downloaded yesterday and a git
 * input fetched today that unpack to the same tree-SHA share a row.
 */
static std::optional<Hash> peekTreeHashBridge(
    const fetchers::Settings & settings,
    std::string_view fingerprint,
    ContentAddressMethod method,
    const CanonPath & subpath)
{
    if (!subpath.isRoot())
        return std::nullopt;
    if (method != ContentAddressMethod::Raw::NixArchive)
        return std::nullopt;
    /* `bareTreeOid` is the single canonical decoder: it returns an OID
       only for a suffix-free `tree:<sha>` (any `;a=`/`;e`/`;l`/`;d=`/
       `;shape=` suffix means the NAR differs from a vanilla tree dump)
       and yields nullopt — never throws — on a malformed OID, so this
       consult and the post-walk writeback below observe identical
       bridge-eligibility (they previously drifted: this site threw on a
       bad hex where the writeback swallowed it). */
    auto treeHash = bareTreeOid(fingerprint);
    if (!treeHash)
        return std::nullopt;
    return fetchers::TreeHashToNarHash::peek(settings, *treeHash);
}

std::pair<StorePath, Hash> fetchToStore2(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name,
    ContentAddressMethod method,
    PathFilter * filter,
    RepairFlag repair,
    const StorePathSet & refs)
{
    std::optional<fetchers::Cache::Key> cacheKey;

    /* Even when the caller passes a filter, we now ask the source for
       its fingerprint: filter-shape caching keys on the source
       fingerprint plus the observed accepted shape, so we still want
       the source's identity. */
    auto [subpath, fingerprint] = path.accessor->getFingerprint(path.path);

    StoreReferences storeRefs{.others = refs, .self = false};

    auto reconstructStorePath = [&](const Hash & hash) {
        return store.makeFixedOutputPathFromCA(name, ContentAddressWithReferences::fromParts(method, hash, storeRefs));
    };

    /* The cache-hit sites below share one cascade: reconstruct the CA
       path from the cached hash, register a temp root unless this is a
       DryRun, and return the (path, hash) pair if the path is usable
       (DryRun needs no bytes; otherwise it must be valid in the store).
       A non-usable hit (stale cache row, GC'd path) returns nullopt so
       the caller falls through to the walk. Per-site side effects (memo
       population, debug lines, the bridge writeback) stay at the call
       site. */
    auto tryReturnCached = [&](const Hash & hash) -> std::optional<std::pair<StorePath, Hash>> {
        auto storePath = reconstructStorePath(hash);
        if (mode != FetchMode::DryRun)
            store.addTempRoot(storePath);
        if (mode == FetchMode::DryRun || store.isValidPath(storePath))
            return std::pair{std::move(storePath), hash};
        return std::nullopt;
    };

    /* Filtered branch: collect the post-filter shape, look up a row
       under (fingerprint, method, subpath, shapeHash). If hit and the
       reconstructed CA path is valid/substitutable, return it. Either
       way, prepare an `accepted`-set path filter for the miss path so
       the user predicate is not run again during the NAR walk. */
    std::optional<FilteredShape> shape;
    std::optional<fetchers::Cache::Key> filteredCacheKey;
    std::optional<std::string> filteredMemoKey;
    if (filter && fingerprint && method == ContentAddressMethod::Raw::NixArchive) {
        shape = collectFilteredShape(*path.accessor, path.path, *filter);
        auto shapeHashStr = shape->shapeHash.to_string(HashFormat::SRI, true);
        filteredMemoKey = makeMemoKey(*fingerprint, method.render(), subpath.abs(), shapeHashStr);

        /* In-process memo: cargo-workspace dedup without SQLite. */
        if (auto cached = getConcurrent(filteredNarHashMemo, *filteredMemoKey)) {
            if (auto hit = tryReturnCached(*cached)) {
                debug("filtered source '%s' in-process memo hit in '%s'", path, store.printStorePath(hit->first));
                return std::move(*hit);
            }
        }

        fetchers::FilteredSourceKey k{
            .sourceFingerprint = *fingerprint,
            .method = std::string{method.render()},
            .subpath = subpath.abs(),
            .shapeHash = shapeHashStr,
        };
        filteredCacheKey = fetchers::FilteredSourcePathToHash::key(k);

        if (auto res = settings.getCache()->lookup(*filteredCacheKey)) {
            auto hash = fetchers::FilteredSourcePathToHash::fromValue(*res);
            /* Populate in-process memo so subsequent calls skip the
               SQLite roundtrip. */
            filteredNarHashMemo.emplace(*filteredMemoKey, hash);
            if (auto hit = tryReturnCached(hash)) {
                debug(
                    "filtered source '%s' cache hit in '%s' (hash '%s')",
                    path,
                    store.printStorePath(hit->first),
                    hash.to_string(HashFormat::SRI, true));
                return std::move(*hit);
            }
        }
    }

    std::optional<std::string> sourceMemoKey;
    if (fingerprint && !filter) {
        sourceMemoKey = makeMemoKey(*fingerprint, method.render(), subpath.abs());

        if (auto cached = getConcurrent(sourceNarHashMemo, *sourceMemoKey)) {
            if (auto hit = tryReturnCached(*cached)) {
                debug("source path '%s' in-process memo hit in '%s'", path, store.printStorePath(hit->first));
                return std::move(*hit);
            }
        }

        cacheKey = makeSourcePathToHashCacheKey(*fingerprint, method, subpath);
        if (auto res = settings.getCache()->lookup(*cacheKey)) {
            auto hash = fetchers::SourcePathToHash::fromValue(*res);
            sourceNarHashMemo.emplace(*sourceMemoKey, hash);
            if (auto hit = tryReturnCached(hash)) {
                debug(
                    "source path '%s' cache hit in '%s' (hash '%s')",
                    path,
                    store.printStorePath(hit->first),
                    hash.to_string(HashFormat::SRI, true));
                return std::move(*hit);
            }
            debug("source path '%s' not in store", path);
        }

        if (auto bridged = peekTreeHashBridge(settings, *fingerprint, method, subpath)) {
            if (auto hit = tryReturnCached(*bridged)) {
                debug(
                    "source path '%s' cross-pipeline cache hit via treeHashToNarHash in '%s' (hash '%s')",
                    path,
                    store.printStorePath(hit->first),
                    bridged->to_string(HashFormat::SRI, true));
                /* Promote the cross-pipeline hit into this source's own
                   sourcePathToHash row so the next lookup hits directly. */
                settings.getCache()->upsert(*cacheKey, fetchers::SourcePathToHash::toValue(*bridged));
                return std::move(*hit);
            }
        }

        /* Track Z.gap1: a whole git input's `fingerprint` is the commit
           rev (not a bare `tree:<sha>`), so `peekTreeHashBridge` above
           never fires for it. But the root tree OID IS a sound bridge
           key — `treeOID → narHash` is content-determined. Ask the
           accessor for its root tree OID and consult `treeHashToNarHash`
           on it, so two commits with the same root tree (and a tarball
           with the same tree) share one walk. Gated on root + NixArchive
           inside `getRootTreeHash` (it returns nullopt for blob roots and
           for subtree-rooted accessors whose `tree:` fingerprint already
           bridges). */
        if (subpath.isRoot() && method == ContentAddressMethod::Raw::NixArchive) {
            if (auto rootTree = path.accessor->getRootTreeHash()) {
                if (auto bridged = fetchers::TreeHashToNarHash::peek(settings, *rootTree)) {
                    if (auto hit = tryReturnCached(*bridged)) {
                        debug(
                            "source path '%s' cross-pipeline cache hit via root-tree-OID %s in '%s' (hash '%s')",
                            path,
                            rootTree->gitRev(),
                            store.printStorePath(hit->first),
                            bridged->to_string(HashFormat::SRI, true));
                        settings.getCache()->upsert(*cacheKey, fetchers::SourcePathToHash::toValue(*bridged));
                        return std::move(*hit);
                    }
                }
            }
        }
    } else if (!fingerprint) {
        static auto barf = getEnv("_NIX_TEST_BARF_ON_UNCACHEABLE").value_or("") == "1";
        if (barf && !filter)
            throw Error("source path '%s' is uncacheable (filter=%d)", path, (bool) filter);
        debug("source path '%s' is uncacheable", path);
    }

    Activity act(
        *logger,
        lvlChatty,
        actUnknown,
        fmt(mode == FetchMode::DryRun ? "hashing '%s'" : "copying '%s' to the store", path));

    /* On the miss path, if we collected a shape, route through an
       accepted-set filter rather than the user predicate so the
       expensive content walk doesn't re-run user code.
     *
     * `accepted` holds absolute paths (path on the source accessor),
     * matching how `PathFilter` is called everywhere else. */
    PathFilter acceptedFilter = [&](const std::string & p) { return shape->accepted.contains(CanonPath(p)); };
    auto & filter2 = shape ? acceptedFilter : (filter ? *filter : defaultPathFilter);

    auto [storePath, hash] =
        mode == FetchMode::DryRun
            ? [&]() {
                  auto [storePath, hash] =
                      store.computeStorePath(name, path, method, HashAlgorithm::SHA256, refs, filter2);
                  debug(
                      "hashed '%s' to '%s' (hash '%s')",
                      path,
                      store.printStorePath(storePath),
                      hash.to_string(HashFormat::SRI, true));
                  return std::make_pair(storePath, hash);
              }()
            : [&]() {
                  auto storePath = store.addToStore(name, path, method, HashAlgorithm::SHA256, refs, filter2, repair);
                  auto info = store.queryPathInfo(storePath);
                  auto hash = method == ContentAddressMethod::Raw::NixArchive ? info->narHash : ({
                      if (!info->ca || info->ca->method != method)
                          throw Error("path '%s' lacks a CA field", store.printStorePath(storePath));
                      info->ca->hash;
                  });
                  debug(
                      "copied '%s' to '%s' (hash '%s')",
                      path,
                      store.printStorePath(storePath),
                      hash.to_string(HashFormat::SRI, true));
                  return std::make_pair(storePath, hash);
              }();

    if (filteredCacheKey) {
        settings.getCache()->upsert(*filteredCacheKey, fetchers::FilteredSourcePathToHash::toValue(hash));
        if (filteredMemoKey)
            filteredNarHashMemo.emplace(*filteredMemoKey, hash);
    } else if (cacheKey) {
        settings.getCache()->upsert(*cacheKey, fetchers::SourcePathToHash::toValue(hash));
        if (sourceMemoKey)
            sourceNarHashMemo.emplace(*sourceMemoKey, hash);
        /* If the source was a bare tree:<sha>, populate the cross-
           pipeline projection too, for the symmetric case (git input
           first, tarball later). Same canonical `bareTreeOid` decoder as
           the consult side (`peekTreeHashBridge`) so eligibility cannot
           drift. */
        auto bareTree = fingerprint && method == ContentAddressMethod::Raw::NixArchive && subpath.isRoot()
                            ? bareTreeOid(*fingerprint)
                            : std::nullopt;
        if (bareTree) {
            settings.getCache()->upsert(
                fetchers::TreeHashToNarHash::key(*bareTree), fetchers::TreeHashToNarHash::toValue(hash));
        } else if (method == ContentAddressMethod::Raw::NixArchive && subpath.isRoot()) {
            /* Track Z.gap1 (writeback): a whole git input keyed on the
               commit rev (not a bare tree:<sha>) still has a root tree
               OID. Populate `treeHashToNarHash` on it so the NEXT commit
               with the same root tree — and any tarball with that tree —
               hits the bridge above instead of re-walking. `getRootTreeHash`
               returns nullopt for non-git / blob-rooted / already-`tree:`
               accessors, so this only fires for the whole-input git case. */
            if (auto rootTree = path.accessor->getRootTreeHash())
                settings.getCache()->upsert(
                    fetchers::TreeHashToNarHash::key(*rootTree), fetchers::TreeHashToNarHash::toValue(hash));
        }
    }

    return {storePath, hash};
}

} // namespace nix
