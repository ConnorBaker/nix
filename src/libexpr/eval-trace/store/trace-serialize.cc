#include "trace-serialize.hh"
#include "../binary-frame.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/input-resolution-internal.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/compression.hh"
#include "nix/util/error.hh"
#include "nix/util/hash.hh"

#include <cassert>
#include <cstring>

namespace nix::eval_trace {

namespace {

inline constexpr std::string_view kValuesBlobMagic = "vals2";

// Binary framing helpers (readUint64, readFramedString, readFetcherAttr)
// live in eval-trace/binary-frame.hh and are shared with input-resolution.cc.

}

// ── BLOB binding helpers (ensure non-null pointer for empty BLOBs) ────

void bindBlob(SQLiteStmt::Use & use, const std::vector<uint8_t> & blob)
{
    // sqlite3_bind_blob with a null pointer binds NULL, not empty BLOB.
    // Use a sentinel address for empty blobs.
    static const uint8_t empty = 0;
    use(reinterpret_cast<const unsigned char *>(
            blob.empty() ? &empty : blob.data()),
        blob.size());
}

void bindBlob(SQLiteStmt::Use & use, std::string_view blob)
{
    static const uint8_t empty = 0;
    const void * rawData = blob.empty()
        ? static_cast<const void *>(&empty)
        : static_cast<const void *>(blob.data());
    auto * data = reinterpret_cast<const unsigned char *>(rawData);
    use(data, blob.size());
}

void bindRuntimeRootStorePath(
    SQLiteStmt::Use & use,
    const StoreDirConfig & store,
    const RuntimeRootStorePath & storePath)
{
    bindBlob(use, encodeStorePathBlob(store, storePath.value).value);
}

Hash decodePersistedHashBlob(std::string_view blob)
{
    if (blob.empty())
        throw Error("missing persisted hash blob");

    auto algo = [&]() -> HashAlgorithm {
        switch (static_cast<HashAlgorithm>(blob.front())) {
        case HashAlgorithm::MD5:
        case HashAlgorithm::SHA1:
        case HashAlgorithm::SHA256:
        case HashAlgorithm::SHA512:
        case HashAlgorithm::BLAKE3:
            return static_cast<HashAlgorithm>(blob.front());
        default:
            throw Error("invalid persisted hash algorithm tag %d",
                static_cast<unsigned char>(blob.front()));
        }
    }();

    blob.remove_prefix(1);
    auto expectedSize = regularHashSize(algo);
    if (blob.size() != expectedSize)
        throw Error("invalid persisted hash blob length %d for %s",
            static_cast<uint64_t>(blob.size()),
            printHashAlgo(algo));

    Hash hash(algo);
    std::memcpy(hash.hash, blob.data(), blob.size());
    hash.hashSize = blob.size();
    return hash;
}

DepSource decodeRuntimeRootSourceBlob(std::string_view blob)
{
    auto source = decodeDepSourceBlob(blob);
    if (!source)
        throw Error("invalid runtime root dep source blob");
    return *source;
}

RuntimeFetchIdentityDepKey decodeRuntimeRootFetchIdentityBlob(std::string_view blob)
{
    if (!blob.starts_with("rfi1"))
        throw Error("invalid runtime root fetch-identity blob prefix");

    blob.remove_prefix(4);
    size_t offset = 0;
    auto attrCount = readUint64(blob, offset);
    if (!attrCount)
        throw Error("invalid runtime root fetch-identity blob attr count");

    fetchers::Attrs attrs;
    for (uint64_t i = 0; i < *attrCount; ++i) {
        auto name = readFramedString(blob, offset);
        auto attr = readFetcherAttr(blob, offset);
        if (!name || !attr)
            throw Error("invalid runtime root fetch-identity blob payload");
        attrs.emplace(std::move(*name), std::move(*attr));
    }

    if (offset != blob.size())
        throw Error("invalid runtime root fetch-identity blob length");

    return RuntimeFetchIdentityDepKey{.inputAttrs = std::move(attrs)};
}

RuntimeRootNarHash decodeRuntimeRootNarHashBlob(std::string_view blob)
{
    return RuntimeRootNarHash{decodePersistedHashBlob(blob)};
}

RuntimeRootStorePath decodeRuntimeRootStorePathBlob(const StoreDirConfig & store, std::string_view blob)
{
    return RuntimeRootStorePath{decodeStorePathBlob(store, blob)};
}

PersistedHashBlob encodePersistedHashBlob(const Hash & hash)
{
    std::string encoded;
    encoded.reserve(1 + hash.hashData().size());
    encoded.push_back(static_cast<char>(hash.algo));
    encoded.append(hash.hashData());
    return PersistedHashBlob{std::move(encoded)};
}

EncodedStorePathBlob encodeStorePathBlob(const StoreDirConfig & store, const StorePath & storePath)
{
    return EncodedStorePathBlob{store.printStorePath(storePath)};
}

StorePath decodeStorePathBlob(const StoreDirConfig & store, std::string_view blob)
{
    return store.parseStorePath(blob);
}

// ── BLOB serialization for dep key sets ──────────────────────────────
//
// keys_blob: packed dep entries keyed by dep type, zstd compressed.
// Simple deps retain the 9-byte layout (type[1] + sourceId[4] + keyId[4]).
// Structured deps persist their typed components directly.
// Trace-context deps persist attrPathId + nodeStamp without overloading
// the generic source/key slots.
// Note: uses native byte order for uint32_t fields. The database is a
// local cache (safe to delete), so cross-endianness portability is not required.

struct __attribute__((packed)) SimpleDepKeyBlobEntry {
    uint8_t type;
    uint32_t sourceId;
    uint32_t keyId;
    uint32_t governingRepoId;
};
static_assert(sizeof(SimpleDepKeyBlobEntry) == 13);

struct __attribute__((packed)) StructuredDepKeyBlobEntry {
    uint8_t type;
    uint32_t sourceId;
    uint32_t filePathId;
    uint8_t format;
    uint32_t dataPathId;
    uint8_t suffix;
    uint32_t hasKeyId;
    uint32_t dirSetHashId;
    uint32_t governingRepoId;
};
static_assert(sizeof(StructuredDepKeyBlobEntry) == 27);

struct __attribute__((packed)) TraceContextDepKeyBlobEntry {
    uint8_t type;
    uint32_t attrPathId;
};
static_assert(sizeof(TraceContextDepKeyBlobEntry) == 5);

std::vector<uint8_t> SqliteTraceStorage::serializeKeys(const std::vector<Dep::Key> & keys)
{
    std::vector<uint8_t> blob;
    blob.reserve(keys.size() * sizeof(StructuredDepKeyBlobEntry));

    for (auto & key : keys) {
        if (key.isStructured()) {
            StructuredDepKeyBlobEntry entry{
                .type = std::to_underlying(key.kind),
                .sourceId = key.sourceId.value,
                .filePathId = key.filePathId.value,
                .format = key.format,
                .dataPathId = key.dataPathId.value,
                .suffix = std::to_underlying(key.suffix),
                .hasKeyId = key.hasKeyId.value,
                .dirSetHashId = key.dirSetHashId.value,
                .governingRepoId = key.governingRepoId.value,
            };
            auto * raw = reinterpret_cast<const uint8_t *>(&entry);
            blob.insert(blob.end(), raw, raw + sizeof(entry));
            continue;
        }
        if (key.isTraceContext()) {
            TraceContextDepKeyBlobEntry entry{
                .type = std::to_underlying(key.kind),
                .attrPathId = key.attrPathId.value,
            };
            auto * raw = reinterpret_cast<const uint8_t *>(&entry);
            blob.insert(blob.end(), raw, raw + sizeof(entry));
            continue;
        }
        SimpleDepKeyBlobEntry entry{
            .type = std::to_underlying(key.kind),
            .sourceId = key.sourceId.value,
            .keyId = key.depKeyId().value,
            .governingRepoId = key.governingRepoId.value,
        };
        auto * raw = reinterpret_cast<const uint8_t *>(&entry);
        blob.insert(blob.end(), raw, raw + sizeof(entry));
    }

    if (!blob.empty()) {
        auto compressed = nix::compress(
            CompressionAlgo::zstd,
            {reinterpret_cast<const char *>(blob.data()), blob.size()},
            false, 1);
        return {compressed.begin(), compressed.end()};
    }
    return blob;
}

std::vector<Dep::Key> SqliteTraceStorage::deserializeKeys(
    const void * blob, size_t size)
{
    if (size == 0)
        return {};

    auto decompressed = nix::decompress("zstd",
        {static_cast<const char *>(blob), size});

    std::vector<Dep::Key> keys;
    const uint8_t * p = reinterpret_cast<const uint8_t *>(decompressed.data());
    const uint8_t * end = p + decompressed.size();

    while (p < end) {
        auto type = static_cast<CanonicalQueryKind>(*p);
        if (isStructuredQueryKind(type)) {
            if (p + sizeof(StructuredDepKeyBlobEntry) > end)
                break;
            StructuredDepKeyBlobEntry entry{};
            std::memcpy(&entry, p, sizeof(entry));
            p += sizeof(entry);
            auto format = parseStructuredFormat(static_cast<char>(entry.format));
            if (!format)
                break;
            auto key = Dep::Key::makeStructured(
                type,
                DepSourceId(entry.sourceId),
                FilePathId(entry.filePathId),
                *format,
                this->pools.dataPathPool.fromRaw(entry.dataPathId),
                static_cast<ShapeSuffix>(entry.suffix),
                StringId(entry.hasKeyId),
                StringId(entry.dirSetHashId));
            key.governingRepoId = RepoRootId{entry.governingRepoId};
            keys.push_back(key);
            continue;
        }
        if (isTraceContextQueryKind(type)) {
            if (p + sizeof(TraceContextDepKeyBlobEntry) > end)
                break;
            TraceContextDepKeyBlobEntry entry{};
            std::memcpy(&entry, p, sizeof(entry));
            p += sizeof(entry);
            keys.push_back(Dep::Key::makeTraceContext(
                type,
                AttrPathId(entry.attrPathId)));
            continue;
        }
        if (p + sizeof(SimpleDepKeyBlobEntry) > end)
            break;
        SimpleDepKeyBlobEntry entry{};
        std::memcpy(&entry, p, sizeof(entry));
        p += sizeof(entry);
        Dep::Key key = [&]() {
            if (type == CanonicalQueryKind::DerivedStorePath) {
                return Dep::Key::makeDerivedStorePath(
                    DepSourceId(entry.sourceId),
                    DerivedStorePathDepKeyId{DepKeyId(entry.keyId)});
            }
            if (type == CanonicalQueryKind::StorePathAvailability) {
                return Dep::Key::makeStorePathAvailability(
                    DepSourceId(entry.sourceId),
                    StorePathAvailabilityDepKeyId{DepKeyId(entry.keyId)});
            }
            if (type == CanonicalQueryKind::RuntimeFetchIdentity) {
                return Dep::Key::makeRuntimeFetchIdentity(
                    DepSourceId(entry.sourceId),
                    RuntimeFetchIdentityDepKeyId{DepKeyId(entry.keyId)});
            }
            return Dep::Key::makeSimple(
                type,
                DepSourceId(entry.sourceId),
                SimpleDepKeyId(entry.keyId));
        }();
        key.governingRepoId = RepoRootId{entry.governingRepoId};
        keys.push_back(key);
    }

    return keys;
}

// ── BLOB serialization for dep hash values ───────────────────────────
//
// Schema-grouped value blob format (self-describing):
//   Header: "vals2" [hashAlgorithm:byte] [numEntries:u32] [entryTypes: numEntries bytes]
//           (0x00=string, 0x01=digest)
//   Digest block: [numDigests:u32] [digestData: numDigests*kEvalTraceDigestSize bytes]
//   String block: [numStrings:u32] [foreach: [len:u8][data:len bytes]]
// Entire blob zstd-compressed.

static void appendU32(std::vector<uint8_t> & blob, uint32_t val)
{
    uint8_t buf[4];
    std::memcpy(buf, &val, 4);
    blob.insert(blob.end(), buf, buf + 4);
}

std::vector<uint8_t> SqliteTraceStorage::serializeValues(const std::vector<Dep> & deps)
{
    if (deps.empty()) return {};

    std::vector<uint8_t> blob;
    blob.insert(blob.end(), kValuesBlobMagic.begin(), kValuesBlobMagic.end());
    blob.push_back(evalTraceHashAlgorithmTag(getEvalTraceHashAlgorithm()));

    uint32_t numEntries = static_cast<uint32_t>(deps.size());
    appendU32(blob, numEntries);

    // Build type map and partition into digest/string groups
    std::vector<const DepHash *> digests;
    std::vector<std::string_view> strings;
    for (auto & dep : deps) {
        if (auto * h = std::get_if<DepHash>(&dep.hash)) {
            blob.push_back(1);
            digests.push_back(h);
        } else {
            blob.push_back(0);
            strings.push_back(std::get<std::string>(dep.hash));
        }
    }

    // Digest block — contiguous eval-trace digests
    appendU32(blob, static_cast<uint32_t>(digests.size()));
    for (auto * h : digests)
        blob.insert(blob.end(), h->value.bytes.begin(), h->value.bytes.end());

    // String block — length-prefixed variable strings
    appendU32(blob, static_cast<uint32_t>(strings.size()));
    for (auto sv : strings) {
        assert(sv.size() <= 255);
        blob.push_back(static_cast<uint8_t>(sv.size()));
        blob.insert(blob.end(), sv.begin(), sv.end());
    }

    auto compressed = nix::compress(CompressionAlgo::zstd,
        {reinterpret_cast<const char *>(blob.data()), blob.size()}, false, 1);
    return {compressed.begin(), compressed.end()};
}

std::vector<DepHashValue> SqliteTraceStorage::deserializeValues(
    const void * blob, size_t size)
{
    if (size == 0)
        return {};

    auto decompressed = nix::decompress("zstd",
        {static_cast<const char *>(blob), size});
    const uint8_t * p = reinterpret_cast<const uint8_t *>(decompressed.data());
    const uint8_t * end = p + decompressed.size();

    auto readU32 = [&]() -> uint32_t {
        if (p + 4 > end) throw Error("values blob: truncated u32");
        uint32_t val;
        std::memcpy(&val, p, 4);
        p += 4;
        return val;
    };

    if (static_cast<size_t>(end - p) < kValuesBlobMagic.size() + 1)
        throw Error("values blob: truncated header");
    if (std::memcmp(p, kValuesBlobMagic.data(), kValuesBlobMagic.size()) != 0)
        throw Error("values blob: invalid header");
    p += kValuesBlobMagic.size();
    EvalTraceHashAlgorithm algorithm;
    try {
        algorithm = parseEvalTraceHashAlgorithmTag(static_cast<char>(*p++));
    } catch (std::exception & e) {
        throw Error("values blob: %s", e.what());
    }
    if (algorithm != getEvalTraceHashAlgorithm())
        throw Error("values blob: hash algorithm %s does not match active algorithm %s",
            evalTraceHashAlgorithmName(algorithm),
            evalTraceHashAlgorithmName(getEvalTraceHashAlgorithm()));

    // Header
    uint32_t numEntries = readU32();
    if (p + numEntries > end)
        throw Error("values blob: truncated entry types (need %d, have %d)",
                     numEntries, static_cast<uint32_t>(end - p));
    std::vector<uint8_t> entryTypes(p, p + numEntries);
    p += numEntries;

    // Count type occurrences for validation
    uint32_t expectedDigests = 0, expectedStrings = 0;
    for (auto t : entryTypes) {
        if (t == 1) expectedDigests++;
        else if (t == 0) expectedStrings++;
        else throw Error("values blob: invalid entry type %d", t);
    }

    // Digest block
    uint32_t numDigests = readU32();
    if (numDigests != expectedDigests)
        throw Error("values blob: digest count %d != expected %d",
                     numDigests, expectedDigests);
    if (static_cast<size_t>(numDigests) > static_cast<size_t>(end - p) / kEvalTraceDigestSize)
        throw Error("values blob: truncated digest data");
    std::vector<DepHash> digests;
    digests.reserve(numDigests);
    for (uint32_t i = 0; i < numDigests; i++) {
        digests.push_back(DepHash{EvalTraceHash::fromBlob(p, kEvalTraceDigestSize)});
        p += kEvalTraceDigestSize;
    }

    // String block
    uint32_t numStrings = readU32();
    if (numStrings != expectedStrings)
        throw Error("values blob: string count %d != expected %d",
                     numStrings, expectedStrings);
    std::vector<std::string> stringValues;
    stringValues.reserve(numStrings);
    for (uint32_t i = 0; i < numStrings; i++) {
        if (p >= end)
            throw Error("values blob: truncated string length at index %d", i);
        uint8_t len = *p++;
        if (p + len > end)
            throw Error("values blob: truncated string data at index %d", i);
        stringValues.emplace_back(reinterpret_cast<const char *>(p), len);
        p += len;
    }

    if (p != end)
        throw Error("values blob: %d trailing bytes", static_cast<uint32_t>(end - p));

    // Reconstruct in original order
    std::vector<DepHashValue> result;
    result.reserve(numEntries);
    size_t digestIndex = 0, stringIndex = 0;
    for (auto t : entryTypes) {
        if (t == 1)
            result.push_back(std::move(digests[digestIndex++]));
        else
            result.push_back(std::move(stringValues[stringIndex++]));
    }
    return result;
}

} // namespace nix::eval_trace
