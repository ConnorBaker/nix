#pragma once
///@file

#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/nixexpr.hh"

#include <string>
#include <vector>

namespace nix {

class EvalState;
struct Value;

// Concrete payload types are defined in `traced-data-nodes.hh` —
// only the dispatcher TU and the three parsing TUs include that
// header.  Callers that only construct `ExprTracedData` or operate
// on the abstract `TracedDataNode` base (the test suite, the eval-
// trace cache) include just this file; the concrete types are a
// forward-declared closed set.
struct JsonDataNode;
struct TomlDataNode;
struct DirDataNode;
struct DirScalarNode;

/**
 * Parsed data-tree node (JSON, TOML, or directory listing).
 *
 * GC-allocated with plain `gc` (no `gc_cleanup`).  DOM internals
 * (malloc'd by nlohmann::json / toml::value) leak when GC collects,
 * but zero-copy children (pointer into root's DOM) eliminate the
 * N-copy multiplication that was the main OOM cause.  One leaked
 * DOM per fromJSON/fromTOML call.
 *
 * No virtuals.  `tag_` identifies which concrete type this is; all
 * payload access goes through free functions that switch on `tag_`
 * and `static_cast` to the concrete payload type.  Zero vtable,
 * zero vptr.  The previous virtual interface took ~7 indirect calls
 * per scalar leaf access (kind, formatTag, canonicalValue,
 * materializeScalar, plus container traversal); the switch-based
 * dispatch inlines the leaf cases and the compiler can prove the
 * impossible branches unreachable.
 */
struct TracedDataNode : gc {
    /// Payload tag.  `<Format><Role>`: Role ∈ {Object, Array,
    /// Scalar-of-kind}; Format is inferred from the Tag prefix.
    /// Directory nodes have a flat listing shape and collapse to
    /// just two tags (object-container vs scalar leaf).
    enum class Tag : uint8_t {
        JsonObject,
        JsonArray,
        JsonString,
        JsonNumber,
        JsonBool,
        JsonNull,
        TomlObject,
        TomlArray,
        TomlString,
        TomlNumber,
        TomlBool,
        TomlNull,
        DirObject,
        DirScalar,
    };

    /// Legacy `Kind` vocabulary kept so existing call sites
    /// (`ExprTracedData::eval`'s switch statement) don't churn.
    enum class Kind : uint8_t { Object, Array, String, Number, Bool, Null };

    Tag tag_;

    Kind kind() const noexcept
    {
        switch (tag_) {
        case Tag::JsonObject:
        case Tag::TomlObject:
        case Tag::DirObject:
            return Kind::Object;
        case Tag::JsonArray:
        case Tag::TomlArray:
            return Kind::Array;
        case Tag::JsonString:
        case Tag::TomlString:
        case Tag::DirScalar:
            return Kind::String;
        case Tag::JsonNumber:
        case Tag::TomlNumber:
            return Kind::Number;
        case Tag::JsonBool:
        case Tag::TomlBool:
            return Kind::Bool;
        case Tag::JsonNull:
        case Tag::TomlNull:
            return Kind::Null;
        }
        return Kind::Null;
    }

    StructuredFormat formatTag() const noexcept
    {
        switch (tag_) {
        case Tag::JsonObject:
        case Tag::JsonArray:
        case Tag::JsonString:
        case Tag::JsonNumber:
        case Tag::JsonBool:
        case Tag::JsonNull:
            return StructuredFormat::Json;
        case Tag::TomlObject:
        case Tag::TomlArray:
        case Tag::TomlString:
        case Tag::TomlNumber:
        case Tag::TomlBool:
        case Tag::TomlNull:
            return StructuredFormat::Toml;
        case Tag::DirObject:
        case Tag::DirScalar:
            return StructuredFormat::Directory;
        }
        return StructuredFormat::Json;
    }

    /// Payload-access free functions.  Each switches on `tag_` and
    /// `static_cast`s to the concrete type.  Definitions in
    /// `src/libexpr/eval-trace/data/traced-data-dispatch.cc`.
    std::vector<std::string> objectKeys() const;
    TracedDataNode * objectGet(const std::string & key) const;
    size_t arraySize() const;
    TracedDataNode * arrayGet(size_t index) const;
    void materializeScalar(EvalState & state, Value & v) const;
    std::string canonicalValue() const;

protected:
    explicit TracedDataNode(Tag tag) noexcept : tag_(tag) {}
    // Not virtual — there is nothing to destroy polymorphically
    // (payload destructors run directly on concrete types via the
    // GC, not through the base).
    ~TracedDataNode() = default;
};

/**
 * Expr subclass that lazily materializes parsed data nodes into Nix values.
 *
 * For objects/arrays: creates lazy attrsets/lists with thunks pointing to
 * child ExprTracedData nodes. No dep is recorded for containers — the key
 * set is eagerly materialized as part of the attrset structure, but only
 * the whole-file Content dep (from readFile) covers it.
 *
 * For scalars: records a StructuredContent dep (active-backend digest of the
 * canonical scalar value) and materializes the Nix value.
 *
 * Uses interned IDs (session-scoped pools) instead of strings to avoid
 * O(N) string allocation during tree construction. Structured dep keys stay
 * typed all the way through recording and persistence.
 */
struct ExprTracedData : Expr, gc {
    TracedDataNode * node;
    DepSourceId sourceId;   ///< interned dep source (registered identity)
    FilePathId filePathId;  ///< interned file path
    DataPathId dataPathId;  ///< DataPath trie node

    ExprTracedData(TracedDataNode * node, DepSourceId sourceId,
                   FilePathId filePathId, DataPathId dataPathId)
        : node(node)
        , sourceId(sourceId)
        , filePathId(filePathId)
        , dataPathId(dataPathId)
    {}

    void eval(EvalState & state, Env & env, Value & v) override;
    void show(const SymbolTable &, std::ostream &) const override {}
    void showForHash(const SymbolTable &, std::ostream &, const CanonPath &) const override {}
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}
};

} // namespace nix
