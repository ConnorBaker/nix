#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-inline.hh"

#include <algorithm>
#include <ranges>
#include <span>

namespace nix {

Bindings Bindings::emptyBindings;

/* Allocate a new SoA layout for an attribute set with a specific capacity.
   Memory layout after header:
     [names: Symbol × capacity] [padding to 8-byte align] [values: Value* × capacity] [positions: PosIdx × capacity]
*/
Bindings * EvalMemory::allocBindings(size_t capacity)
{
    if (capacity == 0)
        return &Bindings::emptyBindings;
    if (capacity > std::numeric_limits<Bindings::size_type>::max())
        throw Error("attribute set of size %d is too big", capacity);

    stats.nrAttrsets++;
    stats.nrAttrsInAttrsets += capacity;

    // Calculate SoA layout sizes with proper alignment
    size_t headerSize = sizeof(Bindings);
    size_t namesSize = capacity * sizeof(Symbol);
    size_t namesEnd = headerSize + namesSize;
    size_t valuesStart = (namesEnd + 7) & ~size_t{7};  // 8-byte align for Value*
    size_t valuesSize = capacity * sizeof(Value *);
    size_t positionsSize = capacity * sizeof(PosIdx);

    size_t totalSize = valuesStart + valuesSize + positionsSize;

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
