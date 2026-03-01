#pragma once
///@file

#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/serialise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/std-hash.hh"
#include "nix/util/traced-data-ids.hh"
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

struct Value;
struct SourcePath;

/**
 * Dependency types for eval trace verification (BSàlC verifying trace check).
 *
 * Each DepType maps to a DepKind via depKind(), which classifies its
 * verification behavior:
 *
 *   Normal: Verified by recomputing a hash or re-resolving a reference.
 *     Existence, EnvVar, System, NARContent, CopiedPath, UnhashedFetch
 *
 *   ContentOverrideable: Coarse file/dir dep, overrideable by StructuredContent.
 *     Content, Directory
 *
 *   Volatile: Always fails verification; aborts recovery.
 *     CurrentTime, Exec
 *
 *   Structural: Fine-grained dep deferred to second verification pass.
 *     StructuredContent
 *
 *   ParentContext: Verified by checking parent trace hash.
 *     ParentContext
 *
 * NOTE: Symlink targets are not tracked. resolveSymlinks() follows symlinks
 * but only records the final resolved file as a Content/Directory dep.
 * Changes to intermediate symlink targets will NOT invalidate traced results.
 */
enum class DepType : uint8_t {
    /** File content was read (evalFile, readFile, parseExprFromFile). */
    Content = 1,
    /** Directory listing was read (readDir). Coarse dep: hashes entire listing.
     *  Can be overridden by StructuredContent deps with format tag 'd' (per-entry). */
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
    /** BLAKE3 of canonical representation of a scalar at a data path within
     *  a structured source (JSON/TOML file or directory listing). Key is a
     *  JSON object: {"f":"path","t":"j","p":["nodes","rev"],"s":"keys","h":"key"}.
     *  Enables two-level verification: if Content/Directory dep fails but all
     *  StructuredContent deps pass, trace is valid. */
    StructuredContent = 12,
    /** Creation-time structural fingerprint (#keys, #len) recorded by
     *  ExprTracedData::eval() when materializing containers. Same JSON key
     *  format as StructuredContent. Always ignored during verification — serves
     *  as a conservative bound that doesn't block fine-grained override. */
    ImplicitShape = 13,
    /** Raw readFile content observed by a string builtin (stringLength, hashString,
     *  substring, match, split, replaceStrings) or eqValues. Recorded at the
     *  consumer site, not at readFile. Prevents StructuredContent two-level
     *  override from incorrectly covering raw byte observations.
     *  Same verification as Content (BLAKE3 of file bytes), but classified as
     *  Normal (not ContentOverrideable) — cannot be overridden by SC deps. */
    RawContent = 14,

    /** Not a real dep type. Must remain last — used to size the descriptor table. */
    EndSentinel_,
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
    case DepType::StructuredContent: return "structuredContent";
    case DepType::ImplicitShape: return "implicitShape";
    case DepType::RawContent: return "rawContent";
    case DepType::EndSentinel_: break;
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
 * Behavioral classification of DepType values.
 *
 * Each DepType maps to exactly one DepKind via depKind(). Adding a new
 * DepType without updating depKind() produces a -Wswitch compiler warning,
 * ensuring the categorization is kept in sync.
 */
enum class DepKind : uint8_t {
    /** Recomputable hash-verified dep (Existence, EnvVar, System, NARContent, CopiedPath, UnhashedFetch). */
    Normal = 0,
    /** Always fails verification; aborts recovery (CurrentTime, Exec). */
    Volatile = 1,
    /** Coarse file/dir dep overrideable by StructuredContent (Content, Directory). */
    ContentOverrideable = 2,
    /** Fine-grained structural dep; deferred to second verification pass (StructuredContent). */
    Structural = 3,
    /** Parent context dep; verified via trace hash lookup (ParentContext). */
    ParentContext = 4,
    /** Creation-time structural fingerprint; always ignored during verification (ImplicitShape). */
    ImplicitStructural = 5,
};

/**
 * Behavioral descriptor for a DepType. One entry per DepType in a constexpr
 * table. Adding a new DepType without a descriptor triggers -Wswitch in
 * the initializer function.
 */
struct DepKindDescriptor {
    DepKind kind;
    bool isBlake3;        ///< true = stores BLAKE3 hash; false = stores string
    bool isOverrideable;  ///< true = can be overridden by StructuredContent
    bool isVolatile;      ///< true = always fails verification
};

/**
 * Build the descriptor for a single DepType. Constexpr; -Wswitch on the
 * DepType switch ensures all variants are covered.
 */
inline constexpr DepKindDescriptor makeDescriptor(DepType type)
{
    switch (type) {
    case DepType::Content:
        return {DepKind::ContentOverrideable, true, true, false};
    case DepType::Directory:
        return {DepKind::ContentOverrideable, true, true, false};
    case DepType::Existence:
        return {DepKind::Normal, false, false, false};
    case DepType::EnvVar:
        return {DepKind::Normal, true, false, false};
    case DepType::CurrentTime:
        return {DepKind::Volatile, false, false, true};
    case DepType::System:
        return {DepKind::Normal, true, false, false};
    case DepType::UnhashedFetch:
        return {DepKind::Normal, false, false, false};
    case DepType::ParentContext:
        return {DepKind::ParentContext, true, false, false};
    case DepType::CopiedPath:
        return {DepKind::Normal, false, false, false};
    case DepType::Exec:
        return {DepKind::Volatile, false, false, true};
    case DepType::NARContent:
        return {DepKind::Normal, true, false, false};
    case DepType::StructuredContent:
        return {DepKind::Structural, true, false, false};
    case DepType::ImplicitShape:
        return {DepKind::ImplicitStructural, true, false, false};
    case DepType::RawContent:
        return {DepKind::Normal, true, false, false};
    case DepType::EndSentinel_:
        break;
    }
    unreachable();
}

/// Constexpr descriptor table indexed by DepType value.
/// DepType values start at 1, so index 0 is unused (default-initialized).
inline constexpr auto depDescriptors = [] {
    constexpr size_t N = std::to_underlying(DepType::EndSentinel_);
    std::array<DepKindDescriptor, N> table{};
    table[std::to_underlying(DepType::Content)] = makeDescriptor(DepType::Content);
    table[std::to_underlying(DepType::Directory)] = makeDescriptor(DepType::Directory);
    table[std::to_underlying(DepType::Existence)] = makeDescriptor(DepType::Existence);
    table[std::to_underlying(DepType::EnvVar)] = makeDescriptor(DepType::EnvVar);
    table[std::to_underlying(DepType::CurrentTime)] = makeDescriptor(DepType::CurrentTime);
    table[std::to_underlying(DepType::System)] = makeDescriptor(DepType::System);
    table[std::to_underlying(DepType::UnhashedFetch)] = makeDescriptor(DepType::UnhashedFetch);
    table[std::to_underlying(DepType::ParentContext)] = makeDescriptor(DepType::ParentContext);
    table[std::to_underlying(DepType::CopiedPath)] = makeDescriptor(DepType::CopiedPath);
    table[std::to_underlying(DepType::Exec)] = makeDescriptor(DepType::Exec);
    table[std::to_underlying(DepType::NARContent)] = makeDescriptor(DepType::NARContent);
    table[std::to_underlying(DepType::StructuredContent)] = makeDescriptor(DepType::StructuredContent);
    table[std::to_underlying(DepType::ImplicitShape)] = makeDescriptor(DepType::ImplicitShape);
    table[std::to_underlying(DepType::RawContent)] = makeDescriptor(DepType::RawContent);
    return table;
}();

/// Look up the descriptor for a DepType. O(1) table lookup.
inline constexpr const DepKindDescriptor & describe(DepType type)
{
    return depDescriptors[std::to_underlying(type)];
}

/**
 * Classify a DepType into its behavioral kind.
 */
inline constexpr DepKind depKind(DepType type)
{
    return describe(type).kind;
}

/**
 * Human-readable name for a DepKind.
 */
inline constexpr std::string_view depKindName(DepKind kind)
{
    switch (kind) {
    case DepKind::Normal: return "normal";
    case DepKind::Volatile: return "volatile";
    case DepKind::ContentOverrideable: return "contentOverrideable";
    case DepKind::Structural: return "structural";
    case DepKind::ParentContext: return "parentContext";
    case DepKind::ImplicitStructural: return "implicitStructural";
    }
    unreachable();
}

/**
 * Returns true if the dep type is volatile (always fails verification).
 */
inline constexpr bool isVolatile(DepType type)
{
    return describe(type).isVolatile;
}

/**
 * Returns true if the dep type is a coarse content/directory dep
 * that can be overridden by fine-grained StructuredContent deps.
 */
inline constexpr bool isContentOverrideable(DepType type)
{
    return describe(type).isOverrideable;
}

/**
 * Returns true if the dep type stores a BLAKE3 hash (not a string).
 */
inline constexpr bool isBlake3Dep(DepType type)
{
    return describe(type).isBlake3;
}

/**
 * Format tag for StructuredContent deps identifying the data source type.
 * Stored as the "t" field in JSON dep keys: {"t":"j",...}.
 */
enum class StructuredFormat : char {
    Json      = 'j',
    Toml      = 't',
    Directory = 'd',
};

/**
 * Convert a StructuredFormat to its wire-format character.
 */
inline constexpr char structuredFormatChar(StructuredFormat f)
{
    return static_cast<char>(f);
}

/**
 * Parse a wire-format character to a StructuredFormat, or nullopt if invalid.
 */
inline constexpr std::optional<StructuredFormat> parseStructuredFormat(char c)
{
    switch (c) {
    case 'j': return StructuredFormat::Json;
    case 't': return StructuredFormat::Toml;
    case 'd': return StructuredFormat::Directory;
    default: return std::nullopt;
    }
}

/**
 * Human-readable name for a StructuredFormat (for diagnostics/logging).
 */
inline constexpr std::string_view structuredFormatName(StructuredFormat f)
{
    switch (f) {
    case StructuredFormat::Json: return "json";
    case StructuredFormat::Toml: return "toml";
    case StructuredFormat::Directory: return "directory";
    }
    unreachable();
}

/**
 * Shape suffix for StructuredContent deps on containers.
 * Appended to the data path in dep keys.
 */
enum class ShapeSuffix : uint8_t {
    None = 0,  ///< Scalar leaf access — no suffix
    Len  = 1,  ///< List/array length (#len)
    Keys = 2,  ///< Object/attrset key set (#keys)
    Type = 3,  ///< Container type — "object" or "array" (#type)
};

/**
 * Display name for a ShapeSuffix (for diagnostics).
 * Returns "", "len", "keys", or "type".
 */
inline constexpr std::string_view shapeSuffixName(ShapeSuffix s)
{
    switch (s) {
    case ShapeSuffix::None: return "";
    case ShapeSuffix::Len: return "len";
    case ShapeSuffix::Keys: return "keys";
    case ShapeSuffix::Type: return "type";
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
 * Compact interned dependency: stores StringInternTable indices instead of
 * owned strings. DepSourceId and DepKeyId share the same index space as
 * StringId — all three are uint32_t indices into InterningPools::strings.
 * Zero per-dep heap allocation; string data lives in the arena.
 * Resolve via pools.resolve(sourceId) / pools.resolve(keyId).
 */
struct CompactDep {
    DepType type;
    DepSourceId sourceId;    ///< Flake input name (interned in StringInternTable)
    DepKeyId keyId;          ///< Dep key string (interned in StringInternTable)
    DepHashValue expectedHash;
};

/**
 * A half-open range [start, end) into a session-wide dep vector,
 * representing the deps recorded during a single thunk/app evaluation.
 */
struct DepRange {
    std::vector<CompactDep> * deps;
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

inline bool Dep::operator==(const Dep & other) const {
    return type == other.type && source == other.source && key == other.key;
}
inline auto Dep::operator<=>(const Dep & other) const {
    if (auto cmp = type <=> other.type; cmp != 0) return cmp;
    if (auto cmp = source <=> other.source; cmp != 0) return cmp;
    return key <=> other.key;
}

// ═══════════════════════════════════════════════════════════════════════
// Provenance — eval-trace origin data for PosTable positions
// ═══════════════════════════════════════════════════════════════════════

/**
 * Full provenance record for a position originating from traced data
 * (JSON, TOML, directory listings). Stored in a ProvenanceTable indexed
 * by the opaque Pos::ProvenanceRef::id.
 */
struct ProvenanceRecord {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    char format;             ///< 'j', 't', 'd' (StructuredFormat char)
};

/**
 * Flat table of provenance records. Append-only within a session.
 * Owned by InterningPools (Lifetime 1).
 */
struct ProvenanceTable {
    std::vector<ProvenanceRecord> records;

    uint32_t allocate(DepSourceId srcId, FilePathId fpId, DataPathId dpId, char fmt) {
        uint32_t id = static_cast<uint32_t>(records.size());
        records.push_back({srcId, fpId, dpId, fmt});
        return id;
    }

    const ProvenanceRecord & resolve(uint32_t id) const {
        assert(id < records.size());
        return records[id];
    }

    void clear() { records.clear(); }
};

} // namespace nix
