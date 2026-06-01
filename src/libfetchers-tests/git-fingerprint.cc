/* Track B (integration): subpath-aware fingerprint over a real Git
 * repository.
 *
 * The proposal's §6.1 reproducer: build a repo with two commits that
 * share a stable `/sub` subtree but differ in another file. The
 * subpath-aware `GitSourceAccessor::getFingerprint("/sub")` must
 * return *the same* `tree:<sha>` fingerprint for both commits
 * (because the subtree's tree-OID is identical), even though the
 * commits' OIDs differ.
 *
 * This test uses a real on-disk git repo built via libgit2, which
 * matches the production code path (no mocks).
 */

#include "nix/fetchers/git-utils.hh"
#include "nix/fetchers/filtered-shape.hh"
#include "nix/fetchers/git-promisor.hh"
#include "nix/fetchers/source-view-git.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/source-view.hh"

#include <git2/blob.h>
#include <git2/commit.h>
#include <git2/global.h>
#include <git2/index.h>
#include <git2/object.h>
#include <git2/oid.h>
#include <git2/refs.h>
#include <git2/remote.h>
#include <git2/repository.h>
#include <git2/signature.h>
#include <git2/tree.h>
#include <git2/blob.h>
#include <git2/odb.h>

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

namespace nix::fetchers {

class GitFingerprintTest : public ::testing::Test
{
    std::unique_ptr<AutoDelete> delTmpDir;

protected:
    std::filesystem::path tmpDir;
    git_repository * repo = nullptr;

public:
    void SetUp() override
    {
        tmpDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmpDir, true);
        git_libgit2_init();
        ASSERT_EQ(git_repository_init(&repo, tmpDir.string().c_str(), 0), 0);
    }

    void TearDown() override
    {
        if (repo) {
            git_repository_free(repo);
            repo = nullptr;
        }
        delTmpDir.reset();
    }

    /* Write a file under the worktree root. */
    void writeWorktreeFile(std::string_view rel, std::string_view content)
    {
        auto path = tmpDir / rel;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream f(path);
        f << content;
    }

    /* Write an EXECUTABLE regular file (git records mode 100755). */
    void writeWorktreeExec(std::string_view rel, std::string_view content)
    {
        writeWorktreeFile(rel, content);
        auto path = tmpDir / rel;
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add);
    }

    /* Create a SYMLINK under the worktree root (git records mode 120000;
       the symlink target is stored verbatim as the blob). */
    void writeWorktreeSymlink(std::string_view rel, std::string_view target)
    {
        auto path = tmpDir / rel;
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::create_symlink(target, path);
    }

    /* Stage the worktree and commit, returning the commit OID.
       Uses parent OID if non-null. */
    git_oid commitWorktree(const char * msg, const git_oid * parentOid = nullptr)
    {
        git_index * idx = nullptr;
        EXPECT_EQ(git_repository_index(&idx, repo), 0);
        EXPECT_EQ(git_index_add_all(idx, nullptr, 0, nullptr, nullptr), 0);

        git_oid treeOid;
        EXPECT_EQ(git_index_write_tree(&treeOid, idx), 0);
        git_index_free(idx);

        git_tree * tree = nullptr;
        EXPECT_EQ(git_tree_lookup(&tree, repo, &treeOid), 0);

        git_signature * sig = nullptr;
        EXPECT_EQ(git_signature_now(&sig, "Test", "test@example.com"), 0);

        git_oid commitOid;
        std::vector<const git_commit *> parents;
        git_commit * parent = nullptr;
        if (parentOid) {
            EXPECT_EQ(git_commit_lookup(&parent, repo, parentOid), 0);
            parents.push_back(parent);
        }

        EXPECT_EQ(
            git_commit_create(&commitOid, repo, "HEAD", sig, sig, "UTF-8", msg, tree, parents.size(), parents.data()),
            0);

        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        return commitOid;
    }

    Hash toHash(const git_oid & oid)
    {
        char buf[GIT_OID_SHA1_HEXSIZE + 1] = {0};
        git_oid_tostr(buf, sizeof(buf), &oid);
        return Hash::parseAny(std::string(buf), HashAlgorithm::SHA1);
    }

    /** Resolve a commit OID to its root tree OID via libgit2. */
    Hash treeOidOf(const git_oid & commitOid)
    {
        git_commit * commit = nullptr;
        EXPECT_EQ(git_commit_lookup(&commit, repo, &commitOid), 0);
        auto * treeOid = git_commit_tree_id(commit);
        Hash result = toHash(*treeOid);
        git_commit_free(commit);
        return result;
    }

    /* The load-bearing equivalence, factored so the deterministic
       leaf-kind tests and the RapidCheck property share ONE assertion
       body. Both legs read through the SAME accessor type
       (`getAccessor`) so the only difference under test is structural:
       did `synthesiseTree(base, accepted)` build the same tree that the
       filtered walk over `base` with the same accepted set serialises?

         Leg A (synthetic): synthesiseTree → accessor → hashPath(root).
         Leg B (filtered walk): accessor->hashPath(root, accepted-filter).

       This mirrors `MaterialisationScheduler::computeNarHash` exactly:
       the synthetic OID is built over `sub.acceptedPaths` (Leg A), and
       the cold DryRun walk runs with the accepted-set filter
       (`shape->accepted.contains`, fetch-to-store.cc) (Leg B). Returns
       the (synthetic, walk) hash pair so callers can RC_ASSERT/EXPECT. */
    std::pair<Hash, Hash> synthVsWalk(
        GitRepo & gitRepo, SourceAccessor & baseAccessor, const Hash & baseTreeOid, const std::set<CanonPath> & accepted)
    {
        auto syntheticOid = gitRepo.synthesiseTree(baseTreeOid, accepted);
        auto syntheticNar = gitRepo.getAccessor(syntheticOid, {}, "")->hashPath(CanonPath::root);

        PathFilter acceptedFilter = [&](const std::string & p) { return accepted.contains(CanonPath(p)); };
        auto filteredWalkNar = baseAccessor.hashPath(CanonPath::root, acceptedFilter);

        return {syntheticNar, filteredWalkNar};
    }
};

TEST_F(GitFingerprintTest, RootFingerprintForCommitedTree)
{
    /* Single commit with a file. The root fingerprint should fall
       back to the accessor's input-level `fingerprint` (set
       externally by `Input::getAccessorUnchecked` — here we set it
       ourselves to mimic that). */
    writeWorktreeFile("hello.txt", "hi");
    auto commit = commitWorktree("first");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    accessor->fingerprint = "git:" + rev.gitRev();

    /* Root: no override engaged → returns whatever
       SourceAccessor::getFingerprint default does, which is
       `(path, fingerprint)`. */
    auto [retPath, fp] = accessor->getFingerprint(CanonPath::root);
    ASSERT_TRUE(fp.has_value());
    EXPECT_EQ(*fp, "git:" + rev.gitRev());
}

/* End-to-end regression for the Track Z.gap1 dead-bridge bug: the
   whole-input narHash bridge keys on `getRootTreeHash()`, but the
   deferred `builtins.path`-over-git path wraps the git accessor in a
   `SourceViewAccessor` before handing it to `fetchToStore2`. Before the
   `SourceViewAccessor::getRootTreeHash` forward, the wrapped accessor
   returned the base-class nullopt — so the bridge was DEAD on every
   wrapped path (the bug `whole-input-tree-dedup.sh` missed because it
   uses the UNWRAPPED fetchTree accessor). This asserts the OID survives
   the wrapper. */
TEST_F(GitFingerprintTest, RootTreeHashSurvivesSourceViewWrapper)
{
    writeWorktreeFile("hello.txt", "hi");
    writeWorktreeFile("sub/inner.txt", "deep");
    auto commit = commitWorktree("first");
    auto rev = toHash(commit);
    auto rootTree = treeOidOf(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    accessor->fingerprint = "git:" + rev.gitRev();

    /* The leaf git accessor surfaces the root tree OID. */
    ASSERT_EQ(accessor->getRootTreeHash(), rootTree);

    /* And — the fix — a Root SourceView wrapper FORWARDS it (this is the
       accessor shape the deferred builtins.path path actually feeds to
       fetchToStore2). Red before the SourceViewAccessor override. */
    auto rootView = sourceViewRoot(accessor);
    EXPECT_EQ(rootView->getRootTreeHash(), rootTree)
        << "Root SourceView did not forward the git root tree OID — Track Z.gap1 bridge is dead on the wrapped path";
}

/* Forge inputs (github:/gitlab:/sourcehut:) end up as a `GitSourceAccessor`
   rooted at the TARBALL-CACHE TREE OID, not a commit — see github.cc's
   `tarballCache->getAccessor(treeHash, ...)`. The PROPOSAL §6.3 claim is that
   forge inputs "get the tree-OID bridge for free": a forge input and a plain
   git input that resolve to the same tree must key the SAME
   `treeHashToNarHash` row, which requires a TREE-rooted accessor to report the
   same `getRootTreeHash()` + same NAR as a COMMIT-rooted accessor of that tree.

   This pins that property directly via the two accessor SHAPES — it does NOT go
   through github.cc / the InputScheme (a faked-forge VM test's job). It holds
   because `GitSourceAccessor`'s ctor peels a commit to its root tree
   (peelToTreeOrBlob), so both accessors wrap the identical tree object; that
   peel is precisely the invariant the bridge relies on, and a regression that
   broke it (or made a tree-rooted accessor report a different OID/NAR) fails
   here. Adversarial gap #3: nothing tested the tree-rooted (forge) shape. */
TEST_F(GitFingerprintTest, ForgeTreeRootedAccessorSharesRootTreeHashWithCommit)
{
    writeWorktreeFile("hello.txt", "hi");
    writeWorktreeFile("sub/inner.txt", "deep");
    auto commit = commitWorktree("first");
    auto rev = toHash(commit);
    auto rootTree = treeOidOf(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});

    /* The plain git-input shape: accessor rooted at the COMMIT. */
    auto commitAccessor = gitRepo->getAccessor(rev, {}, "");

    /* The forge/tarball shape: accessor rooted at the TREE OID directly
       (exactly what github.cc/tarball.cc construct via
       `tarballCache->getAccessor(treeHash, ...)`). */
    auto treeAccessor = gitRepo->getAccessor(rootTree, {}, "");

    /* Both must surface the same root tree OID → both key the same
       treeHashToNarHash bridge row → a forge input and a git input of the same
       tree share one NAR-hash walk. */
    ASSERT_EQ(commitAccessor->getRootTreeHash(), rootTree);
    EXPECT_EQ(treeAccessor->getRootTreeHash(), rootTree)
        << "Tree-rooted (forge) accessor did not report the root tree OID — forge inputs would not hit the tree-OID bridge";

    /* And they must read byte-identically (same tree ⇒ same NAR), the soundness
       premise of sharing a bridge row. */
    EXPECT_EQ(commitAccessor->hashPath(CanonPath::root), treeAccessor->hashPath(CanonPath::root))
        << "commit-rooted and tree-rooted accessors of the same tree produced different NARs";
}

TEST_F(GitFingerprintTest, NarAlteringAccessorOptionsSuppressRootTreeHash)
{
    /* SOUNDNESS regression (adversarial review): `getRootTreeHash`
       keys the cross-pipeline `treeHashToNarHash` bridge on "the NAR
       of this accessor's tree == vanilla NAR of OID T". An accessor
       option that alters the bytes while keeping the tree OID (LFS
       smudge replaces pointer-file content; export-ignore drops files)
       MUST return nullopt, or a non-LFS source resolving to the same
       tree T would read back the altered narHash → wrong store path.

       Unlike the recipe-level NAR alterations (Subtree/Subset/Overlay),
       LFS sits on the RAW leaf `GitSourceAccessor` (no wrapper), so
       only an options check inside `getRootTreeHash` catches it. Red
       before the `smudgeLfs || exportIgnore` guard in git-utils.cc. */
    writeWorktreeFile("hello.txt", "hi");
    auto commit = commitWorktree("first");
    auto rev = toHash(commit);
    auto rootTree = treeOidOf(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});

    /* Baseline: vanilla options surface the OID. */
    EXPECT_EQ(gitRepo->getAccessor(rev, {}, "")->getRootTreeHash(), rootTree);

    /* LFS smudge alters the NAR but not the tree OID → must suppress.
       (We don't need a real LFS object; the guard is unconditional on
       the option, which is the property that keeps the bridge sound.
       The `lfs::Fetch` the smudging accessor builds eagerly parses the
       `origin` remote URL, so give the repo one.) */
    git_remote * remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repo, "origin", "https://example.invalid/repo.git"), 0);
    git_remote_free(remote);
    EXPECT_EQ(gitRepo->getAccessor(rev, {.smudgeLfs = true}, "")->getRootTreeHash(), std::nullopt)
        << "LFS-smudging accessor leaked the unfiltered tree OID — poisons treeHashToNarHash for non-LFS consumers";

    /* Export-ignore likewise: production wraps it in
       GitExportIgnoreSourceAccessor, which inherits the base nullopt —
       this asserts the end-to-end production shape stays nullopt. */
    EXPECT_EQ(gitRepo->getAccessor(rev, {.exportIgnore = true}, "")->getRootTreeHash(), std::nullopt)
        << "export-ignore accessor leaked the unfiltered tree OID";
}

TEST_F(GitFingerprintTest, SubtreeFingerprintIsContentKeyed)
{
    /* The §6.1 reproducer:
     *   r1: /sub/file.txt = "stable"; /other.txt = "v1"
     *   r2: /sub/file.txt = "stable"; /other.txt = "v2"
     *
     * Both have identical /sub trees (same blob, same tree-OID).
     * Different commit OIDs (because /other.txt changed).
     *
     * `getFingerprint("/sub")` against either rev's accessor must
     * return the same `tree:<sha>` fingerprint.
     */
    writeWorktreeFile("sub/file.txt", "stable content");
    writeWorktreeFile("other.txt", "v1");
    auto c1 = commitWorktree("v1");

    /* Modify /other.txt only — /sub unchanged. */
    writeWorktreeFile("other.txt", "v2");
    auto c2 = commitWorktree("v2", &c1);

    EXPECT_FALSE(git_oid_equal(&c1, &c2)) << "commits should differ";

    auto rev1 = toHash(c1);
    auto rev2 = toHash(c2);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto a1 = gitRepo->getAccessor(rev1, {}, "");
    auto a2 = gitRepo->getAccessor(rev2, {}, "");
    /* Mimic how Input::getAccessorUnchecked sets the fingerprint. */
    a1->fingerprint = "git:" + rev1.gitRev();
    a2->fingerprint = "git:" + rev2.gitRev();

    auto [p1, fp1] = a1->getFingerprint(CanonPath("/sub"));
    auto [p2, fp2] = a2->getFingerprint(CanonPath("/sub"));

    ASSERT_TRUE(fp1.has_value());
    ASSERT_TRUE(fp2.has_value());
    /* Both subtree fingerprints anchor at root. */
    EXPECT_EQ(p1, CanonPath::root);
    EXPECT_EQ(p2, CanonPath::root);
    /* The subtree fingerprint must be tree-OID-keyed and shared. */
    EXPECT_EQ(*fp1, *fp2);
    EXPECT_TRUE(fp1->starts_with("tree:")) << "got '" << *fp1 << "'";
}

TEST_F(GitFingerprintTest, BlobFingerprintIncludesMode)
{
    /* `blob:<sha>;m=<mode>` — the same blob OID under different
     * file modes (executable bit, symlink) must produce different
     * fingerprints. We stage a regular file and an executable
     * version of the same content, expecting them to share the
     * blob OID but differ in the mode suffix.
     */
    writeWorktreeFile("regular.txt", "shared content");
    /* Make the same content executable in a different filename. */
    writeWorktreeFile("executable.txt", "shared content");
    auto path = tmpDir / "executable.txt";
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);

    auto commit = commitWorktree("two-files");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    accessor->fingerprint = "git:" + rev.gitRev();

    auto [_, regFp] = accessor->getFingerprint(CanonPath("/regular.txt"));
    auto [__, exeFp] = accessor->getFingerprint(CanonPath("/executable.txt"));

    ASSERT_TRUE(regFp.has_value());
    ASSERT_TRUE(exeFp.has_value());
    EXPECT_TRUE(regFp->starts_with("blob:")) << "got '" << *regFp << "'";
    EXPECT_TRUE(exeFp->starts_with("blob:")) << "got '" << *exeFp << "'";
    /* Same blob OID → same prefix up to ";m="; different modes →
       different full fingerprints. */
    EXPECT_NE(*regFp, *exeFp);
    /* Both should contain ";m=". */
    EXPECT_NE(regFp->find(";m="), std::string::npos);
    EXPECT_NE(exeFp->find(";m="), std::string::npos);
}

TEST_F(GitFingerprintTest, MissingSubpathFallsBackToInputFingerprint)
{
    /* For a path that doesn't exist in the tree, getFingerprint
       should fall back to the input-level fingerprint rather than
       returning nullopt — preserving the existing cache-key
       behaviour for callers that haven't fully migrated. */
    writeWorktreeFile("hello.txt", "hi");
    auto commit = commitWorktree("first");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    accessor->fingerprint = "git:" + rev.gitRev();

    auto [retPath, fp] = accessor->getFingerprint(CanonPath("/nonexistent.txt"));
    ASSERT_TRUE(fp.has_value());
    /* Falls back to whole-input identity. */
    EXPECT_EQ(*fp, "git:" + rev.gitRev());
}

/* ---------- Track G: readBlob lock split correctness ----------
 *
 * The readBlob refactor splits the State-lock-protected tree walk
 * from a lock-free git_odb_read. The behavioural test is "bytes
 * survive the round trip" — a regression here would mean blobs come
 * back wrong. We pile on a few file types (regular, executable,
 * symlink) because the phase 2 path branches on them.
 */

TEST_F(GitFingerprintTest, ReadBlobBytesAreCorrect)
{
    writeWorktreeFile("regular.txt", "regular content\n");
    writeWorktreeFile("nested/deep.txt", "deep content");
    auto commit = commitWorktree("read-test");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");

    EXPECT_EQ(accessor->readFile(CanonPath("/regular.txt")), "regular content\n");
    EXPECT_EQ(accessor->readFile(CanonPath("/nested/deep.txt")), "deep content");
}

TEST_F(GitFingerprintTest, ReadBlobLargeContent)
{
    /* The phase-2 ODB read returns the entire blob; verify it
       handles content larger than typical pipe buffers. */
    std::string big(64 * 1024, 'A'); // 64KB
    writeWorktreeFile("big.txt", big);
    auto commit = commitWorktree("big");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    EXPECT_EQ(accessor->readFile(CanonPath("/big.txt")), big);
}

TEST_F(GitFingerprintTest, ReadBlobBinaryContent)
{
    /* Embedded NUL bytes — verify the phase-2 read doesn't go
       through any text-truncating path. */
    std::string bin;
    for (int i = 0; i < 256; ++i)
        bin.push_back(static_cast<char>(i));
    writeWorktreeFile("bin.dat", bin);
    auto commit = commitWorktree("bin");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    EXPECT_EQ(accessor->readFile(CanonPath("/bin.dat")), bin);
}

TEST_F(GitFingerprintTest, SynthesiseTreeProducesSubsetOfBase)
{
    /* Track H stretch: synthesiseTree produces a Git tree object
       containing only the entries from `baseTree` whose paths are
       in the accepted set, *without reading any blob bytes*. The
       OID is content-determined, so two synthesise calls with the
       same inputs produce the same OID — the cargo-workspace
       property at the tree-synthesis level. */
    writeWorktreeFile("kept.txt", "kept content");
    writeWorktreeFile("dropped.txt", "dropped content");
    writeWorktreeFile("dir/inner.txt", "deep");
    writeWorktreeFile("dir/also.txt", "also deep");
    auto commit = commitWorktree("subset");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    accessor->fingerprint = "git:" + rev.gitRev();
    auto [_, fp] = accessor->getFingerprint(CanonPath::root);
    /* Get the base tree OID. For a commit, the root fingerprint is
       the input-level form; we derive the tree by going through
       getCommitTree. */
    auto baseTreeOid = treeOidOf(commit);

    /* Accept only kept.txt and dir/inner.txt. */
    std::set<CanonPath> accepted = {CanonPath("/kept.txt"), CanonPath("/dir/inner.txt")};
    auto syntheticOid1 = gitRepo->synthesiseTree(baseTreeOid, accepted);

    /* Determinism: same inputs → same OID. */
    auto syntheticOid2 = gitRepo->synthesiseTree(baseTreeOid, accepted);
    EXPECT_EQ(syntheticOid1, syntheticOid2);

    /* Read back the synthetic tree via a fresh accessor and verify
       only accepted paths are present. */
    auto syntheticAccessor = gitRepo->getAccessor(syntheticOid1, {}, "");
    EXPECT_TRUE(syntheticAccessor->pathExists(CanonPath("/kept.txt")));
    EXPECT_TRUE(syntheticAccessor->pathExists(CanonPath("/dir/inner.txt")));
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/dropped.txt")));
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/dir/also.txt")));
    /* The accepted file's bytes must match the base — the
       synthesise step doesn't modify content, just selects entries. */
    EXPECT_EQ(syntheticAccessor->readFile(CanonPath("/kept.txt")), "kept content");
}

TEST_F(GitFingerprintTest, SynthesiseTreeEmptyAcceptedSetProducesEmptyTree)
{
    /* Edge case: an empty accept set produces an empty tree (the
       synthetic OID is the well-known `4b825dc6...` OID for an
       empty tree). Determinism: same empty-set call same OID. */
    writeWorktreeFile("a.txt", "A");
    writeWorktreeFile("b.txt", "B");
    auto commit = commitWorktree("two-files");
    auto baseTreeOid = treeOidOf(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto syntheticOid = gitRepo->synthesiseTree(baseTreeOid, {});

    /* The synthetic tree must exist (we wrote it) and must not
       have any entries from the base. */
    auto syntheticAccessor = gitRepo->getAccessor(syntheticOid, {}, "");
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/a.txt")));
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/b.txt")));
    /* Determinism. */
    auto syntheticOid2 = gitRepo->synthesiseTree(baseTreeOid, {});
    EXPECT_EQ(syntheticOid, syntheticOid2);
}

TEST_F(GitFingerprintTest, SynthesiseTreeDifferentSubsetsDifferentOids)
{
    /* Different accept sets ⇒ different synthetic trees. */
    writeWorktreeFile("a.txt", "A");
    writeWorktreeFile("b.txt", "B");
    auto commit = commitWorktree("two-files");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto baseTreeOid = treeOidOf(commit);

    auto onlyA = gitRepo->synthesiseTree(baseTreeOid, {CanonPath("/a.txt")});
    auto onlyB = gitRepo->synthesiseTree(baseTreeOid, {CanonPath("/b.txt")});
    auto both = gitRepo->synthesiseTree(baseTreeOid, {CanonPath("/a.txt"), CanonPath("/b.txt")});
    EXPECT_NE(onlyA, onlyB);
    EXPECT_NE(onlyA, both);
    EXPECT_NE(onlyB, both);
}

TEST_F(GitFingerprintTest, ResolveSubsetViewToSyntheticGitTree)
{
    /* End-to-end Track H stretch: a Subset SourceView over a Git
       base produces a synthetic tree OID via resolveViewToGitTree.
       This is what the materialisation scheduler will use to skip
       NAR walks on Git-shaped sources. */
    writeWorktreeFile("included.rs", "// included");
    writeWorktreeFile("target/build-output", "junk");
    auto commit = commitWorktree("rust-pkg");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    accessor->fingerprint = "tree:" + treeOidOf(commit).gitRev();
    /* For this test we use a tree-rooted accessor's fingerprint
       directly so resolveViewToGitTree has a `tree:<sha>` to
       extract. */

    auto shape = hashString(HashAlgorithm::SHA256, "exclude-target-shape");
    auto view = sourceViewSubset(accessor, shape, {CanonPath("/included.rs")});

    auto resolved = resolveViewToGitTree(*view, *gitRepo);
    ASSERT_TRUE(resolved.has_value());

    /* The synthetic tree contains only included.rs; verify by
       opening an accessor over the synthetic OID. */
    auto syntheticAccessor = gitRepo->getAccessor(*resolved, {}, "");
    EXPECT_TRUE(syntheticAccessor->pathExists(CanonPath("/included.rs")));
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/target/build-output")));
}

TEST_F(GitFingerprintTest, ResolveSubsetSubpathViewToSyntheticGitTree)
{
    /* Stage-4: Subset with a non-root subpath. The recipe re-anchors
       at the subpath; resolveViewToGitTree should ask base for the
       subpath's tree fingerprint, then synthesise a tree containing
       only the accepted children of that subtree. */
    writeWorktreeFile("subdir/included.rs", "// included");
    writeWorktreeFile("subdir/excluded.rs", "// excluded");
    writeWorktreeFile("other.txt", "// not in subpath");
    auto commit = commitWorktree("subset-subpath");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    accessor->fingerprint = "tree:" + treeOidOf(commit).gitRev();

    auto shape = hashString(HashAlgorithm::SHA256, "subdir-only-included-shape");
    /* Subset rooted at /subdir, accepting only /included.rs (relative
       to subdir). */
    auto view = sourceViewSubset(accessor, shape, {CanonPath("/included.rs")}, CanonPath("/subdir"));

    auto resolved = resolveViewToGitTree(*view, *gitRepo);
    ASSERT_TRUE(resolved.has_value()) << "expected synthesised tree OID for Subset-with-subpath";

    /* Open the synthesised tree as an accessor; verify it contains
       included.rs at the root and nothing from outside /subdir. */
    auto syntheticAccessor = gitRepo->getAccessor(*resolved, {}, "");
    EXPECT_TRUE(syntheticAccessor->pathExists(CanonPath("/included.rs")));
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/excluded.rs")));
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/other.txt")));
}

/* The load-bearing equivalence the whole synthesiseTree feature rests on:
   the synthetic tree, walked, must have the SAME narHash as the filtered
   SourceView walk it is meant to replace (the `git-utils.hh` docstring
   says they are "equivalent"). This was never tested — the existing tests
   only check pathExists/readFile, never narHash, and hand-pick accept-sets
   where every accepted directory has an accepted child.

   The bug (PROPOSAL.md §6.3): when an accepted path is a DIRECTORY with
   NO accepted children (a trie leaf), `synthesiseTreeRecursive` splices
   the entire base subtree verbatim by OID — including filter-rejected
   files — whereas the filtered walk renders that directory empty. So the
   two narHashes diverge. This is reachable because `collectFilteredShape`
   inserts a directory into the accepted set whenever the filter accepts
   it, independent of its children. */
TEST_F(GitFingerprintTest, SynthesiseTreeNarHashMatchesFilteredWalk_DirectoryLeaf)
{
    /* /dir is accepted as an entry, but NONE of /dir's children are. The
       filtered view renders /dir as an empty directory; the synthetic
       tree must do the same. */
    writeWorktreeFile("dir/rejected-a.txt", "a");
    writeWorktreeFile("dir/rejected-b.txt", "b");
    writeWorktreeFile("kept.txt", "kept");
    auto commit = commitWorktree("dir-leaf");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    auto baseTreeOid = treeOidOf(commit);

    /* Accepted set: /kept.txt and /dir (the directory itself, with no
       accepted children) — exactly the shape collectFilteredShape
       produces for a filter that accepts /dir but rejects /dir/*. */
    std::set<CanonPath> accepted = {CanonPath("/kept.txt"), CanonPath("/dir")};

    auto syntheticOid = gitRepo->synthesiseTree(baseTreeOid, accepted);
    auto syntheticAccessor = gitRepo->getAccessor(syntheticOid, {}, "");

    /* Structural symptom (the directly observable bug): the synthetic
       /dir must be EMPTY — it must NOT contain the rejected children. */
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/dir/rejected-a.txt")))
        << "synthetic tree leaked a filter-rejected file under an accepted directory leaf";
    EXPECT_FALSE(syntheticAccessor->pathExists(CanonPath("/dir/rejected-b.txt")));
    /* /dir itself should still exist (it was accepted) as an empty dir. */
    EXPECT_TRUE(syntheticAccessor->pathExists(CanonPath("/kept.txt")));

    auto syntheticNar = syntheticAccessor->hashPath(CanonPath::root);

    /* THE load-bearing contract (the one no prior test asserted):
       narHash(synthetic tree) == narHash(filtered walk of the base).
       Build the filtered walk's narHash directly via collectFilteredShape
       + an accepted-set filter, exactly as fetchToStore2 does
       (fetch-to-store.cc:339), over the SAME base + the SAME accept-set.
       This is the cross-component equivalence the §H' feature rests on. */
    PathFilter acceptFilter = [&](const std::string & p) { return accepted.contains(CanonPath(p)); };
    auto walkAccepted = collectFilteredShape(*accessor, CanonPath::root, acceptFilter);
    PathFilter shapeFilter = [&](const std::string & p) { return walkAccepted.accepted.contains(CanonPath(p)); };
    auto filteredWalkNar = accessor->hashPath(CanonPath::root, shapeFilter);

    EXPECT_EQ(syntheticNar, filteredWalkNar)
        << "synthetic-tree narHash differs from the filtered-walk narHash — the synthesiseTree "
           "shortcut is NOT equivalent to the walk it replaces (directory-leaf divergence, §6.3)";

    /* Bonus invariant: the synthetic narHash must not depend on a
       FILTER-REJECTED file's content (it isn't in the synthetic tree). */
    writeWorktreeFile("dir/rejected-a.txt", "DIFFERENT CONTENT");
    auto commit2 = commitWorktree("dir-leaf-perturbed", &commit);
    auto baseTreeOid2 = treeOidOf(commit2);
    auto syntheticNar2 =
        gitRepo->getAccessor(gitRepo->synthesiseTree(baseTreeOid2, accepted), {}, "")->hashPath(CanonPath::root);
    EXPECT_EQ(syntheticNar, syntheticNar2)
        << "synthetic narHash changed when a FILTER-REJECTED file's content changed — "
           "the rejected file is leaking into the synthetic tree (directory-leaf bug)";
}

TEST_F(GitFingerprintTest, ReadBlobConcurrent)
{
    /* The lock split is for eval-cores>1; we can simulate concurrent
       reads with std::thread. The State lock now protects only the
       brief tree walk; multiple threads reading different blobs
       should not stall on each other. The test isn't a stress test
       — it's a smoke check that the lock-free Phase 2 doesn't crash
       or corrupt under contention. */
    for (int i = 0; i < 16; ++i)
        writeWorktreeFile(fmt("file-%d.txt", i), fmt("contents %d", i));
    auto commit = commitWorktree("many");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");

    constexpr int nThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> mismatches{0};
    for (int t = 0; t < nThreads; ++t) {
        threads.emplace_back([&accessor, &mismatches]() {
            for (int rep = 0; rep < 32; ++rep) {
                for (int i = 0; i < 16; ++i) {
                    auto got = accessor->readFile(CanonPath(fmt("/file-%d.txt", i)));
                    auto want = fmt("contents %d", i);
                    if (got != want)
                        ++mismatches;
                }
            }
        });
    }
    for (auto & th : threads)
        th.join();
    EXPECT_EQ(mismatches.load(), 0);
}

/* The verbatim-splice leaves the §6.3 audit flagged as having ZERO
   narHash-equivalence coverage: a SYMLINK (mode 120000) and an
   EXECUTABLE regular file (mode 100755) are taken through
   `synthesiseTreeRecursive`'s `else` branch (git-utils.cc ~877) by OID +
   mode verbatim. If that branch mis-rendered the mode (e.g. dropped the
   exec bit, or treated a symlink blob as a regular file), the synthetic
   tree's NAR would diverge from the filtered walk's. This pins both. */
TEST_F(GitFingerprintTest, SynthesiseTreeNarHashMatchesFilteredWalk_SymlinkAndExecLeaves)
{
    writeWorktreeExec("script.sh", "#!/bin/sh\necho hi\n");
    writeWorktreeSymlink("link-to-script", "script.sh");
    writeWorktreeFile("plain.txt", "plain");
    writeWorktreeFile("rejected.txt", "rejected");
    auto commit = commitWorktree("symlink-exec");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    auto baseTreeOid = treeOidOf(commit);

    /* Accept the symlink + exec + a plain file; reject one plain file so
       the subset is proper. */
    std::set<CanonPath> accepted = {
        CanonPath("/script.sh"), CanonPath("/link-to-script"), CanonPath("/plain.txt")};

    auto [syntheticNar, filteredWalkNar] = synthVsWalk(*gitRepo, *accessor, baseTreeOid, accepted);
    EXPECT_EQ(syntheticNar, filteredWalkNar)
        << "synthetic-tree narHash differs from the filtered walk for a tree with a symlink (120000) "
           "and an executable (100755) leaf — the verbatim-splice else-branch mis-renders a mode";
}

#ifndef COVERAGE

/* FIXME: RC_GTEST_FIXTURE_PROP doesn't call SetUpTestSuite; this dummy
   TEST_F forces the fixture (libgit2 init + temp repo) to initialise
   before the PROP runs. Mirrors the same workaround in
   input-materialisation.cc / derived-path.cc. */
TEST_F(GitFingerprintTest, _RapidCheckInit) {}

/* THE load-bearing property (the deliverable §6.3 demanded, for
   which only hand-picked TEST_F cases existed):

     narHash(synthesiseTree(base, S)'s tree) == narHash(filtered walk of
     base with accepted-set S)   for ARBITRARY trees × accept-subsets.

   This is the equivalence `MaterialisationScheduler::computeNarHash`
   now relies on: on a `treeHashToNarHash` MISS it walks the filtered
   view to fill the row keyed on the synthetic OID, so the synthetic
   tree's NAR MUST equal the walk's or the cached row is poison.

   GENERATOR SHAPE (hand-rolled — a full `Arbitrary<git-tree>` is too
   heavy; we stage a randomised layout via the worktree + git index):

     - `kinds`: one entry per fixed CANDIDATE path, each ∈
       {absent, regular, executable(100755), symlink(120000)}. The
       candidate list deliberately spans nested paths so the staged tree
       has random depth, random per-leaf modes, and the
       verbatim-splice leaf kinds (symlink/exec) appear. Staging a nested
       file auto-creates its ancestor *directories* in git's tree.
       (gitlink/160000 is deliberately DROPPED — both legs render a
       submodule as an empty dir, so it is a vacuous case.)

     - `acceptBits`: a bit-vector consumed positionally over the SORTED
       set of all tree nodes (files AND their ancestor directories) to
       seed a raw accept-predicate. That predicate is then run through
       `collectFilteredShape` (the EXACT production call —
       fetch-to-store.cc / primops.cc) to derive the accepted set S.
       Routing through `collectFilteredShape` makes S ancestor-closed by
       construction — the production invariant: a child is reachable only
       if every ancestor directory passed the filter. Feeding a
       non-ancestor-closed set is out of contract and would cause
       spurious divergence (synthesise descends by OID; the walk can't
       reach a child under a rejected dir). Because directories are
       independently acceptable, S can still accept a directory while
       rejecting all its children — the §6.3 DIRECTORY-LEAF case
       (accepted dir, no accepted children ⇒ must synthesise an EMPTY
       dir). RC's shrinker drives toward minimal counterexamples.

   RC_PRE drops the empty-root sample (no staged file ⇒ no commit/tree to
   anchor on). The empty accept-set is NOT excluded — synthesise(∅) is an
   empty tree and the filtered walk of ∅ is the root-only NAR, which must
   also agree (and does: the root is always included by both legs). */
RC_GTEST_FIXTURE_PROP(
    GitFingerprintTest,
    SynthesiseTreeNarHashEquivalsFilteredWalk,
    (const std::vector<int> & kinds, const std::vector<char> & acceptBits))
{
    /* Fixed candidate paths spanning depth + the load-bearing leaf
       kinds. Random `kinds[i]` decides presence + mode of each. */
    static const std::vector<std::string> candidates = {
        "a.txt",
        "b.sh",
        "c-link",
        "dir/inner.txt",
        "dir/inner.sh",
        "dir/inner-link",
        "dir/sub/deep.txt",
        "nested/x",
        "nested/y-link",
    };

    /* Stage each present candidate at its randomised kind. */
    bool anyStaged = false;
    for (size_t i = 0; i < candidates.size(); ++i) {
        int kind = kinds.size() > i ? std::abs(kinds[i]) % 4 : 0; // 0 if unspecified
        const auto & rel = candidates[i];
        switch (kind) {
        case 0:
            break; // absent
        case 1:
            writeWorktreeFile(rel, fmt("content-%zu", i));
            anyStaged = true;
            break;
        case 2:
            writeWorktreeExec(rel, fmt("#!/bin/sh\n# %zu\n", i));
            anyStaged = true;
            break;
        case 3:
            writeWorktreeSymlink(rel, fmt("target-%zu", i));
            anyStaged = true;
            break;
        }
    }
    /* No staged file ⇒ empty root tree ⇒ no commit to anchor on. */
    RC_PRE(anyStaged);

    auto commit = commitWorktree("prop");
    auto rev = toHash(commit);
    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(rev, {}, "");
    auto baseTreeOid = treeOidOf(commit);

    /* Enumerate every node actually present in the tree — each staged
       file PLUS all its ancestor directories — and sort. These seed a
       positionally-chosen RAW accept-predicate; an unset/short
       bit-vector rejects. Tracking ancestors lets the directory-leaf
       pattern (accept a dir, reject its children) arise. */
    std::set<CanonPath> allNodes;
    for (size_t i = 0; i < candidates.size(); ++i) {
        int kind = kinds.size() > i ? std::abs(kinds[i]) % 4 : 0;
        if (kind == 0)
            continue;
        CanonPath p("/" + candidates[i]);
        allNodes.insert(p);
        for (auto a = p.parent(); a && !a->isRoot(); a = a->parent())
            allNodes.insert(*a);
    }

    std::set<CanonPath> rawAccept;
    {
        size_t bit = 0;
        for (auto & node : allNodes) {
            bool accept = acceptBits.size() > bit ? (acceptBits[bit] & 1) : false;
            if (accept)
                rawAccept.insert(node);
            ++bit;
        }
    }

    /* Run the raw predicate through `collectFilteredShape` — the exact
       production call — to get the ancestor-closed accepted set S that a
       Subset recipe would carry. (A child only survives if its ancestor
       dirs survived, so S is automatically ancestor-closed.) */
    PathFilter rawFilter = [&](const std::string & p) { return rawAccept.contains(CanonPath(p)); };
    auto accepted = collectFilteredShape(*accessor, CanonPath::root, rawFilter).accepted;
    std::set<CanonPath> acceptedSet(accepted.begin(), accepted.end());

    /* THE equivalence: synthetic tree's NAR == filtered walk's NAR, over
       the SAME base + SAME accepted set, for this arbitrary tree. */
    auto [syntheticNar, filteredWalkNar] = synthVsWalk(*gitRepo, *accessor, baseTreeOid, acceptedSet);
    RC_ASSERT(syntheticNar == filteredWalkNar);
}

#endif

/* ---------- Lazy-git partial-clone internals (no network) ----------
 *
 * The load-bearing partial-clone paths — `GitSourceAccessor::readBlob`'s
 * on-demand backfill fallback and `prefetchSubtree`'s bulk enumeration —
 * were previously untested (no test ever attached a `GitPromisorProvider`
 * to an accessor). `GitPromisorProvider` is a pure-virtual interface, so
 * we stub it here: the only thing the REAL provider needs a server for is
 * the actual network transfer; the accessor-side control flow
 * (ENOTFOUND → ensureObjects → refresh → re-read; enumerate-missing →
 * coalesce) is fully exercisable against a libgit2 repo with a recording
 * stub standing in for the network. */

namespace {

/* Records every `ensureObjects` call. On fetch, "restores" each wanted
   OID by writing its saved bytes back into the repo's ODB (simulating
   what the real provider's pack-index would do), unless `failWith` is
   set, in which case it throws — exercising the readBlob addTrace+rethrow
   arm. */
struct RecordingProvider : GitPromisorProvider
{
    git_repository * repo;
    /* oid hex → blob bytes to restore on fetch. */
    std::map<std::string, std::string> restorable;
    std::vector<std::vector<Hash>> calls; // each ensureObjects invocation's wants
    std::optional<std::string> failWith;

    explicit RecordingProvider(git_repository * repo)
        : repo(repo)
    {
    }

    bool supportsFilteredFetch() override
    {
        return true;
    }

    void ensureObjects(std::span<const Hash> wants, FetchFilter) override
    {
        std::vector<Hash> v(wants.begin(), wants.end());
        calls.push_back(v);
        if (failWith)
            throw Error("%s", *failWith);
        for (auto & w : wants) {
            auto it = restorable.find(w.gitRev());
            if (it != restorable.end()) {
                git_oid oid;
                EXPECT_EQ(git_blob_create_from_buffer(&oid, repo, it->second.data(), it->second.size()), 0);
            }
        }
    }
};

} // namespace

class GitPromisorInternalsTest : public GitFingerprintTest
{
protected:
    /* Read a loose blob's bytes, then delete its loose object file so a
       subsequent `git_odb_read` returns GIT_ENOTFOUND (simulating a
       partial clone that fetched trees but not this blob). Returns the
       saved bytes. The blob OID is resolvable from the tree either way —
       only the blob *object* is removed. */
    std::string withholdBlob(const Hash & blobOid)
    {
        auto hex = blobOid.gitRev();
        auto loose = tmpDir / ".git" / "objects" / hex.substr(0, 2) / hex.substr(2);
        /* Recover the bytes via libgit2 before deleting. */
        git_odb * odb = nullptr;
        EXPECT_EQ(git_repository_odb(&odb, repo), 0);
        git_oid oid;
        git_oid_fromstr(&oid, hex.c_str());
        git_odb_object * obj = nullptr;
        EXPECT_EQ(git_odb_read(&obj, odb, &oid), 0);
        std::string bytes((const char *) git_odb_object_data(obj), git_odb_object_size(obj));
        git_odb_object_free(obj);
        git_odb_free(odb);
        std::filesystem::remove(loose);
        return bytes;
    }
};

TEST_F(GitPromisorInternalsTest, ReadBlobBacksFillsViaProviderOnEnotfound)
{
    writeWorktreeFile("a.txt", "hello backfill");
    auto commit = commitWorktree("c1");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    /* Resolve the blob OID through a (full) accessor first. */
    auto blobOid = gitRepo->getAccessor(rev, {}, "")->getFingerprint(CanonPath("/a.txt")).second;

    /* Find the blob OID via a tree walk, withhold its bytes. */
    git_oid cOid;
    git_oid_fromstr(&cOid, rev.gitRev().c_str());
    auto bytes = [&] {
        git_commit * c = nullptr;
        EXPECT_EQ(git_commit_lookup(&c, repo, &cOid), 0);
        const git_tree * t = nullptr;
        git_commit_tree(const_cast<git_tree **>(&t), c);
        const git_tree_entry * e = git_tree_entry_byname(t, "a.txt");
        Hash boid = toHash(*git_tree_entry_id(e));
        git_tree_free(const_cast<git_tree *>(t));
        git_commit_free(c);
        return std::make_pair(boid, withholdBlob(boid));
    }();

    auto provider = std::make_shared<RecordingProvider>(repo);
    provider->restorable[bytes.first.gitRev()] = bytes.second;

    /* A fresh accessor with the provider attached. Reading the withheld
       blob must: ENOTFOUND → provider->ensureObjects({blobOid}) →
       refresh → re-read → return the correct bytes. */
    auto accessor = gitRepo->getAccessor(rev, {.provider = provider}, "");
    EXPECT_EQ(accessor->readFile(CanonPath("/a.txt")), "hello backfill");

    /* The fallback fired exactly once, asking for exactly the missing OID. */
    ASSERT_EQ(provider->calls.size(), 1u);
    ASSERT_EQ(provider->calls[0].size(), 1u);
    EXPECT_EQ(provider->calls[0][0], bytes.first);
}

TEST_F(GitPromisorInternalsTest, ReadBlobRethrowsWhenProviderCannotBackfill)
{
    writeWorktreeFile("a.txt", "unrecoverable");
    auto commit = commitWorktree("c1");
    auto rev = toHash(commit);

    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    git_oid cOid;
    git_oid_fromstr(&cOid, rev.gitRev().c_str());
    git_commit * c = nullptr;
    ASSERT_EQ(git_commit_lookup(&c, repo, &cOid), 0);
    git_tree * t = nullptr;
    git_commit_tree(&t, c);
    Hash boid = toHash(*git_tree_entry_id(git_tree_entry_byname(t, "a.txt")));
    git_tree_free(t);
    git_commit_free(c);
    withholdBlob(boid);

    /* Provider that fails its fetch — readBlob must surface a thrown
       error (with the "while fetching missing Git blob" trace), not
       silently return empty or crash. */
    auto provider = std::make_shared<RecordingProvider>(repo);
    provider->failWith = "simulated transport failure";

    auto accessor = gitRepo->getAccessor(rev, {.provider = provider}, "");
    EXPECT_THROW(accessor->readFile(CanonPath("/a.txt")), Error);
    EXPECT_EQ(provider->calls.size(), 1u); // it did try
}

TEST_F(GitPromisorInternalsTest, PrefetchSubtreeCoalescesMissingBlobsAndSkipsGitlinks)
{
    /* Build a tree with two blobs at different depths. */
    writeWorktreeFile("top.txt", "top");
    writeWorktreeFile("dir/deep.txt", "deep");
    auto commit = commitWorktree("c1");
    auto rev = toHash(commit);

    /* Withhold BOTH blobs so they're enumerated as missing. */
    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto blobOf = [&](const char * dir, const char * name) {
        git_oid cOid;
        git_oid_fromstr(&cOid, rev.gitRev().c_str());
        git_commit * c = nullptr;
        git_commit_lookup(&c, repo, &cOid);
        git_tree * root = nullptr;
        git_commit_tree(&root, c);
        const git_tree_entry * e;
        git_tree * sub = nullptr;
        if (dir) {
            const git_tree_entry * de = git_tree_entry_byname(root, dir);
            git_tree_lookup(&sub, repo, git_tree_entry_id(de));
            e = git_tree_entry_byname(sub, name);
        } else
            e = git_tree_entry_byname(root, name);
        Hash h = toHash(*git_tree_entry_id(e));
        if (sub)
            git_tree_free(sub);
        git_tree_free(root);
        git_commit_free(c);
        return h;
    };
    auto top = blobOf(nullptr, "top.txt");
    auto deep = blobOf("dir", "deep.txt");
    withholdBlob(top);
    withholdBlob(deep);

    auto provider = std::make_shared<RecordingProvider>(repo);
    auto accessor = gitRepo->getAccessor(rev, {.provider = provider}, "");

    /* Exhaustive prefetch: BOTH missing blobs collected into ONE
       coalesced ensureObjects call (not one per blob). */
    accessor->prefetchSubtree(CanonPath::root, std::numeric_limits<unsigned>::max());
    ASSERT_EQ(provider->calls.size(), 1u);
    std::set<Hash> got(provider->calls[0].begin(), provider->calls[0].end());
    EXPECT_EQ(got.count(top), 1u);
    EXPECT_EQ(got.count(deep), 1u);
    /* Only the two blobs — tree objects (present) and any gitlink
       (GIT_OBJECT_COMMIT, never a fetchable blob) are NOT enumerated. */
    EXPECT_EQ(provider->calls[0].size(), 2u);
}

TEST_F(GitPromisorInternalsTest, PrefetchSubtreeIsNoopWithoutProvider)
{
    writeWorktreeFile("a.txt", "x");
    auto commit = commitWorktree("c1");
    auto gitRepo = GitRepo::openRepo(tmpDir, {});
    auto accessor = gitRepo->getAccessor(toHash(commit), {}, ""); // no provider
    /* Must not throw or fetch — a no-op when nothing is missing / no
       provider is attached. */
    EXPECT_NO_THROW(accessor->prefetchSubtree(CanonPath::root, std::numeric_limits<unsigned>::max()));
}

} // namespace nix::fetchers
