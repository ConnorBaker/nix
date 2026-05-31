#include "nix/util/union-source-accessor.hh"

namespace nix {

ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> && accessors)
{
    return make_ref<UnionSourceAccessor>(std::move(accessors));
}

ref<SourceAccessor> makeLayer(std::vector<ref<SourceAccessor>> accessors)
{
    /* L6a: flatten nested `UnionSourceAccessor`s. Layer associativity
       means a child `Union` contributes its children directly; we
       splice rather than wrapping again. */
    std::vector<ref<SourceAccessor>> flat;
    flat.reserve(accessors.size());
    for (auto & a : accessors) {
        if (auto u = a.dynamic_pointer_cast<UnionSourceAccessor>()) {
            for (auto & inner : u->accessors)
                flat.push_back(inner);
        } else {
            flat.push_back(a);
        }
    }

    /* L6e: empty Layer collapses to the empty accessor. */
    if (flat.empty())
        return makeEmptySourceAccessor();

    /* L6d: singleton short-circuit — `Layer([a]) ≡ a`. */
    if (flat.size() == 1)
        return flat[0];

    return makeUnionSourceAccessor(std::move(flat));
}

} // namespace nix
