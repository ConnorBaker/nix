#include "trace-serialize.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/compression.hh"

#include <cassert>
#include <cstring>

namespace nix::eval_trace {

// ── BLOB binding helper (ensures non-null pointer for empty BLOBs) ───

void bindBlobVec(SQLiteStmt::Use & use, const std::vector<uint8_t> & blob)
{
    // sqlite3_bind_blob with a null pointer binds NULL, not empty BLOB.
    // Use a sentinel address for empty blobs.
    static const uint8_t empty = 0;
    use(reinterpret_cast<const unsigned char *>(
            blob.empty() ? &empty : blob.data()),
        blob.size());
}

// ── BLOB serialization for dep key sets ──────────────────────────────
//
// keys_blob: packed 9-byte entries (type[1] + sourceId[4] + keyId[4]),
// zstd compressed. Stored in DepKeySets table, shared across traces with
// the same dep structure (same struct_hash).
// Note: uses native byte order for uint32_t fields. The database is a
// local cache (safe to delete), so cross-endianness portability is not required.

struct __attribute__((packed)) DepKeyBlobEntry {
    uint8_t type;
    uint32_t sourceId;
    uint32_t keyId;
};
static_assert(sizeof(DepKeyBlobEntry) == 9);

std::vector<uint8_t> TraceStore::serializeKeys(const std::vector<Dep::Key> & keys)
{
    std::vector<uint8_t> blob;
    blob.reserve(keys.size() * sizeof(DepKeyBlobEntry));

    for (auto & key : keys) {
        DepKeyBlobEntry entry{std::to_underlying(key.type), key.sourceId.value, key.keyId.value};
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

std::vector<Dep::Key> TraceStore::deserializeKeys(
    const void * blob, size_t size)
{
    if (size == 0)
        return {};

    auto decompressed = nix::decompress("zstd",
        {static_cast<const char *>(blob), size});

    std::vector<Dep::Key> keys;
    const uint8_t * p = reinterpret_cast<const uint8_t *>(decompressed.data());
    const uint8_t * end = p + decompressed.size();

    while (p + sizeof(DepKeyBlobEntry) <= end) {
        DepKeyBlobEntry entry;
        std::memcpy(&entry, p, sizeof(entry));
        p += sizeof(entry);

        keys.push_back({
            static_cast<DepType>(entry.type),
            DepSourceId(entry.sourceId),
            DepKeyId(entry.keyId)
        });
    }

    return keys;
}

// ── BLOB serialization for dep hash values ───────────────────────────
//
// values_blob: per-entry hashLen[1] + hashData[hashLen], zstd compressed.
// Stored in Traces table. Entries are positionally matched with keys_blob
// in the corresponding DepKeySets row.

std::vector<uint8_t> TraceStore::serializeValues(const std::vector<Dep> & deps)
{
    std::vector<uint8_t> blob;
    blob.reserve(deps.size() * 33);  // BLAKE3: 1 + 32 bytes typical

    for (auto & dep : deps) {
        auto [hashData, hashSize] = blobData(dep.hash);
        assert(hashSize <= 255 && "dep hash value exceeds single-byte length prefix");
        blob.push_back(static_cast<uint8_t>(hashSize));
        blob.insert(blob.end(), hashData, hashData + hashSize);
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

std::vector<DepHashValue> TraceStore::deserializeValues(
    const void * blob, size_t size, const std::vector<Dep::Key> & keys)
{
    if (size == 0)
        return {};

    auto decompressed = nix::decompress("zstd",
        {static_cast<const char *>(blob), size});

    std::vector<DepHashValue> values;
    const uint8_t * p = reinterpret_cast<const uint8_t *>(decompressed.data());
    const uint8_t * end = p + decompressed.size();
    size_t idx = 0;

    while (p < end && idx < keys.size()) {
        uint8_t hashLen = *p++;
        if (p + hashLen > end) break;

        // Use the dep type from the corresponding key to determine whether
        // this is a Blake3Hash or a variable-length string. Without this,
        // a 32-byte store path string would be incorrectly decoded as Blake3.
        if (isBlake3Dep(keys[idx].type) && hashLen == 32)
            values.push_back(Blake3Hash::fromBlob(p, 32));
        else
            values.push_back(std::string(reinterpret_cast<const char *>(p), hashLen));
        p += hashLen;
        idx++;
    }

    // Warn on key/value count mismatch (could indicate DB corruption)
    if (values.size() != keys.size())
        warn("deserializeValues: got %d values but expected %d (keys count)",
             values.size(), keys.size());

    return values;
}

} // namespace nix::eval_trace
