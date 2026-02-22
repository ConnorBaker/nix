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
     *  a structured source (JSON/TOML file or directory listing). Key format:
     *  "filepath\tf:datapath" where f is format tag ('j'=JSON, 't'=TOML, 'd'=directory).
     *  Enables two-level verification: if Content/Directory dep fails but all
     *  StructuredContent deps pass, trace is valid. */
    StructuredContent = 12,
    /** Creation-time structural fingerprint (#keys, #len) recorded by
     *  ExprTracedData::eval() when materializing containers. Same key format
     *  as StructuredContent. Always ignored during verification — serves as
     *  a conservative bound that doesn't block fine-grained override. */
    ImplicitShape = 13,
    /** Raw readFile content observed by a string builtin (stringLength, hashString,
     *  substring, match, split, replaceStrings) or eqValues. Recorded at the
     *  consumer site, not at readFile. Prevents StructuredContent two-level
     *  override from incorrectly covering raw byte observations.
     *  Same verification as Content (BLAKE3 of file bytes), but classified as
     *  Normal (not ContentOverrideable) — cannot be overridden by SC deps. */
    RawContent = 14,
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
 * Classify a DepType into its behavioral kind.
 * Exhaustive switch — adding a new DepType without a case here triggers -Wswitch.
 */
inline constexpr DepKind depKind(DepType type)
{
    switch (type) {
    case DepType::Content:
    case DepType::Directory:
        return DepKind::ContentOverrideable;
    case DepType::Existence:
    case DepType::EnvVar:
    case DepType::System:
    case DepType::NARContent:
    case DepType::CopiedPath:
    case DepType::UnhashedFetch:
    case DepType::RawContent:
        return DepKind::Normal;
    case DepType::CurrentTime:
    case DepType::Exec:
        return DepKind::Volatile;
    case DepType::StructuredContent:
        return DepKind::Structural;
    case DepType::ImplicitShape:
        return DepKind::ImplicitStructural;
    case DepType::ParentContext:
        return DepKind::ParentContext;
    }
    unreachable();
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
 * Shorthand for the most frequent DepKind check (~6 call sites).
 */
inline constexpr bool isVolatile(DepType type)
{
    return depKind(type) == DepKind::Volatile;
}

/**
 * Returns true if the dep type is a coarse content/directory dep
 * that can be overridden by fine-grained StructuredContent deps.
 */
inline constexpr bool isContentOverrideable(DepType type)
{
    return depKind(type) == DepKind::ContentOverrideable;
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
    case DepType::StructuredContent:
    case DepType::ImplicitShape:
    case DepType::RawContent:
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
 * Format tag for StructuredContent deps identifying the data source type.
 * Stored as a single char in dep keys: "filepath\tf:datapath".
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
 * Wire-format string for a ShapeSuffix (appended to data path in dep keys).
 * Returns "", "#len", or "#keys".
 */
inline constexpr std::string_view shapeSuffixString(ShapeSuffix s)
{
    switch (s) {
    case ShapeSuffix::None: return "";
    case ShapeSuffix::Len: return "#len";
    case ShapeSuffix::Keys: return "#keys";
    case ShapeSuffix::Type: return "#type";
    }
    unreachable();
}

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
 * Parse a data path, splitting off any trailing shape suffix (#len or #keys).
 * Returns the base data path (without suffix) and the parsed ShapeSuffix.
 */
inline std::pair<std::string, ShapeSuffix> parseShapeSuffix(std::string_view dataPath)
{
    if (dataPath.size() >= 4 && dataPath.substr(dataPath.size() - 4) == "#len")
        return {std::string(dataPath.substr(0, dataPath.size() - 4)), ShapeSuffix::Len};
    if (dataPath.size() >= 5 && dataPath.substr(dataPath.size() - 5) == "#keys")
        return {std::string(dataPath.substr(0, dataPath.size() - 5)), ShapeSuffix::Keys};
    if (dataPath.size() >= 5 && dataPath.substr(dataPath.size() - 5) == "#type")
        return {std::string(dataPath.substr(0, dataPath.size() - 5)), ShapeSuffix::Type};
    return {std::string(dataPath), ShapeSuffix::None};
}

/**
 * Build a StructuredContent dep key in the canonical wire format:
 *   "filepath\tf:datapath[#suffix]"
 */
inline std::string buildStructuredDepKey(
    std::string_view depKey, StructuredFormat format,
    std::string_view dataPath, ShapeSuffix shape = ShapeSuffix::None)
{
    std::string result;
    result.reserve(depKey.size() + 3 + dataPath.size() + 5);
    result += depKey;
    result += '\t';
    result += structuredFormatChar(format);
    result += ':';
    result += dataPath;
    result += shapeSuffixString(shape);
    return result;
}

/**
 * Build a StructuredContent dep key with a raw string suffix (e.g., "#has:key").
 * The suffix is appended verbatim — caller must ensure it is properly escaped
 * (use escapeDataPathKey for key names embedded in the suffix).
 */
inline std::string buildStructuredDepKey(
    std::string_view depKey, StructuredFormat format,
    std::string_view dataPath, std::string_view rawSuffix)
{
    std::string result;
    result.reserve(depKey.size() + 3 + dataPath.size() + rawSuffix.size());
    result += depKey;
    result += '\t';
    result += structuredFormatChar(format);
    result += ':';
    result += dataPath;
    result += rawSuffix;
    return result;
}

/**
 * Escape a data path key segment for use in dep key construction.
 * Keys containing '.', '[', ']', '"', '\', or '#' are quoted with "..."
 * and inner '"' / '\' are backslash-escaped.
 *
 * The '#' quoting is critical for preventing ambiguity with shape suffixes
 * (#len, #keys, #type) and #has: prefixes. A key like "#has:foo" is escaped
 * to "\"#has:foo\"", which cannot collide with the #has: dep key syntax
 * because the quoted form never appears bare in a data path.
 *
 * Inverse: unescapeDataPathKey (must be used when parsing #has:key from dep keys).
 */
inline std::string escapeDataPathKey(std::string_view key)
{
    bool needsQuote = false;
    for (char c : key) {
        if (c == '.' || c == '[' || c == ']' || c == '"' || c == '\\' || c == '#') {
            needsQuote = true;
            break;
        }
    }
    if (!needsQuote) return std::string(key);

    std::string out;
    out.reserve(key.size() + 4);
    out += '"';
    for (char c : key) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

/**
 * Unescape a single data path key segment. Reverses escapeDataPathKey.
 */
inline std::string unescapeDataPathKey(std::string_view key)
{
    if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
        std::string result;
        result.reserve(key.size() - 2);
        for (size_t i = 1; i + 1 < key.size(); i++) {
            if (key[i] == '\\' && i + 2 < key.size())
                i++; // skip backslash
            result += key[i];
        }
        return result;
    }
    return std::string(key);
}

/**
 * Parse and pretty-print a StructuredContent dep key for diagnostics.
 * Input:  "flake.lock\tj:.nodes.nixpkgs.locked.rev#len"
 * Output: "flake.lock [json] .nodes.nixpkgs.locked.rev #len"
 * Falls back to the raw key on parse failure.
 */
inline std::string formatStructuredDepKey(std::string_view key)
{
    auto sep = key.find('\t');
    if (sep == std::string::npos || sep + 2 >= key.size())
        return std::string(key);
    auto format = parseStructuredFormat(key[sep + 1]);
    if (!format || key[sep + 2] != ':')
        return std::string(key);
    auto filePath = key.substr(0, sep);
    auto dataPathWithSuffix = key.substr(sep + 3);
    auto [dataPath, shape] = parseShapeSuffix(dataPathWithSuffix);

    std::string result;
    result += filePath;
    result += " [";
    result += structuredFormatName(*format);
    result += "] ";
    result += dataPath;
    auto suffix = shapeSuffixString(shape);
    if (!suffix.empty()) {
        result += ' ';
        result += suffix;
    }
    return result;
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

inline bool Dep::operator==(const Dep & other) const {
    return type == other.type && source == other.source && key == other.key;
}
inline auto Dep::operator<=>(const Dep & other) const {
    if (auto cmp = type <=> other.type; cmp != 0) return cmp;
    if (auto cmp = source <=> other.source; cmp != 0) return cmp;
    return key <=> other.key;
}

} // namespace nix
