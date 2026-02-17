#pragma once
///@file

#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/serialise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/util.hh"

#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace nix {

class Value;
struct SourcePath;

/**
 * Dependency types for eval cache invalidation.
 *
 * Category A (Hash Oracle): Validated by recomputing a hash and comparing.
 *   Content, Directory, Existence, EnvVar, System
 *
 * Category B (Reference Resolution): Validated by re-resolving a reference
 *   (store path computation or re-fetch) and comparing.
 *   CopiedPath, UnhashedFetch
 *
 * Category C (Volatile): Always invalidates -- cached values are never reused.
 *   CurrentTime, Exec
 *
 * Category D (Structural): Validated by checking parent context hash.
 *   ParentContext
 *
 * NOTE: Symlink targets are not tracked. resolveSymlinks() follows symlinks
 * but only records the final resolved file as a Content/Directory dep.
 * Changes to intermediate symlink targets will NOT invalidate cached entries.
 */
enum class DepType : int {
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
    /** Parent evaluation context — invalidates when parent dep set changes. */
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
 * Human-readable name for a DepType, for debug logging.
 */
const char * depTypeName(DepType type);

/**
 * Fixed-size BLAKE3-256 hash. Stack-allocated, no heap allocation.
 * Used throughout the eval cache for content hashes, dep set hashes,
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

    /** Format as hex for logging (cold path only). */
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
    return false;
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
};

/**
 * A half-open range [start, end) into a session-wide dep vector,
 * representing the deps recorded during a single thunk/app evaluation.
 */
struct EpochRange {
    std::vector<Dep> * deps;
    uint32_t start;
    uint32_t end;
};

/**
 * Key for dep deduplication within a single FileLoadTracker scope.
 * Two deps with the same (type, source, key) are considered duplicates
 * regardless of their expectedHash value.
 */
struct DepKey {
    DepType type;
    std::string source;
    std::string key;
    bool operator==(const DepKey &) const = default;
    struct Hash {
        size_t operator()(const DepKey & k) const noexcept {
            size_t h = std::hash<int>{}(static_cast<int>(k.type));
            h ^= std::hash<std::string>{}(k.source) * 1099511628211ULL;
            h ^= std::hash<std::string>{}(k.key) * 14695981039346656037ULL;
            return h;
        }
    };
};

/**
 * RAII tracker that records file dependencies during evaluation.
 *
 * All deps are recorded into a single thread-local sessionDeps vector.
 * Each tracker records its startIndex at construction time. The range
 * [startIndex, sessionDeps.size()) represents deps recorded during
 * this tracker's lifetime. Deps from previously-evaluated thunks are
 * replayed via replayedRanges (epoch ranges from before this tracker
 * started).
 *
 * This is safe because evaluation is single-threaded.
 */
struct FileLoadTracker {
    static thread_local FileLoadTracker * activeTracker;
    static thread_local std::vector<Dep> sessionDeps;

    FileLoadTracker * previous;
    std::vector<Dep> * mySessionDeps;
    uint32_t startIndex;
    std::vector<EpochRange> replayedRanges;
    std::unordered_set<const Value *> replayedValues;
    std::unordered_set<DepKey, DepKey::Hash> recordedKeys;

    FileLoadTracker()
        : previous(activeTracker)
        , mySessionDeps(&sessionDeps)
        , startIndex(mySessionDeps->size())
    {
        activeTracker = this;
    }
    ~FileLoadTracker() { activeTracker = previous; }

    FileLoadTracker(const FileLoadTracker &) = delete;
    FileLoadTracker & operator=(const FileLoadTracker &) = delete;

    /**
     * Record a dependency into the session-wide dep vector.
     * Deduplicates by (type, source, key) within the active tracker scope.
     */
    static void record(const Dep & dep);

    /**
     * Collect all deps: session range [startIndex, current) plus
     * all replayed epoch ranges.
     */
    std::vector<Dep> collectDeps() const;

    /**
     * Returns true if there is at least one active tracker.
     */
    static bool isActive() { return activeTracker != nullptr; }

    /**
     * Clear the session-wide dep vector. Called from resetFileCache().
     */
    static void clearSessionDeps();
};

/**
 * RAII guard that temporarily suspends dep recording by setting
 * activeTracker to nullptr. On destruction, restores the previous tracker.
 *
 * Used in ExprOrigChild::eval() to prevent recording the parent's massive
 * file deps (e.g., 10K+ deps from evaluating all of nixpkgs). Nested
 * FileLoadTrackers created within the suspended scope work correctly:
 * their constructors set activeTracker = this, and their destructors
 * restore nullptr (the suspended value).
 */
struct SuspendFileLoadTracker {
    FileLoadTracker * saved;

    SuspendFileLoadTracker()
        : saved(FileLoadTracker::activeTracker)
    {
        FileLoadTracker::activeTracker = nullptr;
    }
    ~SuspendFileLoadTracker() { FileLoadTracker::activeTracker = saved; }

    SuspendFileLoadTracker(const SuspendFileLoadTracker &) = delete;
    SuspendFileLoadTracker & operator=(const SuspendFileLoadTracker &) = delete;
};

/**
 * Compute a BLAKE3 hash of data. Zero-allocation, returns stack-allocated Blake3Hash.
 */
Blake3Hash depHash(std::string_view data);

/**
 * Compute a BLAKE3 hash of a path's NAR serialization using streaming API.
 * Unlike depHash() which hashes raw file bytes, this captures the executable
 * bit via the NAR format. Used for builtins.path filtered file deps where
 * the resulting store path depends on permissions.
 */
Blake3Hash depHashPath(const SourcePath & path);

/**
 * Compute a BLAKE3 hash of a directory listing using streaming API.
 * Each entry is hashed as "name:typeInt;" where typeInt is the
 * numeric value of the optional file type (-1 if unknown).
 * The entries map is iterated in its natural (lexicographic) order.
 */
Blake3Hash depHashDirListing(const SourceAccessor::DirEntries & entries);

/**
 * Stat-cached Content hash: looks up the physical file's stat metadata in
 * the persistent stat-hash cache before falling back to depHash(readFile()).
 */
Blake3Hash depHashFile(const SourcePath & path);

/**
 * Stat-cached NARContent hash: like depHashPath() but checks stat cache first.
 */
Blake3Hash depHashPathCached(const SourcePath & path);

/**
 * Stat-cached Directory hash: like depHashDirListing() but checks stat cache
 * for the directory's own stat metadata first.
 */
Blake3Hash depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries);

/**
 * Resolve an absolute path to an (inputName, relativePath) pair using
 * a mount-point-to-input mapping. Walks up the path trying each prefix.
 */
std::optional<std::pair<std::string, CanonPath>> resolveToInput(
    const CanonPath & absPath,
    const std::map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

/**
 * Record a file dependency, resolving to an input-relative path if possible.
 * In non-flake mode (mountToInput empty), records absolute paths with
 * inputName="". In flake mode, paths that can't be resolved to any input
 * are recorded with inputName="<absolute>" so they are validated directly
 * against the real filesystem.
 */
void recordDep(
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const std::map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

} // namespace nix
