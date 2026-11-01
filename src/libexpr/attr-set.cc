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

const Bindings * EvalMemory::shareLayer(const Bindings & top, const Bindings & base, std::optional<size_t> size)
{
    stats.nrAttrsets++;
    auto b = new (allocBytes(sizeof(Bindings) + sizeof(Attr))) Bindings();
    b->numAttrs = top.numAttrs;
    b->numLayers = base.numLayers + 1;
    Bindings::Layering l{&base, top.owner()};
    std::memcpy(static_cast<void *>(b->attrs), &l, sizeof l);
    if (size)
        b->numAttrsInChain = *size;
    else
        b->countChain();
    return b;
}

void Bindings::countChain()
{
    auto & base = *baseLayer();
    auto own = layer();

    size_type duplicates = 0;

    /* Look the names of the smaller side up in the larger one. The base's
       are looked up in this layer by galloping on from the last one found,
       so that a few over many cost a few searches, not a pass over this
       layer, and as many cost about one pass. */
    if (own.size() > base.size()) {
        auto i = own.begin();
        for (const auto & attr : base) {
            size_t n = own.end() - i, lo = 0, hi = 1;
            while (hi <= n && i[hi - 1] < attr) {
                lo = hi;
                hi *= 2;
            }
            i = std::lower_bound(i + lo, i + std::min(hi, n), attr);
            if (i == own.end())
                break;
            if (i->name == attr.name)
                ++duplicates;
        }
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
    std::sort(own(), own() + numAttrs);
}

Value & Value::mkAttrs(BindingsBuilder & bindings)
{
    mkAttrs(bindings.finish());
    return *this;
}

} // namespace nix
