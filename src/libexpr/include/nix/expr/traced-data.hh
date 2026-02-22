#pragma once
///@file

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-trace-deps.hh"
#include "nix/expr/nixexpr.hh"

#include <string>
#include <vector>

namespace nix {

class EvalState;
struct Value;

/**
 * Virtual interface for a node in a parsed data tree (JSON, TOML, or directory listing).
 * Format-agnostic: JsonDataNode, TomlDataNode, and DirDataNode implement this.
 * GC-allocated (inherits from gc, not gc_cleanup — std::string members
 * and backing DOM data leak when GC collects, bounded by file size).
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
 * the whole-file Content dep (from readFile) covers it. This is intentional:
 *
 *   - If code only iterates keys (e.g., mapAttrs, attrNames) without
 *     forcing leaf values, no StructuredContent deps are recorded.
 *     The Content dep alone controls invalidation → any file change
 *     triggers re-evaluation. This is correct because the key set IS
 *     part of the cached result's structure.
 *
 *   - If code accesses specific leaf values (e.g., result.nixpkgs.rev),
 *     StructuredContent deps are recorded for those leaves. The two-level
 *     override allows the trace to survive key-set changes (additions,
 *     removals) because the cached result is the leaf value, not the
 *     full attrset.
 *
 *   - Container Values are registered in a thread-local provenance map
 *     (see registerTracedContainer in dependency-tracker.hh). Shape-observing
 *     builtins (length, attrNames, hasAttr) check this map and record
 *     StructuredContent shape deps (#len for lists, #keys for attrsets,
 *     #has:key for specific attribute existence checks).
 *     The map key is a stable internal pointer (Bindings* for attrsets,
 *     first-element Value* for lists) that survives Value copies.
 *
 * For scalars: records a StructuredContent dep (BLAKE3 of canonical value)
 * and materializes the Nix value.
 *
 * The dep key format is: "filepath\tf:datapath" where f is the format tag
 * and datapath uses '.' for object keys and '[N]' for array indices.
 * Keys containing '.', '[', ']', '"', or '\' are quoted: "key\.with\.dots".
 */
struct ExprTracedData : Expr, gc {
    TracedDataNode * node;
    std::string depSource;  // flake input name (from provenance resolution)
    std::string depKey;     // base file path (before \t separator)
    std::string dataPath;   // dot/bracket path within data structure
    /// Owned provenance for ProvenanceRef pointers. Contains copies of the
    /// string fields above — this duplication is intentional because:
    /// 1. ProvenanceRef consumers see only TracedContainerProvenance*, not ExprTracedData.
    /// 2. String copies are bounded by file size (one per JSON/TOML/directory node).
    /// 3. Using string_view into this->depSource etc. would be fragile if ExprTracedData
    ///    were ever moved (it isn't — GC-allocated — but copies are simpler and safer).
    /// Must be declared AFTER depSource/depKey/dataPath for correct initialization order.
    TracedContainerProvenance provenance;

    ExprTracedData(TracedDataNode * node, std::string depSource,
                   std::string depKey, std::string dataPath)
        : node(node)
        , depSource(std::move(depSource))
        , depKey(std::move(depKey))
        , dataPath(std::move(dataPath))
        , provenance{this->depSource, this->depKey, this->dataPath, node->formatTag()}
    {}

    void eval(EvalState & state, Env & env, Value & v) override;
    void show(const SymbolTable &, std::ostream &) const override {}
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}
};

} // namespace nix
