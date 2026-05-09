/// path-accessor-preservation.cc — Regression tests for the bug where
/// `path + "/${x}"` string concatenation produced a new path value whose
/// accessor was `rootFS` rather than the original path's accessor.
///
/// Symptom in the wild: `lib.fileset.toSource` threw "Filesystem roots are
/// not the same" because its internal `path + "/${name}"` walk (in
/// nixpkgs `lib/fileset/internal.nix::_normaliseTreeFilter`) changed the
/// accessor, so `splitRoot root` and `splitRoot fileset._internalBase`
/// disagreed — even though both resolved to the same on-disk location.
///
/// The fix is in `ExprConcatStrings::eval` (`src/libexpr/eval.cc`):
/// when the first element of the concatenation is a path, the resulting
/// path now carries that first path's accessor, not `rootFS`.
///
/// These tests evaluate real Nix expressions through `EvalState`, so they
/// exercise the actual parser + `ExprConcatStrings` path.

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/util/source-path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/store/store-api.hh"

#include <gtest/gtest.h>

namespace nix {

class PathAccessorPreservationTest : public LibExprTest
{
public:
    PathAccessorPreservationTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {
    }

protected:
    /// Build a SourcePath using an in-memory accessor disjoint from both
    /// `rootFS` and `storeFS` — this lets us assert that the concat path
    /// preserves the specific accessor the caller provided, not some
    /// global default.
    std::pair<ref<SourceAccessor>, SourcePath> makeMemAccessorWithFile(
        const std::string & content = "hello")
    {
        auto mem = make_ref<MemorySourceAccessor>();
        mem->addFile(CanonPath("/test-dir/leaf.txt"), std::string(content));
        return {mem, SourcePath(mem, CanonPath("/test-dir"))};
    }
};

// ── Positive: concat preserves the accessor ────────────────────────────

/// The core invariant: `path + "/leaf.txt"` yields a path value whose
/// accessor is the SAME instance as `path.accessor`.
///
/// Pre-fix, the resulting path's accessor was `state.rootFS` (via
/// `state.rootPath(...)`), which is a different `SourceAccessor` instance
/// even when the underlying on-disk location matches.
TEST_F(PathAccessorPreservationTest, PathPlusString_PreservesAccessor)
{
    auto [memAccessor, basePath] = makeMemAccessorWithFile();

    // Evaluate `path + "/leaf.txt"` where `path` is bound to our
    // memory-accessor-backed SourcePath.  The resulting path value must
    // carry the SAME accessor instance.
    Value vBase;
    vBase.mkPath(basePath, state.mem);

    auto concatExpr = state.parseExprFromString(
        "path: path + \"/leaf.txt\"", state.rootPath("."));
    Value vFn;
    concatExpr->eval(state, state.baseEnv, vFn);
    state.forceValue(vFn, noPos);

    Value vApplied;
    state.callFunction(vFn, vBase, vApplied, noPos);
    state.forceValue(vApplied, noPos);

    ASSERT_EQ(vApplied.type(), nPath);
    auto resultPath = vApplied.path();

    // The critical invariant: accessor IDENTITY (number) is preserved.
    EXPECT_EQ(resultPath.accessor->number, memAccessor->number)
        << "ExprConcatStrings on a path must preserve the first path's accessor. "
        << "Pre-fix, this returned rootFS instead.";

    // And the path itself should be correctly composed.
    EXPECT_EQ(resultPath.path.abs(), "/test-dir/leaf.txt");
}

// ── Positive: concat with interpolation also preserves ─────────────────

/// String interpolation `${name}` inside the concatenated string produces
/// the same ExprConcatStrings AST shape; the fix must cover it too.
TEST_F(PathAccessorPreservationTest, PathPlusInterp_PreservesAccessor)
{
    auto [memAccessor, basePath] = makeMemAccessorWithFile();

    Value vBase;
    vBase.mkPath(basePath, state.mem);

    auto concatExpr = state.parseExprFromString(
        "path: path + \"/${\"leaf\"}.txt\"", state.rootPath("."));
    Value vFn;
    concatExpr->eval(state, state.baseEnv, vFn);
    state.forceValue(vFn, noPos);

    Value vApplied;
    state.callFunction(vFn, vBase, vApplied, noPos);
    state.forceValue(vApplied, noPos);

    ASSERT_EQ(vApplied.type(), nPath);
    EXPECT_EQ(vApplied.path().accessor->number, memAccessor->number);
    EXPECT_EQ(vApplied.path().path.abs(), "/test-dir/leaf.txt");
}

// ── Equality: two concatenations on the same base compare equal ────────

/// Repeated `path + "/leaf"` calls on the same `path` value produce path
/// values that compare equal via `SourcePath::operator==`.  Before the
/// fix, both paths ended up with `rootFS`, so they WERE equal — this test
/// is here to pin the post-fix invariant that they share the ORIGINAL
/// accessor and remain equal.
TEST_F(PathAccessorPreservationTest, RepeatedConcat_PathsCompareEqual)
{
    auto [memAccessor, basePath] = makeMemAccessorWithFile();

    Value vBase;
    vBase.mkPath(basePath, state.mem);

    auto concatExpr = state.parseExprFromString(
        "path: path + \"/leaf.txt\"", state.rootPath("."));
    Value vFn;
    concatExpr->eval(state, state.baseEnv, vFn);
    state.forceValue(vFn, noPos);

    Value vA, vB;
    state.callFunction(vFn, vBase, vA, noPos);
    state.forceValue(vA, noPos);
    state.callFunction(vFn, vBase, vB, noPos);
    state.forceValue(vB, noPos);

    EXPECT_EQ(vA.path(), vB.path());
    EXPECT_EQ(vA.path().accessor->number, vB.path().accessor->number);
    EXPECT_EQ(vA.path().accessor->number, memAccessor->number);
}

// ── Negative: pre-fix misbehavior reproduced — dirOf chains diverge ───

/// This is the closest we can unit-test to the `lib.fileset.toSource`
/// failure mode: climb `dirOf` from two paths that SHOULD share an
/// accessor.  Before the fix, one climb started from the original
/// accessor and the other from `rootFS`, so the roots compared unequal.
///
/// With the fix, both climbs use the original accessor, so
/// `splitRoot`-style comparisons succeed.
TEST_F(PathAccessorPreservationTest, SplitRootEquivalent_AccessorsMatch)
{
    auto [memAccessor, basePath] = makeMemAccessorWithFile();

    Value vBase;
    vBase.mkPath(basePath, state.mem);

    // Evaluate two operations on the same Base path:
    //   rootA = splitRoot basePath
    //   rootB = splitRoot (basePath + "/leaf.txt")
    // rootA climbs via `dirOf` preserving the accessor (prim_dirOf
    // preserves); rootB's inner `basePath + "/leaf.txt"` goes through
    // ExprConcatStrings, which must preserve the accessor so that the
    // downstream climb also finishes with the original accessor.
    auto expr = state.parseExprFromString(R"(
        path:
        let
          climb = base:
            if base == builtins.dirOf base
              then base
              else climb (builtins.dirOf base);
          rootA = climb path;
          rootB = climb (path + "/leaf.txt");
        in
        rootA == rootB
    )", state.rootPath("."));
    Value vFn;
    expr->eval(state, state.baseEnv, vFn);
    state.forceValue(vFn, noPos);

    Value vResult;
    state.callFunction(vFn, vBase, vResult, noPos);
    state.forceValue(vResult, noPos);

    ASSERT_EQ(vResult.type(), nBool);
    EXPECT_TRUE(vResult.boolean())
        << "splitRoot on a path and on (path + suffix) must produce the "
        << "same root, i.e. the same accessor identity.  Pre-fix, the "
        << "`path + suffix` form leaked through state.rootPath and the "
        << "roots compared unequal — the exact symptom nixpkgs' "
        << "lib.fileset.toSource reports as 'Filesystem roots are not the same'.";
}

// ── Production accessor case: storeFS vs rootFS ────────────────────────

/// The bug manifests in production when a flake-source path (accessor:
/// state.storeFS) meets a runtime-constructed path that would otherwise
/// have been rebuilt via state.rootPath (accessor: state.rootFS). This
/// test reproduces that specific divergence directly, using the actual
/// accessor instances from `EvalState` — not a MemorySourceAccessor stand-in.
///
/// Specifically: `flakePath + "/leaf.txt"` where `flakePath.accessor ==
/// state.storeFS`. Post-fix, the resulting path shares accessor identity
/// with `flakePath`. Pre-fix, `ExprConcatStrings::eval`'s `state.rootPath`
/// call swapped in `state.rootFS`, and the result compared unequal to the
/// original accessor.
TEST_F(PathAccessorPreservationTest, StoreFSPath_ConcatPreservesStoreFSAccessor)
{
    // Construct a SourcePath rooted at `state.storeFS`, mirroring
    // flake.cc's `makeMountedStorePath`. We don't need the path to exist
    // on disk — the accessor-identity check runs before any I/O.
    auto storeFSPath = SourcePath{
        state.storeFS.cast<SourceAccessor>(),
        CanonPath("/nix/store/synthetic-source"),
    };

    Value vBase;
    vBase.mkPath(storeFSPath, state.mem);

    auto concatExpr = state.parseExprFromString(
        "path: path + \"/leaf.txt\"", state.rootPath("."));
    Value vFn;
    concatExpr->eval(state, state.baseEnv, vFn);
    state.forceValue(vFn, noPos);

    Value vApplied;
    state.callFunction(vFn, vBase, vApplied, noPos);
    state.forceValue(vApplied, noPos);

    ASSERT_EQ(vApplied.type(), nPath);
    auto resultPath = vApplied.path();

    // Post-fix: result's accessor is storeFS (same as the source).
    EXPECT_EQ(resultPath.accessor->number, state.storeFS->number)
        << "storeFS-rooted path + string must remain storeFS-rooted. "
        << "Pre-fix, ExprConcatStrings rebuilt via state.rootPath and the "
        << "result was rootFS-flavored instead — the exact production case "
        << "that broke lib.fileset.toSource on flake evaluations.";

    // And explicitly NOT rootFS.
    EXPECT_NE(resultPath.accessor->number, state.rootFS->number)
        << "storeFS and rootFS are distinct SourceAccessor instances; "
        << "the concat result must carry the source's accessor.";
}

/// Companion: starting from a `state.rootFS`-rooted path, the concat
/// result stays rootFS-rooted. This pins the "preserve whatever accessor
/// you started with" invariant for the rootFS case too.
TEST_F(PathAccessorPreservationTest, RootFSPath_ConcatPreservesRootFSAccessor)
{
    auto rootFSPath = state.rootPath(CanonPath("/tmp/synthetic-source"));

    Value vBase;
    vBase.mkPath(rootFSPath, state.mem);

    auto concatExpr = state.parseExprFromString(
        "path: path + \"/leaf.txt\"", state.rootPath("."));
    Value vFn;
    concatExpr->eval(state, state.baseEnv, vFn);
    state.forceValue(vFn, noPos);

    Value vApplied;
    state.callFunction(vFn, vBase, vApplied, noPos);
    state.forceValue(vApplied, noPos);

    ASSERT_EQ(vApplied.type(), nPath);
    EXPECT_EQ(vApplied.path().accessor->number, state.rootFS->number);
}

/// storeFS and rootFS are distinct accessors with different monotonic
/// `number` values. This guards the invariant the main test above depends on.
/// If this fails, the harness is misconfigured and the other storeFS/rootFS
/// tests become vacuous.
TEST_F(PathAccessorPreservationTest, StoreFSAndRootFS_AreDistinctAccessors)
{
    EXPECT_NE(state.storeFS->number, state.rootFS->number)
        << "The test expects storeFS and rootFS to be distinct SourceAccessor "
        << "instances. If this ever becomes true, the other storeFS/rootFS "
        << "tests compare equal vacuously and must be redesigned.";
}

// ── Coverage: integer concat is unaffected ─────────────────────────────

TEST_F(PathAccessorPreservationTest, IntAddition_Unaffected)
{
    auto intExpr = state.parseExprFromString(
        "1 + 2 + 3", state.rootPath("."));
    Value v;
    intExpr->eval(state, state.baseEnv, v);
    state.forceValue(v, noPos);
    ASSERT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 6);
}

// ── Coverage: string concat is unaffected ──────────────────────────────

TEST_F(PathAccessorPreservationTest, StringConcat_Unaffected)
{
    auto strExpr = state.parseExprFromString(
        "\"foo\" + \"bar\"", state.rootPath("."));
    Value v;
    strExpr->eval(state, state.baseEnv, v);
    state.forceValue(v, noPos);
    ASSERT_EQ(v.type(), nString);
    EXPECT_EQ(std::string(v.string_view()), "foobar");
}

} // namespace nix
