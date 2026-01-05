#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-inline.hh"

#include <algorithm>
#include <ranges>
#include <span>

namespace nix {

Bindings Bindings::emptyBindings;

/* Allocate a new SoA layout for an attribute set with a specific capacity.
   Memory layout after header (no padding needed):
     [positions: PosIdx × capacity] [names: Symbol × capacity] [values: Value* × capacity]
*/
Bindings * EvalMemory::allocBindings(size_t capacity)
{
    if (capacity == 0)
        return &Bindings::emptyBindings;
    if (capacity > std::numeric_limits<Bindings::size_type>::max())
        throw Error("attribute set of size %d is too big", capacity);

    stats.nrAttrsets++;
    stats.nrAttrsInAttrsets += capacity;

    // SoA layout: [header 24B][positions 4B×cap][names 4B×cap][values 8B×cap]
    // No padding needed: 24 + 8*cap is always 8-byte aligned for Value* array
    size_t totalSize = sizeof(Bindings) + capacity * (sizeof(PosIdx) + sizeof(Symbol) + sizeof(Value *));

    auto * bindings = new (allocBytes(totalSize)) Bindings();
    bindings->capacity_ = static_cast<Bindings::size_type>(capacity);
    return bindings;
}

Value & BindingsBuilder::alloc(Symbol name, PosIdx pos)
{
    auto value = mem.get().allocValue();
    bindings->push_back(Attr(name, value, pos));
    return *value;
}

Value & BindingsBuilder::alloc(std::string_view name, PosIdx pos)
{
    return alloc(symbols.get().create(name), pos);
}

void Bindings::sort()
{
    if (numAttrs <= 1)
        return;

    // Use std::views::zip to sort all three arrays in parallel - no heap allocation needed
    auto namesSpan = std::span(namesArray(), numAttrs);
    auto valuesSpan = std::span(valuesArray(), numAttrs);
    auto positionsSpan = std::span(positionsArray(), numAttrs);

    auto zipped = std::views::zip(namesSpan, valuesSpan, positionsSpan);
    std::sort(zipped.begin(), zipped.end(), [](const auto & a, const auto & b) {
        return std::get<0>(a) < std::get<0>(b);
    });
}

Value & Value::mkAttrs(BindingsBuilder & bindings)
{
    mkAttrs(bindings.finish());
    return *this;
}

} // namespace nix
