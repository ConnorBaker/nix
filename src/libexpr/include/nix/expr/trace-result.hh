#pragma once
///@file

#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/expr/symbol-table.hh"

#include "nix/util/error.hh"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nix::eval_trace {

// ── Shared types (used by TraceStore, TracedExpr, serialization) ────

enum class ResultKind : uint8_t {
    Placeholder = 0,
    FullAttrs = 1,
    String = 2,
    Missing = 3,
    Misc = 4,
    Failed = 5,
    Bool = 6,
    /** Stores full content for lists of plain strings (e.g., meta.platforms).
     *  Avoids creating N thunks + N child trace lookups that the generic
     *  List kind would require. */
    ListOfStrings = 7,
    Int = 8,
    Path = 9,
    Null = 10,
    Float = 11,
    List = 12,
};

struct placeholder_t {};
struct missing_t {};
struct misc_t {};
struct failed_t {};

struct int_t { NixInt x; };
struct path_t { std::string path; };
struct null_t {};
struct float_t { double x; };
struct list_t { size_t size; };

typedef uint64_t AttrId;
typedef std::pair<AttrId, Symbol> AttrKey;
typedef std::pair<std::string, NixStringContext> string_t;

/**
 * Per-attr TracedData origin reconstruction info for cache materialization.
 * Stored only for attrsets that originated from traced data sources (JSON/TOML/readDir).
 * Empty for the vast majority of attrsets (plain Nix code).
 */
struct attrs_t {
    std::vector<Symbol> names;
    /// Deduplicated TracedData origins for reconstruction during materialization.
    struct Origin {
        std::string depSource;
        std::string depKey;
        std::string dataPath;
        char format; ///< 'j', 't', 'd' (StructuredFormat char)
    };
    std::vector<Origin> origins;       ///< deduplicated
    std::vector<int8_t> originIndices; ///< per-attr; -1 = no origin. Empty when origins is empty.
};

typedef std::variant<
    attrs_t,
    string_t,
    placeholder_t,
    missing_t,
    misc_t,
    failed_t,
    bool,
    int_t,
    std::vector<std::string>,
    path_t,
    null_t,
    float_t,
    list_t>
    CachedResult;

/**
 * Human-readable name for a ResultKind.
 */
inline std::string_view resultKindName(ResultKind k)
{
    switch (k) {
    case ResultKind::Placeholder: return "Placeholder";
    case ResultKind::FullAttrs: return "FullAttrs";
    case ResultKind::String: return "String";
    case ResultKind::Missing: return "Missing";
    case ResultKind::Misc: return "Misc";
    case ResultKind::Failed: return "Failed";
    case ResultKind::Bool: return "Bool";
    case ResultKind::ListOfStrings: return "ListOfStrings";
    case ResultKind::Int: return "Int";
    case ResultKind::Path: return "Path";
    case ResultKind::Null: return "Null";
    case ResultKind::Float: return "Float";
    case ResultKind::List: return "List";
    }
    unreachable();
}

} // namespace nix::eval_trace
