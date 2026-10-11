#include <gtest/gtest.h>

#include <sstream>

#include "nix/expr/fetch-tree.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/path.hh"
#include "nix/util/memo.hh"
#include "nix/util/signals.hh"

namespace nix {

class LazyFetcherAttrTest : public LibExprTest
{
protected:
    Value & emitLazy(fetchers::LazyAttr lazy)
    {
        fetchers::Input input;
        input.attrs = {{"type", std::string("git")}, {"revCount", std::move(lazy)}};
        Value attrs;
        emitTreeAttrs(state, noPos, dummyPath(), input, attrs, false, false);
        return *attrs.attrs()->get(state.symbols.create("revCount"))->value;
    }

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
            make_ref<fetchers::LazyAttr::element_type>(
                fetchers::LazyAttr::element_type{[&calls]() -> fetchers::ResolvedAttr {
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
            make_ref<fetchers::LazyAttr::element_type>(
                fetchers::LazyAttr::element_type{[&calls]() -> fetchers::ResolvedAttr {
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

TEST_F(LazyFetcherAttrTest, resolvedTypesAndSharedComputationsRetainTheirOwners)
{
    size_t cells = 0;
    for (bool memoized : {false, true}) {
        for (fetchers::ResolvedAttr expected :
             {fetchers::ResolvedAttr{std::string("lazy\0value", 10)},
              fetchers::ResolvedAttr{uint64_t(42)},
              fetchers::ResolvedAttr{Explicit<bool>{true}}}) {
            SCOPED_TRACE(memoized);
            SCOPED_TRACE(expected.index());
            int calls = 0;
            auto owner = std::make_shared<int>(42);
            std::weak_ptr<int> borrowed = owner;
            Value * first;
            Value * second;
            {
                fun<fetchers::ResolvedAttr()> compute = [&, owner, expected]() {
                    EXPECT_EQ(*owner, 42);
                    ++calls;
                    return expected;
                };
                auto lazy = make_ref<fetchers::LazyAttr::element_type>(
                    memoized ? memo(std::move(compute)) : std::move(compute));
                first = &emitLazy(lazy);
                second = &emitLazy(lazy);
            }
            owner.reset();
            EXPECT_FALSE(borrowed.expired());
            EXPECT_EQ(calls, 0);
            for (auto value : {first, second}) {
                for (int repeat = 0; repeat < 2; ++repeat) {
                    state.forceValue(*value, noPos);
                    std::visit(
                        overloaded{
                            [&](const std::string & s) { EXPECT_THAT(*value, IsStringEq(s)); },
                            [&](uint64_t n) { EXPECT_THAT(*value, IsIntEq(static_cast<int64_t>(n))); },
                            [&](const Explicit<bool> & b) {
                                ASSERT_EQ(value->type(), nBool);
                                EXPECT_EQ(value->boolean(), b.t);
                            },
                        },
                        expected);
                }
            }
            EXPECT_EQ(calls, memoized ? 1 : 2);
            ++cells;
        }
    }
    EXPECT_EQ(cells, 6);
    RecordProperty("finite_cells", cells);
}

TEST_F(LazyFetcherAttrTest, failuresRetryOnlyWhenRecoverable)
{
    size_t cells = 0;
    for (bool memoized : {false, true}) {
        for (int failure : {0, 1, 2}) {
            SCOPED_TRACE(memoized);
            SCOPED_TRACE(failure);
            int calls = 0;
            fun<fetchers::ResolvedAttr()> compute = [&]() -> fetchers::ResolvedAttr {
                if (++calls == 1) {
                    if (failure == 0)
                        throw Error("ordinary lazy failure");
                    if (failure == 1)
                        throw RecoverableEvalError(state, "recoverable lazy failure");
                    throw Interrupted("interrupted lazy failure");
                }
                return uint64_t(42);
            };
            auto & value = emitLazy(make_ref<fetchers::LazyAttr::element_type>(
                memoized ? memo(std::move(compute)) : std::move(compute)));
            if (failure == 0) {
                EXPECT_THROW(state.forceValue(value, noPos), Error);
                EXPECT_THROW(state.forceValue(value, noPos), Error);
                EXPECT_EQ(calls, 1);
            } else {
                if (failure == 1) {
                    EXPECT_THROW(state.forceValue(value, noPos), RecoverableEvalError);
                } else {
                    EXPECT_THROW(state.forceValue(value, noPos), Interrupted);
                }
                EXPECT_EQ(calls, 1);
                state.forceValue(value, noPos);
                EXPECT_THAT(value, IsIntEq(42));
                state.forceValue(value, noPos);
                EXPECT_EQ(calls, 2);
            }
            ++cells;
        }
    }
    {
        SCOPED_TRACE("interrupted application");
        int calls = 0;
        Value function, argument, application;
        PrimOp primOp{
            .name = "retryInterruptedApplication",
            .arity = 1,
            .impl = [&](EvalState &, CallSite, Value * const * args, Value & result) {
                ASSERT_EQ(args[0], &argument);
                ASSERT_THAT(*args[0], IsIntEq(41));
                if (++calls == 1) {
                    // The primop receives an intermediate result; mutate the application itself.
                    application.mkInt(-1);
                    throw Interrupted("interrupted application after partial result");
                }
                result.mkInt(args[0]->integer().value + 1);
            },
        };
        function.mkPrimOp(&primOp);
        argument.mkInt(41);
        application.mkApp(&function, &argument);
        EXPECT_THROW(state.forceValue(application, noPos), Interrupted);
        EXPECT_EQ(calls, 1);
        state.forceValue(application, noPos);
        EXPECT_THAT(application, IsIntEq(42));
        state.forceValue(application, noPos);
        EXPECT_EQ(calls, 2);
        ++cells;
    }
    EXPECT_EQ(cells, 7);
    RecordProperty("finite_cells", cells);
}

TEST_F(LazyFetcherAttrTest, selfDemandIsBlackholedBeforeReenteringTheHostComputation)
{
    size_t cells = 0;
    for (bool memoized : {false, true}) {
        SCOPED_TRACE(memoized);
        int calls = 0;
        Value * value = nullptr;
        fun<fetchers::ResolvedAttr()> compute = [&]() -> fetchers::ResolvedAttr {
            ++calls;
            state.forceValue(*value, noPos);
            return uint64_t(42);
        };
        value = &emitLazy(make_ref<fetchers::LazyAttr::element_type>(
            memoized ? memo(std::move(compute)) : std::move(compute)));
        EXPECT_THROW(state.forceValue(*value, noPos), InfiniteRecursionError);
        EXPECT_THROW(state.forceValue(*value, noPos), InfiniteRecursionError);
        EXPECT_EQ(calls, 1);
        ++cells;
    }
    EXPECT_EQ(cells, 2);
    RecordProperty("finite_cells", cells);
}

TEST_F(LazyFetcherAttrTest, deepForcingCanDisplayAndStopAtTheLazyAttribute)
{
    size_t cells = 0;
    for (bool debugAtEmission : {false, true}) {
        for (int association : {0, 1, 2}) {
            SCOPED_TRACE(debugAtEmission);
            SCOPED_TRACE(association);
            auto nested = make_ref<EvalState>(LookupPath{}, state.store, fetchSettings, evalSettings);
            auto debugger = +[](ref<EvalState> es, const ValMap & extraEnv) {
                EXPECT_FALSE(es->debugTraces.empty());
                const auto & trace = es->debugTraces.front();
                EXPECT_EQ(es->getStaticEnv(trace.expr), es->staticBaseEnv);
                EXPECT_EQ(&trace.env, &es->baseEnv);
                EXPECT_TRUE(extraEnv.empty());
                std::ostringstream display;
                trace.expr.show(es->symbols, display);
                EXPECT_EQ(display.str(), "<lazy fetcher attribute>");
                es->debugStop = false;
                return ReplExitStatus::Continue;
            };
            nested->debugRepl = debugAtEmission ? debugger : nullptr;
            int calls = 0;
            fetchers::Input input;
            input.attrs = {
                {"type", std::string("git")},
                {"revCount", make_ref<fetchers::LazyAttr::element_type>([&]() -> fetchers::ResolvedAttr {
                     EXPECT_FALSE(nested->debugStop);
                     ++calls;
                     return uint64_t(42);
                 })}};
            Value attrs;
            emitTreeAttrs(*nested, noPos, dummyPath(), input, attrs, false, false);
            auto expr = attrs.attrs()->get(nested->symbols.create("revCount"))->value->thunk().expr;
            nested->exprEnvs.erase(expr);
            if (association != 0)
                nested->exprEnvs.emplace(
                    expr, association == 1 ? nested->staticBaseEnv : state.staticBaseEnv);
            EXPECT_EQ(state.getStaticEnv(*expr), nested->staticBaseEnv);
            nested->debugRepl = debugger;
            nested->debugStop = true;
            nested->forceValueDeep(attrs);
            EXPECT_FALSE(nested->debugStop);
            EXPECT_TRUE(nested->debugTraces.empty());
            EXPECT_EQ(calls, 1);
            ++cells;
        }
    }
    EXPECT_EQ(cells, 6);
    RecordProperty("finite_cells", cells);
}

} // namespace nix
