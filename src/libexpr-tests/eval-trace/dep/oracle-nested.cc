#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepTrackingTest : public TraceCacheFixture
{
public:
    DepTrackingTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "dep-tracking-test");
    }
};

// ── Group 7: Nested Attrs with Independent Deps (Adapton: per-node traces) ──

TEST_F(DepTrackingTest, NestedAttrs_ChildFile_DepInvalidation)
{
    TempTestFile file("alpha");
    auto expr = "{ a = builtins.readFile " + file.path.string() + "; b = 42; }";

    // Fresh evaluation — force root and all children, recording traces at each level
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(2));

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("alpha"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(42));
    }

    // Verification — all traces should be verified and served from cache.
    // Children with zero own deps have a trace-context dep linking them
    // to the parent's result hash, enabling cache hits on warm path.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("alpha"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(42));

        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file — only child 'a' has a dep on it in its trace.
    // Different size ensures stat oracle metadata changes.
    file.modify("beta-modified");
    invalidateFileCache(file.path);

    // Verification after modification:
    // - Root has no direct file deps -> trace verified (BSàlC: clean)
    // - Child 'a' has Content dep on modified file -> trace invalid -> fresh re-evaluation
    // - Child 'b' has no file deps -> trace verified (BSàlC: clean)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("beta-modified"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(42));

        // rootLoader called once (for child 'a' fresh re-evaluation via navigateToReal)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 8: Dep Isolation (Adapton: independent DDG subgraphs) ──────

TEST_F(DepTrackingTest, DepIsolation_Sibling_Unaffected)
{
    TempTestFile fileA("aaa");
    TempTestFile fileB("bbb");
    auto expr = "{ a = builtins.readFile " + fileA.path.string()
              + "; b = builtins.readFile " + fileB.path.string() + "; }";

    // Fresh evaluation — force root and both children, recording independent traces
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(2));

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("aaa"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("bbb"));
    }

    // Verification — all traces verified
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("aaa"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("bbb"));

        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify only fileA -> child 'a' trace invalidates, child 'b' unaffected (Adapton: minimal re-evaluation).
    // Different size ensures stat oracle metadata changes.
    fileA.modify("AAA-modified");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("AAA-modified"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("bbb"));

        // rootLoader called once (for child 'a' fresh re-evaluation via navigateToReal)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 9: Deep Nesting with Leaf-Only Dep (Adapton: deep DDG chain) ──

TEST_F(DepTrackingTest, DeepNesting_LeafFile_OnlyDep)
{
    TempTestFile file("leaf-val");
    auto expr = "{ l1 = { l2 = { leaf = builtins.readFile "
              + file.path.string() + "; }; }; }";

    // Fresh evaluation — force all 4 levels (root -> l1 -> l2 -> leaf), recording traces at each
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(1));

        auto * l1 = root.attrs()->get(createSymbol("l1"));
        ASSERT_NE(l1, nullptr);
        state.forceValue(*l1->value, noPos);

        auto * l2 = l1->value->attrs()->get(createSymbol("l2"));
        ASSERT_NE(l2, nullptr);
        state.forceValue(*l2->value, noPos);

        auto * leaf = l2->value->attrs()->get(createSymbol("leaf"));
        ASSERT_NE(leaf, nullptr);
        state.forceValue(*leaf->value, noPos);
        EXPECT_THAT(*leaf->value, IsStringEq("leaf-val"));
    }

    // Verification — all level traces verified.
    // Intermediates have trace-context deps; leaf has Content dep.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * l1 = root.attrs()->get(createSymbol("l1"));
        ASSERT_NE(l1, nullptr);
        state.forceValue(*l1->value, noPos);

        auto * l2 = l1->value->attrs()->get(createSymbol("l2"));
        ASSERT_NE(l2, nullptr);
        state.forceValue(*l2->value, noPos);

        auto * leaf = l2->value->attrs()->get(createSymbol("leaf"));
        ASSERT_NE(leaf, nullptr);
        state.forceValue(*leaf->value, noPos);
        EXPECT_THAT(*leaf->value, IsStringEq("leaf-val"));

        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file -> only the leaf trace's dep changes (Adapton: minimal dirty propagation).
    // Different size ensures stat oracle metadata changes.
    file.modify("new-leaf-value");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * l1 = root.attrs()->get(createSymbol("l1"));
        ASSERT_NE(l1, nullptr);
        state.forceValue(*l1->value, noPos);

        auto * l2 = l1->value->attrs()->get(createSymbol("l2"));
        ASSERT_NE(l2, nullptr);
        state.forceValue(*l2->value, noPos);

        auto * leaf = l2->value->attrs()->get(createSymbol("leaf"));
        ASSERT_NE(leaf, nullptr);
        state.forceValue(*leaf->value, noPos);
        EXPECT_THAT(*leaf->value, IsStringEq("new-leaf-value"));

        // rootLoader called once (for leaf fresh re-evaluation via navigateToReal chain)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 10: Mixed Oracle Dep Types in Single Expression ────────────

TEST_F(DepTrackingTest, MixedDeps_FileAndEnvVar_WarmHit)
{
    TempTestFile file("hello");
    ScopedEnvVar env("NIX_TEST_MIXED", "world");
    auto expr = "(builtins.readFile " + file.path.string()
              + R"() + (builtins.getEnv "NIX_TEST_MIXED"))";

    // Fresh evaluation — records both Content and EnvVar oracle deps in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verification — both oracle deps valid -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }
}

TEST_F(DepTrackingTest, MixedDeps_FileChange_Invalidates)
{
    TempTestFile file("hello");
    ScopedEnvVar env("NIX_TEST_MIXED_F", "world");
    auto expr = "(builtins.readFile " + file.path.string()
              + R"() + (builtins.getEnv "NIX_TEST_MIXED_F"))";

    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file -> Content oracle dep invalidates (EnvVar oracle dep still valid).
    // Different size ensures stat oracle metadata changes.
    file.modify("HELLO-MODIFIED");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("HELLO-MODIFIEDworld"));
    }
}

TEST_F(DepTrackingTest, MixedDeps_EnvVarChange_Invalidates)
{
    TempTestFile file("hello");
    ScopedEnvVar env("NIX_TEST_MIXED_E", "world");
    auto expr = "(builtins.readFile " + file.path.string()
              + R"() + (builtins.getEnv "NIX_TEST_MIXED_E"))";

    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify env var -> EnvVar oracle dep invalidates (Content oracle dep still valid)
    setenv("NIX_TEST_MIXED_E", "WORLD", 1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("helloWORLD"));
    }
}

// ── Group 11: Parent Trace Cascade via Import (Shake: transitive dirty) ──

// ── OR-2 fix: bare-import soundness ──────────────────────────────────
//
// The test fixture below drives the Nix loader through the C++ Bindings
// API (attrs()->get) to simulate a consumer that never emits a Nix-level
// ExprSelect — exactly the shape where access-time maybeRecordNixBindingDep
// would not fire.  Before the OR-2 fix the trace had zero deps on nixFile
// and any content mutation silently served a stale result.  The fix emits
// a FileBytes backstop at ExprParseFile::eval, so every content mutation
// now correctly invalidates the trace.

TEST_F(DepTrackingTest, BareImport_ContentChange_Invalidates)
{
    // OR-2 positive soundness pin.  Edit to the imported file's content
    // (even a comment-only edit, the easiest way to keep the observable
    // unchanged while perturbing bytes) must trigger re-evaluation.
    TempTestFile dataFile("hello");
    std::string nixContent = "{ x = builtins.readFile " + dataFile.path.string() + "; }";
    TempTestFile nixFile(nixContent);
    auto expr = "import " + nixFile.path.string();

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(1));
        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
        EXPECT_EQ(loaderCalls, 0) << "precision pre-condition: unchanged file must hit cache";
    }

    nixFile.modify(nixContent + " # this comment changes the file hash");
    invalidateFileCache(nixFile.path);
    state.resetFileCache();

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
        EXPECT_EQ(loaderCalls, 1)
            << "OR-2 soundness: content edit to bare-imported .nix must re-evaluate "
               "(no per-binding SP dep exists because nothing fires ExprSelect)";
    }
}

TEST_F(DepTrackingTest, BareImport_ValueChange_Invalidates)
{
    // OR-2 negative-result pin: an edit that *does* change the observed
    // value must propagate through to the user.  Before the fix the trace
    // had zero deps on nixFile and served the stale "hello" value; the
    // fix makes the FileBytes dep fail and the loader re-evaluates.
    TempTestFile dataFile("hello");
    std::string nixContent = "{ x = builtins.readFile " + dataFile.path.string() + "; }";
    TempTestFile nixFile(nixContent);
    auto expr = "import " + nixFile.path.string();

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
    }

    TempTestFile dataFile2("world");
    nixFile.modify("{ x = builtins.readFile " + dataFile2.path.string() + "; }");
    invalidateFileCache(nixFile.path);
    state.resetFileCache();

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("world"))
            << "OR-2 soundness: post-mutation value must be observed";
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepTrackingTest, BareImport_UnrelatedFileUnchanged_CacheHit)
{
    // OR-2 precision companion: the FileBytes backstop is scoped to the
    // imported file.  Edits to an unrelated file must NOT invalidate the
    // cached import.  Together with BareImport_ContentChange_Invalidates
    // this pins "backstop + scope = sound and precise".
    TempTestFile dataFile("hello");
    std::string nixContent = "{ x = builtins.readFile " + dataFile.path.string() + "; }";
    TempTestFile nixFile(nixContent);
    TempTestFile unrelated("noise");
    auto expr = "import " + nixFile.path.string();

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
    }

    unrelated.modify("noise-v2 # unrelated edit");
    invalidateFileCache(unrelated.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
        EXPECT_EQ(loaderCalls, 0)
            << "OR-2 precision: FileBytes is per-file; unrelated edit must not invalidate";
    }
}

TEST_F(DepTrackingTest, NixBindingSCOverride_Comment_CacheHit)
{
    // Expression accesses a binding via ExprSelect inside the Nix
    // expression.  maybeRecordNixBindingDep records both Content dep
    // and SC dep.  Comment-only change → Content fails, SC passes →
    // override → cache hit.
    TempTestFile dataFile("hello");
    std::string nixContent = "{ x = builtins.readFile " + dataFile.path.string() + "; }";
    TempTestFile nixFile(nixContent);
    auto expr = "(import " + nixFile.path.string() + ").x";

    {
        auto cache = makeCache(expr);
        EXPECT_THAT(forceRoot(*cache), IsStringEq("hello"));
    }

    nixFile.modify(nixContent + " # comment");
    invalidateFileCache(nixFile.path);
    state.resetFileCache();

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        EXPECT_THAT(forceRoot(*cache), IsStringEq("hello"));
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(DepTrackingTest, NixBindingSCOverride_Binding_CacheMiss)
{
    // Negative: accessed binding expression changes → SC dep fails →
    // re-evaluation.
    TempTestFile dataFile("hello");
    std::string nixContent = "{ x = builtins.readFile " + dataFile.path.string() + "; }";
    TempTestFile nixFile(nixContent);
    auto expr = "(import " + nixFile.path.string() + ").x";

    {
        auto cache = makeCache(expr);
        EXPECT_THAT(forceRoot(*cache), IsStringEq("hello"));
    }

    TempTestFile dataFile2("world");
    nixFile.modify("{ x = builtins.readFile " + dataFile2.path.string() + "; }");
    invalidateFileCache(nixFile.path);
    state.resetFileCache();

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        EXPECT_THAT(forceRoot(*cache), IsStringEq("world"));
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 12: Directory oracle deps via builtins.readDir ─────────────

TEST_F(DepTrackingTest, DirectoryDep_ReadDir_WarmHit)
{
    // Create a temp directory with two files
    auto dir = std::filesystem::canonical(std::filesystem::temp_directory_path())
             / ("nix-test-readdir-" + std::to_string(getpid()));
    createDirs(dir);
    std::ofstream(dir / "a.txt") << "a";
    std::ofstream(dir / "b.txt") << "b";

    auto expr = "builtins.attrNames (builtins.readDir " + dir.string() + ")";

    // Fresh evaluation — records Directory oracle dep on dir in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nList);
    }

    // Verification — dir oracle unchanged -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.type(), nList);
    }

    deletePath(dir);
}

TEST_F(DepTrackingTest, DirectoryDep_ReadDir_Invalidation)
{
    // Create a temp directory with one file
    auto dir = std::filesystem::canonical(std::filesystem::temp_directory_path())
             / ("nix-test-readdir-inv-" + std::to_string(getpid()));
    createDirs(dir);
    std::ofstream(dir / "a.txt") << "a";

    auto expr = "builtins.length (builtins.attrNames (builtins.readDir " + dir.string() + "))";

    // Fresh evaluation — records Directory oracle dep in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Add another file -> Directory oracle hash changes (Shake: dirty input)
    std::ofstream(dir / "b.txt") << "b";
    invalidateFileCache(dir);

    // Verification should fail (Shake: dirty input) -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(2));
    }

    deletePath(dir);
}

} // namespace nix::eval_trace
