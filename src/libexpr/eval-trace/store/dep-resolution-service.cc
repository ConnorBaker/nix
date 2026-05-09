/// dep-resolution-service.cc — resolveDepHash implementation.
///
/// The per-CQK hash computation logic. resolveDepHash is the single
/// leaf function for all dep resolution paths (typestate L3, verifyTrace
/// pre-population). Subsumption is enforced here via if constexpr on
/// TaggedDepType. L1 caching is internal via ComputedHash.

#include "dep-resolution-service.hh"
#include "parse-caches.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/strand-local.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "../toml-canonical.hh"
#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-environment/environment.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/store/store-api.hh"
#include "nix/util/source-accessor.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/git-utils.hh"

#include "expr-config-private.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace nix::eval_trace {

// ═══════════════════════════════════════════════════════════════════════
// computeCurrentHash + helpers (moved from trace-verify-deps.cc)
// ── Trace verification helpers (BSàlC verifying trace check) ─────────

static void attributeStructuredFormatTime(StructuredFormat format, uint64_t us)
{
    switch (format) {
    case StructuredFormat::Json:
        nrDepHashStructuredJsonUs += us;
        break;
    case StructuredFormat::Toml:
        nrDepHashStructuredTomlUs += us;
        break;
    case StructuredFormat::Directory:
        nrDepHashStructuredDirUs += us;
        break;
    case StructuredFormat::Nix:
        nrDepHashStructuredNixUs += us;
        break;
    }
}

// resolveDepPath removed — live code resolves typed DepSource values via
// SemanticRegistry::resolve().

/// Cached SourcePath resolution for structured deps. Resolves typed dep
/// sources from the interning pool only on first access per unique
/// (source, filePath).
/// Returns by value: boost::unordered_flat_map doesn't guarantee reference
/// stability across insertions, and the DirSet loop calls this for multiple
/// dirs. The copy is cheap (shared_ptr refcount bump + CanonPath string).
static std::optional<SourcePath> resolveDepPathCached(
    FileCacheKey key,
    const InterningPools & pools,
    const SemanticRegistry & resolver,
    ParseCaches & caches,
    const StrandToken<FileStrandTag> & tok)
{
    auto it = caches.sourcePathCache.access(tok).find(key);
    if (it != caches.sourcePathCache.access(tok).end())
        return it->second;
    auto source = pools.resolveDepSource(key.sourceId);
    auto filePath = std::string(pools.resolve(key.filePathId));
    auto result = resolver.resolve(source, filePath);
    caches.sourcePathCache.access(tok).emplace(key, result);
    return result;
}

// ── Structured content navigation helpers (used by TrackedSource) ────

/**
 * Navigate a JSON DOM using a JSON path array. Returns nullptr if path is invalid.
 * Path components: strings for object keys, numbers for array indices.
 */
static const nlohmann::json * navigateJson(const nlohmann::json & root, const StructuredPath & path)
{
    const nlohmann::json * node = &root;
    for (auto & component : path) {
        if (component.isIndex()) {
            if (!node->is_array()) return nullptr;
            auto idx = static_cast<size_t>(component.index);
            if (idx >= node->size()) return nullptr;
            node = &(*node)[idx];
        } else {
            if (!node->is_object()) return nullptr;
            auto it = node->find(component.key);
            if (it == node->end()) return nullptr;
            node = &*it;
        }
    }
    return node;
}

/**
 * Navigate a TOML DOM using a JSON path array. Returns nullptr if path is invalid.
 */
static const toml::value * navigateToml(const toml::value & root, const StructuredPath & path)
{
    const toml::value * node = &root;
    for (auto & component : path) {
        if (component.isIndex()) {
            if (!node->is_array()) return nullptr;
            auto idx = static_cast<size_t>(component.index);
            auto & arr = toml::get<std::vector<toml::value>>(*node);
            if (idx >= arr.size()) return nullptr;
            node = &arr[idx];
        } else {
            if (!node->is_table()) return nullptr;
            auto & table = toml::get<toml::table>(*node);
            auto it = table.find(component.key);
            if (it == table.end()) return nullptr;
            node = &it->second;
        }
    }
    return node;
}

// tomlCanonical() is in src/libexpr/eval-trace/toml-canonical.hh
// (shared with fromTOML.cc's TomlDataNode::canonicalValue())

// ── TrackedSource: navigable DOM view for shape-suffix resolution ────

namespace {

/// Navigable view of a parsed structured source (JSON DOM, TOML DOM,
/// or directory listing). Created on-stack during verification, holds
/// a non-owning reference to the cached DOM in VerificationSession.
struct TrackedSource {
    virtual ~TrackedSource() = default;
    virtual bool navigate(const StructuredPath & path) = 0;
    virtual std::optional<DepHashValue> hashLeaf() = 0;
    virtual bool isArray() = 0;
    virtual bool isObject() = 0;
    virtual size_t size() = 0;
    virtual std::vector<std::string> keys() = 0;
    virtual bool hasKey(const std::string & key) = 0;
};

struct JsonSource final : TrackedSource {
    const nlohmann::json & root;
    const nlohmann::json * node = nullptr;
    explicit JsonSource(const nlohmann::json & r) : root(r) {}
    bool navigate(const StructuredPath & path) override {
        node = navigateJson(root, path);
        return node != nullptr;
    }
    std::optional<DepHashValue> hashLeaf() override {
        return DepHashValue(depHash(node->dump()));
    }
    bool isArray() override { return node->is_array(); }
    bool isObject() override { return node->is_object(); }
    size_t size() override { return node->size(); }
    std::vector<std::string> keys() override {
        std::vector<std::string> k;
        for (auto & [key, _] : node->items()) k.push_back(key);
        return k;
    }
    bool hasKey(const std::string & key) override { return node->contains(key); }
};

struct TomlSource final : TrackedSource {
    const toml::value & root;
    const toml::value * node = nullptr;
    explicit TomlSource(const toml::value & r) : root(r) {}
    bool navigate(const StructuredPath & path) override {
        node = navigateToml(root, path);
        return node != nullptr;
    }
    std::optional<DepHashValue> hashLeaf() override {
        return DepHashValue(depHash(tomlCanonical(*node)));
    }
    bool isArray() override { return node->is_array(); }
    bool isObject() override { return node->is_table(); }
    size_t size() override {
        if (node->is_array()) return toml::get<std::vector<toml::value>>(*node).size();
        return toml::get<toml::table>(*node).size();
    }
    std::vector<std::string> keys() override {
        std::vector<std::string> k;
        for (auto & [key, _] : toml::get<toml::table>(*node)) k.push_back(key);
        return k;
    }
    bool hasKey(const std::string & key) override {
        return toml::get<toml::table>(*node).count(key) > 0;
    }
};

struct DirSource final : TrackedSource {
    const SourceAccessor::DirEntries & entries;
    SourceAccessor::DirEntries::const_iterator leafIt;
    bool atLeaf = false;
    explicit DirSource(const SourceAccessor::DirEntries & e) : entries(e) {}
    bool navigate(const StructuredPath & path) override {
        if (path.empty()) return true;
        if (path.size() != 1 || !path[0].isKey()) return false;
        leafIt = entries.find(path[0].key);
        if (leafIt == entries.end()) return false;
        atLeaf = true;
        return true;
    }
    std::optional<DepHashValue> hashLeaf() override {
        if (!atLeaf) return std::nullopt;
        return DepHashValue(depHash(dirEntryTypeString(leafIt->second)));
    }
    bool isArray() override { return false; }
    bool isObject() override { return true; }
    size_t size() override { return entries.size(); }
    std::vector<std::string> keys() override {
        std::vector<std::string> k;
        for (auto & [key, _] : entries) k.push_back(key);
        return k;
    }
    bool hasKey(const std::string & key) override {
        return entries.count(key) > 0;
    }
};

static std::optional<DepHashValue> resolveShapeSuffix(
    TrackedSource & source, ShapeSuffix shape,
    bool isHasKey, const std::string & hasKeyName)
{
    switch (shape) {
    case ShapeSuffix::Len:
        if (!source.isArray()) return std::nullopt;
        return DepHashValue(depHash(std::to_string(source.size())));
    case ShapeSuffix::Keys: {
        if (!source.isObject()) return std::nullopt;
        return DepHashValue(canonicalKeysHash(source.keys()));
    }
    case ShapeSuffix::Type:
        if (source.isObject()) return DepHashValue(sentinel(SentinelHash::Object));
        if (source.isArray()) return DepHashValue(sentinel(SentinelHash::Array));
        return std::nullopt;
    case ShapeSuffix::None:
        if (isHasKey) {
            if (!source.isObject()) return std::nullopt;
            return DepHashValue(source.hasKey(hasKeyName) ? sentinel(SentinelHash::One) : sentinel(SentinelHash::Zero));
        }
        return source.hashLeaf();
    }
    unreachable();
}

} // anonymous namespace

// ── computeCurrentHash ───────────────────────────────────────────────
//
// INVARIANT: This function must be a pure function of (dep.key, dep.key.kind,
// dep.source, current filesystem/environment state). It must NEVER read
// dep.expectedHash. Structural variant recovery constructs deps with a
// placeholder expectedHash (EvalTraceHash{}), so reading it would silently
// produce wrong results or type-mismatch failures.

/// Shared pattern for path-backed deps:
/// resolve path → lstat → compute the current hash directly, with missing-file
/// and exception handling.
///
/// Path-backed dep hash computation: resolve path, lstat, compute hash.
/// Missing-file and exception handling are included.
template<typename HashFn>
static std::optional<DepHashValue> computePathHashedDep(
    const DepSource & source, const std::string & key,
    const SemanticRegistry & resolver,
    HashFn && hashFn)
{
    auto path = resolver.resolve(source, key);
    if (!path) {
        return std::nullopt;
    }
    if (!path->maybeLstat()) {
        return DepHashValue(sentinel(SentinelHash::Missing));
    }
    try {
        return DepHashValue(std::forward<HashFn>(hashFn)(*path));
    } catch (std::exception &) {
        return std::nullopt;
    }
}

template<typename TaggedDepType>
std::optional<DepHashValue> resolveDepHash(
    EvalState & state, VerificationSession & session, const TaggedDepType & taggedDep,
    const SemanticRegistry & resolver,
    const InterningPools & pools,
    ParseCaches & caches,
    const StrandToken<FileStrandTag> & tok)
{
    using Origin = typename TaggedDepType::OriginTag;
    static_assert(
        std::is_same_v<Origin, dep_origin::CurrentTrace>
        || std::is_same_v<Origin, dep_origin::HistoricalCandidate>,
        "resolveDepHash handles only CurrentTrace and HistoricalCandidate "
        "origins.  Adding a new origin requires auditing the subsumption "
        "shortcut: if the new origin's dep.hash equals hash(op(current F)) "
        "under trace-scoped isFileVerified(traceId, F), extend the `if constexpr` below; "
        "otherwise ensure the new origin falls through to the compute path "
        "the way CandidateDep does.");

    const Dep & dep = taggedDep.value();

    // Subsumption: if the file's Content dep was verified unchanged,
    // fine-grained deps (StructuredProjection, ImplicitStructure) are valid
    // — return the stored hash without computation.
    //
    // Subsumption is SOUND only for Structural / ImplicitStructural kinds
    // (their stored hash encodes hash(op(source))) AND only on the
    // CurrentTrace path.  `isFileVerified(traceId, F)` is set only for the
    // same trace when current F matches that trace's recording of F — so for
    // CurrentTraceDep, `dep.hash` equals `hash(op(current F))` and may be
    // returned directly and written to L1 as VerifiedHash.
    //
    // CandidateDep intentionally falls through to the compute path.  The
    // candidate's `dep.hash` was recorded against the candidate's own file
    // content, which is NOT guaranteed to match current F even when some
    // current trace verified F.  tryStructuralVariantRecovery calls
    // `computePresortedTraceHash(repDeps)` and looks up the result in
    // history's `TraceHash` index; history entries were computed from each
    // candidate's RECORDED `.hash` values, so if we returned `candidate
    // dep.hash` here the lookup would become tautological and every
    // candidate sharing a Structural key K on a verified file F would match
    // regardless of whether current F agrees with the candidate's recorded
    // F.  Returning the computed `hash(op(current F))` keeps recovery
    // honest: the candidate matches iff its recorded trace hash equals what
    // its dep set resolves to NOW.
    if constexpr (std::is_same_v<Origin, dep_origin::CurrentTrace>) {
        auto behavior = queryBehavior(dep.key.kind);
        bool canSubsumeShortcut =
            (behavior == QueryBehavior::Structural
             || behavior == QueryBehavior::ImplicitStructural)
            && session.isFileVerified(taggedDep.traceId(), dep.key);
        if (canSubsumeShortcut) {
            nrContentSubsumptionSkips++;
            // Subsumption shortcut fires on the per-dep hot path
            // (~16,790 per worst commit).  No per-dep log line: the
            // The trace-scoped recovery subsumption mark is logged once
            // per invalid trace in trace-store-verify.cc. Logging per dep
            // would produce 16K redundant lines on large traces.
            session.cacheVerifiedHash(dep.key,
                VerifiedHash{dep.hash},
                grantVerifiedSubsumption(taggedDep));
            return dep.hash;
        }
    }

    // Compute the current hash.  Result is cached in L1 as ComputedHash
    // (unconditional — ComputedHash is a pure function of current state and
    // sound for any origin to write).
    auto computeHash = [&]() -> std::optional<DepHashValue> {
    switch (dep.key.kind) {
    case CanonicalQueryKind::FileBytes:
    case CanonicalQueryKind::RawBytes: {
        auto source = pools.resolveDepSource(dep.key.sourceId);
        auto key = std::string(pools.resolve(dep.key.simpleKeyId()));
        return computePathHashedDep(source, key, resolver,
            [](const SourcePath & p) { return depHash(p.readFile()); });
    }
    case CanonicalQueryKind::NarIdentity: {
        auto source = pools.resolveDepSource(dep.key.sourceId);
        auto key = std::string(pools.resolve(dep.key.simpleKeyId()));
        return computePathHashedDep(source, key, resolver,
            [](const SourcePath & p) { return depHashPath(p); });
    }
    case CanonicalQueryKind::DirectoryEntries: {
        auto source = pools.resolveDepSource(dep.key.sourceId);
        auto key = std::string(pools.resolve(dep.key.simpleKeyId()));
        return computePathHashedDep(source, key, resolver,
            [](const SourcePath & p) { return depHashDirListing(p.readDirectory()); });
    }
    case CanonicalQueryKind::ExistenceCheck: {
        auto source = pools.resolveDepSource(dep.key.sourceId);
        auto key = std::string(pools.resolve(dep.key.simpleKeyId()));
        auto path = resolver.resolve(source, key);
        if (!path) return std::nullopt;
        auto st = path->maybeLstat();
        return DepHashValue(st
            ? fmt("type:%d", static_cast<int>(st->type))
            : std::string("missing"));
    }
    case CanonicalQueryKind::EnvironmentLookup: {
        auto key = std::string(pools.resolve(dep.key.simpleKeyId()));
        EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
        auto observation = environment.readEnvVar(observeOnly, key);
        return DepHashValue(depHash(observation.value));
    }
    case CanonicalQueryKind::SessionSystemValue: {
        EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
        auto observation = environment.observeSessionSystem(observeOnly);
        return DepHashValue(depHash(observation.currentSystem.value));
    }
    case CanonicalQueryKind::DerivedStorePath: {
        auto source = pools.resolveDepSource(dep.key.sourceId);
        auto decodedKey = decodeDerivedStorePathDepKey(pools, dep.key.derivedStorePathKeyId());
        auto sourcePath = resolver.resolve(source, decodedKey.pathKey.abs());
        if (!sourcePath) return std::nullopt;
        if (!sourcePath->maybeLstat())
            return DepHashValue(sentinel(SentinelHash::Missing));
        try {
            EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
            auto observation = environment.observeDerivedStorePath(
                observeOnly,
                DerivedStorePathRequest{
                    .sourcePath = CoercedPathRequest{
                        .path = *sourcePath,
                    },
                    .storeName = decodedKey.storeName.value,
                });
            if (!observation.storePath)
                return std::nullopt;
            return DepHashValue(state.store->printStorePath(*observation.storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case CanonicalQueryKind::RuntimeFetchIdentity: {
        try {
            auto decodedKey = decodeRuntimeFetchIdentityDepKey(pools, dep.key.runtimeFetchIdentityKeyId());
            auto input = makeRuntimeFetchIdentityInput(state.fetchSettings, decodedKey);
            if (!input)
                return std::nullopt;
            EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
            auto observation = environment.observeRuntimeFetchIdentity(
                observeOnly,
                FetchIdentityRequest{
                    .input = OriginalFetchInput{std::make_shared<const fetchers::Input>(std::move(*input))},
                });
            if (!observation.storePath)
                return std::nullopt;
            return DepHashValue(state.store->printStorePath(*observation.storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case CanonicalQueryKind::ImplicitStructure:
        // ImplicitShape uses the same key format and hash computation as
        // StructuredContent. Verified only for failed sources without StructuredProjection deps.
        [[fallthrough]];
    case CanonicalQueryKind::StructuredProjection: {
        if (!dep.key.isStructured())
            return std::nullopt;

        // Aggregated DirSet dep: iterate all directories, check each for the key
        if (dep.key.dirSetHashId) {
            nrDepHashScDirSetMisses++;
            auto hasKeyName = dep.key.hasKeyId
                ? std::string(pools.resolve(dep.key.hasKeyId)) : std::string();
            if (hasKeyName.empty()) return std::nullopt;

            auto dsHash = std::string(pools.resolve(dep.key.dirSetHashId));
            auto it = pools.dirSets.find(dsHash);
            if (it == pools.dirSets.end()) return std::nullopt;

            bool found = false;
            for (auto & dir : it->second) {
                auto fck = FileCacheKey{dir.sourceId, dir.filePathId};
                auto path = resolveDepPathCached(fck, pools, resolver, caches, tok);
                if (!path) continue;
                try {
                    auto cacheIt = caches.dirListingCache.access(tok).find(fck);
                    if (cacheIt == caches.dirListingCache.access(tok).end())
                        cacheIt = caches.dirListingCache.access(tok).emplace(fck, path->readDirectory()).first;
                    if (cacheIt->second.count(hasKeyName)) {
                        found = true;
                        break;
                    }
                } catch (...) {
                    continue;
                }
            }
            return DepHashValue(depHash(found ? "1" : "0"));
        }

        auto fck = FileCacheKey{dep.key.sourceId, dep.key.filePathId};
        auto format = dep.key.structuredFormat();
        auto dataPath = resolveStructuredPath(pools, dep.key.dataPathId);
        auto hasKeyName = dep.key.hasKeyId
            ? std::string(pools.resolve(dep.key.hasKeyId)) : std::string();
        bool isHasKey = !hasKeyName.empty();
        auto shape = dep.key.suffix;
        auto failureKey = StructuredSourceCacheKey{fck, format};
        auto & failureCache = caches.structuredSourceFailureCache.access(tok);
        auto failureCacheStart = timerStart();
        if (auto failureIt = failureCache.find(failureKey); failureIt != failureCache.end()) {
            attributeStructuredFormatTime(format, elapsedUs(failureCacheStart));
            return failureIt->second;
        }
        auto cacheSourceWideStructuredFailure = [&](std::optional<DepHashValue> result) {
            failureCache.insert_or_assign(failureKey, result);
            return result;
        };

        // Resolve SourcePath only when needed (DOM cache miss).
        auto resolveSourcePath = [&]() -> std::optional<SourcePath> {
            auto resolved = resolveDepPathCached(fck, pools, resolver, caches, tok);
            if (resolved && resolved->maybeLstat())
                return resolved;
            return std::nullopt;
        };

        try {
            switch (format) {
            case StructuredFormat::Json: {
                auto scStart = timerStart();
                auto cacheIt = caches.jsonDomCache.access(tok).find(fck);
                if (cacheIt == caches.jsonDomCache.access(tok).end()) {
                    auto path = resolveSourcePath();
                    if (!path) {
                        nrDepHashStructuredJsonUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(DepHashValue(sentinel(SentinelHash::Missing)));
                    }
                    try {
                        cacheIt = caches.jsonDomCache.access(tok).emplace(fck,
                            nlohmann::json::parse(path->readFile())).first;
                    } catch (...) {
                        nrDepHashStructuredJsonUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(std::nullopt);
                    }
                }
                JsonSource source(cacheIt->second);
                if (!source.navigate(dataPath)) { nrDepHashStructuredJsonUs += elapsedUs(scStart); return std::nullopt; }
                auto r = resolveShapeSuffix(source, shape, isHasKey, hasKeyName);
                nrDepHashStructuredJsonUs += elapsedUs(scStart);
                return r;
            }
            case StructuredFormat::Toml: {
                auto scStart = timerStart();
                auto cacheIt = caches.tomlDomCache.access(tok).find(fck);
                if (cacheIt == caches.tomlDomCache.access(tok).end()) {
                    auto path = resolveSourcePath();
                    if (!path) {
                        nrDepHashStructuredTomlUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(DepHashValue(sentinel(SentinelHash::Missing)));
                    }
                    try {
                        auto contents = path->readFile();
                        std::istringstream stream(std::move(contents));
                        cacheIt = caches.tomlDomCache.access(tok).emplace(fck, toml::parse(
                            stream, "verifyTrace"
#if HAVE_TOML11_4
                            , toml::spec::v(1, 0, 0)
#endif
                        )).first;
                    } catch (...) {
                        nrDepHashStructuredTomlUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(std::nullopt);
                    }
                }
                TomlSource source(cacheIt->second);
                if (!source.navigate(dataPath)) { nrDepHashStructuredTomlUs += elapsedUs(scStart); return std::nullopt; }
                auto r = resolveShapeSuffix(source, shape, isHasKey, hasKeyName);
                nrDepHashStructuredTomlUs += elapsedUs(scStart);
                return r;
            }
            case StructuredFormat::Directory: {
                auto scStart = timerStart();
                auto cacheIt = caches.dirListingCache.access(tok).find(fck);
                if (cacheIt == caches.dirListingCache.access(tok).end()) {
                    auto path = resolveSourcePath();
                    if (!path) {
                        nrDepHashStructuredDirUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(DepHashValue(sentinel(SentinelHash::Missing)));
                    }
                    try {
                        cacheIt = caches.dirListingCache.access(tok).emplace(fck, path->readDirectory()).first;
                    } catch (...) {
                        nrDepHashStructuredDirUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(std::nullopt);
                    }
                }
                DirSource source(cacheIt->second);
                if (!source.navigate(dataPath)) { nrDepHashStructuredDirUs += elapsedUs(scStart); return std::nullopt; }
                auto r = resolveShapeSuffix(source, shape, isHasKey, hasKeyName);
                nrDepHashStructuredDirUs += elapsedUs(scStart);
                return r;
            }
            case StructuredFormat::Nix: {
                auto scStart = timerStart();
                // Nix AST structural dep: parse .nix file, compute per-binding hash.
                // parseExprFromString is thread-safe: reentrant lexer, stack-local
                // parser state, atomic BumpMemoryResource, concurrent SymbolTable/PosTable.
                if (dataPath.size() != 1 || !dataPath[0].isKey())
                    { nrDepHashStructuredNixUs += elapsedUs(scStart); return std::nullopt; }
                auto bindingName = dataPath[0].key;
                auto cacheIt = caches.nixAstCache.access(tok).find(fck);
                if (cacheIt == caches.nixAstCache.access(tok).end()) {
                    auto path = resolveSourcePath();
                    if (!path) {
                        nrDepHashStructuredNixUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(DepHashValue(sentinel(SentinelHash::Missing)));
                    }
                    try {
                        auto source = path->readFile();
                        auto ast = state.parseExprFromString(
                            std::move(source), path->parent());
                        auto [exprAttrs, scopeExprs] = findNonRecExprAttrs(ast);
                        ParseCaches::NixAstBindings hashes;
                        if (exprAttrs) {
                            auto parentDir = path->parent().path;
                            auto scopeHash = computeNixScopeHash(scopeExprs, state.symbols, &parentDir);
                            for (auto & [sym, attrDef] : *exprAttrs->attrs) {
                                auto name = std::string(state.symbols[sym]);
                                const Expr * exprToShow = attrDef.e;
                                if (attrDef.kind == ExprAttrs::AttrDef::Kind::InheritedFrom) {
                                    auto * iv = dynamic_cast<ExprInheritFrom *>(attrDef.e);
                                    if (iv && exprAttrs->inheritFromExprs
                                        && static_cast<size_t>(iv->displ) < exprAttrs->inheritFromExprs->size())
                                        exprToShow = (*exprAttrs->inheritFromExprs)[iv->displ];
                                    else
                                        exprToShow = nullptr;
                                }
                                hashes[name] = computeNixBindingHash(
                                    scopeHash, name, static_cast<int>(attrDef.kind),
                                    exprToShow, state.symbols, &parentDir);
                            }
                        }
                        cacheIt = caches.nixAstCache.access(tok).emplace(fck, std::move(hashes)).first;
                    } catch (...) {
                        nrDepHashStructuredNixUs += elapsedUs(scStart);
                        return cacheSourceWideStructuredFailure(std::nullopt);
                    }
                }
                auto & hashes = cacheIt->second;
                if (hashes.empty()) { nrDepHashStructuredNixUs += elapsedUs(scStart); return std::nullopt; }
                auto it = hashes.find(bindingName);
                if (it == hashes.end()) { nrDepHashStructuredNixUs += elapsedUs(scStart); return std::nullopt; }
                nrDepHashStructuredNixUs += elapsedUs(scStart);
                return DepHashValue(DepHash{it->second.value});
            }
            }
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }
    case CanonicalQueryKind::StorePathAvailability: {
        // Check batched results first (populated by verifyTrace pre-pass)
        auto keyId = dep.key.storePathAvailabilityKeyId();
        auto spIt = session.storePathValid.find(keyId);
        if (spIt != session.storePathValid.end())
            return DepHashValue(spIt->second ? std::string("valid") : std::string("missing"));
        // Fallback for non-batched callers.
        try {
            auto decodedKey = decodeStorePathAvailabilityDepKey(pools, keyId);
            auto storePath = decodedKey.storePath;
            return DepHashValue(state.store->isValidPath(storePath)
                ? std::string("valid") : std::string("missing"));
        } catch (std::exception &) {
            return DepHashValue(std::string("missing"));
        }
    }
    case CanonicalQueryKind::GitRevisionIdentity: {
        // For GitRevisionIdentity deps, governingRepoId IS the repo itself
        // (set at record time in `recordGitIdentityObservation`).  When
        // absent (value == 0), we fall back below to resolving the simple
        // key id as a repo path string; we can't populate the per-id cache
        // in that case because there's no id to key by.
        RepoRootId repoId = dep.key.governingRepoId;
        try {
            if (repoId.value != 0) {
                auto gitIt = session.gitIdentityCache.find(repoId);
                if (gitIt != session.gitIdentityCache.end())
                    return gitIt->second
                        ? std::optional(DepHashValue(DepHash{gitIt->second->value}))
                        : std::nullopt;
            }
            // resolveRepoRoot returns the interned absolute path for the
            // repo id.  If repoId==0 (no governingRepoId), fall back to
            // resolving the simple key id as a repo path directly — the
            // legacy path that still works correctly for read-only
            // verification, it just can't cache per-id.
            std::string repoPath = repoId.value != 0
                ? std::string(pools.resolveRepoRoot(repoId))
                : std::string(pools.resolve(dep.key.simpleKeyId()));
            auto hash = computeGitIdentityHash(std::filesystem::path(repoPath));
            // Log once per repo per session on the compute path (cache
            // miss).  Subsequent checks hit `session.gitIdentityCache`
            // and return early above without reaching this line.
            if (verbosity >= lvlDebug) {
                debug("eval-trace/resolve: git-identity computed repoPath='%s' hash=%s",
                      repoPath,
                      hash ? hash->value.toHex() : std::string("(null)"));
            }
            if (repoId.value != 0)
                session.gitIdentityCache[repoId] = hash;
            return hash ? std::optional(DepHashValue(DepHash{hash->value})) : std::nullopt;
        } catch (...) {
            if (repoId.value != 0)
                session.gitIdentityCache[repoId] = std::nullopt;
            return std::nullopt;
        }
    }
    case CanonicalQueryKind::VolatileTime:
    case CanonicalQueryKind::VolatileExec:
    case CanonicalQueryKind::TraceValueContext:
    case CanonicalQueryKind::TraceParentSlot:
        return std::nullopt;
    }
    unreachable();
    }(); // end computeHash lambda

    // Cache in L1 as ComputedHash.  Sound for any origin — ComputedHash is
    // by construction `resolve(key, current_state)`.
    session.cacheComputedHash(dep.key,
        computeHash ? std::optional{ComputedHash{*computeHash}} : std::nullopt);
    return computeHash;
}

// Explicit instantiations — the two live origin tags.
template std::optional<DepHashValue> resolveDepHash<CurrentTraceDep>(
    EvalState &, VerificationSession &, const CurrentTraceDep &,
    const SemanticRegistry &,
    const InterningPools &, ParseCaches &, const StrandToken<FileStrandTag> &);
template std::optional<DepHashValue> resolveDepHash<CandidateDep>(
    EvalState &, VerificationSession &, const CandidateDep &,
    const SemanticRegistry &,
    const InterningPools &, ParseCaches &, const StrandToken<FileStrandTag> &);

} // namespace nix::eval_trace
