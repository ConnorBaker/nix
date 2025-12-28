#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-inline.hh"

#include <algorithm>

namespace nix {

ImmerBindings ImmerBindings::emptyImmerBindings;

NixList * EvalMemory::allocImmerList(NixList list)
{
    stats.nrListElems += list.size();
    // Allocate on GC heap and use placement new
    auto * ptr = static_cast<NixList *>(allocBytes(sizeof(NixList)));
    return new (ptr) NixList(std::move(list));
}

ImmerBindings * EvalMemory::allocImmerBindings(AttrMap map, PosIdx pos)
{
    stats.nrAttrsets++;
    stats.nrAttrsInAttrsets += map.size();
    // Allocate on GC heap and use placement new
    auto * ptr = static_cast<ImmerBindings *>(allocBytes(sizeof(ImmerBindings)));
    return new (ptr) ImmerBindings(std::move(map), pos);
}

ImmerBindings * ImmerBindingsBuilder::finish()
{
    return mem.get().allocImmerBindings(transient.persistent(), pos);
}

Value & ImmerBindingsBuilder::alloc(Symbol name, PosIdx attrPos)
{
    auto value = mem.get().allocValue();
    transient.set(name, AttrValue(value, attrPos));
    return *value;
}

Value & ImmerBindingsBuilder::alloc(std::string_view name, PosIdx attrPos)
{
    return alloc(symbols.get().create(name), attrPos);
}

Value & Value::mkAttrs(ImmerBindingsBuilder & bindings)
{
    mkAttrs(bindings.finish());
    return *this;
}

} // namespace nix
