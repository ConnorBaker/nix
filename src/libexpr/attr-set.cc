#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-inline.hh"

namespace nix {

const Bindings & Bindings::emptySingleton()
{
    static const Bindings empty{AttrMap{}};
    return empty;
}

NixList * EvalMemory::allocImmerList(NixList list)
{
    stats.nrListElems += list.size();
    // Allocate on GC heap and use placement new
    auto * ptr = static_cast<NixList *>(allocBytes(sizeof(NixList)));
    return new (ptr) NixList(std::move(list));
}

Bindings * EvalMemory::allocBindings(AttrMap map, PosIdx pos)
{
    stats.nrAttrsets++;
    stats.nrAttrsInAttrsets += map.size();
    // Allocate on GC heap and use placement new
    auto * ptr = static_cast<Bindings *>(allocBytes(sizeof(Bindings)));
    return new (ptr) Bindings(std::move(map), pos);
}

Bindings * BindingsBuilder::finish()
{
    return mem.get().allocBindings(transient.persistent(), pos);
}

Value & BindingsBuilder::alloc(Symbol name, PosIdx attrPos)
{
    auto value = mem.get().allocValue();
    transient.set(name, AttrValue(value, attrPos));
    return *value;
}

Value & BindingsBuilder::alloc(std::string_view name, PosIdx attrPos)
{
    return alloc(symbols.get().create(name), attrPos);
}

Value & Value::mkAttrs(BindingsBuilder & bindings)
{
    mkAttrs(bindings.finish());
    return *this;
}

} // namespace nix
