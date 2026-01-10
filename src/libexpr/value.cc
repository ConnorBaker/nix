#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"

namespace nix {

size_t Value::attrsSize() const noexcept
{
    assert(isa<tAttrs>() && "attrsSize() called on non-attrs value");
    return getStorage<Attrs>().bindings->size();
}

Value::AttrRef Value::attrsGet(Symbol name) const noexcept
{
    assert(isa<tAttrs>() && "attrsGet() called on non-attrs value");
    auto * attr = getStorage<Attrs>().bindings->get(name);
    if (attr) {
        assert(attr->value != nullptr && "attrsGet() found null value in Bindings");
        assert(attr->value->isValid() && "attrsGet() found invalid value in Bindings");
        return AttrRef{attr->value, attr->pos};
    }
    return AttrRef{};
}

Value Value::vEmptyList = []() {
    Value res;
    res.setStorage(List{.size = 0, .elems = nullptr});
    return res;
}();

Value Value::vNull = []() {
    Value res;
    res.mkNull();
    return res;
}();

Value Value::vTrue = []() {
    Value res;
    res.mkBool(true);
    return res;
}();

Value Value::vFalse = []() {
    Value res;
    res.mkBool(false);
    return res;
}();

} // namespace nix
