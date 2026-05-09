#include "nix/expr/eval-trace/deps/recording.hh"
#include "../binary-frame.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/input-resolution-internal.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"
#include "nix/util/source-path.hh"

#include <filesystem>

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// PathObject fallback resolution
// ═══════════════════════════════════════════════════════════════════════

static std::optional<ResolvedDepPath> resolveViaPathObject(
    const SourcePath & path,
    const std::optional<PathObject> & origin)
{
    if (!origin)
        return std::nullopt;

    if (!(path.path == origin->rootPath || path.path.isWithin(origin->rootPath)))
        return std::nullopt;

    return ResolvedDepPath{
        .source = origin->source,
        .key = (path.path == origin->rootPath
            ? CanonPath::root
            : path.path.removePrefix(origin->rootPath)).abs(),
    };
}

// ═══════════════════════════════════════════════════════════════════════
// resolveDepPathKey — uses SemanticRegistry reverse index
// ═══════════════════════════════════════════════════════════════════════

ResolvedDepPath resolveDepPathKey(
    const SourcePath & path,
    const eval_trace::SemanticRegistry & registry,
    const std::optional<PathObject> & origin)
{
    // Prefer SemanticRegistry reverse lookup (replaces resolveToInput + mount
    // table) so recording stays aligned with the canonical mounted identity
    // whenever one exists.
    auto mountResolved = registry.reverseResolve(path.path);
    if (mountResolved && mountResolved->first.kind() == DepSourceKind::Registered) {
        eval_trace::nrResolveViaRegistry++;
        return ResolvedDepPath{
            .source = std::move(mountResolved->first),
            .key = mountResolved->second.abs(),
        };
    }
    // Fall back to PathObject provenance for dirty/runtime-local aliases that
    // are not reverse-resolvable through the registry.
    if (auto resolved = resolveViaPathObject(path, origin)) {
        eval_trace::nrResolveViaPathObject++;
        return *resolved;
    }

    // Absolute path (outside all mounted inputs). Either a non-Registered
    // mount (e.g., Absolute-tagged) returned by reverseResolve, or a pure
    // filesystem-absolute path. Both fall under the "absolute" counter for
    // diagnostic purposes — the interesting signal is "how much of eval
    // bypassed the Registered-source keying lattice".
    eval_trace::nrResolveViaAbsolute++;
    if (mountResolved) {
        return ResolvedDepPath{
            .source = std::move(mountResolved->first),
            .key = mountResolved->second.abs(),
        };
    }
    return ResolvedDepPath{
        .source = DepSource::makeAbsolute(),
        .key = path.path.abs(),
    };
}

static const eval_trace::SemanticRegistry * registryFromAccess(const eval_trace::TraceAccess & access)
{
    auto * scope = access.depRecordingContext().currentScope();
    return scope ? scope->registry : nullptr;
}

// Binary framing helpers (appendUint64, readUint64, readFramedString,
// appendFetcherAttr, readFetcherAttr) live in eval-trace/binary-frame.hh
// and are shared with trace-serialize.cc.
using eval_trace::appendUint64;
using eval_trace::readUint64;
using eval_trace::readFramedString;
using eval_trace::appendFetcherAttr;
using eval_trace::readFetcherAttr;

static std::string_view encodedKeyBlobView(const std::vector<uint8_t> & blob)
{
    return {reinterpret_cast<const char *>(blob.data()), blob.size()};
}

void feedCanonicalDepKeyMaterial(
    eval_trace::CanonicalHashBuilder & builder,
    const InterningPools & pools,
    const Dep::Key & key)
{
    if (key.isTraceContext())
        throw Error("internal error: trace-context dep key requires vocab-aware hashing");

    builder.field("dep.key.kind", key.kind);
    builder.field("dep.key.source", pools.resolve(key.sourceId));

    if (key.isStructured()) {
        builder.field("dep.key.structured.file", pools.resolve(key.filePathId));
        builder.field("dep.key.structured.format", static_cast<uint8_t>(key.format));
        auto path = pools.dataPathPool.collectPath(key.dataPathId);
        builder.field("dep.key.structured.path.count", static_cast<uint64_t>(path.size()));
        for (auto & node : path) {
            if (node.arrayIndex >= 0) {
                builder.field("dep.key.structured.path.component.type", std::string_view("array-index"));
                builder.field("dep.key.structured.path.component.index", node.arrayIndex);
            } else {
                builder.field("dep.key.structured.path.component.type", std::string_view("object-key"));
                builder.field("dep.key.structured.path.component.key", node.component);
            }
        }
        builder.field("dep.key.structured.suffix", key.suffix);
        builder.field("dep.key.structured.has-key.present", key.hasKeyId.value != 0);
        if (key.hasKeyId.value != 0)
            builder.field("dep.key.structured.has-key", pools.resolve(key.hasKeyId));
        builder.field("dep.key.structured.dir-set.present", key.dirSetHashId.value != 0);
        if (key.dirSetHashId.value != 0)
            builder.field("dep.key.structured.dir-set", pools.resolve(key.dirSetHashId));
        return;
    }

    switch (key.kind) {
    case CanonicalQueryKind::DerivedStorePath:
        builder.field("dep.key.derived-store-path",
            encodedKeyBlobView(encodeDerivedStorePathDepKey(
                pools.resolve(key.derivedStorePathKeyId())).value));
        return;
    case CanonicalQueryKind::StorePathAvailability:
        builder.field("dep.key.store-path-availability",
            encodedKeyBlobView(encodeStorePathAvailabilityDepKey(
                pools.resolve(key.storePathAvailabilityKeyId())).value));
        return;
    case CanonicalQueryKind::RuntimeFetchIdentity:
        builder.field("dep.key.runtime-fetch-identity",
            encodedKeyBlobView(encodeRuntimeFetchIdentityDepKey(
                pools.resolve(key.runtimeFetchIdentityKeyId())).value));
        return;
    case CanonicalQueryKind::FileBytes:
    case CanonicalQueryKind::DirectoryEntries:
    case CanonicalQueryKind::ExistenceCheck:
    case CanonicalQueryKind::EnvironmentLookup:
    case CanonicalQueryKind::SessionSystemValue:
    case CanonicalQueryKind::VolatileExec:
    case CanonicalQueryKind::NarIdentity:
    case CanonicalQueryKind::RawBytes:
    case CanonicalQueryKind::GitRevisionIdentity:
    case CanonicalQueryKind::VolatileTime:
        builder.field("dep.key.simple", pools.resolve(key.simpleKeyId()));
        return;
    case CanonicalQueryKind::StructuredProjection:
    case CanonicalQueryKind::ImplicitStructure:
    case CanonicalQueryKind::TraceValueContext:
    case CanonicalQueryKind::TraceParentSlot:
        unreachable();
    }
}

EncodedDerivedStorePathDepKeyBlob encodeDerivedStorePathDepKey(const DerivedStorePathDepKey & key)
{
    std::string blob;
    blob.reserve(4 + (2 * sizeof(uint64_t)) + key.pathKey.abs().size() + key.storeName.value.size());
    blob.append("dsp1", 4);
    appendUint64(blob, key.pathKey.abs().size());
    blob.append(key.pathKey.abs());
    appendUint64(blob, key.storeName.value.size());
    blob.append(key.storeName.value);
    return EncodedDerivedStorePathDepKeyBlob{std::vector<uint8_t>(blob.begin(), blob.end())};
}

DerivedStorePathDepKey decodeDerivedStorePathDepKey(const InterningPools & pools, DerivedStorePathDepKeyId keyId)
{
    auto blob = pools.resolveEncodedDepKeyBlobForPersistence(eraseDepKeyType(keyId));
    if (!blob.starts_with("dsp1"))
        throw Error("internal error: invalid derived-store-path dep-key encoding");

    size_t offset = 4;
    auto pathKey = readFramedString(blob, offset);
    auto storeName = readFramedString(blob, offset);
    if (!pathKey || !storeName || offset != blob.size())
        throw Error("internal error: malformed derived-store-path dep-key encoding");

    return DerivedStorePathDepKey{
        .pathKey = CanonPath(*pathKey),
        .storeName = SimpleDepKeyAtom{std::move(*storeName)},
    };
}

EncodedStorePathAvailabilityDepKeyBlob encodeStorePathAvailabilityDepKey(const StorePathAvailabilityDepKey & key)
{
    std::string blob;
    blob.reserve(4 + sizeof(uint64_t) + key.storePath.to_string().size());
    blob.append("spa1", 4);
    appendUint64(blob, key.storePath.to_string().size());
    blob.append(key.storePath.to_string());
    return EncodedStorePathAvailabilityDepKeyBlob{std::vector<uint8_t>(blob.begin(), blob.end())};
}

StorePathAvailabilityDepKey decodeStorePathAvailabilityDepKey(const InterningPools & pools, StorePathAvailabilityDepKeyId keyId)
{
    auto blob = pools.resolveEncodedDepKeyBlobForPersistence(eraseDepKeyType(keyId));
    if (!blob.starts_with("spa1"))
        throw Error("internal error: invalid store-path-availability dep-key encoding");

    size_t offset = 4;
    auto storePath = readFramedString(blob, offset);
    if (!storePath || offset != blob.size())
        throw Error("internal error: malformed store-path-availability dep-key encoding");

    return StorePathAvailabilityDepKey{
        .storePath = StorePath(*storePath),
    };
}

EncodedRuntimeFetchIdentityDepKeyBlob encodeRuntimeFetchIdentityDepKey(const RuntimeFetchIdentityDepKey & key)
{
    std::string blob;
    blob.reserve(4 + sizeof(uint64_t));
    blob.append("rfi1", 4);
    appendUint64(blob, key.inputAttrs.size());
    for (const auto & [name, attr] : key.inputAttrs) {
        appendUint64(blob, name.size());
        blob.append(name);
        appendFetcherAttr(blob, attr);
    }
    return EncodedRuntimeFetchIdentityDepKeyBlob{std::vector<uint8_t>(blob.begin(), blob.end())};
}

RuntimeRootSourceKey makeRuntimeRootSourceKey(const RuntimeFetchIdentityDepKey & key)
{
    auto encoded = encodeRuntimeFetchIdentityDepKey(key);
    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::RuntimeRootSourceKey>();
    builder.field(
        "runtime-fetch-identity",
        std::string_view(
            reinterpret_cast<const char *>(encoded.value.data()),
            encoded.value.size()));
    return RuntimeRootSourceKey{builder.finish()};
}

RuntimeFetchIdentityDepKey decodeRuntimeFetchIdentityDepKey(const InterningPools & pools, RuntimeFetchIdentityDepKeyId keyId)
{
    auto blob = pools.resolveEncodedDepKeyBlobForPersistence(eraseDepKeyType(keyId));
    if (!blob.starts_with("rfi1"))
        throw Error("internal error: invalid runtime-fetch-identity dep-key encoding");

    size_t offset = 4;
    auto count = readUint64(blob, offset);
    if (!count)
        throw Error("internal error: malformed runtime-fetch-identity dep-key encoding");

    fetchers::Attrs attrs;
    for (uint64_t i = 0; i < *count; ++i) {
        auto name = readFramedString(blob, offset);
        auto value = readFetcherAttr(blob, offset);
        if (!name || !value)
            throw Error("internal error: malformed runtime-fetch-identity dep-key encoding");
        attrs.emplace(std::move(*name), std::move(*value));
    }

    if (offset != blob.size())
        throw Error("internal error: malformed runtime-fetch-identity dep-key encoding");

    return RuntimeFetchIdentityDepKey{
        .inputAttrs = std::move(attrs),
    };
}

std::optional<fetchers::Input> makeRuntimeFetchIdentityInput(
    const fetchers::Settings & settings,
    const RuntimeFetchIdentityDepKey & key)
{
    return fetchers::Input::fromAttrs(settings, fetchers::Attrs{key.inputAttrs});
}

std::string renderRuntimeFetchIdentityDisplay(const RuntimeFetchIdentityDepKey & key)
{
    std::string rendered = "attrs{";
    bool first = true;
    for (const auto & [name, attr] : key.inputAttrs) {
        if (!first)
            rendered += ", ";
        first = false;
        rendered += name;
        rendered += "=";
        std::visit(overloaded{
            [&](const std::string & value) { rendered += fmt("\"%s\"", value); },
            [&](uint64_t value) { rendered += std::to_string(value); },
            [&](Explicit<bool> value) { rendered += value.t ? "true" : "false"; },
            [&](const fetchers::LazyAttr &) { rendered += "<lazy>"; },
        }, attr);
    }
    rendered += "}";
    return rendered;
}

std::string renderSimpleDepKeyDisplay(
    const InterningPools & pools,
    const Dep::Key & key)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (key.kind) {
    case CanonicalQueryKind::DerivedStorePath:
        try {
            auto decoded = decodeDerivedStorePathDepKey(pools, key.derivedStorePathKeyId());
            return fmt("%s -> %s", decoded.pathKey.abs(), decoded.storeName.value);
        } catch (...) {
            return "<invalid derivedStorePath key>";
        }
    case CanonicalQueryKind::StorePathAvailability:
        try {
            auto decoded = decodeStorePathAvailabilityDepKey(pools, key.storePathAvailabilityKeyId());
            return std::string(decoded.storePath.to_string());
        } catch (...) {
            return "<invalid storePathAvailability key>";
        }
    case CanonicalQueryKind::RuntimeFetchIdentity:
        try {
            auto decoded = decodeRuntimeFetchIdentityDepKey(pools, key.runtimeFetchIdentityKeyId());
            return renderRuntimeFetchIdentityDisplay(decoded);
        } catch (...) {
            return "<invalid runtimeFetchIdentity key>";
        }
    default:
        break;
    }
#pragma GCC diagnostic pop
    return std::string(pools.resolve(key.simpleKeyId()));
}

// ═══════════════════════════════════════════════════════════════════════
// recordDep — dep recording via SemanticRegistry
// ═══════════════════════════════════════════════════════════════════════

void recordDep(
    const eval_trace::TraceAccess & access,
    const SourcePath & path,
    const DepHashValue & hash,
    CanonicalQueryKind depType,
    const std::optional<PathObject> & origin,
    SimpleDepKeyAtom storeName)
{
    auto * registry = registryFromAccess(access);
    if (!registry)
        return;

    auto depPath = resolveDepPathKey(path, *registry, origin);
    auto key = depPath.key;
    // Governing-repo attachment: file-content deps carry the repo root
    // they live under so the verifier can scope the git-identity skip
    // optimization to same-repo deps only.  Non-file-backed deps (None)
    // get RepoRootId{} (no governing repo).
    auto governingRepoId = (repoRootAddressingKind(depType) != RepoRootAddressingKind::None)
        ? access.tracingPools().internGoverningRepo(path.path.abs())
        : RepoRootId{};
    if (depType == CanonicalQueryKind::DerivedStorePath) {
        access.record(
            depPath.source,
            DerivedStorePathDepKey{
                .pathKey = CanonPath(key),
                .storeName = storeName,
            },
            hash,
            governingRepoId);
        return;
    }

    if (!isAbsoluteDepSource(depPath.source)) {
        access.record(depType, depPath.source, SimpleDepKeyAtom{std::move(key)}, hash, governingRepoId);
    } else if (depType == CanonicalQueryKind::ExistenceCheck
               || depType == CanonicalQueryKind::DirectoryEntries) {
        // ExistenceCheck and DirectoryEntries deps must be recorded even
        // when the file/directory does not exist — their purpose is to
        // track presence/absence transitions.  The maybeLstat guard would
        // silently drop these deps for absent paths, causing the trace to
        // trivially verify after a missing→exists transition.
        access.record(depType, depPath.source, SimpleDepKeyAtom{std::move(key)}, hash, governingRepoId);
    } else if (maybeLstat(std::filesystem::path(path.path.abs()))) {
        access.record(depType, depPath.source, SimpleDepKeyAtom{std::move(key)}, hash, governingRepoId);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// recordFileBytesDepViaCache
// ═══════════════════════════════════════════════════════════════════════

void recordFileBytesDepViaCache(
    const eval_trace::TraceAccess & access,
    EvalState & state,
    const SourcePath & path,
    const std::optional<PathObject> & origin)
{
    auto * registry = registryFromAccess(access);
    if (!registry)
        return;

    auto depPath = resolveDepPathKey(path, *registry, origin);

    auto & pools = access.tracingPools();
    auto sourceId = pools.intern<DepSourceId>(depPath.source);
    auto keyId = pools.intern(SimpleDepKeyAtom{depPath.key});

    // governingRepoId is excluded from Dep::Key identity (see Key::Hash in
    // types.hh), so the dedup probe need only match kind + sourceId + keyId.
    auto depKey = Dep::Key::makeSimple(CanonicalQueryKind::FileBytes, sourceId, keyId);
    if (access.scopeContainsDepKey(depKey))
        return;

    // maybeLstat guard for absolute-source FileBytes: a missing file must
    // drop the dep rather than record it with a bogus hash. Matches
    // recordDep's behavior at the corresponding branch.
    if (isAbsoluteDepSource(depPath.source)
        && !maybeLstat(std::filesystem::path(path.path.abs())))
        return;

    depKey.governingRepoId = (repoRootAddressingKind(CanonicalQueryKind::FileBytes) != RepoRootAddressingKind::None)
        ? access.tracingPools().internGoverningRepo(path.path.abs())
        : RepoRootId{};

    auto hash = getOrReadFileContentHash(state, path);

    access.record(Dep{std::move(depKey), DepHashValue(hash)});
}

// ═══════════════════════════════════════════════════════════════════════
// Provenance resolution via SemanticRegistry
// ═══════════════════════════════════════════════════════════════════════

std::optional<ResolvedDepPath> resolveProvenanceViaRegistry(
    const eval_trace::TraceAccess & access,
    const SourcePath & path,
    const std::optional<PathObject> & origin)
{
    auto * registry = registryFromAccess(access);
    if (!registry)
        return std::nullopt;
    return resolveDepPathKey(path, *registry, origin);
}

std::optional<PathObject> resolvePathObjectViaRegistry(const SourcePath & path)
{
    auto * depCtx = eval_trace::currentFiberDepCtx();
    if (!depCtx) depCtx = eval_trace::currentStandaloneDepCtx();
    auto * scope = depCtx ? depCtx->currentScope() : nullptr;
    if (!scope || !scope->registry) return std::nullopt;
    return scope->registry->resolvePathObject(path);
}

std::optional<PathObject> resolvePathObjectViaRegistry(
    const eval_trace::TraceAccess & access,
    const SourcePath & path)
{
    auto * registry = registryFromAccess(access);
    if (!registry)
        return std::nullopt;
    return registry->resolvePathObject(path);
}

[[gnu::cold]] void maybeRecordRawContentDep(const eval_trace::TraceAccess & access, const Value & v)
{
    if (v.type() != nString) return;
    auto * pub = v.publication();
    if (!pub || !pub->text) return;
    // RawBytes' key is an absolute file path — look up its governing repo
    // for per-trace git-identity coverage scoping.
    auto governingRepoId = access.tracingPools().internGoverningRepo(pub->text->key);
    access.record(
        CanonicalQueryKind::RawBytes,
        pub->text->source,
        SimpleDepKeyAtom{pub->text->key},
        DepHashValue(pub->text->contentHash),
        governingRepoId);
}

} // namespace nix
