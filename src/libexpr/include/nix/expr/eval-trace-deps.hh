#pragma once
///@file

#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/serialise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/std-hash.hh"
#include "nix/util/util.hh"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nix {

class Value;
struct SourcePath;

/**
 * Dependency types for eval trace verification (BSàlC verifying trace check).
 *
 * Category A (Hash Oracle): Verified by recomputing a hash and comparing.
 *   Content, Directory, Existence, EnvVar, System
 *
 * Category B (Reference Resolution): Verified by re-resolving a reference
 *   (store path computation or re-fetch) and comparing.
 *   CopiedPath, UnhashedFetch
 *
 * Category C (Volatile): Always invalidates — verification always fails.
 *   CurrentTime, Exec
 *
 * Category D (Structural): Verified by checking parent context hash.
 *   ParentContext
 *
 * NOTE: Symlink targets are not tracked. resolveSymlinks() follows symlinks
 * but only records the final resolved file as a Content/Directory dep.
 * Changes to intermediate symlink targets will NOT invalidate traced results.
 */
enum class DepType : uint8_t {
    /** File content was read (evalFile, readFile, parseExprFromFile). */
    Content = 1,
    /** Directory listing was read (readDir). */
    Directory = 2,
    /** File existence was checked (pathExists). */
    Existence = 3,
    /** Environment variable value (getEnv). */
    EnvVar = 4,
    /** Wall clock time (currentTime) — always invalidates. */
    CurrentTime = 5,
    /** currentSystem setting value. */
    System = 6,
    /** Unhashed fetchurl/fetchTarball/fetchGit — validated by re-fetching. */
    UnhashedFetch = 7,
    /** Parent evaluation context — invalidates when parent trace changes. */
    ParentContext = 8,
    /** Path coerced to store path — invalidates when store path would change. */
    CopiedPath = 9,
    /** builtins.exec output — always invalidates (cannot re-execute to verify). */
    Exec = 10,
    /** BLAKE3 of NAR serialization — captures file content AND executable bit.
     *  Used for per-file deps in builtins.path with a filter function.
     *  Unlike Content (raw bytes only), this detects chmod +x changes. */
    NARContent = 11,
};

/**
 * Human-readable name for a DepType.
 */
inline std::string depTypeName(DepType type)
{
    switch (type) {
    case DepType::Content: return "content";
    case DepType::Directory: return "directory";
    case DepType::Existence: return "existence";
    case DepType::EnvVar: return "envvar";
    case DepType::CurrentTime: return "currentTime";
    case DepType::System: return "system";
    case DepType::UnhashedFetch: return "unhashedFetch";
    case DepType::ParentContext: return "parentContext";
    case DepType::CopiedPath: return "copiedPath";
    case DepType::Exec: return "exec";
    case DepType::NARContent: return "narContent";
    }
    unreachable();
}

/**
 * Fixed-size BLAKE3-256 hash. Stack-allocated, no heap allocation.
 * Used throughout the eval trace system for content hashes, trace hashes,
 * and stat-hash-cache entries.
 */
struct Blake3Hash {
    std::array<uint8_t, 32> bytes{};

    bool operator==(const Blake3Hash &) const = default;
    auto operator<=>(const Blake3Hash &) const = default;

    const unsigned char * data() const { return bytes.data(); }
    static constexpr size_t size() { return 32; }

    /** View as string_view for feeding into HashSink. */
    std::string_view view() const {
        return {reinterpret_cast<const char *>(bytes.data()), 32};
    }

    /** Construct from a Hash object (asserts hashSize == 32). */
    static Blake3Hash fromHash(const Hash & h) {
        Blake3Hash result;
        assert(h.hashSize == 32);
        std::memcpy(result.bytes.data(), h.hash, 32);
        return result;
    }

    /** Construct from a raw BLOB pointer. */
    static Blake3Hash fromBlob(const void * data, size_t len) {
        Blake3Hash result;
        assert(len == 32);
        std::memcpy(result.bytes.data(), data, 32);
        return result;
    }

    /** For use as unordered_map key — first 8 bytes are already well-distributed. */
    struct Hasher {
        size_t operator()(const Blake3Hash & h) const noexcept {
            uint64_t v;
            std::memcpy(&v, h.bytes.data(), sizeof(v));
            return v;
        }
    };

    /** Format as hex for logging (debug path only). */
    std::string toHex() const {
        static constexpr char hex[] = "0123456789abcdef";
        std::string out(64, '\0');
        for (size_t i = 0; i < 32; i++) {
            out[2 * i] = hex[bytes[i] >> 4];
            out[2 * i + 1] = hex[bytes[i] & 0xf];
        }
        return out;
    }
};

/**
 * A dep's expected hash value: either a fixed-size BLAKE3 hash (for content,
 * directory, NAR, envvar, system, and parent context deps) or a variable-length
 * string (for existence checks like "type:1"/"missing", and store paths for
 * CopiedPath/UnhashedFetch).
 */
using DepHashValue = std::variant<Blake3Hash, std::string>;

/** Feed a DepHashValue into a Sink. */
inline void hashDepValue(Sink & sink, const DepHashValue & v) {
    std::visit(overloaded{
        [&](const Blake3Hash & h) { sink(h.view()); },
        [&](const std::string & s) { sink(s); },
    }, v);
}

/** Get (data, size) for SQLite BLOB binding. */
inline std::pair<const unsigned char *, size_t> blobData(const DepHashValue & v) {
    return std::visit(overloaded{
        [](const Blake3Hash & h) -> std::pair<const unsigned char *, size_t> {
            return {h.data(), h.size()};
        },
        [](const std::string & s) -> std::pair<const unsigned char *, size_t> {
            return {reinterpret_cast<const unsigned char *>(s.data()), s.size()};
        },
    }, v);
}

/**
 * Returns true if the dep type stores a BLAKE3 hash (not a string).
 */
inline bool isBlake3Dep(DepType type) {
    switch (type) {
    case DepType::Content:
    case DepType::Directory:
    case DepType::NARContent:
    case DepType::EnvVar:
    case DepType::System:
    case DepType::ParentContext:
        return true;
    case DepType::Existence:
    case DepType::CopiedPath:
    case DepType::UnhashedFetch:
    case DepType::CurrentTime:
    case DepType::Exec:
        return false;
    }
    unreachable();
}

/**
 * Sentinel inputName for deps on absolute filesystem paths that are outside
 * any flake input tree. Validated directly against the real filesystem,
 * not through any input accessor. Uses angle brackets to avoid collision
 * with real flake input names.
 */
inline constexpr std::string_view absolutePathDep = "<absolute>";

/**
 * A single dependency: records that a particular resource was accessed,
 * along with a hash of its content at the time of access.
 *
 * For file deps (Content/Directory/Existence), `key` holds the path:
 *   - relative path for flake inputs (with non-empty `source`)
 *   - absolute path for non-flake evaluations (source == "")
 *   - absolute path for out-of-tree flake deps (source == "<absolute>")
 *
 * For non-file deps, `key` holds a descriptive identifier:
 *   - EnvVar: the environment variable name (e.g., "HOME")
 *   - CurrentTime: "currentTime"
 *   - System: "currentSystem"
 *   - UnhashedFetch: the fetch URL
 */
struct Dep {
    /** Flake input name (from lockfile keyMap), "" for self or non-flake,
     *  or "<absolute>" for out-of-tree filesystem paths. */
    std::string source;
    /** Path or identifier key (see above). */
    std::string key;
    /** Hash of content, synthetic hash, or store path (for UnhashedFetch). */
    DepHashValue expectedHash;
    /** What kind of access was performed. */
    DepType type;

    /** Identity is the key (type, source, key); expectedHash is the observed value. */
    bool operator==(const Dep & other) const;
    auto operator<=>(const Dep & other) const;
};

/**
 * A half-open range [start, end) into a session-wide dep vector,
 * representing the deps recorded during a single thunk/app evaluation.
 */
struct DepRange {
    std::vector<Dep> * deps;
    uint32_t start;
    uint32_t end;
};

/**
 * Key for dep deduplication within a single DependencyTracker scope.
 * Two deps with the same (type, source, key) are considered duplicates
 * regardless of their expectedHash value.
 */
struct DepKey {
    DepType type;
    std::string source;
    std::string key;
    bool operator==(const DepKey &) const = default;
    auto operator<=>(const DepKey &) const = default;

    explicit DepKey(const Dep & dep)
        : type(dep.type), source(dep.source), key(dep.key) {}
    DepKey(DepType type, std::string source, std::string key)
        : type(type), source(std::move(source)), key(std::move(key)) {}

    struct Hash {
        size_t operator()(const DepKey & k) const noexcept {
            return hashValues(std::to_underlying(k.type), k.source, k.key);
        }
    };
};

inline bool Dep::operator==(const Dep & other) const { return DepKey(*this) == DepKey(other); }
inline auto Dep::operator<=>(const Dep & other) const { return DepKey(*this) <=> DepKey(other); }

} // namespace nix
