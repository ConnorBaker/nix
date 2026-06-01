#include <gtest/gtest.h>

#include <sys/stat.h>

#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"

// Needed for template specialisations (see local-store.cc).
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

namespace nix {

/* item (b): `LocalStore::assembleCAPathFromBase` materialises a store path
   that differs from an already-valid BASE by a few files, by
   reflinking/hardlinking the unchanged majority from the base and writing
   only the changed files. These tests use a per-process temp local store
   and a small on-disk directory tree.

   The load-bearing test is `AssembleFromBaseExpectedPathMismatchThrows`:
   because the assembler builds a NOVEL tree and `makeFromCA` stores narHash
   verbatim, the soundness guarantee is that the path is derived from the
   *measured* hash — so content that does not hash to the expected name is
   rejected, never silently committed. */
class RegisterAssembledCAPathTest : public ::testing::Test
{
protected:
    std::filesystem::path storeRoot;
    ref<LocalStore> store;

    RegisterAssembledCAPathTest()
        : storeRoot(createTempDir())
        , store([&] {
            auto s = openStore("local?root=" + storeRoot.string());
            auto local = s.dynamic_pointer_cast<LocalStore>();
            assert(local);
            return ref<LocalStore>(local);
        }())
    {
    }

    /* Write a {a.txt, sub/b.txt} tree to a fresh dir and return it. */
    std::filesystem::path makeTreeDir(std::string_view a, std::string_view b)
    {
        auto dir = createTempDir();
        createDirs(dir / "sub");
        writeFile(dir / "a.txt", std::string(a));
        writeFile(dir / "sub" / "b.txt", std::string(b));
        return dir;
    }

    StorePath addDir(const std::filesystem::path & dir, std::string_view name)
    {
        auto accessor = makeFSSourceAccessor(dir);
        return static_cast<Store &>(*store).addToStore(
            name, SourcePath{accessor, CanonPath::root}, ContentAddressMethod::Raw::NixArchive);
    }
};

TEST_F(RegisterAssembledCAPathTest, AssembleFromBaseComputesCorrectPath)
{
    auto baseDir = makeTreeDir("alpha", "beta");
    auto base = addDir(baseDir, "source");

    auto targetDir = makeTreeDir("alpha", "beta2");
    auto targetAccessor = makeFSSourceAccessor(targetDir);
    /* The canonical path for the target content (added under the SAME name
       so the CA path matches what assembly should produce). */
    auto reference = addDir(targetDir, "source");
    auto refInfo = store->queryPathInfo(reference);

    std::set<CanonPath> changed{CanonPath("sub/b.txt")};
    std::set<CanonPath> deleted{};

    /* No expectedPath: the assembler computes the true hash and returns
       whatever path it names. It must equal the reference path. */
    auto got = store->assembleCAPathFromBase(base, targetAccessor, changed, deleted, "source", std::nullopt);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, reference) << "assembled path must equal the canonical CA path for the target content";
    EXPECT_TRUE(store->isValidPath(*got));
    EXPECT_EQ(store->queryPathInfo(*got)->narHash, refInfo->narHash);
    EXPECT_EQ(readFile(store->toRealPath(*got) / "a.txt"), "alpha");
    EXPECT_EQ(readFile(store->toRealPath(*got) / "sub" / "b.txt"), "beta2");

    /* The UNCHANGED file (a.txt) is sourced from base, NOT byte-copied:
       reflinked (distinct inode, shared extents) on a CoW store, or
       hardlinked (same inode) on a non-CoW store — whichever
       `tryCloneFile` yields on the filesystem the test store happens to
       sit on. Both are correct; only an independent byte copy would be
       the regression. Asserting *which* sharing mechanism fired is
       filesystem-specific, so the authoritative extent-sharing check
       lives in the `dirty-tree-assemble-cow` NixOS test (which pins a
       btrfs store and verifies `btrfs fi du` exclusive==0). Here we just
       assert the content is correct via the unchanged file (already done
       above) and pin the CHANGED file's independence below. */

    /* The CHANGED file (sub/b.txt) must NOT share base's inode (it was
       freshly written with new bytes, never linked from base). */
    struct stat stBaseB, stAsmB;
    ASSERT_EQ(::lstat((store->toRealPath(base) / "sub" / "b.txt").c_str(), &stBaseB), 0);
    ASSERT_EQ(::lstat((store->toRealPath(*got) / "sub" / "b.txt").c_str(), &stAsmB), 0);
    EXPECT_NE(stBaseB.st_ino, stAsmB.st_ino) << "changed file must be freshly written, not linked from base";
}

TEST_F(RegisterAssembledCAPathTest, AssembleFromBaseExpectedPathMismatchThrows)
{
    auto baseDir = makeTreeDir("alpha", "beta");
    auto base = addDir(baseDir, "source");

    auto targetDir = makeTreeDir("alpha", "beta2");
    auto targetAccessor = makeFSSourceAccessor(targetDir);

    /* Pass a WRONG expectedPath (the base's own path, which the assembled
       changed-content tree will not match). The production seam relies on
       this throwing errno 102 (or the seam comparing and falling back); we
       assert the throw + that nothing wrong is registered under the
       expected name. */
    std::set<CanonPath> changed{CanonPath("sub/b.txt")};
    std::set<CanonPath> deleted{};

    EXPECT_THROW(
        store->assembleCAPathFromBase(base, targetAccessor, changed, deleted, "source", /*expectedPath=*/base), Error)
        << "assembling content that does not hash to expectedPath must throw";
    /* The expectedPath (base) was already valid (it's a real path); the
       point is no NEW wrong path was committed under it — base is unchanged. */
    EXPECT_EQ(readFile(store->toRealPath(base) / "sub" / "b.txt"), "beta")
        << "the mismatch must not have overwritten the base";
}

TEST_F(RegisterAssembledCAPathTest, AssembleFromBaseNonPrefixReturnsNullopt)
{
    auto baseDir = createTempDir();
    writeFile(baseDir / "a.txt", "alpha"); // base lacks sub/b.txt
    auto base = addDir(baseDir, "source");

    auto targetDir = makeTreeDir("alpha", "beta");
    auto targetAccessor = makeFSSourceAccessor(targetDir);

    std::set<CanonPath> changed{}; // nothing "changed" → sub/b.txt expected from base, but absent
    std::set<CanonPath> deleted{};

    auto got = store->assembleCAPathFromBase(base, targetAccessor, changed, deleted, "source", std::nullopt);
    EXPECT_FALSE(got.has_value()) << "a base missing an expected-unchanged file must return nullopt (caller copies)";
}

} // namespace nix
