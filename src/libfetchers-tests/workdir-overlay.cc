/* Item 4: makeWorkdirOverlay factory.
 *
 * This is the libfetchers-side glue between `GitRepo::WorkdirInfo`
 * (the libgit2 dirty-workdir status snapshot) and the libutil
 * `recipe::Overlay` source-view shape. The factory's job is purely
 * mechanical: thread `wd.dirtyFiles` into the overlay's `entries`
 * and `wd.deletedFiles` into its `whiteouts`. We test it here
 * (rather than in libutil-tests) because `WorkdirInfo` is a
 * libfetchers struct.
 */

#include "nix/fetchers/source-view-git.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-view.hh"

#include <gtest/gtest.h>

namespace nix {

namespace {

ref<MemorySourceAccessor> makeBaseTree()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->addFile(CanonPath("flake.nix"), "committed-bytes");
    m->addFile(CanonPath("src/main.rs"), "fn main() {}");
    m->addFile(CanonPath("README.md"), "v1");
    return m;
}

ref<MemorySourceAccessor> makeWorkdirSnapshot()
{
    /* The workdir snapshot only needs paths the overlay routes to
       it. dirtyFiles entries must exist here; deletions don't need
       backing bytes (they're whiteouts). */
    auto m = make_ref<MemorySourceAccessor>();
    m->addFile(CanonPath("README.md"), "v2 dirty");
    m->addFile(CanonPath("src/lib.rs"), "added file");
    return m;
}

} // anonymous namespace

TEST(WorkdirOverlay, EntriesFromDirtyFiles)
{
    auto base = makeBaseTree();
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    wd.dirtyFiles.insert(CanonPath("/README.md"));
    wd.deletedFiles.clear();

    auto view = makeWorkdirOverlay(base, workdir, wd);

    /* Modified file routes to workdir bytes. */
    EXPECT_EQ(view->readFile(CanonPath("/README.md")), "v2 dirty");
    /* Unchanged file still served by base tree. */
    EXPECT_EQ(view->readFile(CanonPath("/flake.nix")), "committed-bytes");
}

TEST(WorkdirOverlay, WhiteoutsFromDeletedFiles)
{
    auto base = makeBaseTree();
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    wd.deletedFiles.insert(CanonPath("/src/main.rs"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    /* Deleted file disappears. */
    EXPECT_FALSE(view->maybeLstat(CanonPath("/src/main.rs")).has_value());
    /* Sibling files in the same directory still visible. */
    EXPECT_TRUE(view->maybeLstat(CanonPath("/flake.nix")).has_value());
}

TEST(WorkdirOverlay, AddedFileSurfacedThroughOverlay)
{
    auto base = makeBaseTree();
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    /* `dirtyFiles` includes both modified and added files. */
    wd.dirtyFiles.insert(CanonPath("/src/lib.rs"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    /* Added file appears in the merged dir listing... */
    auto entries = view->readDirectory(CanonPath("/src"));
    EXPECT_TRUE(entries.contains("lib.rs"));
    EXPECT_TRUE(entries.contains("main.rs"));
    /* ...and reads route to the workdir snapshot. */
    EXPECT_EQ(view->readFile(CanonPath("/src/lib.rs")), "added file");
}

TEST(WorkdirOverlay, ProvenanceRecordsTheDelta)
{
    /* The factory threads dirtyFiles/deletedFiles into provenance.
       This lets downstream code (e.g. error rendering, dirty-flag
       UX) introspect what changed without re-running the libgit2
       status walk. */
    auto base = makeBaseTree();
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    wd.dirtyFiles.insert(CanonPath("/README.md"));
    wd.dirtyFiles.insert(CanonPath("/src/lib.rs"));
    wd.deletedFiles.insert(CanonPath("/src/main.rs"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    auto prov = view->getProvenance();
    EXPECT_EQ(prov.dirtyFiles, wd.dirtyFiles);
    EXPECT_EQ(prov.deletedFiles, wd.deletedFiles);
}

TEST(WorkdirOverlay, EmptyWorkdirInfoIsIdentityOverlay)
{
    /* If the workdir is clean (no dirty files, no deletions), the
       overlay view exposes the base tree unchanged. This is the
       no-op case that lets the same code path handle dirty and
       clean workdirs. */
    auto base = makeBaseTree();
    auto workdir = make_ref<MemorySourceAccessor>(); /* empty */

    GitRepo::WorkdirInfo wd; /* empty */

    auto view = makeWorkdirOverlay(base, workdir, wd);

    EXPECT_EQ(view->readFile(CanonPath("/README.md")), "v1");
    EXPECT_EQ(view->readFile(CanonPath("/flake.nix")), "committed-bytes");
    EXPECT_TRUE(view->maybeLstat(CanonPath("/src/main.rs")).has_value());

    auto prov = view->getProvenance();
    EXPECT_TRUE(prov.dirtyFiles.empty());
    EXPECT_TRUE(prov.deletedFiles.empty());
}

TEST(WorkdirOverlay, IdentityIsLocalCapability)
{
    /* Even when the base tree is content-addressed (`tree:<sha>`),
       the overlay view is LocalCapability because synthesising a
       Git tree from arbitrary overlay bytes is the deferred Track
       H stretch. */
    auto base = makeBaseTree();
    base->fingerprint = "tree:abcdef0123456789";
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    wd.dirtyFiles.insert(CanonPath("/README.md"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    EXPECT_EQ(view->getIdentity(CanonPath::root).purity, ViewPurity::LocalCapability);
}

TEST(WorkdirOverlay, RootFingerprintIsNulloptWhenDirty)
{
    /* Soundness: when ANY entry or whiteout exists, the view's root
       fingerprint must NOT be the base's tree OID. Otherwise a
       cache row keyed on `tree:<headRev>` would hit even though
       the dirty bytes differ. */
    auto base = makeBaseTree();
    base->fingerprint = "tree:abcdef0123456789";
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    wd.dirtyFiles.insert(CanonPath("/README.md"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    auto [_, fp] = view->getFingerprint(CanonPath::root);
    EXPECT_FALSE(fp.has_value()) << "got '" << fp.value_or("(null)") << "' — must be nullopt";
}

TEST(WorkdirOverlay, FingerprintNulloptForAncestorOfDirtyPath)
{
    /* `/src` is an ancestor of `/src/main.rs`. With a dirty
       `/src/main.rs`, asking for `/src`'s fingerprint must miss
       (the directory's content has changed). */
    auto base = makeBaseTree();
    base->fingerprint = "tree:abcdef0123456789";
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    wd.dirtyFiles.insert(CanonPath("/src/main.rs"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    auto [_, fp] = view->getFingerprint(CanonPath("/src"));
    EXPECT_FALSE(fp.has_value()) << "got '" << fp.value_or("(null)") << "' — must be nullopt";
}

TEST(WorkdirOverlay, FingerprintForCleanSiblingFallsBackToBase)
{
    /* `/flake.nix` is a sibling of dirty `/README.md` — both live
       under `/`, but `/flake.nix` itself isn't in any entry's
       subtree. Querying its fingerprint should propagate base's
       per-subpath answer (assuming the base accessor produces
       per-subpath fingerprints). For a base whose getFingerprint
       returns its top-level `fingerprint` field for any path
       (MemorySourceAccessor's default), we get that. */
    auto base = makeBaseTree();
    base->fingerprint = "tree:basetree";
    auto workdir = makeWorkdirSnapshot();

    GitRepo::WorkdirInfo wd;
    wd.dirtyFiles.insert(CanonPath("/README.md"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    auto [_, fp] = view->getFingerprint(CanonPath("/flake.nix"));
    EXPECT_EQ(fp, "tree:basetree");
}

TEST(WorkdirOverlay, ReadingWhiteoutThrowsFileNotFound)
{
    /* Negative: reading a deleted path must throw, not return base
       bytes. The whiteout is a hard hide. */
    auto base = makeBaseTree();
    auto workdir = make_ref<MemorySourceAccessor>();

    GitRepo::WorkdirInfo wd;
    wd.deletedFiles.insert(CanonPath("/flake.nix"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    EXPECT_THROW(view->readFile(CanonPath("/flake.nix")), std::exception);
    EXPECT_FALSE(view->maybeLstat(CanonPath("/flake.nix")).has_value());
}

TEST(WorkdirOverlay, IntermediateDirectoriesSurfaceInListings)
{
    /* When a dirty entry is deep in the tree (e.g. /dir1/foo) but
       the base tree doesn't have /dir1, listing the parent / must
       still surface dir1 as a directory child. Without this the
       NAR walk skips the new subtree entirely.

       Without this synthesised-ancestor logic, fetchGit.sh's "added
       a new dir1/foo" sequence sees an empty /dir1 in the resulting
       store path. */
    auto base = makeBaseTree();
    auto workdir = make_ref<MemorySourceAccessor>();
    workdir->addFile(CanonPath("dir1/foo"), "foo-bytes");

    GitRepo::WorkdirInfo wd;
    wd.dirtyFiles.insert(CanonPath("/dir1/foo"));

    auto view = makeWorkdirOverlay(base, workdir, wd);

    auto rootEntries = view->readDirectory(CanonPath::root);
    EXPECT_TRUE(rootEntries.contains("dir1")) << "expected synthesised /dir1 in root listing for new entry /dir1/foo";

    auto stat = view->maybeLstat(CanonPath("/dir1"));
    ASSERT_TRUE(stat.has_value()) << "maybeLstat on /dir1 returned nullopt";
    EXPECT_EQ(stat->type, SourceAccessor::tDirectory);

    EXPECT_EQ(view->readFile(CanonPath("/dir1/foo")), "foo-bytes");
}

TEST(WorkdirOverlay, NoHeadRevSkipsOverlay)
{
    /* If the repo has no commits yet, makeWorkdirOverlay shouldn't
       be called at all. We test the corresponding contract: an
       overlay with empty entries+whiteouts is a no-op pass-through
       to the base. */
    auto base = makeBaseTree();
    auto workdir = make_ref<MemorySourceAccessor>();

    GitRepo::WorkdirInfo wd; /* empty: no headRev, no dirty, no deleted */

    auto view = makeWorkdirOverlay(base, workdir, wd);

    /* Behaves identically to the base tree. */
    EXPECT_EQ(view->readFile(CanonPath("/flake.nix")), "committed-bytes");
    EXPECT_EQ(view->readDirectory(CanonPath::root).size(), base->readDirectory(CanonPath::root).size());
    EXPECT_TRUE(view->getProvenance().dirtyFiles.empty());
    EXPECT_TRUE(view->getProvenance().deletedFiles.empty());
}

} // namespace nix
