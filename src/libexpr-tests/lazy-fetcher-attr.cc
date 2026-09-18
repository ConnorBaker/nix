#include <gtest/gtest.h>

#include "nix/expr/fetch-tree.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/path.hh"

namespace nix {

class LazyFetcherAttrTest : public LibExprTest
{
protected:
    StorePath dummyPath()
    {
        return StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-test"};
    }
};

TEST_F(LazyFetcherAttrTest, nonLazyAttrProducesImmediateValue)
{
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign("revCount", uint64_t(5));

    Value v;
    emitTreeAttrs(state, noPos, dummyPath(), input, v, false, false);
    state.forceValue(v, noPos);

    auto * rcAttr = v.attrs()->get(state.symbols.create("revCount"));
    ASSERT_NE(rcAttr, nullptr);
    state.forceValue(*rcAttr->value, noPos);
    EXPECT_EQ(rcAttr->value->integer().value, 5);
}

TEST_F(LazyFetcherAttrTest, lazyAttrProducesThunk)
{
    int calls = 0;
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign(
        "revCount",
        fetchers::LazyAttr(
            make_ref<fetchers::LazyAttrComputation>(
                fetchers::LazyAttrComputation{.compute = [&calls]() -> fetchers::ResolvedAttr {
                    calls++;
                    return uint64_t(42);
                }})));

    Value v;
    emitTreeAttrs(state, noPos, dummyPath(), input, v, false, false);
    state.forceValue(v, noPos);

    auto * rcAttr = v.attrs()->get(state.symbols.create("revCount"));
    ASSERT_NE(rcAttr, nullptr);

    // Not yet forced, so the lazy function should not have been called
    EXPECT_EQ(calls, 0);

    // Force the thunk
    state.forceValue(*rcAttr->value, noPos);
    EXPECT_EQ(rcAttr->value->integer().value, 42);
    EXPECT_EQ(calls, 1);
}

TEST_F(LazyFetcherAttrTest, lazyFunctionOnlyCalledOnAccess)
{
    int calls = 0;
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    input.attrs.insert_or_assign("lastModified", uint64_t(1000));
    input.attrs.insert_or_assign(
        "revCount",
        fetchers::LazyAttr(
            make_ref<fetchers::LazyAttrComputation>(
                fetchers::LazyAttrComputation{.compute = [&calls]() -> fetchers::ResolvedAttr {
                    calls++;
                    return uint64_t(99);
                }})));

    Value v;
    emitTreeAttrs(state, noPos, dummyPath(), input, v, false, false);
    state.forceValue(v, noPos);

    // Access lastModified, so should not trigger lazy revCount
    auto * lmAttr = v.attrs()->get(state.symbols.create("lastModified"));
    ASSERT_NE(lmAttr, nullptr);
    state.forceValue(*lmAttr->value, noPos);
    EXPECT_EQ(lmAttr->value->integer().value, 1000);
    EXPECT_EQ(calls, 0);

    // Now access revCount
    auto * rcAttr = v.attrs()->get(state.symbols.create("revCount"));
    ASSERT_NE(rcAttr, nullptr);
    state.forceValue(*rcAttr->value, noPos);
    EXPECT_EQ(rcAttr->value->integer().value, 99);
    EXPECT_EQ(calls, 1);
}

/* 04 section 1.9, the fetchers' and the language's representation: a fetched tree's attributes
   carry `treeHash`, the name's hash, as a string, and `narHash` as a lazy
   attribute built through the shim, which building the set does not force.
   The input itself carries no `narHash`, lazy or not.  `emitTreeAttrs`
   (fetchTree.cc) emits `narHash` as a thunk when the input has `treeHash`
   and no `narHash`; the thunk is not forced here, the dummy path having no
   tree to walk. */
TEST_F(LazyFetcherAttrTest, treeHashIsAStringAndNarHashIsNotForced)
{
    fetchers::Input input;
    input.attrs.insert_or_assign("type", std::string("git"));
    /* The SHA-256 git id of the empty tree, as SRI. */
    auto treeHash = std::string("sha256-bvGbQSJcU2nxwQTUXY2F76mwV7U7FLS5uTnddN7MUyE=");
    input.attrs.insert_or_assign("treeHash", treeHash);

    Value v;
    emitTreeAttrs(state, noPos, dummyPath(), input, v, false, false);
    state.forceValue(v, noPos);

    auto * thAttr = v.attrs()->get(state.symbols.create("treeHash"));
    ASSERT_NE(thAttr, nullptr);
    EXPECT_EQ(std::string(state.forceStringNoCtx(*thAttr->value, noPos, "while reading treeHash")), treeHash);

    EXPECT_NE(v.attrs()->get(state.symbols.create("narHash")), nullptr);

    EXPECT_FALSE(input.getNarHash());
}

} // namespace nix
