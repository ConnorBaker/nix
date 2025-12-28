#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/immer-gc-policy.hh"

#include <immer/map.hpp>
#include <immer/map_transient.hpp>

#include <algorithm>
#include <cassert>
#include <functional>
#include <ranges>
#include <optional>

namespace nix {

/**
 * Value stored in Immer attribute maps.
 * Contains both the value pointer and position information for error messages.
 */
struct AttrValue {
    Value* value = nullptr;
    PosIdx pos;

    AttrValue() = default;
    AttrValue(Value* v, PosIdx p = noPos) : value(v), pos(p) {}

    bool operator==(const AttrValue& other) const noexcept {
        return value == other.value && pos == other.pos;
    }
};

/**
 * Hash function for Symbol to use in immer::map.
 */
struct SymbolHash {
    using is_transparent = void;

    std::size_t operator()(Symbol s) const noexcept {
        return std::hash<uint32_t>{}(s.getId());
    }
};

/**
 * Equality function for Symbol.
 */
struct SymbolEqual {
    using is_transparent = void;

    bool operator()(Symbol a, Symbol b) const noexcept {
        return a == b;
    }
};

/**
 * Persistent attribute map type for Nix.
 *
 * Uses immer::map (CHAMP trie) which provides:
 * - O(log32 n) ~ O(1) lookup, insert, erase
 * - Structural sharing for efficient derivation
 */
using AttrMap = immer::map<Symbol, AttrValue, SymbolHash, SymbolEqual, gc_memory_policy>;

/**
 * Transient (mutable) version of AttrMap for efficient batch construction.
 */
using AttrMapTransient = typename AttrMap::transient_type;

/**
 * Wrapper for Immer-based attribute bindings.
 * Stores the attribute map and the position of the attribute set itself.
 */
struct ImmerBindings {
    AttrMap map;
    PosIdx pos;

    /**
     * An instance of ImmerBindings with 0 attributes.
     * This object must never be modified.
     */
    static ImmerBindings emptyImmerBindings;

    ImmerBindings() = default;
    ImmerBindings(AttrMap m, PosIdx p = {}) : map(std::move(m)), pos(p) {}

    std::size_t size() const noexcept { return map.size(); }
    bool empty() const noexcept { return map.empty(); }

    const AttrValue* get(Symbol name) const noexcept {
        return map.find(name);
    }
};

class EvalMemory;
struct Value;

/**
 * Map one attribute name to its value.
 */
struct Attr
{
    /* the placement of `name` and `pos` in this struct is important.
       both of them are uint32 wrappers, they are next to each other
       to make sure that Attr has no padding on 64 bit machines. that
       way we keep Attr size at two words with no wasted space. */
    Symbol name;
    PosIdx pos;
    Value * value = nullptr;
    Attr(Symbol name, Value * value, PosIdx pos = noPos)
        : name(name)
        , pos(pos)
        , value(value) {};
    Attr() {};

    auto operator<=>(const Attr & a) const
    {
        return name <=> a.name;
    }
};

static_assert(
    sizeof(Attr) == 2 * sizeof(uint32_t) + sizeof(Value *),
    "performance of the evaluator is highly sensitive to the size of Attr. "
    "avoid introducing any padding into Attr if at all possible, and do not "
    "introduce new fields that need not be present for almost every instance.");

/**
 * Builder for Immer-based attribute sets.
 * Uses a transient map for efficient batch construction, then converts
 * to persistent ImmerBindings on finish().
 *
 * Example usage:
 *   ImmerBindingsBuilder builder(mem, symbols);
 *   builder.insert(sym1, val1, pos1);
 *   builder.insert(sym2, val2, pos2);
 *   ImmerBindings* bindings = builder.finish();
 */
class ImmerBindingsBuilder final
{
public:
    using value_type = std::pair<Symbol, AttrValue>;
    using size_type = std::size_t;

    // Public for C API compatibility
    std::reference_wrapper<EvalMemory> mem;
    std::reference_wrapper<SymbolTable> symbols;

private:
    AttrMapTransient transient;
    PosIdx pos;

public:
    ImmerBindingsBuilder(EvalMemory & mem, SymbolTable & symbols, PosIdx pos = noPos)
        : mem(mem)
        , symbols(symbols)
        , transient(AttrMap{}.transient())
        , pos(pos)
    {
    }

    /**
     * Create a builder initialized with an existing AttrMap.
     * Useful for efficient merge operations.
     */
    ImmerBindingsBuilder(EvalMemory & mem, SymbolTable & symbols, const AttrMap & base, PosIdx pos = noPos)
        : mem(mem)
        , symbols(symbols)
        , transient(base.transient())
        , pos(pos)
    {
    }

    /**
     * Insert an attribute into the builder.
     * If an attribute with the same name already exists, it is replaced.
     */
    void insert(Symbol name, Value * value, PosIdx attrPos = noPos)
    {
        assert(value != nullptr && "inserting null value");
        assert(value->isValid() && "inserting invalid value");
        transient.set(name, AttrValue(value, attrPos));
    }

    /**
     * Insert an attribute, matching the Attr struct format.
     */
    void insert(const Attr & attr)
    {
        transient.set(attr.name, AttrValue(attr.value, attr.pos));
    }

    /**
     * Check if an attribute exists.
     * Note: This temporarily creates a persistent snapshot.
     */
    bool contains(Symbol name)
    {
        return transient.persistent().find(name) != nullptr;
    }

    /**
     * Get an attribute by name.
     * Returns pointer to AttrValue if found, nullptr otherwise.
     * Note: This temporarily creates a persistent snapshot.
     */
    const AttrValue * get(Symbol name)
    {
        return transient.persistent().find(name);
    }

    /**
     * Get the current size.
     * Note: This temporarily creates a persistent snapshot.
     */
    size_type size()
    {
        return transient.persistent().size();
    }

    /**
     * Check if empty.
     * Note: This temporarily creates a persistent snapshot.
     */
    bool empty()
    {
        return transient.persistent().empty();
    }

    /**
     * Allocate a new Value and insert an attribute pointing to it.
     * Returns a reference to the newly allocated Value.
     */
    Value & alloc(Symbol name, PosIdx attrPos = noPos);

    /**
     * Allocate a new Value and insert an attribute pointing to it.
     * Returns a reference to the newly allocated Value.
     */
    Value & alloc(std::string_view name, PosIdx attrPos = noPos);

    /**
     * Complete construction and return GC-allocated ImmerBindings.
     */
    ImmerBindings * finish();

    /**
     * Get the persistent map without finalizing.
     * Useful for intermediate operations.
     * Note: This temporarily creates a persistent snapshot.
     */
    AttrMap persistentMap()
    {
        return transient.persistent();
    }
};

/**
 * Backwards-compatible type alias.
 * New code should use ImmerBindingsBuilder directly.
 */
using BindingsBuilder = ImmerBindingsBuilder;

// Template implementation for Value::forEachAttr
// Must be defined here after ImmerBindings is complete
template<typename F>
void Value::forEachAttr(F && f) const
{
    assert(isa<tAttrsImmer>() && "forEachAttr() called on non-attrs value");
    for (const auto & [name, attrValue] : getStorage<ImmerAttrs>().bindings->map) {
        assert(attrValue.value != nullptr && "forEachAttr() found null value in ImmerBindings");
        assert(attrValue.value->isValid() && "forEachAttr() found invalid value in ImmerBindings");
        f(name, attrValue.value, attrValue.pos);
    }
}

} // namespace nix
