#include "nix/expr/eval-hash.hh"

#include "nix/util/hash.hh"

#include <cstring>

namespace nix {

namespace {

/**
 * Magic prefix bytes for back-reference hashes.
 * This ensures back-refs don't collide with regular content hashes.
 */
constexpr uint8_t BACKREF_PREFIX = 0xFF;
constexpr uint8_t STRUCTURAL_TAG = 0x01;
constexpr uint8_t CONTENT_TAG = 0x02;

/**
 * Combine multiple hashes using a streaming approach.
 * Each hash's bytes are fed into a new hash computation.
 */
template<typename HashType>
HashType combineHashes(std::initializer_list<HashType> hashes)
{
    HashSink sink(evalHashAlgo);
    for (const auto & h : hashes) {
        sink({reinterpret_cast<const char *>(h.data()), h.size()});
    }
    auto result = sink.finish();
    return HashType{result.hash};
}

/**
 * Create a back-reference hash that encodes the depth.
 * Format: [BACKREF_PREFIX, TAG, depth as little-endian 64-bit]
 * Then hash those bytes to get a fixed-size result.
 */
template<typename HashType>
HashType makeBackRef(size_t depth, uint8_t tag)
{
    uint8_t data[10];
    data[0] = BACKREF_PREFIX;
    data[1] = tag;
    // Store depth as little-endian 64-bit
    for (int i = 0; i < 8; i++) {
        data[2 + i] = static_cast<uint8_t>((depth >> (i * 8)) & 0xFF);
    }

    HashSink sink(evalHashAlgo);
    sink({reinterpret_cast<const char *>(data), sizeof(data)});
    auto result = sink.finish();
    return HashType{result.hash};
}

} // anonymous namespace

// StructuralHash implementation

StructuralHash StructuralHash::backRef(size_t depth)
{
    return makeBackRef<StructuralHash>(depth, STRUCTURAL_TAG);
}

StructuralHash StructuralHash::combine(std::initializer_list<StructuralHash> hashes)
{
    return combineHashes(hashes);
}

StructuralHash StructuralHash::fromString(std::string_view s)
{
    return StructuralHash{hashString(evalHashAlgo, s)};
}

// ContentHash implementation

ContentHash ContentHash::backRef(size_t depth)
{
    return makeBackRef<ContentHash>(depth, CONTENT_TAG);
}

ContentHash ContentHash::combine(std::initializer_list<ContentHash> hashes)
{
    return combineHashes(hashes);
}

ContentHash ContentHash::fromString(std::string_view s)
{
    return ContentHash{hashString(evalHashAlgo, s)};
}

ContentHash ContentHash::fromBytes(std::span<const uint8_t> bytes)
{
    return ContentHash{hashString(evalHashAlgo, {reinterpret_cast<const char *>(bytes.data()), bytes.size()})};
}

} // namespace nix
