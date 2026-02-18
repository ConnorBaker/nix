#pragma once
///@file

#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/expr/symbol-table.hh"

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

typedef std::variant<
    std::vector<Symbol>,
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

} // namespace nix::eval_trace
