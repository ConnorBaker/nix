#pragma once

#include "nix/expr/value.hh"
#include <rapidcheck.h>

namespace nix::eval_trace::test {

/// Compare two forced Value objects using RC_ASSERT.
///
/// Both values must be from the same EvalState GC context (i.e., from the
/// same fixture instance).  All makeCache calls within a single TEST_F share
/// one EvalState, so Value objects from any iteration are valid.
///
/// For Phase 1 scalar types (nInt, nBool, nFloat, nNull), data is stored
/// inline in Value — no GC pointer lifetime concerns at all.
/// For nString, string_view() returns a pointer into GC-managed StringData;
/// safe as long as both values are from the same EvalState and no GC
/// collection occurs between calls (guaranteed within a single test body).
///
/// For nAttrs: compares key names (sorted Symbol order) and scalar values at
/// each key.  Both values must come from the same EvalState so Symbol ids are
/// comparable by identity.
///
/// The depth parameter limits recursive comparison of nested containers.
/// At depth == 0, nested nAttrs/nList elements are skipped (key/size match
/// is still asserted).  Callers should use the default (depth = 3).
/// RC_PRE(newValue != currentValue) in mutation callers handles any edge case
/// where generateMutation() produces the same value as the original.
inline void assertValuesEqual(
    const nix::Value & a,
    const nix::Value & b,
    int depth = 3)
{
    RC_ASSERT(a.type() == b.type());
    switch (a.type()) {
    case nInt:
        RC_ASSERT(a.integer().value == b.integer().value);
        break;
    case nFloat:
        RC_ASSERT(a.fpoint() == b.fpoint());
        break;
    case nBool:
        RC_ASSERT(a.boolean() == b.boolean());
        break;
    case nNull:
        break;  // null == null always
    case nString:
        RC_ASSERT(std::string_view(a.string_view()) ==
                  std::string_view(b.string_view()));
        break;
    case nAttrs: {
        RC_ASSERT(a.attrs()->size() == b.attrs()->size());
        // Compare key names (sorted) and scalar values at each key.
        // The Bindings iterator yields Attr entries in sorted Symbol order.
        // Since both attrsets come from the same EvalState, Symbol ids are
        // interned in the same SymbolTable — identical strings get the same id.
        auto itA = a.attrs()->begin();
        auto itB = b.attrs()->begin();
        auto endA = a.attrs()->end();
        auto endB = b.attrs()->end();
        while (itA != endA && itB != endB) {
            // Assert key names match
            RC_ASSERT(itA->name == itB->name);
            // Compare values at matching keys
            if (itA->value && itB->value) {
                auto * va = itA->value;
                auto * vb = itB->value;
                if (va->type() == vb->type()) {
                    switch (va->type()) {
                    case nInt:
                        RC_ASSERT(va->integer().value == vb->integer().value);
                        break;
                    case nFloat:
                        RC_ASSERT(va->fpoint() == vb->fpoint());
                        break;
                    case nBool:
                        RC_ASSERT(va->boolean() == vb->boolean());
                        break;
                    case nString:
                        RC_ASSERT(std::string_view(va->string_view()) ==
                                  std::string_view(vb->string_view()));
                        break;
                    case nNull:
                        break;  // null == null always
                    case nAttrs:
                        if (depth > 0)
                            assertValuesEqual(*va, *vb, depth - 1);
                        break;
                    case nList:
                        if (depth > 0)
                            assertValuesEqual(*va, *vb, depth - 1);
                        break;
                    case nPath:
                    case nFunction:
                    case nExternal:
                    case nThunk:
                    case nFailed:
                        break;  // not compared in property tests
                    }
                }
            }
            ++itA;
            ++itB;
        }
        break;
    }
    case nList: {
        // Compare element-by-element
        RC_ASSERT(a.listSize() == b.listSize());
        auto listA = a.listView();
        auto listB = b.listView();
        for (size_t i = 0; i < a.listSize(); ++i) {
            auto * va = listA[i];
            auto * vb = listB[i];
            if (va && vb && va->type() == vb->type()) {
                switch (va->type()) {
                case nInt:
                    RC_ASSERT(va->integer().value == vb->integer().value);
                    break;
                case nFloat:
                    RC_ASSERT(va->fpoint() == vb->fpoint());
                    break;
                case nBool:
                    RC_ASSERT(va->boolean() == vb->boolean());
                    break;
                case nString:
                    RC_ASSERT(std::string_view(va->string_view()) ==
                              std::string_view(vb->string_view()));
                    break;
                case nNull:
                    break;  // null == null always
                case nAttrs:
                    if (depth > 0)
                        assertValuesEqual(*va, *vb, depth - 1);
                    break;
                case nList:
                    if (depth > 0)
                        assertValuesEqual(*va, *vb, depth - 1);
                    break;
                case nPath:
                case nFunction:
                case nExternal:
                case nThunk:
                case nFailed:
                    break;
                }
            }
        }
        break;
    }
    case nPath:
    case nFunction:
    case nExternal:
    case nThunk:
    case nFailed:
        RC_ASSERT(false);  // not expected in property tests
    }
}

}  // namespace nix::eval_trace::test
