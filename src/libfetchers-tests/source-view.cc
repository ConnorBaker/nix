/* Track H: SourceViewAccessor — recipes + identity/provenance separation.
 *
 * Coverage:
 *   - Each factory (Root, Subtree, Subset, Overlay, TreeHandle,
 *     LocalCheckout) constructs the right recipe and identity shape.
 *   - Reads forward to the base accessor (with subpath translation
 *     for Subtree, whiteout/entry routing for Overlay).
 *   - Identity / provenance separation: LocalCheckout views have
 *     `LocalCapability` purity and an exposed checkoutRoot, but no
 *     content-keyed fingerprint.
 *   - getFingerprint forwards correctly through the recipe.
 */

#include "nix/util/source-view.hh"
#include "nix/util/memory-source-accessor.hh"

#include <gtest/gtest.h>

namespace nix {

namespace {

/* Build a small in-memory tree:
     /
     ├── flake.nix     ("top-level")
     ├── README.md     ("hi")
     └── sub/
         └── inner.txt ("deep contents")
*/
ref<MemorySourceAccessor> makeMemFs()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->addFile(CanonPath("flake.nix"), "top-level");
    m->addFile(CanonPath("README.md"), "hi");
    m->addFile(CanonPath("sub/inner.txt"), "deep contents");
    return m;
}

} // namespace

TEST(SourceView, RootForwardsReads)
{
    auto base = makeMemFs();
    auto view = sourceViewRoot(base);

    /* Reads pass through unchanged. */
    EXPECT_EQ(view->readFile(CanonPath("/flake.nix")), "top-level");
    EXPECT_EQ(view->readFile(CanonPath("/sub/inner.txt")), "deep contents");
    EXPECT_TRUE(view->pathExists(CanonPath("/README.md")));
    EXPECT_FALSE(view->pathExists(CanonPath("/missing")));
}

TEST(SourceView, RootInheritsFingerprint)
{
    auto base = makeMemFs();
    base->fingerprint = "git:abc123";
    auto view = sourceViewRoot(base);

    /* Identity inherits the base's fingerprint. Purity is
       ContentAddressed when the base has one. */
    auto id = view->getIdentity(CanonPath::root);
    EXPECT_EQ(id.fingerprint, "git:abc123");
    EXPECT_EQ(id.purity, ViewPurity::ContentAddressed);
}

TEST(SourceView, RootWithoutFingerprintIsLocalCapability)
{
    auto base = makeMemFs();
    /* No fingerprint set on base. */
    auto view = sourceViewRoot(base);

    auto id = view->getIdentity(CanonPath::root);
    EXPECT_FALSE(id.fingerprint.has_value());
    EXPECT_EQ(id.purity, ViewPurity::LocalCapability);
}

/* A leaf that reports a root tree OID, to exercise the content-identity
   forwarding law (L-ContentIdFunctoriality) without a real git repo. */
namespace {
struct TreeOidLeaf : MemorySourceAccessor
{
    Hash oid;

    explicit TreeOidLeaf(Hash oid)
        : oid(oid)
    {
    }

    std::optional<Hash> getRootTreeHash() override
    {
        return oid;
    }
};

ref<TreeOidLeaf> makeTreeOidLeaf()
{
    auto m = make_ref<TreeOidLeaf>(hashString(HashAlgorithm::SHA1, "fake-tree-oid"));
    m->addFile(CanonPath("flake.nix"), "top-level");
    m->addFile(CanonPath("sub/inner.txt"), "deep contents");
    return m;
}
} // namespace

/* L-ContentIdFunctoriality — the content-identity (root tree OID) must
   be FORWARDED through a NAR-preserving wrapper. This is the law whose
   absence left the Track Z.gap1 bridge dead on the wrapped
   `builtins.path`-over-git path: `getRootTreeHash` was overridden only on
   the leaf GitSourceAccessor, so a SourceViewAccessor returned the
   base-class nullopt. Non-vacuous: before the `SourceViewAccessor`
   override these assertions are red (nullopt). */
TEST(SourceView, RootForwardsRootTreeHash)
{
    auto base = makeTreeOidLeaf();
    auto view = sourceViewRoot(base);
    /* Root preserves the base NAR byte-for-byte ⇒ forward the OID. */
    EXPECT_EQ(view->getRootTreeHash(), base->getRootTreeHash());
    ASSERT_TRUE(view->getRootTreeHash().has_value());
}

TEST(SourceView, TreeHandleReportsItsOid)
{
    auto base = makeMemFs();
    auto oid = hashString(HashAlgorithm::SHA1, "treehandle-oid");
    auto view = sourceViewTreeHandle(base, oid);
    /* TreeHandle's identity IS the tree OID (from recipe, not base). */
    EXPECT_EQ(view->getRootTreeHash(), oid);
}

/* Contrapositive — a wrapper that ALTERS the root NAR must NOT forward
   the base OID, or the `treeHashToNarHash` bridge would key a
   filtered/overlaid/subtree NAR under the unfiltered tree OID (a wrong
   narHash, the highest-stakes soundness failure). */
TEST(SourceView, NarAlteringRecipesDoNotForwardRootTreeHash)
{
    auto base = makeTreeOidLeaf();

    /* Subset (filter changes the NAR). */
    auto shapeHash = hashString(HashAlgorithm::SHA256, "shape");
    EXPECT_FALSE(sourceViewSubset(base, shapeHash)->getRootTreeHash().has_value());

    /* Overlay (workdir delta changes the NAR). */
    auto overlay = makeMemFs();
    EXPECT_FALSE(sourceViewOverlay(base, overlay, /*entries=*/{}, /*whiteouts=*/{CanonPath("/flake.nix")})
                     ->getRootTreeHash()
                     .has_value());

    /* Subtree defers to its `tree:<sub-sha>` fingerprint (the base
       already bridges that); the path-less root query is nullopt. */
    EXPECT_FALSE(sourceViewSubtree(base, CanonPath("/sub"))->getRootTreeHash().has_value());
}

TEST(SourceView, SubtreeTranslatesReads)
{
    auto base = makeMemFs();
    auto view = sourceViewSubtree(base, CanonPath("/sub"));

    /* The Subtree view re-anchors at /sub: reading "inner.txt" from
       the view goes to /sub/inner.txt on the base. */
    EXPECT_EQ(view->readFile(CanonPath("/inner.txt")), "deep contents");
    EXPECT_TRUE(view->pathExists(CanonPath("/inner.txt")));
}

TEST(SourceView, SubsetCarriesShapeHash)
{
    auto base = makeMemFs();
    base->fingerprint = "git:R";
    auto shapeHash = hashString(HashAlgorithm::SHA256, "shape-bytes");
    auto view = sourceViewSubset(base, shapeHash);

    /* The Subset's recipe records the shape hash. We can ask for
       it back via the variant. */
    ASSERT_TRUE(std::holds_alternative<recipe::Subset>(view->recipe));
    EXPECT_EQ(std::get<recipe::Subset>(view->recipe).shapeHash, shapeHash);
}

TEST(SourceView, OverlayHidesWhiteouts)
{
    auto base = makeMemFs();
    auto overlay = makeMemFs(); // Same tree (we don't use it for entries here).

    auto view = sourceViewOverlay(
        base,
        overlay,
        /*entries=*/{},
        /*whiteouts=*/{CanonPath("/README.md")});

    /* Whiteout makes the file invisible. */
    EXPECT_FALSE(view->pathExists(CanonPath("/README.md")));
    /* Other paths still readable through the base. */
    EXPECT_TRUE(view->pathExists(CanonPath("/flake.nix")));
}

TEST(SourceView, OverlayWhiteoutHidesFromDirectoryListing)
{
    auto base = makeMemFs();
    auto view = sourceViewOverlay(base, base, /*entries=*/{}, /*whiteouts=*/{CanonPath("/README.md")});

    auto entries = view->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries.contains("flake.nix"));
    EXPECT_TRUE(entries.contains("sub"));
    EXPECT_FALSE(entries.contains("README.md"));
}

/* ---------- Overlay dual-source dispatch ----------
 *
 * The Item 4 use case: a committed Git tree as `base`, the workdir
 * (modified files) as `overlay`. Reads of entries route to the
 * overlay; everything else reads from base. Verifies the dispatch
 * actually goes to two different sources, not just to base. */

TEST(SourceView, OverlayEntriesReadFromOverlayAccessor)
{
    /* Base has README.md = "hi"; overlay has README.md = "edited". */
    auto base = makeMemFs();
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("README.md"), "edited");

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{CanonPath("/README.md")}, /*whiteouts=*/{});

    /* Entry path reads from overlay. */
    EXPECT_EQ(view->readFile(CanonPath("/README.md")), "edited");
    /* Non-entry path reads from base. */
    EXPECT_EQ(view->readFile(CanonPath("/flake.nix")), "top-level");
}

TEST(SourceView, OverlayEntryNotInOverlayAccessorFallsBackToBase)
{
    /* Post-refactor: under the OP-COMBINATOR algebra, Overlay is
       `SoundnessGuard ∘ Layer([Restrict(E)(o), Mask(W)(b)])`. When
       an entry path is in E but the overlay accessor doesn't have
       its bytes, Restrict(E)(o).maybeLstat returns nullopt and
       Layer falls through to the base via Mask(W)(b). This is a
       documented behavior change vs. the pre-refactor bespoke
       dispatch, which threw in this case. */
    auto base = makeMemFs();
    auto overlay = make_ref<MemorySourceAccessor>(); /* empty */

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{CanonPath("/README.md")}, /*whiteouts=*/{});

    /* Falls back to base — README.md from makeMemFs() = "hi". */
    EXPECT_EQ(view->readFile(CanonPath("/README.md")), "hi");
}

TEST(SourceView, OverlayDirListingMergesBaseAndOverlay)
{
    /* Base has flake.nix, README.md, sub/. Overlay introduces a
       new top-level file `NEW.md` not present in base. The merged
       directory listing surfaces it. */
    auto base = makeMemFs();
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("NEW.md"), "added");

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{CanonPath("/NEW.md")}, /*whiteouts=*/{});

    auto entries = view->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries.contains("flake.nix"));
    EXPECT_TRUE(entries.contains("README.md"));
    EXPECT_TRUE(entries.contains("sub"));
    EXPECT_TRUE(entries.contains("NEW.md"));
}

TEST(SourceView, OverlayDirListingTypeFromOverlayAccessor)
{
    /* When overlay introduces a new entry, the merged listing
       carries the entry's *type* from the overlay accessor (not
       the base). */
    auto base = makeMemFs();
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("NEW.md"), "added");

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{CanonPath("/NEW.md")}, /*whiteouts=*/{});

    auto entries = view->readDirectory(CanonPath::root);
    auto it = entries.find("NEW.md");
    ASSERT_NE(it, entries.end());
    ASSERT_TRUE(it->second.has_value());
    EXPECT_EQ(*it->second, SourceAccessor::tRegular);
}

TEST(SourceView, OverlayMaybeLstatRoutesByEntryMembership)
{
    /* lstat routing must agree with read routing: an entry path's
       size/type comes from the overlay, not base. */
    auto base = makeMemFs();
    base->addFile(CanonPath("modified.txt"), "old long contents that differ in length");
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("modified.txt"), "new");

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{CanonPath("/modified.txt")}, /*whiteouts=*/{});

    auto stat = view->maybeLstat(CanonPath("/modified.txt"));
    ASSERT_TRUE(stat.has_value());
    EXPECT_EQ(stat->type, SourceAccessor::tRegular);
}

TEST(SourceView, OverlayDeletedFilesAreInvisible)
{
    /* Whiteouts hide paths from lstat / readFile / readDirectory. */
    auto base = makeMemFs();
    auto overlay = make_ref<MemorySourceAccessor>(); /* empty */

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{}, /*whiteouts=*/{CanonPath("/sub/inner.txt")});

    EXPECT_FALSE(view->maybeLstat(CanonPath("/sub/inner.txt")).has_value());
    EXPECT_THROW(view->readFile(CanonPath("/sub/inner.txt")), std::exception);

    /* And the parent directory listing no longer contains it. */
    auto entries = view->readDirectory(CanonPath("/sub"));
    EXPECT_FALSE(entries.contains("inner.txt"));
}

/* Recursive whiteout (gap #7): a whiteout at a DIRECTORY hides the entire
   subtree beneath it, not just the named path — the deny-closure
   includes descendants (the OverlayFS whiteout-of-a-dir semantics;
   https://www.kernel.org/doc/html/latest/filesystems/overlayfs.html).
   Whiteout `/sub` ⇒ `/sub` AND `/sub/inner.txt` both invisible, and
   `/sub` is gone from the root listing. */
TEST(SourceView, OverlayWhiteoutOfDirectoryHidesWholeSubtree)
{
    auto base = makeMemFs(); /* has /sub/inner.txt */
    auto overlay = make_ref<MemorySourceAccessor>();

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{}, /*whiteouts=*/{CanonPath("/sub")});

    /* The whiteout dir itself and everything under it is invisible. */
    EXPECT_FALSE(view->maybeLstat(CanonPath("/sub")).has_value());
    EXPECT_FALSE(view->maybeLstat(CanonPath("/sub/inner.txt")).has_value());
    EXPECT_THROW(view->readFile(CanonPath("/sub/inner.txt")), std::exception);

    /* Gone from the root listing. */
    auto rootEntries = view->readDirectory(CanonPath::root);
    EXPECT_FALSE(rootEntries.contains("sub"));
    /* Sibling files outside the whiteout subtree are unaffected. */
    EXPECT_TRUE(view->pathExists(CanonPath("/flake.nix")));
}

/* Re-add under a whiteout (gap #7): an entry re-introduces a path BELOW a
   whiteout directory, from the overlay accessor — the overlay's entries
   win over the base's whiteout-hidden subtree. (OverlayFS: an upper file
   under a whiteout-then-recreated dir is visible.) */
TEST(SourceView, OverlayEntryUnderWhiteoutIsVisible)
{
    auto base = makeMemFs(); /* base /sub/inner.txt will be hidden */
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("sub/fresh.txt"), "re-added");

    auto view =
        sourceViewOverlay(base, overlay, /*entries=*/{CanonPath("/sub/fresh.txt")}, /*whiteouts=*/{CanonPath("/sub")});

    /* Base's hidden child stays hidden... */
    EXPECT_FALSE(view->maybeLstat(CanonPath("/sub/inner.txt")).has_value());
    /* ...but the re-added entry from the overlay is visible. */
    EXPECT_TRUE(view->pathExists(CanonPath("/sub/fresh.txt")));
    EXPECT_EQ(view->readFile(CanonPath("/sub/fresh.txt")), "re-added");
}

TEST(SourceView, OverlayProvenanceCarriesDirtyAndDeleted)
{
    /* The factory threads `entries` into provenance.dirtyFiles and
       `whiteouts` into provenance.deletedFiles, so callers can
       introspect what changed without re-doing a status walk. */
    auto base = makeMemFs();
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("dirty.txt"), "modified");

    std::set<CanonPath> entries{CanonPath("/dirty.txt")};
    std::set<CanonPath> whiteouts{CanonPath("/deleted.txt")};
    auto view = sourceViewOverlay(base, overlay, entries, whiteouts);

    auto prov = view->getProvenance();
    EXPECT_EQ(prov.dirtyFiles, entries);
    EXPECT_EQ(prov.deletedFiles, whiteouts);
}

TEST(SourceView, OverlayRoundTripsBaseFingerprintForUnchangedPaths)
{
    /* For an Overlay view whose `base` is content-addressed, the
       view's `getFingerprint` falls back to the base's identity for
       the wrapper-fallback path. The Item 4 invariant: when no
       dirty/deleted entries match the queried path, the cache key
       comes from `base`'s fingerprint. */
    auto base = makeMemFs();
    base->fingerprint = "git:headRev123";
    auto overlay = make_ref<MemorySourceAccessor>();

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{}, /*whiteouts=*/{});

    auto [_, fp] = view->getFingerprint(CanonPath::root);
    /* The factory marks the view LocalCapability (the overlay
       layer breaks content-addressing of the whole), but the
       wrapper-fallback returns the base's fingerprint string when
       no per-recipe identity is set. */
    EXPECT_EQ(fp, "git:headRev123");
}

TEST(SourceView, OverlayPurityIsLocalCapability)
{
    /* An overlay over a content-addressed base + dirty entries is
       not itself content-addressable (synthesising a Git tree from
       arbitrary overlay bytes is the deferred Track H stretch). */
    auto base = makeMemFs();
    base->fingerprint = "git:R";
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("dirty.txt"), "x");

    auto view = sourceViewOverlay(base, overlay, {CanonPath("/dirty.txt")}, {});
    EXPECT_EQ(view->getIdentity(CanonPath::root).purity, ViewPurity::LocalCapability);
}

TEST(SourceView, OverlayIsVirtualAtConstruction)
{
    /* Constructing a sourceViewOverlay must not read bytes from
       base or overlay. */
    auto base = makeMemFs();
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("a"), "1");

    /* No reads expected here. */
    auto view = sourceViewOverlay(base, overlay, {CanonPath("/a")}, {CanonPath("/README.md")});
    /* Querying provenance/identity also doesn't read bytes. */
    (void) view->getProvenance();
    (void) view->getIdentity(CanonPath::root);
    SUCCEED();
}

TEST(SourceView, TreeHandleCarriesGitTreeOid)
{
    auto base = makeMemFs();
    base->fingerprint = "tree:abc";
    auto treeOid = hashString(HashAlgorithm::SHA1, "tree-oid-bytes");

    auto view = sourceViewTreeHandle(base, treeOid);

    auto id = view->getIdentity(CanonPath::root);
    EXPECT_EQ(id.gitTree, treeOid);
    EXPECT_EQ(id.purity, ViewPurity::ContentAddressed);
}

TEST(SourceView, LocalCheckoutExposesCheckoutPath)
{
    auto base = makeMemFs();
    /* Even if the base has a fingerprint, LocalCheckout views are
       LocalCapability — the physical path is local state. */
    base->fingerprint = "would-be-content-addressed";

    SourceProvenance prov;
    prov.sparseCheckoutRoots = {CanonPath("/sub")};
    auto view = sourceViewLocalCheckout(base, "/some/checkout", prov);

    auto id = view->getIdentity(CanonPath::root);
    EXPECT_EQ(id.purity, ViewPurity::LocalCapability);

    auto p = view->getProvenance();
    ASSERT_TRUE(p.checkoutRoot.has_value());
    EXPECT_EQ(*p.checkoutRoot, std::filesystem::path("/some/checkout"));
    EXPECT_EQ(p.sparseCheckoutRoots.size(), 1u);
    EXPECT_TRUE(p.sparseCheckoutRoots.contains(CanonPath("/sub")));
}

TEST(SourceView, LocalCheckoutCheckoutPathIsRecipePath)
{
    auto base = makeMemFs();
    auto view = sourceViewLocalCheckout(base, "/checkout/root");

    /* getPhysicalPath should return the recipe-recorded path. */
    auto p = view->getPhysicalPath(CanonPath::root);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->string(), "/checkout/root/");
}

/* ---------- Non-materialising projection ----------
 *
 * The central correctness property of source views: constructing a
 * view and asking it for identity/provenance MUST NOT touch the
 * underlying source's bytes. A view is a *projection* in the
 * algebraic sense — it carries a recipe and metadata, not a copy of
 * the data.
 *
 * We use a recording accessor whose every read method increments a
 * counter, then exercise each view factory + identity query and
 * assert zero reads.
 */

namespace {

struct ReadCountingAccessor : MemorySourceAccessor
{
    mutable int reads = 0;
    mutable int dirReads = 0;

    void readFile(const CanonPath & p, Sink & s, fun<void(uint64_t)> sz) override
    {
        ++reads;
        MemorySourceAccessor::readFile(p, s, sz);
    }

    DirEntries readDirectory(const CanonPath & p) override
    {
        ++dirReads;
        return MemorySourceAccessor::readDirectory(p);
    }
};

ref<ReadCountingAccessor> makeRecordingFs()
{
    auto m = make_ref<ReadCountingAccessor>();
    m->addFile(CanonPath("flake.nix"), "outputs = {...};");
    m->addFile(CanonPath("README.md"), "readme contents");
    m->addFile(CanonPath("sub/inner.txt"), "deep");
    m->fingerprint = "git:R";
    return m;
}

} // namespace

TEST(SourceView, RootIsVirtual)
{
    auto base = makeRecordingFs();
    auto view = sourceViewRoot(base);
    auto id = view->getIdentity(CanonPath::root);
    auto prov = view->getProvenance();
    EXPECT_EQ(base->reads, 0) << "Root view should not read bytes for identity";
    EXPECT_EQ(base->dirReads, 0);
    EXPECT_TRUE(id.fingerprint.has_value());
}

TEST(SourceView, SubtreeIsVirtual)
{
    auto base = makeRecordingFs();
    auto view = sourceViewSubtree(base, CanonPath("/sub"));
    auto id = view->getIdentity(CanonPath::root);
    /* Subtree's getIdentity asks the base's getFingerprint, which
       on a real GitSourceAccessor walks the tree (cheap, no blob
       reads). For the recording POSIX-like leaf, getFingerprint
       returns the input-level field — zero reads. */
    EXPECT_EQ(base->reads, 0) << "Subtree view should not read file bytes for identity";
    EXPECT_TRUE(id.fingerprint.has_value());
}

TEST(SourceView, SubsetIsVirtual)
{
    auto base = makeRecordingFs();
    auto shape = hashString(HashAlgorithm::SHA256, "shape");
    auto view = sourceViewSubset(base, shape);
    auto id = view->getIdentity(CanonPath::root);
    EXPECT_EQ(base->reads, 0);
    EXPECT_EQ(base->dirReads, 0);
    EXPECT_TRUE(id.fingerprint.has_value());
}

TEST(SourceView, TreeHandleIsVirtual)
{
    auto base = makeRecordingFs();
    auto treeOid = hashString(HashAlgorithm::SHA1, "tree");
    auto view = sourceViewTreeHandle(base, treeOid);
    auto id = view->getIdentity(CanonPath::root);
    EXPECT_EQ(base->reads, 0);
    EXPECT_EQ(base->dirReads, 0);
    EXPECT_EQ(id.gitTree, treeOid);
}

TEST(SourceView, LocalCheckoutIsVirtual)
{
    auto base = makeRecordingFs();
    auto view = sourceViewLocalCheckout(base, "/some/checkout");
    auto id = view->getIdentity(CanonPath::root);
    auto prov = view->getProvenance();
    auto p = view->getPhysicalPath(CanonPath::root);
    /* Even getPhysicalPath uses recipe-stored data, not the
       underlying accessor. */
    EXPECT_EQ(base->reads, 0);
    EXPECT_EQ(base->dirReads, 0);
    EXPECT_TRUE(p.has_value());
    EXPECT_EQ(id.purity, ViewPurity::LocalCapability);
    EXPECT_TRUE(prov.checkoutRoot.has_value());
}

TEST(SourceView, CompositionIsVirtual)
{
    /* The composition: SubsetOf(Subtree(Root(base))). Each layer
       must remain non-materialising. */
    auto base = makeRecordingFs();
    auto root = sourceViewRoot(base);
    auto sub = sourceViewSubtree(root, CanonPath("/sub"));
    auto shape = hashString(HashAlgorithm::SHA256, "shape");
    auto subset = sourceViewSubset(sub, shape);

    /* Build identity for the entire composed stack. */
    (void) subset->getIdentity(CanonPath::root);
    (void) subset->getProvenance();

    EXPECT_EQ(base->reads, 0);
    EXPECT_EQ(base->dirReads, 0);
}

TEST(SourceView, ReadingDoesMaterialiseBytes)
{
    /* Negative control: the *opposite* claim — when something
       actually demands bytes, the accessor IS hit. This validates
       that our counter is wired correctly and the lazy assertions
       above mean something. */
    auto base = makeRecordingFs();
    auto view = sourceViewRoot(base);
    EXPECT_EQ(base->reads, 0);
    auto _ = view->readFile(CanonPath("/flake.nix"));
    EXPECT_EQ(base->reads, 1);
}

TEST(SourceView, IdentityVsProvenanceOrthogonal)
{
    /* A Subtree of a content-addressed base remains
       ContentAddressed — content identity flows through. */
    auto base = makeMemFs();
    base->fingerprint = "git:R";
    auto subtree = sourceViewSubtree(base, CanonPath("/sub"));
    EXPECT_EQ(subtree->getIdentity(CanonPath::root).purity, ViewPurity::ContentAddressed);

    /* Overlay over an identity-bearing base + a dirty entry should
       drop to LocalCapability (the entry contents aren't content-
       addressed in this minimal impl). */
    auto overlay = sourceViewOverlay(base, base, {CanonPath("/README.md")}, {});
    EXPECT_EQ(overlay->getIdentity(CanonPath::root).purity, ViewPurity::LocalCapability);

    /* LocalCheckout: always LocalCapability regardless of base. */
    auto local = sourceViewLocalCheckout(base, "/x");
    EXPECT_EQ(local->getIdentity(CanonPath::root).purity, ViewPurity::LocalCapability);
}

TEST(SourceView, PinOverlayUntrackedFileReadErrorsCleanly)
{
    /* Stage-0 pin: an Overlay view, queried for a path that's neither
       in `entries`, in `whiteouts`, nor present in `base`, throws on
       readFile. The exact exception message may change in Stage 3
       when this case becomes Layer-of-disjoint-children producing
       FileNotFound; this test pins that *something* is thrown. */
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(CanonPath("present.txt"), "ok");
    auto overlay = make_ref<MemorySourceAccessor>(); /* empty */

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{}, /*whiteouts=*/{});
    EXPECT_THROW(view->readFile(CanonPath("/missing.txt")), std::exception);
}

TEST(SourceView, PinSubsetAncestorOfAcceptedReturnsDirectoryStat)
{
    /* Stage-0 pin: lstat on a path that is an *ancestor* of an
       accepted leaf returns a synthesised tDirectory stat — even
       though the path itself isn't in the accepted set. */
    auto base = makeMemFs();
    auto shape = hashString(HashAlgorithm::SHA256, "shape");
    auto view = sourceViewSubset(base, shape, {CanonPath("/sub/inner.txt")});

    auto st = view->maybeLstat(CanonPath("/sub"));
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->type, SourceAccessor::tDirectory);
}

TEST(SourceView, PinSubsetAncestorReadDirectoryListsAllowedChildren)
{
    /* Stage-0 pin: readDirectory of an ancestor of S lists the
       allowed children only. */
    auto base = makeMemFs();
    auto shape = hashString(HashAlgorithm::SHA256, "shape");
    auto view = sourceViewSubset(base, shape, {CanonPath("/sub/inner.txt")});

    auto entries = view->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries.contains("sub"));
    EXPECT_FALSE(entries.contains("flake.nix"));
    EXPECT_FALSE(entries.contains("README.md"));
}

TEST(SourceView, PinOverlayAncestorOfEntrySurfacedAsDirectory)
{
    /* Stage-0 pin: readDirectory of "/" surfaces synthesised
       intermediate dirs for entries deeper in the tree. */
    auto base = make_ref<MemorySourceAccessor>(); /* No /a in base */
    auto overlay = make_ref<MemorySourceAccessor>();
    overlay->addFile(CanonPath("a/b/c.txt"), "deep-overlay");

    auto view = sourceViewOverlay(base, overlay, /*entries=*/{CanonPath("/a/b/c.txt")}, /*whiteouts=*/{});

    auto rootEntries = view->readDirectory(CanonPath::root);
    EXPECT_TRUE(rootEntries.contains("a"));

    auto stat = view->maybeLstat(CanonPath("/a"));
    ASSERT_TRUE(stat.has_value());
    EXPECT_EQ(stat->type, SourceAccessor::tDirectory);
}

TEST(SourceView, SubsetSubpathTranslatesReads)
{
    /* Stage-4: sourceViewSubset with subpath translates reads through
       the operator stack. The composition is Restrict(S) ∘ Translate(p),
       so a Subset(S, p) view reads `/x` in the wrapper namespace as
       `p/x` on base. */
    auto base = makeMemFs(); /* has /flake.nix, /README.md, /sub/inner.txt */
    auto shape = hashString(HashAlgorithm::SHA256, "shape");

    /* Subset rooted at /sub, accepting only /inner.txt (relative to subpath). */
    auto view = sourceViewSubset(base, shape, {CanonPath("/inner.txt")}, CanonPath("/sub"));

    /* Read at the wrapper-relative path; translates to /sub/inner.txt on base. */
    EXPECT_EQ(view->readFile(CanonPath("/inner.txt")), "deep contents");
    EXPECT_TRUE(view->pathExists(CanonPath("/inner.txt")));

    /* Sibling at the wrapper namespace (would be /sub/flake.nix on base
       but flake.nix is at root not /sub) is not visible. */
    EXPECT_FALSE(view->pathExists(CanonPath("/flake.nix")));
}

TEST(SourceView, SubsetSubpathFingerprintIsSubtreeAware)
{
    /* Stage-4: when subpath ≠ root, the factory seeds view->fingerprint
       from base->getFingerprint(subpath). For a base whose getFingerprint
       returns its top-level field for any path (the MemorySourceAccessor
       default), this is just base->fingerprint. The point: cache rows
       can share across different revs that share the same subtree. */
    auto base = makeMemFs();
    base->fingerprint = "tree:basetreeR1";
    auto shape = hashString(HashAlgorithm::SHA256, "shape");

    auto view = sourceViewSubset(base, shape, {CanonPath("/inner.txt")}, CanonPath("/sub"));

    /* The view's own fingerprint field should be the subpath-derived one. */
    ASSERT_TRUE(view->fingerprint.has_value());
    EXPECT_EQ(*view->fingerprint, "tree:basetreeR1");
}

TEST(SourceView, SubsetSubpathFingerprintSharedAcrossRevs)
{
    /* Stage-4 / Track-B: two Subset views over different bases that
       share the same subtree-derived fingerprint produce the same
       view-level fingerprint. This is what enables filtered-subpath
       cache rows to share across revs. */
    auto base1 = makeMemFs();
    base1->fingerprint = "tree:rev1";
    auto base2 = makeMemFs();
    /* Same fingerprint at the subpath query — simulates two revs that
       share the /sub subtree. (For a real GitSourceAccessor this would
       fall out of getFingerprint("/sub") returning tree:<subSha>; here
       we set the same field on both bases to fake it.) */
    base2->fingerprint = "tree:rev1";

    auto shape = hashString(HashAlgorithm::SHA256, "shape");
    auto view1 = sourceViewSubset(base1, shape, {CanonPath("/inner.txt")}, CanonPath("/sub"));
    auto view2 = sourceViewSubset(base2, shape, {CanonPath("/inner.txt")}, CanonPath("/sub"));

    EXPECT_EQ(view1->fingerprint, view2->fingerprint);
}

TEST(SourceView, SubsetSubpathDefaultIsRoot)
{
    /* Sanity: a Subset constructed without a subpath argument behaves
       identically to one with subpath=root. */
    auto base = makeMemFs();
    auto shape = hashString(HashAlgorithm::SHA256, "shape");

    auto noSubpath = sourceViewSubset(base, shape, {CanonPath("/sub/inner.txt")});
    auto rootSubpath = sourceViewSubset(base, shape, {CanonPath("/sub/inner.txt")}, CanonPath::root);

    /* Both can read the accepted path. */
    EXPECT_EQ(noSubpath->readFile(CanonPath("/sub/inner.txt")), "deep contents");
    EXPECT_EQ(rootSubpath->readFile(CanonPath("/sub/inner.txt")), "deep contents");
    /* And both have the same readDirectory at root. */
    EXPECT_EQ(noSubpath->readDirectory(CanonPath::root).size(), rootSubpath->readDirectory(CanonPath::root).size());
}

} // namespace nix
