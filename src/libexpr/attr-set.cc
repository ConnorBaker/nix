#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-inline.hh"

#include <algorithm>

namespace nix {

const constinit Bindings Bindings::emptyBindings;

/* Allocate a new array of attributes for an attribute set with a specific
   capacity. The space is implicitly reserved after the Bindings
   structure. */
Bindings * EvalMemory::allocBindings(size_t capacity)
{
    if (capacity == 0)
        /* Swear that we are not going to modify this. */
        return const_cast<Bindings *>(&Bindings::emptyBindings);
    if (capacity > std::numeric_limits<Bindings::size_type>::max())
        throw Error("attribute set of size %d is too big", capacity);
    stats.nrAttrsets++;
    stats.nrAttrsInAttrsets += capacity;
    return new (allocBytes<Attr, sizeof(Bindings)>(capacity)) Bindings();
}

const Bindings * EvalMemory::shareLayer(const Bindings & top, const Bindings & base)
{
    assert(!top.isLayered());
    stats.nrAttrsets++;
    auto b = new (allocBytes(sizeof(Bindings))) Bindings();
    b->arrayOf = &top;
    b->numAttrs = top.numAttrs;
    b->baseLayer = &base;
    b->numLayers = base.numLayers + 1;
    b->countChain();
    return b;
}

void Bindings::countChain()
{
    auto & base = *baseLayer;
    auto own = layer();

    size_type duplicates = 0;

    /* If the base is smaller than this layer, iterate both with
       std::set_intersection in O(|base| + |own|); otherwise look each of
       this layer's attributes up in the base, so that a small layer over a
       large base does not iterate the base. */
    if (own.size() > base.size()) {
        std::set_intersection(
            base.begin(),
            base.end(),
            own.begin(),
            own.end(),
            boost::make_function_output_iterator([&]([[maybe_unused]] auto && _) { ++duplicates; }));
    } else {
        for (const auto & attr : own) {
            if (base.get(attr.name))
                ++duplicates;
        }
    }

    numAttrsInChain = base.numAttrsInChain + own.size() - duplicates;
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
    std::sort(attrs, attrs + numAttrs);
}

Value & Value::mkAttrs(BindingsBuilder & bindings)
{
    mkAttrs(bindings.finish());
    return *this;
}

} // namespace nix
