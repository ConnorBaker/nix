#include "nix/expr/tests/libexpr.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/hash.hh"
#include "nix/util/tests/setting-scopes.hh"

#include <gtest/gtest.h>

namespace nix {

namespace {

/* A one-file tree whose `counter` reads "1" on odd reads and "0" on even
   ones: the oracle an impure Nix filter consults.  The evaluator caches
   stats, not contents, so every read reaches this. */
struct OracleAccessor : MemorySourceAccessor
{
    unsigned reads = 0;

    /* `addFile` takes `shared_from_this()`, so the file is added once the
       object is owned, not in the constructor. */
    static ref<OracleAccessor> make()
    {
        auto oracle = make_ref<OracleAccessor>();
        oracle->addFile(CanonPath("counter"), "0");
        return oracle;
    }

    using MemorySourceAccessor::readFile;

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        if (path != CanonPath("counter"))
            return MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
        std::string contents = ++reads % 2 == 1 ? "1" : "0";
        sizeCallback(contents.size());
        sink(std::string_view(contents));
    }
};

} // namespace

/* `builtins.path` over a writable in-memory store, with trees mounted into
   the evaluator's view of the store as `EvalState::mountInput` mounts an
   input. */
class AddPathTest : FreshCacheHome, public LibExprTest
{
protected:
    AddPathTest()
        : LibExprTest(openStore("dummy://?read-only=false"), [](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            return settings;
        })
    {
    }

    /* Mounts `tree` at `mountPoint` and returns the path as a Nix literal. */
    std::string mountAt(const CanonPath & mountPoint, ref<SourceAccessor> tree)
    {
        state.storeFS->mount(mountPoint, tree);
        return mountPoint.abs();
    }

    /* The tree to add, under `name` (none: an unnamed tree).  Fresh contents
       and a fresh name every run, so that one test's memo rows are not
       another's hits. */
    static ref<MemorySourceAccessor> sourceTree(std::optional<std::string> name)
    {
        auto tree = make_ref<MemorySourceAccessor>();
        tree->addFile(CanonPath("keep/x"), "x");
        tree->addFile(CanonPath("top"), "t");
        tree->addFile(CanonPath("salt"), Hash::random(HashAlgorithm::SHA256).to_string(HashFormat::Nix32, false));
        tree->fingerprint = std::move(name);
        return tree;
    }

    static std::string freshName()
    {
        return "add-path-test-" + Hash::random(HashAlgorithm::SHA256).to_string(HashFormat::Nix32, false);
    }

    /* `builtins.path` with an impure filter -- `top` is admitted when the
       oracle reads odd, i.e. on every other evaluation of the filter -- and
       `sha256` asserted; the store path string it returns. */
    std::string addPath(const std::string & treePath, const std::string & oraclePath, const Hash & sha256)
    {
        auto v = eval(
            fmt("builtins.path { name = \"tree\"; path = %s; sha256 = \"%s\"; "
                "filter = path: type: baseNameOf path != \"top\" || builtins.readFile %s/counter == \"1\"; }",
                treePath,
                sha256.to_string(HashFormat::SRI, true),
                oraclePath));
        return std::string(state.forceString(v, noPos, "while reading the path builtins.path returned"));
    }
};

/* An unnamed filtered tree with a `sha256` is walked by the filter once: the
   tree copied is the tree the first evaluation of the filter admits, and the
   `sha256` is checked against that same tree.  Before the fix the copy
   applied the filter while dumping and the check wrapped a second
   `filteredPaths` set, so the impure filter gave two trees and the check
   failed with a NAR hash mismatch; this test then throws, and the oracle
   counts two reads. */
TEST_F(AddPathTest, unnamedFilteredTreeWithSha256IsWalkedOnce)
{
    auto oracle = OracleAccessor::make();
    auto oraclePath = mountAt(CanonPath("/add-path-test-oracle"), oracle);
    auto tree = sourceTree(std::nullopt);
    auto treePath =
        mountAt(CanonPath(state.store->printStorePath(StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-tree"})), tree);

    /* The first evaluation of the filter admits everything, so the expected
       tree is the whole tree: named and NAR-hashed here without `addPath`. */
    auto treeHash = merkle::objectHash(objectHashOf(*tree, CanonPath::root).root);
    auto narHash = tree->hashPath(CanonPath::root);

    auto result = addPath(treePath, oraclePath, narHash);
    EXPECT_EQ(result, state.store->printStorePath(gitTreePath(*state.store, "tree", treeHash)));
    /* The filter asked about `top` once: one walk, one tree. */
    EXPECT_EQ(oracle->reads, 1u);
    /* The pair is recorded for named trees only. */
    EXPECT_FALSE(lookupTreeAddress(state.fetchSettings, narHash));
}

/* A named filtered tree with a `sha256`: walked once as well (this the old
   code also did), and the one tree's NAR hash and name are paired in the
   `treeAddress` memo, so a later `builtins.path` with that `sha256` is
   answered without a walk. */
TEST_F(AddPathTest, namedFilteredTreeWithSha256IsWalkedOnceAndPaired)
{
    auto oracle = OracleAccessor::make();
    auto oraclePath = mountAt(CanonPath("/add-path-test-oracle"), oracle);
    auto tree = sourceTree(freshName());
    auto treePath =
        mountAt(CanonPath(state.store->printStorePath(StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-tree"})), tree);

    auto treeHash = merkle::objectHash(objectHashOf(*tree, CanonPath::root).root);
    auto narHash = tree->hashPath(CanonPath::root);

    auto result = addPath(treePath, oraclePath, narHash);
    EXPECT_EQ(result, state.store->printStorePath(gitTreePath(*state.store, "tree", treeHash)));
    EXPECT_EQ(oracle->reads, 1u);
    auto paired = lookupTreeAddress(state.fetchSettings, narHash);
    ASSERT_TRUE(paired);
    EXPECT_EQ(*paired, treeHash);
}

} // namespace nix
