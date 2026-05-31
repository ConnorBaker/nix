/* Track A: Projection<> CRTP — round-trip lookup/upsert through the
 * cache for each concrete projection. The soundness law (equal key →
 * equal value) is exercised by upserting once and looking up twice.
 *
 * These tests use the real fetcher cache SQLite at
 * `~/.cache/nix/fetcher-cache-v4.sqlite`; tests are isolated by using
 * unique key bytes so cross-run contamination is avoided.
 */

#include "nix/fetchers/projection.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/fingerprint.hh"
#include "nix/util/hash.hh"

#include <atomic>
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <unistd.h>

namespace nix::fetchers {

namespace {

/* Each test creates a fingerprint that is unique across the WHOLE test
 * binary AND across rapid re-runs (`--gtest_repeat`, CI retries) of it.
 *
 * The fetcher cache is a real on-disk SQLite at
 * `~/.cache/nix/fetcher-cache-v4.sqlite` that PERSISTS between runs, so a
 * key must not collide with a row written by a previous process. The
 * previous `time(nullptr)` scheme had 1-second granularity: two runs (or
 * two `--gtest_repeat` iterations) in the same second reused the same key,
 * found the warm row, and broke the `computeCalls == 1` / `peek == nullopt`
 * assertions. We now fold in the PID and a per-process atomic counter so
 * every call is genuinely unique (the timestamp is kept only to separate
 * successive *processes* that might recycle a PID). */
Hash uniqueRev(std::string_view tag)
{
    static std::atomic<uint64_t> counter{0};
    return hashString(
        HashAlgorithm::SHA1,
        fmt("nix-projection-test::%s::%lld::%d::%llu",
            tag,
            (long long) time(nullptr),
            (int) getpid(),
            (unsigned long long) counter.fetch_add(1)));
}

const Settings & testSettings()
{
    static Settings s;
    return s;
}

} // namespace

/* ---------- TreeHashToNarHash ---------- */

TEST(Projection, TreeHashToNarHashRoundTrip)
{
    auto rev = uniqueRev("treehash");
    auto narHash = hashString(HashAlgorithm::SHA256, "nar-bytes-stand-in");

    /* Miss → compute → upsert. */
    int computeCalls = 0;
    auto first = TreeHashToNarHash::lookup(testSettings(), rev, [&] {
        ++computeCalls;
        return narHash;
    });
    EXPECT_EQ(first, narHash);
    EXPECT_EQ(computeCalls, 1);

    /* Hit → no compute. */
    auto second = TreeHashToNarHash::lookup(testSettings(), rev, [&] {
        ++computeCalls;
        return Hash::dummy; // would-be-wrong if called
    });
    EXPECT_EQ(second, narHash);
    EXPECT_EQ(computeCalls, 1);
}

TEST(Projection, TreeHashToNarHashPeekDoesNotPopulate)
{
    auto rev = uniqueRev("peek-no-write");

    /* `peek` should never compute or upsert. */
    auto a = TreeHashToNarHash::peek(testSettings(), rev);
    EXPECT_FALSE(a.has_value());

    /* Still missing after peek. */
    auto b = TreeHashToNarHash::peek(testSettings(), rev);
    EXPECT_FALSE(b.has_value());
}

TEST(Projection, TreeHashToNarHashCacheIdempotence)
{
    /* NOTE (formerly mis-named `TreeHashToNarHashSoundnessLaw`): this
       tests the Projection-layer CACHE-IDEMPOTENCE property — first
       writer wins, a second lookup with the same key returns the cached
       value and does NOT recompute. It is genuine and load-bearing, but
       it is NOT the content-addressing *soundness* law.

       The real soundness law — "the narHash stored under tree OID T is
       the canonical narHash of the NAR of tree T" — is a property of the
       WRITEBACK DISCIPLINE in `fetchToStore2` (which key it upserts
       under), not of this CRTP. Because the compute fn here returns a
       stand-in hash decoupled from any tree, this test by construction
       CANNOT catch a writeback that poisons the projection with a wrong
       narHash. That soundness is guarded instead by:
         - `SourceView.NarAlteringRecipesDoNotForwardRootTreeHash` +
           `GitFingerprintTest.RootTreeHashSurvivesSourceViewWrapper`
           (`getRootTreeHash` returns the tree OID iff the accessor's NAR
           IS that tree's NAR — so fetchToStore2 never writes a filtered/
           overlaid NAR under the unfiltered tree OID), and
         - `tests/functional/source/whole-input-tree-dedup.sh` (the
           cross-process narHash-equivalence end-to-end). */
    auto rev = uniqueRev("idempotence");
    auto firstHash = hashString(HashAlgorithm::SHA256, "first-content");

    TreeHashToNarHash::lookup(testSettings(), rev, [&] { return firstHash; });

    auto returned = TreeHashToNarHash::lookup(
        testSettings(), rev, [&] { return hashString(HashAlgorithm::SHA256, "different-content"); });
    EXPECT_EQ(returned, firstHash);
}

/* ---------- bareTreeOid: the canonical bridge-eligibility decoder ----------
 *
 * L-BridgeKeyCanonical: a fingerprint is usable as a cross-pipeline
 * content-identity bridge key (treeHashToNarHash) IFF it is a suffix-free
 * `tree:<hex-sha1>`. Any suffix (`;e`/`;l`/`;s`/`;d=`/`;a=`/`;shape=`)
 * means the NAR differs from a vanilla tree dump → NOT bridge-eligible.
 * `bareTreeOid` is the single decoder all consult/writeback sites route
 * through (it previously drifted: one site threw on malformed hex where
 * others returned nullopt). This property pins it against an INDEPENDENT
 * brute-force reference — same-impl-vs-same-impl would be vacuous. */
TEST(Projection, BareTreeOidMatchesBruteForceReference)
{
    auto isHexSha1 = [](std::string_view s) {
        if (s.size() != 40)
            return false;
        for (char c : s)
            if (!std::isxdigit((unsigned char) c))
                return false;
        return true;
    };
    auto reference = [&](std::string_view fp) {
        return fp.starts_with("tree:") && fp.find(';') == std::string_view::npos
               && isHexSha1(fp.substr(std::string_view("tree:").size()));
    };

    /* A real SHA-1 hex to build well-formed inputs from. */
    auto realSha = hashString(HashAlgorithm::SHA1, "a-tree").gitRev();
    std::vector<std::string> cases = {
        "tree:" + realSha,                        // bare, eligible
        "tree:" + realSha + ";e",                 // export-ignore suffix → ineligible
        "tree:" + realSha + ";l",                 // LFS → ineligible
        "tree:" + realSha + ";shape=sha256-AAAA", // filter sentinel → ineligible (gap4)
        "tree:deadbeef",                          // malformed (short) hex → ineligible, must NOT throw
        "tree:nothexatall_zzzz",                  // non-hex → ineligible, must NOT throw
        "git:" + realSha,                         // commit-rev, not a tree → ineligible
        "blob:" + realSha + ";m=100644",          // blob → ineligible
        "",                                       // empty → ineligible
        "tree:",                                  // prefix only → ineligible
    };
    for (auto & fp : cases) {
        bool elig = bareTreeOid(fp).has_value(); // must never throw
        EXPECT_EQ(elig, reference(fp)) << "bridge-eligibility drift on '" << fp << "'";
        if (elig)
            EXPECT_EQ(bareTreeOid(fp)->gitRev(), realSha);
    }
}

#ifndef COVERAGE
/* Property form: over arbitrary strings, the canonical decoder agrees
   with the brute-force reference and NEVER throws (the drift bug was a
   throw on malformed hex). */
RC_GTEST_PROP(Projection, BareTreeOidNeverThrowsAndMatchesReference, (const std::string & fp))
{
    auto isHexSha1 = [](std::string_view s) {
        if (s.size() != 40)
            return false;
        for (char c : s)
            if (!std::isxdigit((unsigned char) c))
                return false;
        return true;
    };
    bool ref = std::string_view(fp).starts_with("tree:") && fp.find(';') == std::string::npos
               && isHexSha1(std::string_view(fp).substr(5));
    std::optional<Hash> got;
    try {
        got = bareTreeOid(fp); // the drift bug threw here on malformed hex
    } catch (...) {
        RC_FAIL("bareTreeOid threw — bridge-eligibility decoder must total to nullopt, never throw");
    }
    RC_ASSERT(got.has_value() == ref);
}
#endif

/* ---------- GitRevCount ---------- */

TEST(Projection, GitRevCountRoundTrip)
{
    auto rev = uniqueRev("revcount");
    auto value = GitRevCount::lookup(testSettings(), rev, [&] { return uint64_t{42}; });
    EXPECT_EQ(value, 42u);
    /* Hit: compute would return 99 but we should still see 42. */
    auto cached = GitRevCount::lookup(testSettings(), rev, [&] { return uint64_t{99}; });
    EXPECT_EQ(cached, 42u);
}

/* ---------- GitLastModified ---------- */

TEST(Projection, GitLastModifiedRoundTrip)
{
    auto rev = uniqueRev("lastmod");
    auto value = GitLastModified::lookup(testSettings(), rev, [&] { return uint64_t{1234567890}; });
    EXPECT_EQ(value, 1234567890u);
}

TEST(Projection, GitLastModifiedHitDoesNotRecompute)
{
    /* The sibling GitRevCount had a hit-no-recompute test but
       GitLastModified did not — a miss→compute test alone would pass
       even if `lookup` always recomputed. Pin the cache-consulted half. */
    auto rev = uniqueRev("lastmod-hit");
    auto first = GitLastModified::lookup(testSettings(), rev, [&] { return uint64_t{1000}; });
    EXPECT_EQ(first, 1000u);
    /* Second lookup: the compute would return 2000, but the cached 1000
       must win. */
    auto cached = GitLastModified::lookup(testSettings(), rev, [&] { return uint64_t{2000}; });
    EXPECT_EQ(cached, 1000u);
}

TEST(Projection, DistinctRevsDistinctRows)
{
    auto rev1 = uniqueRev("distinct-1");
    auto rev2 = uniqueRev("distinct-2");

    GitRevCount::lookup(testSettings(), rev1, [] { return uint64_t{1}; });
    GitRevCount::lookup(testSettings(), rev2, [] { return uint64_t{2}; });

    EXPECT_EQ(*GitRevCount::peek(testSettings(), rev1), 1u);
    EXPECT_EQ(*GitRevCount::peek(testSettings(), rev2), 2u);
}

/* ---------- FilteredSourcePathToHash ---------- */

TEST(Projection, FilteredSourcePathToHashEncoding)
{
    /* Two keys that differ only in shapeHash must produce different
       cache keys. Two keys identical in all four fields must produce
       the same cache key (the soundness invariant for the filtered-
       shape cache). */
    FilteredSourceKey k1{
        .sourceFingerprint = "tree:T",
        .method = "nar",
        .subpath = "/",
        .shapeHash = "sha256-AAAA",
    };
    FilteredSourceKey k2 = k1;
    k2.shapeHash = "sha256-BBBB";

    auto attrs1 = FilteredSourcePathToHash::toKey(k1);
    auto attrs2 = FilteredSourcePathToHash::toKey(k2);
    EXPECT_NE(attrs1, attrs2);

    auto attrs1again = FilteredSourcePathToHash::toKey(k1);
    EXPECT_EQ(attrs1, attrs1again);
}

TEST(Projection, FilteredSourcePathToHashRoundTrip)
{
    /* The only projection whose actual cache lookup/peek/upsert path was
       previously untested (the others all have a round-trip test; this
       one had only toKey/toValue/fromValue-in-isolation). Miss → compute
       → upsert; hit → no recompute; peek never writes. */
    FilteredSourceKey k{
        .sourceFingerprint = "tree:" + uniqueRev("filtered-rt").gitRev(),
        .method = "nar",
        .subpath = "/",
        .shapeHash = "sha256-Zm9v",
    };
    auto hash = hashString(HashAlgorithm::SHA256, "filtered-source-nar");

    EXPECT_FALSE(FilteredSourcePathToHash::peek(testSettings(), k).has_value());

    int computeCalls = 0;
    auto first = FilteredSourcePathToHash::lookup(testSettings(), k, [&] {
        ++computeCalls;
        return hash;
    });
    EXPECT_EQ(first, hash);
    EXPECT_EQ(computeCalls, 1);

    auto second = FilteredSourcePathToHash::lookup(testSettings(), k, [&] {
        ++computeCalls;
        return Hash::dummy;
    });
    EXPECT_EQ(second, hash);
    EXPECT_EQ(computeCalls, 1); // hit ⇒ no recompute

    /* A key differing only in shapeHash is a distinct row (miss). */
    auto k2 = k;
    k2.shapeHash = "sha256-YmFy";
    EXPECT_FALSE(FilteredSourcePathToHash::peek(testSettings(), k2).has_value());
}

/* ---------- SourceContentToNarHash (the scheduler's persistent registry) ---------- */

TEST(Projection, SourceContentToNarHashRoundTrip)
{
    /* The cross-process content→narHash store the MaterialisationScheduler
       writes (cacheNarHash) and reads (peekNarHash). Miss → compute →
       upsert; hit → no recompute. */
    auto cid = "src-content-id-v1::" + uniqueRev("contentid").gitRev();
    auto narHash = hashString(HashAlgorithm::SHA256, "scheduler-nar-stand-in");

    int computeCalls = 0;
    auto first = SourceContentToNarHash::lookup(testSettings(), cid, [&] {
        ++computeCalls;
        return narHash;
    });
    EXPECT_EQ(first, narHash);
    EXPECT_EQ(computeCalls, 1);

    auto second = SourceContentToNarHash::lookup(testSettings(), cid, [&] {
        ++computeCalls;
        return Hash::dummy;
    });
    EXPECT_EQ(second, narHash);
    EXPECT_EQ(computeCalls, 1);
}

TEST(Projection, SourceContentToNarHashPeekDoesNotPopulate)
{
    auto cid = "src-content-id-v1::" + uniqueRev("contentid-peek").gitRev();
    EXPECT_FALSE(SourceContentToNarHash::peek(testSettings(), cid).has_value());
    /* Still a miss after a peek (peek must not write). */
    EXPECT_FALSE(SourceContentToNarHash::peek(testSettings(), cid).has_value());
}

TEST(Projection, SourceContentToNarHashDistinctContentIdsDistinctRows)
{
    auto a = "src-content-id-v1::" + uniqueRev("cid-a").gitRev();
    auto b = "src-content-id-v1::" + uniqueRev("cid-b").gitRev();
    auto ha = hashString(HashAlgorithm::SHA256, "A");
    auto hb = hashString(HashAlgorithm::SHA256, "B");
    SourceContentToNarHash::lookup(testSettings(), a, [&] { return ha; });
    SourceContentToNarHash::lookup(testSettings(), b, [&] { return hb; });
    EXPECT_EQ(*SourceContentToNarHash::peek(testSettings(), a), ha);
    EXPECT_EQ(*SourceContentToNarHash::peek(testSettings(), b), hb);
    EXPECT_NE(ha, hb);
}

/* ---------- SourcePathToHash (the unfiltered sibling of FilteredSourcePathToHash) ---------- */

TEST(Projection, SourcePathToHashRoundTrip)
{
    SourcePathKey k{
        .sourceFingerprint = "tree:" + uniqueRev("sp").gitRev(),
        .method = "nar",
        .subpath = "/sub",
    };
    auto narHash = hashString(HashAlgorithm::SHA256, "sourcepath-nar");

    int computeCalls = 0;
    auto first = SourcePathToHash::lookup(testSettings(), k, [&] {
        ++computeCalls;
        return narHash;
    });
    EXPECT_EQ(first, narHash);
    EXPECT_EQ(computeCalls, 1);
    auto second = SourcePathToHash::lookup(testSettings(), k, [&] {
        ++computeCalls;
        return Hash::dummy;
    });
    EXPECT_EQ(second, narHash);
    EXPECT_EQ(computeCalls, 1);
}

TEST(Projection, SourcePathToHashKeyFieldsDiscriminate)
{
    /* Each of the three key fields independently changes the row. */
    SourcePathKey base{.sourceFingerprint = "tree:T", .method = "nar", .subpath = "/"};
    auto byFp = base;
    byFp.sourceFingerprint = "tree:U";
    auto byMethod = base;
    byMethod.method = "flat";
    auto bySub = base;
    bySub.subpath = "/other";
    EXPECT_NE(SourcePathToHash::toKey(base), SourcePathToHash::toKey(byFp));
    EXPECT_NE(SourcePathToHash::toKey(base), SourcePathToHash::toKey(byMethod));
    EXPECT_NE(SourcePathToHash::toKey(base), SourcePathToHash::toKey(bySub));
    EXPECT_EQ(SourcePathToHash::toKey(base), SourcePathToHash::toKey(base));
}

/* ---------- Negative cases ---------- */

TEST(Projection, ComputeNotCalledOnHit)
{
    /* The whole point: on a hit, the compute closure must not run.
       This is what saves the work — if the closure runs anyway, the
       cache is just bookkeeping. */
    auto rev = uniqueRev("compute-not-called");
    bool firstComputed = false, secondComputed = false;
    auto narHash = hashString(HashAlgorithm::SHA256, "compute-not-called-content");

    TreeHashToNarHash::lookup(testSettings(), rev, [&] {
        firstComputed = true;
        return narHash;
    });
    EXPECT_TRUE(firstComputed);

    TreeHashToNarHash::lookup(testSettings(), rev, [&] {
        secondComputed = true;
        /* This should never run; if it does, the test fails the next
           assertion. */
        return Hash::dummy;
    });
    EXPECT_FALSE(secondComputed);
}

TEST(Projection, ComputeExceptionDoesNotPersistRow)
{
    /* If `compute` throws, no row should be written. A subsequent
       lookup must miss again. */
    auto rev = uniqueRev("exception-no-persist");

    EXPECT_THROW(
        TreeHashToNarHash::lookup(testSettings(), rev, [&]() -> Hash { throw Error("compute failed"); }), Error);

    /* Still missing — peek returns nullopt. */
    EXPECT_FALSE(TreeHashToNarHash::peek(testSettings(), rev).has_value());
}

TEST(Projection, KeyShapeIsStableAcrossRebuilds)
{
    /* The projection's key encoding is in its toKey() — different
       key encodings = different cache rows. We assert that toKey
       output is deterministic within a process; cross-process
       stability is the soundness law backing cache reuse. */
    auto h = uniqueRev("stable-encoding");
    auto k1 = TreeHashToNarHash::toKey(h);
    auto k2 = TreeHashToNarHash::toKey(h);
    EXPECT_EQ(k1, k2);

    /* And different inputs yield different keys. */
    auto h2 = uniqueRev("stable-encoding-other");
    auto k3 = TreeHashToNarHash::toKey(h2);
    EXPECT_NE(k1, k3);
}

} // namespace nix::fetchers
