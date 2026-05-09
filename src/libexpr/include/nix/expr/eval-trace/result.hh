#pragma once
///@file

#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/expr/eval-trace/child-meta.hh"
#include "nix/expr/eval-trace/semantic-objects.hh"

#include "nix/util/error.hh"

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nix::eval_trace {

// ── Shared types (used by TraceStore, TracedExpr, serialization) ────

inline constexpr uint32_t kSemanticResultEncodingVersion = 2;

/// Persisted result kind.  Epoch v25 (P-RC): the 4 formerly-trivial
/// variants (Placeholder, Missing, Misc, Null) collapse to a single
/// Trivial kind whose payload is a 1-byte sub-tag (see TrivialKind).
enum class ResultKind : uint8_t {
    Trivial = 0,     ///< Placeholder / Missing / Misc / Null (sub-tag in payload)
    FullAttrs = 1,
    String = 2,
    Failed = 5,
    Bool = 6,
    Int = 8,
    Path = 9,
    Float = 11,
    List = 12,
};

/// Sub-tag for the Trivial result kind.  Persisted as a 1-character
/// payload in the Results table.
enum class TrivialKind : uint8_t {
    Placeholder,   ///< "p" — placeholder awaiting real eval
    Missing,       ///< "m" — recorded miss
    Misc,          ///< "s" — miscellaneous (function/external/thunk/failed-to-cache)
    Null,          ///< "n" — evaluated null
};

/// Trivial result variant — absorbs the old placeholder_t / missing_t /
/// misc_t / null_t empty structs.  Distinguished at the value level by
/// `kind`; reaches its own behavior at replay/record time by
/// `trivial.kind`.
struct trivial_t { TrivialKind kind; };

/// Normalized deterministic error kernel.
/// Stores the error message from a failed evaluation so it can be replayed
/// without re-evaluation when deps are unchanged. The message is normalized
/// by the recording path (strip store path prefixes for cross-session stability).
struct failed_t {
    std::string errorMessage;
};

struct int_t { NixInt x; };

struct path_t {
    std::string path;
    SemanticHandle publication;
};
struct float_t { double x; };
struct list_t {
    std::vector<CachedListEntry> entries;
    std::optional<TracedContainerMeta> meta;
};

struct string_t {
    std::string first;
    NixStringContext second;
    SemanticHandle publication;
};

/**
 * Per-attr TracedData origin reconstruction info for cache materialization.
 * Stored only for attrsets that originated from traced data sources (JSON/TOML/readDir).
 * Empty for the vast majority of attrsets (plain Nix code).
 */
struct attrs_t {
    std::vector<CachedAttrEntry> entries;
    std::optional<TracedContainerMeta> meta;
};

typedef std::variant<
    attrs_t,
    string_t,
    trivial_t,
    failed_t,
    bool,
    int_t,
    path_t,
    float_t,
    list_t>
    CachedResult;

/// Convenience predicate: is this a "trivial" cache entry (placeholder /
/// missing / misc / null)?  Used in three call sites that previously
/// checked `holds_alternative<misc_t || missing_t || placeholder_t>` or
/// similar.
inline bool isTrivialResult(const CachedResult & r) noexcept
{
    return std::holds_alternative<trivial_t>(r);
}

/// Make a trivial result of the given sub-kind.
inline trivial_t makePlaceholder() noexcept { return trivial_t{TrivialKind::Placeholder}; }
inline trivial_t makeMissing() noexcept { return trivial_t{TrivialKind::Missing}; }
inline trivial_t makeMisc() noexcept { return trivial_t{TrivialKind::Misc}; }
inline trivial_t makeNull() noexcept { return trivial_t{TrivialKind::Null}; }

struct EncodedResultPayload {
    ResultKind type;
    uint32_t encodingVersion = kSemanticResultEncodingVersion;
    std::string payload;
    std::string auxContext;
};

/**
 * Human-readable name for a ResultKind.
 */
inline std::string_view resultKindName(ResultKind k)
{
    switch (k) {
    case ResultKind::Trivial: return "Trivial";
    case ResultKind::FullAttrs: return "FullAttrs";
    case ResultKind::String: return "String";
    case ResultKind::Failed: return "Failed";
    case ResultKind::Bool: return "Bool";
    case ResultKind::Int: return "Int";
    case ResultKind::Path: return "Path";
    case ResultKind::Float: return "Float";
    case ResultKind::List: return "List";
    }
    unreachable();
}

/// Human-readable name for a TrivialKind sub-tag.
inline std::string_view trivialKindName(TrivialKind k)
{
    switch (k) {
    case TrivialKind::Placeholder: return "Placeholder";
    case TrivialKind::Missing: return "Missing";
    case TrivialKind::Misc: return "Misc";
    case TrivialKind::Null: return "Null";
    }
    unreachable();
}

} // namespace nix::eval_trace
