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

/**
 * Virtual interface for a node in a parsed data tree (JSON, TOML, or directory listing).
 * Format-agnostic: JsonDataNode, TomlDataNode, and DirDataNode implement this.
 * GC-allocated with plain gc (no gc_cleanup). DOM internals (malloc'd by
 * nlohmann::json / toml::value) leak when GC collects, but zero-copy
 * children (pointer into root's DOM) eliminate the N-copy multiplication
 * that was the main OOM cause. One leaked DOM per fromJSON/fromTOML call.
 */
struct TracedDataNode : gc {
    enum class Kind { Object, Array, String, Number, Bool, Null };

    virtual ~TracedDataNode() = default;
    virtual Kind kind() const = 0;

    /** Format tag identifying the data source type. Embedded in dep keys. */
    virtual StructuredFormat formatTag() const = 0;

    /** Object keys (only valid when kind() == Object). */
    virtual std::vector<std::string> objectKeys() const = 0;

    /** Get child node by key (only valid when kind() == Object). GC-allocated. */
    virtual TracedDataNode * objectGet(const std::string & key) const = 0;

    /** Array size (only valid when kind() == Array). */
    virtual size_t arraySize() const = 0;

    /** Get child node by index (only valid when kind() == Array). GC-allocated. */
    virtual TracedDataNode * arrayGet(size_t index) const = 0;

    /** Materialize this scalar into a Nix value (only valid for scalar kinds). */
    virtual void materializeScalar(EvalState & state, Value & v) const = 0;

    /** Canonical string representation for hashing (deterministic per format). */
    virtual std::string canonicalValue() const = 0;
};

/**
 * Expr subclass that lazily materializes parsed data nodes into Nix values.
 *
 * For objects/arrays: creates lazy attrsets/lists with thunks pointing to
 * child ExprTracedData nodes. No dep is recorded for containers — the key
 * set is eagerly materialized as part of the attrset structure, but only
 * the whole-file Content dep (from readFile) covers it.
 *
 * For scalars: records a StructuredContent dep (BLAKE3 of canonical value)
 * and materializes the Nix value.
 *
 * Uses interned IDs (session-scoped pools) instead of strings to avoid
 * O(N) string allocation during tree construction. JSON dep keys are
 * constructed only for non-duplicate deps at recording time.
 */
struct ExprTracedData : Expr, gc {
    TracedDataNode * node;
    DepSourceId sourceId;   ///< interned dep source (flake input name)
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
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}
};

} // namespace nix
