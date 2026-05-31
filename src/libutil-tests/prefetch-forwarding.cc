/* Track K: prefetchSubtree forwarding through wrapper accessors.
 *
 * The base `SourceAccessor::prefetchSubtree` is a no-op. Wrappers
 * (Mounted, Filtering, Union, Caching) override to forward into
 * their wrapped accessors. We use a recording mock leaf to verify
 * each wrapper translates paths correctly and reaches the inner
 * accessor exactly once per call.
 */

#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"

#include <gtest/gtest.h>

#include <vector>

namespace nix {

/* Records every prefetchSubtree call. Inherits MemorySourceAccessor
   so it can claim paths via maybeLstat (Union dispatch needs that). */
struct RecordingLeaf : MemorySourceAccessor
{
    struct Call
    {
        CanonPath path;
        unsigned depth;
    };

    std::vector<Call> calls;

    void prefetchSubtree(const CanonPath & subpath, unsigned depth) override
    {
        calls.push_back({subpath, depth});
    }
};

ref<RecordingLeaf> makeRecordingLeaf()
{
    auto leaf = make_ref<RecordingLeaf>();
    /* Make root a readable directory so wrapper dispatch can find it. */
    leaf->root = MemorySourceAccessor::File::Directory{};
    return leaf;
}

TEST(PrefetchForwarding, BaseClassIsNoop)
{
    /* Plain MemorySourceAccessor doesn't override prefetchSubtree;
       calling it must not throw and must not do anything observable. */
    auto plain = make_ref<MemorySourceAccessor>();
    EXPECT_NO_THROW(plain->prefetchSubtree(CanonPath::root, 1));
    EXPECT_NO_THROW(plain->prefetchSubtree(CanonPath("/x"), 7));
}

TEST(PrefetchForwarding, MountedDispatchesToInnerWithSubpath)
{
    auto leaf = makeRecordingLeaf();
    auto root = makeRecordingLeaf();
    auto mounted = makeMountedSourceAccessor({
        {CanonPath::root, root},
        {CanonPath("/sub"), leaf},
    });

    mounted->prefetchSubtree(CanonPath("/sub/path/in/leaf"), 2);

    /* Mount key is /sub; resolved subpath should be /path/in/leaf,
       sent to `leaf` (not `root`). */
    ASSERT_EQ(leaf->calls.size(), 1u);
    EXPECT_EQ(leaf->calls[0].path, CanonPath("/path/in/leaf"));
    EXPECT_EQ(leaf->calls[0].depth, 2u);
    EXPECT_TRUE(root->calls.empty());
}

TEST(PrefetchForwarding, MountedRoutesToRootWhenNoChildClaims)
{
    auto root = makeRecordingLeaf();
    auto mounted = makeMountedSourceAccessor({{CanonPath::root, root}});

    mounted->prefetchSubtree(CanonPath("/anywhere/at/all"), 1);

    /* Single root mount: everything routes there with full path. */
    ASSERT_EQ(root->calls.size(), 1u);
    EXPECT_EQ(root->calls[0].path, CanonPath("/anywhere/at/all"));
}

TEST(PrefetchForwarding, DefaultDepthIsOne)
{
    auto root = makeRecordingLeaf();
    auto mounted = makeMountedSourceAccessor({{CanonPath::root, root}});

    /* Calling without explicit depth — interface default is depth=1. */
    mounted->prefetchSubtree(CanonPath::root);

    ASSERT_EQ(root->calls.size(), 1u);
    EXPECT_EQ(root->calls[0].depth, 1u);
}

/* CR-3 fan-out on the MUTABLE Switch (MountedSourceAccessor — the
   concurrent_flat_map `forEachMountUnder`). This is the PRODUCTION
   git-submodule path (git.cc mounts submodules via makeSwitch/Mounted);
   the existing fan-out tests in switch-source-accessor.cc cover only the
   immutable twin, so the concurrent-map override was untested. An
   exhaustive root prefetch must reach every strictly-descendant mount at
   its own root. */
TEST(PrefetchForwarding, MountedExhaustivePrefetchFansOutToDescendantMounts)
{
    auto root = makeRecordingLeaf();
    auto subA = makeRecordingLeaf();
    auto subB = makeRecordingLeaf();
    auto mounted = makeMountedSourceAccessor({
        {CanonPath::root, root},
        {CanonPath("/modules/a"), subA},
        {CanonPath("/modules/b"), subB},
    });

    mounted->prefetchSubtree(CanonPath::root, 7);

    /* Root mount owns root → one call at its own root. */
    ASSERT_EQ(root->calls.size(), 1u);
    EXPECT_EQ(root->calls[0].path, CanonPath::root);
    /* BOTH sub-mounts primed from their own roots (the fan-out). */
    ASSERT_EQ(subA->calls.size(), 1u);
    EXPECT_EQ(subA->calls[0].path, CanonPath::root);
    EXPECT_EQ(subA->calls[0].depth, 7u);
    ASSERT_EQ(subB->calls.size(), 1u);
    EXPECT_EQ(subB->calls[0].path, CanonPath::root);
}

/* A narrow prefetch under one mount must NOT fan out to siblings (only
   the owning mount is touched). */
TEST(PrefetchForwarding, MountedNarrowPrefetchDoesNotFanOut)
{
    auto root = makeRecordingLeaf();
    auto subA = makeRecordingLeaf();
    auto mounted = makeMountedSourceAccessor({
        {CanonPath::root, root},
        {CanonPath("/modules/a"), subA},
    });

    /* A path served entirely by root, disjoint from /modules/a. */
    mounted->prefetchSubtree(CanonPath("/other"), 2);
    ASSERT_EQ(root->calls.size(), 1u);
    EXPECT_EQ(root->calls[0].path, CanonPath("/other"));
    EXPECT_TRUE(subA->calls.empty());
}

/* `mount()` is insert_or_assign: re-mounting a key replaces the prior
   accessor (the Item-2 re-mint-after-resetFileCache invariant the source
   comment calls out). Reads must see the second accessor, not the first. */
TEST(PrefetchForwarding, MountReMountReplacesAccessor)
{
    auto first = makeRecordingLeaf();
    auto second = makeRecordingLeaf();
    auto mounted = makeMountedSourceAccessor({{CanonPath::root, makeRecordingLeaf()}});

    mounted->mount(CanonPath("/k"), first);
    mounted->prefetchSubtree(CanonPath("/k/x"), 1);
    ASSERT_EQ(first->calls.size(), 1u);

    /* Re-mount the same key with a different accessor. */
    mounted->mount(CanonPath("/k"), second);
    mounted->prefetchSubtree(CanonPath("/k/y"), 1);
    /* Second wins; first is not called again. */
    EXPECT_EQ(first->calls.size(), 1u);
    ASSERT_EQ(second->calls.size(), 1u);
    EXPECT_EQ(second->calls[0].path, CanonPath("/y"));
}

} // namespace nix
