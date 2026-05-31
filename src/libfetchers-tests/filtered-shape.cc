/* Track D: filtered-shape cache.
 *
 * The walker `collectFilteredShape` is exposed via the testing
 * header `nix/fetchers/filtered-shape.hh`. We test:
 *
 *   - `FilteredSourceKey` encoding: same key bytes ⇒ same row.
 *   - Walker behaviour: the root is included unconditionally; the
 *     filter is consulted only on children; symlinks are recorded by
 *     target string and never recursed through (a critical
 *     soundness property — a symlink pointing outside the source
 *     root must not let the walker escape).
 */

#include "nix/fetchers/filtered-shape.hh"
#include "nix/fetchers/projection.hh"
#include "nix/util/hash.hh"
#include "nix/util/memory-source-accessor.hh"

#include <gtest/gtest.h>

namespace nix::fetchers {

TEST(FilteredShape, EqualKeyEqualEncoding)
{
    /* Soundness invariant: same source, same method, same root,
       same shape → same cache row. */
    FilteredSourceKey a{
        .sourceFingerprint = "git:abcdef",
        .method = "nar",
        .subpath = "/",
        .shapeHash = "sha256-XXXX",
    };
    FilteredSourceKey b = a;

    EXPECT_EQ(FilteredSourcePathToHash::toKey(a), FilteredSourcePathToHash::toKey(b));
}

TEST(FilteredShape, DifferentShapeDifferentKey)
{
    FilteredSourceKey a{
        .sourceFingerprint = "git:R",
        .method = "nar",
        .subpath = "/",
        .shapeHash = "sha256-AAA",
    };
    FilteredSourceKey b = a;
    b.shapeHash = "sha256-BBB";

    EXPECT_NE(FilteredSourcePathToHash::toKey(a), FilteredSourcePathToHash::toKey(b));
}

TEST(FilteredShape, DifferentSourceDifferentKey)
{
    FilteredSourceKey a{
        .sourceFingerprint = "git:R1",
        .method = "nar",
        .subpath = "/",
        .shapeHash = "sha256-S",
    };
    FilteredSourceKey b = a;
    b.sourceFingerprint = "git:R2";

    EXPECT_NE(FilteredSourcePathToHash::toKey(a), FilteredSourcePathToHash::toKey(b));
}

TEST(FilteredShape, DifferentSubpathDifferentKey)
{
    FilteredSourceKey a{
        .sourceFingerprint = "git:R",
        .method = "nar",
        .subpath = "/sub",
        .shapeHash = "sha256-S",
    };
    FilteredSourceKey b = a;
    b.subpath = "/other";

    EXPECT_NE(FilteredSourcePathToHash::toKey(a), FilteredSourcePathToHash::toKey(b));
}

TEST(FilteredShape, KeyShapeOrderingIsTotal)
{
    /* The auto-generated `<=>` lets us put FilteredSourceKey in
       std::set / std::map. Verify it works as a key. */
    FilteredSourceKey a{
        .sourceFingerprint = "a",
        .method = "nar",
        .subpath = "/",
        .shapeHash = "sha256-A",
    };
    FilteredSourceKey b{
        .sourceFingerprint = "a",
        .method = "nar",
        .subpath = "/",
        .shapeHash = "sha256-B",
    };
    EXPECT_TRUE(a < b || b < a); // strict weak ordering: not equivalent, must order
}

TEST(FilteredShape, RoundTripValueEncoding)
{
    auto h = hashString(HashAlgorithm::SHA256, "filtered-result");
    auto attrs = FilteredSourcePathToHash::toValue(h);
    auto h2 = FilteredSourcePathToHash::fromValue(attrs);
    EXPECT_EQ(h, h2);
}

} // namespace nix::fetchers

/* ---------- Walker invariants (track D's behavioural surface) ---------- */

namespace nix {

namespace {

/* A MemorySourceAccessor with a tree containing a symlink. We use it
   to verify the walker doesn't recurse through symlinks even when
   the user filter accepts them. */
ref<MemorySourceAccessor> makeFsWithSymlink()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->addFile(CanonPath("regular.txt"), "regular contents");
    m->addFile(CanonPath("dir/inner.txt"), "deep contents");

    /* Add a symlink at /link pointing to "../escape" — outside the
       source root. If the walker followed it, it would try to
       traverse outside. */
    auto * f = m->open(CanonPath("link"), MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"../escape"}});
    (void) f;

    /* And a symlinked directory at /symlinked-dir → "dir". If the
       walker followed it, it would re-enter /dir/inner.txt under a
       different relative path. */
    m->open(CanonPath("symlinked-dir"), MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"dir"}});
    return m;
}

PathFilter acceptAll = [](const std::string &) { return true; };

PathFilter rejectAll = [](const std::string &) { return false; };

} // anonymous namespace

TEST(FilteredShape, WalkerDoesNotRecurseThroughSymlinks)
{
    /* Critical soundness invariant: a symlink — even an accepted
       one, even one pointing outside the source root — must record
       its target string and stop. The accepted set must NOT contain
       any path *under* a symlinked entry (no recursion through). */
    auto fs = makeFsWithSymlink();
    auto shape = collectFilteredShape(*fs, CanonPath::root, acceptAll);

    /* Root and direct children are present. */
    EXPECT_TRUE(shape.accepted.contains(CanonPath::root));
    EXPECT_TRUE(shape.accepted.contains(CanonPath("/regular.txt")));
    EXPECT_TRUE(shape.accepted.contains(CanonPath("/dir")));
    EXPECT_TRUE(shape.accepted.contains(CanonPath("/dir/inner.txt")));
    EXPECT_TRUE(shape.accepted.contains(CanonPath("/link")));
    EXPECT_TRUE(shape.accepted.contains(CanonPath("/symlinked-dir")));

    /* But NO path *under* the symlinked-dir — that would mean the
       walker followed the symlink. */
    EXPECT_FALSE(shape.accepted.contains(CanonPath("/symlinked-dir/inner.txt")));
}

TEST(FilteredShape, RootIsAlwaysAccepted)
{
    /* PROPOSAL.md §5: "the root is included unconditionally; the
       filter is called only on child entries." A reject-all filter
       still produces a one-element accepted set with the root. */
    auto fs = makeFsWithSymlink();
    auto shape = collectFilteredShape(*fs, CanonPath::root, rejectAll);

    EXPECT_TRUE(shape.accepted.contains(CanonPath::root));
    EXPECT_EQ(shape.accepted.size(), 1u);
}

TEST(FilteredShape, DifferentAcceptedSetsHashDifferently)
{
    /* Two filter runs that produce different accepted sets must
       produce different shape hashes. */
    auto fs = makeFsWithSymlink();
    auto a = collectFilteredShape(*fs, CanonPath::root, acceptAll);

    PathFilter onlyRegular = [](const std::string & p) { return p == "/regular.txt"; };
    auto b = collectFilteredShape(*fs, CanonPath::root, onlyRegular);
    EXPECT_NE(a.shapeHash, b.shapeHash);
}

TEST(FilteredShape, IdenticalAcceptedSetsHashIdentically)
{
    /* The §5 soundness law: two filter functions that produce the
       same accepted set against the same source produce the same
       shape hash. The cache row will share. */
    auto fs1 = makeFsWithSymlink();
    auto fs2 = makeFsWithSymlink();

    PathFilter f1 = [](const std::string &) { return true; };
    PathFilter f2 = [](const std::string & p) {
        /* Different lambda body, same result for our test tree. */
        return p.size() > 0;
    };
    auto a = collectFilteredShape(*fs1, CanonPath::root, f1);
    auto b = collectFilteredShape(*fs2, CanonPath::root, f2);
    EXPECT_EQ(a.shapeHash, b.shapeHash);
    EXPECT_EQ(a.accepted, b.accepted);
}

TEST(FilteredShape, WalkerRunsOncePerCall)
{
    /* Sanity check: one collectFilteredShape call invokes the
       filter exactly once per accepted child (and not on the
       root). If the walker accidentally double-walks, this
       count diverges. */
    auto fs = makeFsWithSymlink();
    int filterCalls = 0;
    PathFilter counting = [&](const std::string &) {
        ++filterCalls;
        return true;
    };
    auto shape = collectFilteredShape(*fs, CanonPath::root, counting);
    /* Tree:
         /regular.txt        (root child)
         /dir                (root child)
         /dir/inner.txt      (dir child)
         /link               (root child)
         /symlinked-dir      (root child)
       Filter is called on each non-root entry exactly once = 5
       calls. (Note `/symlinked-dir` is a symlink; the walker
       records its target without recursing.) */
    EXPECT_EQ(filterCalls, 5);
    /* And the accepted set agrees. */
    EXPECT_EQ(shape.accepted.size(), 6u); // root + 5 children
}

TEST(FilteredShape, FilterReceivesAbsolutePath)
{
    /* The walker must call the user filter with the absolute path
       on the source accessor (master's contract), not a
       relative-to-root path. A regression that passed `/foo` for a
       child of `/some/source/foo` would break user filters that
       inspect path components. */
    auto fs = makeFsWithSymlink();
    std::vector<std::string> seen;
    PathFilter spy = [&](const std::string & p) {
        seen.push_back(p);
        return true;
    };
    /* When called with root = /, child paths are /regular.txt etc. */
    collectFilteredShape(*fs, CanonPath::root, spy);
    /* Verify we saw absolute paths (start with /). */
    for (auto & p : seen)
        EXPECT_TRUE(p.starts_with("/")) << "filter saw '" << p << "'";
    /* And specifically /regular.txt, not "regular.txt". */
    EXPECT_TRUE(std::find(seen.begin(), seen.end(), "/regular.txt") != seen.end());
}

} // namespace nix
