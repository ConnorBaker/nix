#include "eval-trace/helpers.hh"

#include <algorithm>
#include <format>
#include <gtest/gtest.h>

#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-trace/cache/derived-container-builder.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/error.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionListLenTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Dep verification: length records SC #len
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, ListLen_Length_RecordsSCLen)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("builtins.length ({}).items", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("len")))
        << "length must record SC #len\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "length must NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, ListLen_Length_RecordsSCLenForSmallTracedList)
{
    TempJsonFile file(R"({"items": [1]})");
    auto expr = std::format("builtins.length ({}).items", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("len")))
        << "small traced lists must still carry stable list provenance for #len\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, ListLen_Head_NoSCLenDep)
{
    TempJsonFile file(R"({"items": ["a", "b"]})");
    auto expr = std::format("builtins.head ({}).items", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("len")))
        << "head must NOT record SC #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, ListLen_ElemAt_NoSCLenDep)
{
    TempJsonFile file(R"({"items": ["a", "b"]})");
    auto expr = std::format("builtins.elemAt ({}).items 0", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("len")))
        << "elemAt must NOT record SC #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, ListLen_AttrNamesReadDir_NoDirectoryLenDep)
{
    TempDir dir;
    dir.addFile("a", "1");
    dir.addFile("b", "2");
    auto expr = "builtins.length (builtins.attrNames (builtins.readDir "
        + dir.path().string() + "))";

    auto deps = evalAndCollectDeps(expr);
    auto isDirLen = [](const ResolvedDep & dep) {
        return dep.type == CanonicalQueryKind::StructuredProjection
            && dep.structured
            && dep.structured->format == StructuredFormat::Directory
            && dep.structured->suffix == ShapeSuffix::Len;
    };
    EXPECT_TRUE(std::none_of(deps.begin(), deps.end(), isDirLen))
        << "attrNames(readDir) must record directory #keys, not directory #len\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, ListLen_FilteredAttrNamesReadDirEmpty_NoDirectoryLenDep)
{
    TempDir dir;
    dir.addFile("a", "1");
    dir.addFile("b", "2");
    auto expr = "builtins.length (builtins.filter (_: false) "
        "(builtins.attrNames (builtins.readDir " + dir.path().string() + ")))";

    auto deps = evalAndCollectDeps(expr);
    auto isDirLen = [](const ResolvedDep & dep) {
        return dep.type == CanonicalQueryKind::StructuredProjection
            && dep.structured
            && dep.structured->format == StructuredFormat::Directory
            && dep.structured->suffix == ShapeSuffix::Len;
    };
    EXPECT_TRUE(std::none_of(deps.begin(), deps.end(), isDirLen))
        << "empty list outputs must not inherit readDir attrset provenance\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, RecordStructuredDep_DirectoryLenIsInvariantViolation)
{
    auto & pools = state.tracingPools();
    CompactDepComponents c{
        pools.intern<DepSourceId>(DepSource::makeAbsolute()),
        pools.intern<FilePathId>("/tmp/directory-len-invariant"),
        StructuredFormat::Directory,
        pools.dataPathPool.root(),
        ShapeSuffix::Len,
        StringId(),
        StringId(),
    };

    EXPECT_THROW(
        recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Zero))),
        Error)
        << "directory shape must not be represented as #len";
}

TEST_F(DepPrecisionListLenTest, ContainerProvenance_AttrsetDoesNotLeakToEmptyListAtSameValueAddress)
{
    auto & pools = state.tracingPools();
    DepCaptureScope depCapture(pools, testRegistry);
    TraceActivationScope traceActivation(state);
    auto access = TraceAccess::current();
    ASSERT_TRUE(access.has_value());

    Value value;
    {
        auto attrs = state.buildBindings(1);
        attrs.alloc("a").mkInt(1);
        value.mkAttrs(attrs.finish());
    }
    auto sourceId = pools.intern<DepSourceId>(DepSource::makeAbsolute());
    auto filePathId = pools.intern<FilePathId>("/tmp/provenance-leak-test");
    auto * prov = access->allocateProvenance(
        sourceId, filePathId, pools.dataPathPool.root(), StructuredFormat::Directory);
    access->registerTracedContainer(&value, prov);
    ASSERT_EQ(access->lookupTracedContainer(&value), prov);

    value.mkList(state.buildList(0));
    EXPECT_EQ(access->lookupTracedContainer(&value), nullptr)
        << "empty-list provenance lookup must be type-separated from attrset provenance";
}

TEST_F(DepPrecisionListLenTest, ContainerProvenance_AttrsetKeyUsesBindingsIdentity)
{
    auto & pools = state.tracingPools();
    DepCaptureScope depCapture(pools, testRegistry);
    TraceActivationScope traceActivation(state);
    auto access = TraceAccess::current();
    ASSERT_TRUE(access.has_value());

    Value value;
    {
        auto attrs = state.buildBindings(1);
        attrs.alloc("a").mkInt(1);
        value.mkAttrs(attrs.finish());
    }
    auto sourceId = pools.intern<DepSourceId>(DepSource::makeAbsolute());
    auto filePathId = pools.intern<FilePathId>("/tmp/provenance-attrset-source-a");
    auto * prov = access->allocateProvenance(
        sourceId, filePathId, pools.dataPathPool.root(), StructuredFormat::Directory);
    access->registerTracedContainer(&value, prov);
    ASSERT_EQ(access->lookupTracedContainer(&value), prov);

    {
        auto attrs = state.buildBindings(1);
        attrs.alloc("b").mkInt(2);
        value.mkAttrs(attrs.finish());
    }
    EXPECT_EQ(access->lookupTracedContainer(&value), nullptr)
        << "attrset provenance lookup must follow Bindings identity, not Value address";
}

TEST_F(DepPrecisionListLenTest, ContainerProvenance_EmptyAttrsetsUseDistinctBindings)
{
    auto & pools = state.tracingPools();
    DepCaptureScope depCapture(pools, testRegistry);
    TraceActivationScope traceActivation(state);
    auto access = TraceAccess::current();
    ASSERT_TRUE(access.has_value());

    Value first;
    first.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    auto sourceId = pools.intern<DepSourceId>(DepSource::makeAbsolute());
    auto filePathId = pools.intern<FilePathId>("/tmp/provenance-empty-attrset-source-a");
    auto * prov = access->allocateProvenance(
        sourceId, filePathId, pools.dataPathPool.root(), StructuredFormat::Json);
    access->registerTracedContainer(&first, prov);
    ASSERT_EQ(access->lookupTracedContainer(&first), prov);

    Value copied = first;
    EXPECT_EQ(access->lookupTracedContainer(&copied), prov)
        << "bitwise-copied attrsets should retain Bindings-keyed provenance";

    Value sharedEmpty;
    sharedEmpty.mkAttrs(state.buildBindings(0).finish());
    EXPECT_THROW(
        access->registerTracedContainer(&sharedEmpty, prov),
        Error)
        << "registering shared emptyBindings would silently lose container provenance";
    EXPECT_EQ(access->lookupTracedContainer(&sharedEmpty), nullptr)
        << "the shared emptyBindings singleton must never carry container provenance";

    Value second;
    second.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    EXPECT_EQ(access->lookupTracedContainer(&second), nullptr)
        << "empty attrset provenance must not use the shared emptyBindings singleton";
}

TEST_F(DepPrecisionListLenTest, ContainerProvenance_DerivedEmptyAttrsetUsesFreshBindings)
{
    auto & pools = state.tracingPools();
    DepCaptureScope depCapture(pools, testRegistry);
    TraceActivationScope traceActivation(state);
    auto access = TraceAccess::current();
    ASSERT_TRUE(access.has_value());

    Value source;
    source.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    auto sourceId = pools.intern<DepSourceId>(DepSource::makeAbsolute());
    auto filePathId = pools.intern<FilePathId>("/tmp/provenance-derived-empty-attrset-source");
    auto * prov = access->allocateProvenance(
        sourceId, filePathId, pools.dataPathPool.root(), StructuredFormat::Json);
    access->registerTracedContainer(&source, prov);
    ASSERT_EQ(access->lookupTracedContainer(&source), prov);

    DerivedAttrsBuilder derivedBuilder;
    derivedBuilder.addShapePreservingSource(source);
    ASSERT_TRUE(derivedBuilder.willRegisterContainer());

    auto attrs = state.buildBindings(
        0,
        derivedBuilder.willRegisterContainer()
            ? EmptyBindingsAllocation::AllocateFresh
            : EmptyBindingsAllocation::ReuseSharedEmpty);
    Value output;
    derivedBuilder.finishAttrs(output, attrs.finish());

    EXPECT_NE(output.attrs(), &Bindings::emptyBindings)
        << "derived empty attrsets with provenance need a fresh Bindings identity";
    auto * outputProv = access->lookupTracedContainer(&output);
    ASSERT_NE(outputProv, nullptr);
    EXPECT_EQ(outputProv->sourceId, sourceId);
    EXPECT_EQ(outputProv->filePathId, filePathId);
    EXPECT_EQ(outputProv->dataPathId, pools.dataPathPool.root());
    EXPECT_EQ(outputProv->format, StructuredFormat::Json);

    Value defaultEmpty;
    defaultEmpty.mkAttrs(state.buildBindings(0).finish());
    EXPECT_EQ(access->lookupTracedContainer(&defaultEmpty), nullptr)
        << "derived empty provenance must not leak into later default empty attrsets";
}

TEST_F(DepPrecisionListLenTest, ContainerProvenance_ListKeyUsesStableListStorage)
{
    auto & pools = state.tracingPools();
    DepCaptureScope depCapture(pools, testRegistry);
    TraceActivationScope traceActivation(state);
    auto access = TraceAccess::current();
    ASSERT_TRUE(access.has_value());

    Value value;
    auto stableEmpty = state.buildList(0, true);
    value.mkList(stableEmpty);
    auto sourceId = pools.intern<DepSourceId>(DepSource::makeAbsolute());
    auto filePathId = pools.intern<FilePathId>("/tmp/provenance-empty-list-source-a");
    auto * prov = access->allocateProvenance(
        sourceId, filePathId, pools.dataPathPool.root(), StructuredFormat::Json);
    access->registerTracedContainer(&value, prov);
    ASSERT_EQ(access->lookupTracedContainer(&value), prov);

    Value copied = value;
    EXPECT_EQ(access->lookupTracedContainer(&copied), prov)
        << "bitwise-copied heap-empty lists should retain storage-keyed provenance";

    auto defaultEmpty = state.buildList(0);
    value.mkList(defaultEmpty);
    EXPECT_EQ(access->lookupTracedContainer(&value), nullptr)
        << "default empty lists have no stable storage key and must not reuse stale provenance";

    EXPECT_THROW(
        access->registerTracedContainer(&value, prov),
        Error)
        << "registering a list without heap-backed storage would silently lose provenance";

    Value elem;
    elem.mkInt(1);
    Value inlineSmall;
    auto small = state.buildList(1);
    small[0] = &elem;
    inlineSmall.mkList(small);
    EXPECT_EQ(access->lookupTracedContainer(&inlineSmall), nullptr);
    EXPECT_THROW(
        access->registerTracedContainer(&inlineSmall, prov),
        Error)
        << "registering an inline small list would silently lose provenance";
}

TEST_F(DepPrecisionListLenTest, ContainerProvenance_ListKeyDoesNotConflateSameElements)
{
    auto & pools = state.tracingPools();
    DepCaptureScope depCapture(pools, testRegistry);
    TraceActivationScope traceActivation(state);
    auto access = TraceAccess::current();
    ASSERT_TRUE(access.has_value());

    Value elem;
    elem.mkInt(1);

    Value first;
    auto firstList = state.buildList(1, true);
    firstList[0] = &elem;
    first.mkList(firstList);

    auto sourceId = pools.intern<DepSourceId>(DepSource::makeAbsolute());
    auto filePathId = pools.intern<FilePathId>("/tmp/provenance-list-source-a");
    auto * prov = access->allocateProvenance(
        sourceId, filePathId, pools.dataPathPool.root(), StructuredFormat::Json);
    access->registerTracedContainer(&first, prov);
    ASSERT_EQ(access->lookupTracedContainer(&first), prov);

    Value copied = first;
    EXPECT_EQ(access->lookupTracedContainer(&copied), prov)
        << "bitwise-copied heap-backed lists should retain storage-keyed provenance";

    Value sameElementsDifferentStorage;
    auto secondList = state.buildList(1, true);
    secondList[0] = &elem;
    sameElementsDifferentStorage.mkList(secondList);
    EXPECT_EQ(access->lookupTracedContainer(&sameElementsDifferentStorage), nullptr)
        << "list provenance must not follow element vectors across distinct list storage";
}

// ═══════════════════════════════════════════════════════════════════════
// Core bug reproduction (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, LengthPlusElemAt_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = std::format(
        "let j = {}; in (toString (builtins.length j.arr)) + \"-\" + (builtins.elemAt j.arr 0)",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    file.modify(R"({"arr": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len fails
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }
}

TEST_F(DepPrecisionListLenTest, LengthPlusElemAt_NoShapeChange_CacheHit)
{
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = std::format(
        "let j = {}; in (toString (builtins.length j.arr)) + \"-\" + (builtins.elemAt j.arr 0)",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    file.modify(R"({"arr": ["alpha", "CHANGED"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// List length shape deps (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, LengthOnly_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = std::format("let j = {}; in builtins.length j.arr", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["a", "b", "c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(DepPrecisionListLenTest, LengthOnly_ContentChange_CacheHit)
{
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = std::format("let j = {}; in builtins.length j.arr", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["CHANGED!", "b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(2));
    }
}

TEST_F(DepPrecisionListLenTest, RootArrayLength_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"(["x", "y", "z"])");
    auto expr = std::format("builtins.length ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"(["x", "y", "z", "w"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(DepPrecisionListLenTest, NestedListLength_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("let j = {}; in builtins.length j.items", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(DepPrecisionListLenTest, ListElementAdded_PointAccess_CacheHit)
{
    TempJsonFile file(R"({"items": ["alpha", "beta"]})");
    auto expr = std::format("builtins.elemAt ({}).items 0", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha"));
    }

    file.modify(R"({"items": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("alpha"));
    }
}

TEST_F(DepPrecisionListLenTest, ListSizeChange_NoLeafAccess_CacheMiss)
{
    TempJsonFile file(R"(["a", "b", "c"])");
    auto expr = std::format("{}", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"(["a", "b", "c", "d"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- filter (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Filter_Length_ElementRemoved_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 10, 2, 20]})");
    auto expr = std::format(
        "builtins.length (builtins.filter (x: x > 5) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items": [1, 10, 2, 20, 30]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed -> re-eval
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(DepPrecisionListLenTest, Filter_Length_AllKept_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format(
        "builtins.length (builtins.filter (x: true) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len NOT suppressed, detects change
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- tail (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Tail_Length_ElementAdded_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format(
        "builtins.length (builtins.tail ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed -> re-eval
        EXPECT_THAT(v, IsIntEq(3));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- sort (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Sort_Length_ElementAdded_CacheMiss)
{
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = std::format(
        "builtins.length (builtins.sort (a: b: a < b) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [3, 1, 2, 0]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len NOT suppressed, detects change
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(DepPrecisionListLenTest, Sort_Length_ValueChanged_CacheMiss)
{
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = std::format(
        "builtins.length (builtins.sort (a: b: a < b) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [9, 8, 7]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC deps from comparator fail -> re-eval
        EXPECT_THAT(v, IsIntEq(3));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- multi-source ++ (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Concat_EmptyPlusTracked_ValueChanged_CacheHit)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("builtins.length ([] ++ ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [9, 8, 7]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #len passes -- same count
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(DepPrecisionListLenTest, Concat_BothTracked_ElementAdded_CacheMiss)
{
    TempJsonFile fileA(R"({"items": [1, 2]})");
    TempJsonFile fileB(R"({"items": [3, 4]})");
    auto expr = std::format(
        "builtins.length (({}).items ++ ({}).items)", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(4));
    }

    fileB.modify(R"({"items": [3, 4, 5]})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed -> re-eval
        EXPECT_THAT(v, IsIntEq(5));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Scalar dep correctness (from builtins-access.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Map_SameLength_ValueChange_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.map (x: x) j.items))",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["x","y"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("x,y"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// TOML length (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, TOML_LengthChange_CacheMiss)
{
    TempTomlFile file("items = [\"a\", \"b\"]\n");
    auto expr = std::format("let t = {}; in builtins.length t.items", ft(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify("items = [\"a\", \"b\", \"c\"]\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len fails
        EXPECT_THAT(v, IsIntEq(3));
    }
}

} // namespace nix::eval_trace
