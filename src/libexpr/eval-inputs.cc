#include "nix/expr/eval-inputs.hh"

#include "nix/util/hash.hh"

#include <bit>

namespace nix {

namespace {

/**
 * Convert a 64-bit integer to little-endian byte order.
 * This ensures consistent hashes across big-endian and little-endian machines.
 */
inline uint64_t toLittleEndian64(uint64_t v)
{
    if constexpr (std::endian::native == std::endian::big) {
        return ((v & 0xFF00000000000000ULL) >> 56) | ((v & 0x00FF000000000000ULL) >> 40)
            | ((v & 0x0000FF0000000000ULL) >> 24) | ((v & 0x000000FF00000000ULL) >> 8)
            | ((v & 0x00000000FF000000ULL) << 8) | ((v & 0x0000000000FF0000ULL) << 24)
            | ((v & 0x000000000000FF00ULL) << 40) | ((v & 0x00000000000000FFULL) << 56);
    }
    return v;
}

} // anonymous namespace

ContentHash EvalInputs::fingerprint() const
{
    HashSink sink(evalHashAlgo);

    // Helper: write a length-prefixed string to prevent ambiguity.
    // Without length prefixes, ["ab","c"] and ["a","bc"] would hash identically.
    auto writeString = [&sink](const std::string & s) {
        uint64_t len = toLittleEndian64(s.size());
        sink({reinterpret_cast<const char *>(&len), sizeof(len)});
        sink(s);
    };

    // Version info (length-prefixed to prevent collision with currentSystem)
    writeString(nixVersion);

    // Boolean flags as single bytes
    uint8_t flags = 0;
    if (pureEval)
        flags |= 0x01;
    if (impureMode)
        flags |= 0x02;
    if (allowImportFromDerivation)
        flags |= 0x04;
    if (restrictEval)
        flags |= 0x08;
    sink({reinterpret_cast<const char *>(&flags), 1});

    // System (length-prefixed)
    writeString(currentSystem);

    // NIX_PATH entries (order matters)
    // Count encoded in little-endian for cross-machine stability
    uint64_t nixPathSize = toLittleEndian64(nixPath.size());
    sink({reinterpret_cast<const char *>(&nixPathSize), sizeof(nixPathSize)});
    for (const auto & entry : nixPath) {
        writeString(entry);  // Length-prefixed to prevent ambiguity
    }

    // Allowed URIs (sorted set, so order is deterministic)
    // Count encoded in little-endian for cross-machine stability
    uint64_t allowedUrisSize = toLittleEndian64(allowedUris.size());
    sink({reinterpret_cast<const char *>(&allowedUrisSize), sizeof(allowedUrisSize)});
    for (const auto & uri : allowedUris) {
        writeString(uri);  // Length-prefixed to prevent ambiguity
    }

    // Optional flake lock hash
    if (flakeLockHash) {
        uint8_t hasLock = 1;
        sink({reinterpret_cast<const char *>(&hasLock), 1});
        // Include hash algorithm and size for safety against algo changes
        uint8_t algo = static_cast<uint8_t>(flakeLockHash->algo);
        sink({reinterpret_cast<const char *>(&algo), 1});
        uint8_t hashSize = static_cast<uint8_t>(flakeLockHash->hashSize);
        sink({reinterpret_cast<const char *>(&hashSize), 1});
        sink({reinterpret_cast<const char *>(flakeLockHash->hash), flakeLockHash->hashSize});
    } else {
        uint8_t hasLock = 0;
        sink({reinterpret_cast<const char *>(&hasLock), 1});
    }

    // Optional root accessor fingerprint
    if (rootAccessorFingerprint) {
        uint8_t hasAccessor = 1;
        sink({reinterpret_cast<const char *>(&hasAccessor), 1});
        // Include hash algorithm and size for safety against algo changes
        uint8_t algo = static_cast<uint8_t>(rootAccessorFingerprint->algo);
        sink({reinterpret_cast<const char *>(&algo), 1});
        uint8_t hashSize = static_cast<uint8_t>(rootAccessorFingerprint->hashSize);
        sink({reinterpret_cast<const char *>(&hashSize), 1});
        sink({reinterpret_cast<const char *>(rootAccessorFingerprint->hash), rootAccessorFingerprint->hashSize});
    } else {
        uint8_t hasAccessor = 0;
        sink({reinterpret_cast<const char *>(&hasAccessor), 1});
    }

    auto result = sink.finish();
    return ContentHash{result.hash};
}

EvalInputs EvalInputs::fromSettings(
    const std::string & nixVersion,
    bool pureEval,
    bool restrictEval,
    bool impureMode,
    bool allowImportFromDerivation,
    const std::vector<std::string> & nixPath,
    const std::string & currentSystem,
    const std::set<std::string> & allowedUris,
    const std::optional<Hash> & flakeLockHash,
    const std::optional<Hash> & rootAccessorFingerprint)
{
    EvalInputs inputs;
    inputs.nixVersion = nixVersion;
    inputs.pureEval = pureEval;
    inputs.restrictEval = restrictEval;
    inputs.impureMode = impureMode;
    inputs.allowImportFromDerivation = allowImportFromDerivation;
    inputs.nixPath = nixPath;
    inputs.currentSystem = currentSystem;
    inputs.allowedUris = allowedUris;
    inputs.flakeLockHash = flakeLockHash;
    inputs.rootAccessorFingerprint = rootAccessorFingerprint;
    return inputs;
}

} // namespace nix
