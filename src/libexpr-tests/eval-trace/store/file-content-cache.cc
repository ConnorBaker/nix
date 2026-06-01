/**
 * H1 · Cross-process file-content-hash cache (FileContentHashes table)
 *
 * H1 persists `(store_path -> depHash(readFile()))` so that warm verification
 * of a FileBytes dep on a STORE-RESIDENT, fetched (Registered-source) path can
 * skip the readFile()+depHash() recompute that dep-resolution-service.cc
 * otherwise runs unconditionally on every verify. Soundness rests on store-path
 * immutability: a content change yields a different store path = a different key
 * = a clean miss, so a persisted entry is never stale.
 *
 * These tests drive the real record→(new process)→verify path through
 * `SqliteTraceStorage` + `SemanticRegistry` + `resolveCurrentDepHash`. To make a
 * file both `isInStore`-true (a pure storeDir-prefix check) AND physically
 * readable on the verify compute path, the fixture opens a dummy:// store whose
 * `storeDir` is a real temp directory and places the source files there.
 *
 * Non-vacuity is proven two ways:
 *   - H1Hit_AfterFileDeleted_StillVerifies: after the cross-process boundary the
 *     underlying file is DELETED. Without H1 the verify recompute reads a
 *     missing file (sentinel ≠ recorded hash → verify FAILS); WITH H1 the
 *     persisted hash serves and verify succeeds. Reverting H1 flips this red.
 *   - H1Counters_*: assert the fileContentCache hit/store/eligible counters move
 *     exactly as the routing dictates (a control with an Absolute-source dep
 *     records zero eligible/stores — the gate excludes it).
 */

#include "eval-trace/helpers.hh"
#include "eval-trace/semantic-registry-test-access.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/file-system.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

/// Compute a per-test store root under the system temp dir. Used to point the
/// dummy:// store's `storeDir` at a real directory so files written there
/// satisfy BOTH `isInStore` (a storeDir-prefix check) and a real `readFile()`.
/// This must run as part of the LibExprTest base-ctor argument, so it is a free
/// function (the fixture's own members aren't alive yet at that point).
static std::filesystem::path computeStoreRoot()
{
    const auto * info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string leaf = "nix-h1-store-";
    leaf += info ? info->name() : "anon";
    auto root = std::filesystem::temp_directory_path() / leaf;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

/// A TraceStore fixture whose dummy:// store has `storeDir` pointed at a real
/// temp directory, so files written under it satisfy BOTH `isInStore` (string
/// prefix vs storeDir) and a real `readFile()` on the verify compute path.
///
/// Derives from LibExprTest (for initNix/initGC SetUpTestSuite + a ready
/// `state`) and privately inherits `Certifier<BlockingTag>` to mint the
/// blocking proof for record-side `withExclusiveAccess` (the EvalTraceTest
/// pattern; this fixture can't BE EvalTraceTest because it needs a custom store).
class FileContentCacheTest : public LibExprTest, private gdp::Certifier<BlockingTag>
{
public:
    template<typename F>
    decltype(auto) withBs(F && f) { return withProof(std::forward<F>(f)); }

    std::filesystem::path storeRoot;
    ScopedCacheDir cacheDir;  // isolates the eval-trace SQLite DB per test

    FileContentCacheTest()
        : LibExprTest(
            openStore("dummy://", {
                {"read-only", "false"},
                {"store", computeStoreRoot().string()},
            }),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
        , storeRoot(LibExprTest::state.store->storeDir)
    {
    }

    ~FileContentCacheTest() override
    {
        std::filesystem::remove_all(storeRoot);
    }

    // `state` is the public EvalState member inherited from LibExprTest; use it
    // directly. These accessors just shorten the common reaches.
    InterningPools & pools() { return state.tracingPools(); }
    AttrVocabStore & vocab() { return state.vocabStore(); }
    AttrPathId rootPath() { return AttrVocabStore::rootPath(); }

    Hash bootstrapFingerprint() const
    {
        const auto * info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string seed = "h1-file-content-cache";
        if (info) { seed += ":"; seed += info->name(); }
        return hashString(HashAlgorithm::SHA256, seed);
    }

    std::unique_ptr<SqliteTraceStorage> makeDb()
    {
        auto bootstrapKey = SemanticSessionKey::fromSerialized(
            "test-bootstrap:" + bootstrapFingerprint().to_string(HashFormat::Base16, false));
        return std::make_unique<SqliteTraceStorage>(
            state.symbols, state.tracingPools(), vocab(), std::move(bootstrapKey));
    }

    void recreateDb(std::unique_ptr<SqliteTraceStorage> & db)
    {
        db.reset();
        db = makeDb();
    }

    /// Write `content` to `<storeRoot>/<nameWithHashPrefix>-source/<rel>` and
    /// return its absolute path string. The directory name mimics the real
    /// `/nix/store/<narhash>-source` shape (only the prefix matters for the
    /// fixture; isInStore is a pure prefix check).
    std::filesystem::path writeStoreFile(
        std::string_view sourceDirName, std::string_view rel, std::string_view content)
    {
        auto dir = storeRoot / sourceDirName;
        std::filesystem::create_directories(dir);
        auto p = dir / rel;
        writeFile(p, content);
        return p;
    }

    /// Build a SemanticRegistry with a node-key source mounted at a store dir.
    SemanticRegistry registryFor(const std::string & nodeKey, const std::filesystem::path & sourceDir)
    {
        boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> entries;
        entries.emplace(
            DepSource::fromNodeKey(nodeKey),
            SourcePath(getFSSourceAccessor(), CanonPath(sourceDir.string())));
        return SemanticRegistry(std::move(entries));
    }

    /// Record a FileBytes dep keyed (nodeSource, /rel) with the content hash.
    Dep makeNodeFileBytesDep(const std::string & nodeKey, const std::string & rel, std::string_view content)
    {
        return {
            Dep::Key::makeSimple(
                CanonicalQueryKind::FileBytes,
                pools().intern<DepSourceId>(DepSource::fromNodeKey(nodeKey)),
                pools().intern<SimpleDepKeyId>(rel)),
            depHash(content),
        };
    }
};

} // namespace

// ── H1 hit across a process boundary, proven by deleting the file ────────────

TEST_F(FileContentCacheTest, H1Hit_AfterFileDeleted_StillVerifies)
{
    auto srcFile = writeStoreFile("aaaa-source", "data.txt", "h1-content-v1");
    auto registry = registryFor("root", srcFile.parent_path());

    // Cold record: a FileBytes dep on the store-resident file.
    auto db = makeDb();
    withBs([&](const auto & bs) {
        db->withExclusiveAccess(bs, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v", {}},
                       {makeNodeFileBytesDep("root", "/data.txt", "h1-content-v1")});
        });
    });

    // Cold compute persisted the (store_path -> hash) entry; the record path
    // itself does NOT (record doesn't resolve+read), so prime it via one verify
    // BEFORE the boundary: this populates FileContentHashes from the live file.
    {
        auto warm = test::TraceStorageTestAccess::verify(*db, rootPath(), registry, state);
        ASSERT_TRUE(warm.has_value()) << "pre-boundary verify must hit (file present, hash matches)";
    }

    // New process: drop all in-memory caches; the SQLite DB (incl.
    // FileContentHashes) persists. Then DELETE the underlying file so a verify
    // recompute would read a missing file → sentinel ≠ recorded hash → FAIL.
    recreateDb(db);
    std::filesystem::remove(srcFile);
    getFSSourceAccessor()->invalidateCache();

    // Counters are globally enabled in the test harness (§N.10); raw deltas are
    // the documented idiom for counters PathCountersSnapshot doesn't track.
    auto hitsBefore = nrFileContentCacheHits.load();
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), registry, state);

    // WITH H1: the persisted store-path hash serves, verify succeeds despite the
    // missing file. WITHOUT H1 (revert resolveCurrentDepHash): the recompute
    // reads a missing file and verify returns nullopt — this assertion goes red.
    EXPECT_TRUE(result.has_value())
        << "H1 must serve the persisted content hash for the (now-deleted) "
           "store-resident file across the process boundary";
    EXPECT_GE(nrFileContentCacheHits.load() - hitsBefore, 1u)
        << "the served hash must have come from the H1 cache, not a recompute";
}

// ── Counters: eligible/store/hit move exactly as the routing dictates ────────

TEST_F(FileContentCacheTest, H1Counters_StoreResidentNodeDep_IsEligibleAndCached)
{
    auto srcFile = writeStoreFile("bbbb-source", "data.txt", "h1-counters-v1");
    auto registry = registryFor("root", srcFile.parent_path());

    auto db = makeDb();
    withBs([&](const auto & bs) {
        db->withExclusiveAccess(bs, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v", {}},
                       {makeNodeFileBytesDep("root", "/data.txt", "h1-counters-v1")});
        });
    });

    // First verify (cold compute): eligible + store, no hit.
    {
        auto eBefore = nrFileContentCacheEligible.load();
        auto sBefore = nrFileContentCacheStores.load();
        auto hBefore = nrFileContentCacheHits.load();
        auto r = test::TraceStorageTestAccess::verify(*db, rootPath(), registry, state);
        ASSERT_TRUE(r.has_value());
        EXPECT_GE(nrFileContentCacheEligible.load() - eBefore, 1u) << "store-resident node dep is H1-eligible";
        EXPECT_GE(nrFileContentCacheStores.load() - sBefore, 1u) << "cold compute must persist the hash";
        EXPECT_EQ(nrFileContentCacheHits.load() - hBefore, 0u) << "first compute is not a hit";
    }

    // New process: the persisted entry must now produce a HIT.
    recreateDb(db);
    {
        auto sBefore = nrFileContentCacheStores.load();
        auto hBefore = nrFileContentCacheHits.load();
        auto r = test::TraceStorageTestAccess::verify(*db, rootPath(), registry, state);
        ASSERT_TRUE(r.has_value());
        EXPECT_GE(nrFileContentCacheHits.load() - hBefore, 1u) << "cross-process serve from H1";
        EXPECT_EQ(nrFileContentCacheStores.load() - sBefore, 0u) << "a hit must not re-store";
    }
}

TEST_F(FileContentCacheTest, H1Gate_AbsoluteSourceDep_NotEligible)
{
    // An Absolute-source FileBytes dep (a plain absolute path, even if it
    // happens to be under the store dir) is NOT H1-eligible: only fetched,
    // Registered (node-key / runtime-root) sources are narHash-content-addressed.
    // This guards the gate against caching mutable/build-output paths.
    auto srcFile = writeStoreFile("cccc-source", "data.txt", "h1-gate-v1");
    SemanticRegistry registry;  // empty — Absolute resolves directly from the key

    auto absKey = srcFile.string();
    auto db = makeDb();
    withBs([&](const auto & bs) {
        db->withExclusiveAccess(bs, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v", {}},
                       {makeContentDep(pools(), absKey, "h1-gate-v1")});
        });
    });

    auto eBefore = nrFileContentCacheEligible.load();
    auto sBefore = nrFileContentCacheStores.load();
    auto r = test::TraceStorageTestAccess::verify(*db, rootPath(), registry, state);
    ASSERT_TRUE(r.has_value()) << "absolute-source dep still verifies via the normal read path";
    EXPECT_EQ(nrFileContentCacheEligible.load() - eBefore, 0u)
        << "Absolute-source deps must be excluded from H1 (not content-addressed)";
    EXPECT_EQ(nrFileContentCacheStores.load() - sBefore, 0u);
}

} // namespace nix::eval_trace
