/**
 * Unit tests for SqliteTraceStorage::queryEvalInfoExclusive, the read-only helper
 * that backs `nix eval-info`. Drives the SqliteTraceStorage directly — no shell, no
 * TraceSession, no evaluator. Covers: clean miss, session hit with deps,
 * different-attrPath isolation, history fallback, failed-result round-trip,
 * volatile-dep flagging, cross-session recreate.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/util/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

/// The SqliteTraceStorage ctor sets stableRecoveryKey = semanticSessionKey.digest and
/// does not accept a SessionConfig — `setSessionConfig` is SetOnce. To exercise
/// history-fallback behaviour we need two session keys sharing a stable
/// recovery key. `makeSessionConfig` builds one with explicit components so
/// tests can control both halves.
SessionConfig makeSessionConfig(std::string_view policySeed, std::string_view recoverySeed)
{
    auto policyHash = hashString(HashAlgorithm::SHA256, std::string(policySeed));
    auto src = policyHash.bytes();
    EvalTraceHash policyDigest{};
    std::memcpy(policyDigest.bytes.data(), src.data(),
                std::min<size_t>(policyDigest.bytes.size(), src.size()));
    return SessionConfig::forTest(policyDigest, recoverySeed);
}

} // namespace

// ── Clean-miss cases ────────────────────────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_EmptyStore_ReturnsNullopt)
{
    auto db = makeDb();
    // Nothing recorded; query any path.
    auto result = db->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/false);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, QueryEvalInfo_EmptyStore_HistoryFallbackAlsoMisses)
{
    auto db = makeDb();
    auto result = db->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/true);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, QueryEvalInfo_UnrecordedAttr_StillHasRecordedSibling)
{
    auto db = makeDb();
    // Record only at vpath({"known"}).
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"known"}), int_t{NixInt{1}}, {});
    });
    // Querying a sibling that was never recorded must miss, even though the
    // store is non-empty.
    auto miss = db->queryEvalInfoExclusive(vpath({"unseen"}), /*allowHistoryFallback=*/false);
    EXPECT_FALSE(miss.has_value());

    // Sanity: the recorded attr hits.
    auto hit = db->queryEvalInfoExclusive(vpath({"known"}), /*allowHistoryFallback=*/false);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->source, SqliteTraceStorage::EvalInfoRecord::Source::Session);
}

// ── Basic session-hit shape ─────────────────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_RecordedInt_ReturnsSessionHit)
{
    auto db = makeDb();
    TraceId recordedTraceId;

    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), int_t{NixInt{42}}, {});
        recordedTraceId = result.traceId;
    });

    auto record = db->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/false);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->source, SqliteTraceStorage::EvalInfoRecord::Source::Session);
    EXPECT_EQ(record->traceId.value, recordedTraceId.value);
    EXPECT_GT(record->resultId.value, 0u);
    EXPECT_TRUE(std::holds_alternative<int_t>(record->value));
    EXPECT_EQ(std::get<int_t>(record->value).x.value, 42);
    ASSERT_TRUE(record->deps);
    EXPECT_TRUE(record->deps->empty());
}

// ── Dep set round-trip ──────────────────────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_RecordedDeps_RoundTrip)
{
    auto db = makeDb();
    std::vector<Dep> recorded = {
        makeContentDep(pools(), "/a.nix", "content-a"),
        makeContentDep(pools(), "/b.nix", "content-b"),
        makeEnvVarDep(pools(), "HOME", "/home/user"),
        makeSystemDep(pools(), "x86_64-linux"),
    };

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v1", {}}, recorded);
    });

    auto record = db->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/false);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->deps);
    EXPECT_EQ(record->deps->size(), recorded.size());

    // Each recorded dep kind must appear at least once in the query result.
    auto hasKind = [&](CanonicalQueryKind k) {
        return std::any_of(record->deps->begin(), record->deps->end(),
            [&](const Dep & d) { return d.key.kind == k; });
    };
    EXPECT_TRUE(hasKind(CanonicalQueryKind::FileBytes));
    EXPECT_TRUE(hasKind(CanonicalQueryKind::EnvironmentLookup));
    EXPECT_TRUE(hasKind(CanonicalQueryKind::SessionSystemValue));

    // Hashes shape-check: dep vector is an owned shared_ptr.
    EXPECT_GT(record->traceHash.value.bytes.size(), 0u);
    EXPECT_GT(record->keySetHash.value.bytes.size(), 0u);
    EXPECT_GT(record->depKeySetId.value, 0u);
}

// ── Volatile dep flagging ───────────────────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_VolatileDep_ShowsUpInDepList)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep(pools())};

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), int_t{NixInt{7}}, deps);
    });

    auto record = db->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/false);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->deps);

    bool hasVolatile = std::any_of(record->deps->begin(), record->deps->end(),
        [](const Dep & d) { return isVolatile(d.key.kind); });
    EXPECT_TRUE(hasVolatile);

    // Guard against a hypothetical regression that swaps the result payload
    // when a volatile dep is present: the recorded Int must still be 7.
    ASSERT_TRUE(std::holds_alternative<int_t>(record->value));
    EXPECT_EQ(std::get<int_t>(record->value).x.value, 7);
}

// ── Failed / trivial / all result kinds ─────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_FailedResult_RoundTripsErrorMessage)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), failed_t{.errorMessage = "marker-bad"}, {});
    });

    auto record = db->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/false);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(std::holds_alternative<failed_t>(record->value));
    EXPECT_EQ(std::get<failed_t>(record->value).errorMessage, "marker-bad");
}

TEST_F(TraceStoreTest, QueryEvalInfo_BoolFloatListAttrsPath_AllRoundTrip)
{
    auto db = makeDb();

    // Store distinct attr paths so each result lives in its own row.
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"b"}), true, {});
        db->record(ea, vpath({"f"}), float_t{3.141592653589793}, {});
        db->record(ea, vpath({"l"}), list_t{.entries = {}, .meta = std::nullopt}, {});
        db->record(ea, vpath({"a"}), attrs_t{.entries = {}, .meta = std::nullopt}, {});
        db->record(ea, vpath({"n"}), makeNull(), {});
        db->record(ea, vpath({"m"}), makeMissing(), {});
    });

    auto b = db->queryEvalInfoExclusive(vpath({"b"}), false);
    ASSERT_TRUE(b); ASSERT_TRUE(std::holds_alternative<bool>(b->value));
    EXPECT_TRUE(std::get<bool>(b->value));

    auto f = db->queryEvalInfoExclusive(vpath({"f"}), false);
    ASSERT_TRUE(f); ASSERT_TRUE(std::holds_alternative<float_t>(f->value));
    EXPECT_DOUBLE_EQ(std::get<float_t>(f->value).x, 3.141592653589793);

    auto l = db->queryEvalInfoExclusive(vpath({"l"}), false);
    ASSERT_TRUE(l); EXPECT_TRUE(std::holds_alternative<list_t>(l->value));

    auto a = db->queryEvalInfoExclusive(vpath({"a"}), false);
    ASSERT_TRUE(a); EXPECT_TRUE(std::holds_alternative<attrs_t>(a->value));

    auto n = db->queryEvalInfoExclusive(vpath({"n"}), false);
    ASSERT_TRUE(n);
    ASSERT_TRUE(std::holds_alternative<trivial_t>(n->value));
    EXPECT_EQ(std::get<trivial_t>(n->value).kind, TrivialKind::Null);

    auto m = db->queryEvalInfoExclusive(vpath({"m"}), false);
    ASSERT_TRUE(m);
    ASSERT_TRUE(std::holds_alternative<trivial_t>(m->value));
    EXPECT_EQ(std::get<trivial_t>(m->value).kind, TrivialKind::Missing);
}

// ── Path + Trivial sub-kinds ────────────────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_PathResult_RoundTrips)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(),
            path_t{"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test"}, {});
    });
    auto record = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(record);
    ASSERT_TRUE(std::holds_alternative<path_t>(record->value));
    EXPECT_EQ(std::get<path_t>(record->value).path,
        "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test");
}

TEST_F(TraceStoreTest, QueryEvalInfo_TrivialPlaceholder_RoundTrips)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), makePlaceholder(), {});
    });
    auto record = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(record);
    ASSERT_TRUE(std::holds_alternative<trivial_t>(record->value));
    EXPECT_EQ(std::get<trivial_t>(record->value).kind, TrivialKind::Placeholder);
}

TEST_F(TraceStoreTest, QueryEvalInfo_TrivialMisc_RoundTrips)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), makeMisc(), {});
    });
    auto record = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(record);
    ASSERT_TRUE(std::holds_alternative<trivial_t>(record->value));
    EXPECT_EQ(std::get<trivial_t>(record->value).kind, TrivialKind::Misc);
}

// ── Dep kinds with non-trivial encoding shapes ──────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_DirectoryEntriesDep_RoundTrips)
{
    auto db = makeDb();
    auto dirHash = depHash("entries-v1");
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}}, {
            makeDirectoryDep(pools(), "/some/dir", dirHash),
        });
    });
    auto record = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(record);
    ASSERT_EQ(record->deps->size(), 1u);
    EXPECT_EQ((*record->deps)[0].key.kind, CanonicalQueryKind::DirectoryEntries);
    auto resolved = db->resolveDep((*record->deps)[0]);
    EXPECT_EQ(resolved.key, "/some/dir");
}

TEST_F(TraceStoreTest, QueryEvalInfo_ExistenceCheckDep_StringValueRoundTrips)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), int_t{NixInt{0}}, {
            makeExistenceDep(pools(), "/present", /*exists=*/true),
            makeExistenceDep(pools(), "/absent", /*exists=*/false),
        });
    });
    auto record = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(record);
    ASSERT_EQ(record->deps->size(), 2u);

    // Deps are canonical-sorted, order is by `Dep::Key::operator<=>` — but we
    // don't rely on that; just find each by key string and check the stored
    // string value.
    bool sawPresent = false, sawAbsent = false;
    for (const auto & d : *record->deps) {
        EXPECT_EQ(d.key.kind, CanonicalQueryKind::ExistenceCheck);
        auto resolved = db->resolveDep(d);
        const auto * stored = std::get_if<std::string>(&d.hash);
        ASSERT_TRUE(stored) << "ExistenceCheck stores a raw string, not a hash";
        if (resolved.key == "/present") {
            EXPECT_EQ(*stored, "type:1");
            sawPresent = true;
        } else if (resolved.key == "/absent") {
            EXPECT_EQ(*stored, "missing");
            sawAbsent = true;
        }
    }
    EXPECT_TRUE(sawPresent);
    EXPECT_TRUE(sawAbsent);
}

TEST_F(TraceStoreTest, QueryEvalInfo_DerivedStorePathDep_TupleKeyRoundTrips)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), int_t{NixInt{0}}, {
            makeCopiedPathDep(pools(),
                "/source/path",
                "store-name",
                "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-store-name"),
        });
    });
    auto record = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(record);
    ASSERT_EQ(record->deps->size(), 1u);
    const auto & dep = (*record->deps)[0];
    EXPECT_EQ(dep.key.kind, CanonicalQueryKind::DerivedStorePath);
    // Stored value is the output store-path string, not an eval-trace hash.
    const auto * stored = std::get_if<std::string>(&dep.hash);
    ASSERT_TRUE(stored);
    EXPECT_EQ(*stored,
        "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-store-name");
}

// ── Multi-component attr path isolation ─────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_DeepPath_IsolatedFromSiblings)
{
    auto db = makeDb();
    // Record at a.b.c and at a.b.d.
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"a", "b", "c"}), int_t{NixInt{1}}, {});
        db->record(ea, vpath({"a", "b", "d"}), int_t{NixInt{2}}, {});
    });

    auto c = db->queryEvalInfoExclusive(vpath({"a", "b", "c"}), false);
    ASSERT_TRUE(c);
    EXPECT_EQ(std::get<int_t>(c->value).x.value, 1);

    auto d = db->queryEvalInfoExclusive(vpath({"a", "b", "d"}), false);
    ASSERT_TRUE(d);
    EXPECT_EQ(std::get<int_t>(d->value).x.value, 2);

    // The intermediate node a.b has no record.
    auto intermediate = db->queryEvalInfoExclusive(vpath({"a", "b"}), false);
    EXPECT_FALSE(intermediate.has_value());
}

// ── Consistency with loadFullTrace ──────────────────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_DepsMatchLoadFullTrace)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep(pools(), "/x.nix", "X"),
        makeEnvVarDep(pools(), "PATH", "/usr/bin"),
    };
    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto r = db->record(ea, rootPath(), int_t{NixInt{99}}, deps);
        traceId = r.traceId;
    });

    auto record = db->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/false);
    ASSERT_TRUE(record);
    ASSERT_TRUE(record->deps);
    EXPECT_EQ(record->traceId.value, traceId.value);

    // Load via the canonical loadFullTrace helper; dep vectors must match in
    // both key AND recorded value (hash). Comparing only keys would miss a
    // regression that swapped values per dep during query.
    auto viaLoad = withExclusiveStore(*db, [&](const auto & ea) {
        return db->loadFullTrace(ea, traceId);
    });
    ASSERT_TRUE(viaLoad);
    EXPECT_EQ(record->deps->size(), viaLoad->size());
    for (size_t i = 0; i < record->deps->size(); ++i) {
        EXPECT_EQ((*record->deps)[i].key, (*viaLoad)[i].key);
        EXPECT_EQ((*record->deps)[i].hash, (*viaLoad)[i].hash);
    }
}

// ── History fallback: Sessions miss → History hit ───────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_HistoryFallback_SessionsMissRecoveryHit)
{
    // Session A: record with policyA + shared stableRecoveryKey.
    auto dbA = makeDb();
    dbA->setSessionConfig(makeSessionConfig("policy-A", "shared-recovery-key"));
    TraceId recordedInA;
    withExclusiveStore(*dbA, [&](const auto & ea) {
        auto r = dbA->record(ea, rootPath(), int_t{NixInt{100}}, {});
        recordedInA = r.traceId;
    });
    // Close A — the destructor flushes pending writes + commits.
    dbA.reset();

    // Session B: same store file, DIFFERENT semanticSessionKey, SAME stableRecoveryKey.
    // In-memory state is not shared because makeDb() builds a fresh SqliteTraceStorage
    // pointing at the same cache directory.
    auto dbB = makeDb();
    dbB->setSessionConfig(makeSessionConfig("policy-B-different", "shared-recovery-key"));

    // Without history fallback: Sessions under policy-B has no row → miss.
    auto miss = dbB->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/false);
    EXPECT_FALSE(miss.has_value());

    // With history fallback: lookupLatestHistoryForAttr finds the row A wrote.
    auto hit = dbB->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/true);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->source, SqliteTraceStorage::EvalInfoRecord::Source::History);
    EXPECT_EQ(hit->traceId.value, recordedInA.value);
    ASSERT_TRUE(std::holds_alternative<int_t>(hit->value));
    EXPECT_EQ(std::get<int_t>(hit->value).x.value, 100);
}

TEST_F(TraceStoreTest, QueryEvalInfo_HistoryFallback_DifferentRecoveryKey_StillMisses)
{
    auto dbA = makeDb();
    dbA->setSessionConfig(makeSessionConfig("policy-A", "recovery-A"));
    withExclusiveStore(*dbA, [&](const auto & ea) {
        dbA->record(ea, rootPath(), int_t{NixInt{200}}, {});
    });
    dbA.reset();

    auto dbB = makeDb();
    dbB->setSessionConfig(makeSessionConfig("policy-B", "recovery-B-different"));

    // Both Sessions and History use distinct keys — fallback must still miss.
    auto miss = dbB->queryEvalInfoExclusive(rootPath(), /*allowHistoryFallback=*/true);
    EXPECT_FALSE(miss.has_value());
}

// ── Overwrite: second record supersedes first under same session key ───

TEST_F(TraceStoreTest, QueryEvalInfo_ReRecord_ServesLatest)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), int_t{NixInt{1}}, {});
    });
    auto first = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(first);
    auto firstTrace = first->traceId;

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), int_t{NixInt{2}}, {
            makeContentDep(pools(), "/extra.nix", "different"),
        });
    });
    auto second = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(second);
    EXPECT_NE(second->traceId.value, firstTrace.value);
    EXPECT_EQ(std::get<int_t>(second->value).x.value, 2);
    ASSERT_EQ(second->deps->size(), 1u);
    // The single dep must be the FileBytes on /extra.nix — a regression that
    // returned an arbitrary single dep would otherwise pass.
    const auto & dep = (*second->deps)[0];
    EXPECT_EQ(dep.key.kind, CanonicalQueryKind::FileBytes);
    auto resolved = db->resolveDep(dep);
    EXPECT_EQ(resolved.key, "/extra.nix");
}

// ── Cross-session: survives store close + reopen ────────────────────────

TEST_F(TraceStoreTest, QueryEvalInfo_CrossSessionBulkLoad_StillHits)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"survives"}), string_t{"stored", {}}, {
            makeContentDep(pools(), "/f.nix", "c"),
        });
    });
    // Force flush + reopen against the same cache file.
    recreateDb(db);

    auto record = db->queryEvalInfoExclusive(vpath({"survives"}), false);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->source, SqliteTraceStorage::EvalInfoRecord::Source::Session);
    ASSERT_TRUE(std::holds_alternative<string_t>(record->value));
    EXPECT_EQ(std::get<string_t>(record->value).first, "stored");
    ASSERT_TRUE(record->deps);
    EXPECT_EQ(record->deps->size(), 1u);
}

// ── Idempotent re-record: same content → same Traces row (via traceByFullHash dedup)

// Both dbA and dbB open the SAME cache file (per-test-case fingerprint →
// same session key → same on-disk DB path). dbB's `record()` call hits
// `getOrCreateTrace`'s `traceByFullHash` dedup map (bulk-loaded from SQLite
// on open) and reuses the row dbA wrote. The assertion therefore covers two
// invariants at once: content-addressed dedup is intact, AND the traceHash
// stored on the shared row is stable across store lifetimes.
TEST_F(TraceStoreTest, QueryEvalInfo_IdenticalContent_DedupsViaFullHash)
{
    auto dbA = makeDb();
    withExclusiveStore(*dbA, [&](const auto & ea) {
        dbA->record(ea, rootPath(), int_t{NixInt{11}}, {
            makeContentDep(pools(), "/p.nix", "p"),
        });
    });
    auto a = dbA->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(a);
    dbA.reset();

    auto dbB = makeDb();
    withExclusiveStore(*dbB, [&](const auto & ea) {
        dbB->record(ea, rootPath(), int_t{NixInt{11}}, {
            makeContentDep(pools(), "/p.nix", "p"),
        });
    });
    auto b = dbB->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->traceHash, b->traceHash);
    // Dedup: same TraceId reused across store lifetimes.
    EXPECT_EQ(a->traceId.value, b->traceId.value);
}

// ── Different content ⇒ different trace rows and different traceHashes ─

TEST_F(TraceStoreTest, QueryEvalInfo_DifferentDepContent_DistinctRows)
{
    auto dbA = makeDb();
    withExclusiveStore(*dbA, [&](const auto & ea) {
        dbA->record(ea, rootPath(), int_t{NixInt{0}}, {
            makeContentDep(pools(), "/x.nix", "one"),
        });
    });
    auto a = dbA->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(a);
    dbA.reset();

    auto dbB = makeDb();
    withExclusiveStore(*dbB, [&](const auto & ea) {
        dbB->record(ea, rootPath(), int_t{NixInt{0}}, {
            makeContentDep(pools(), "/x.nix", "two-different"),
        });
    });
    auto b = dbB->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(b);
    // Different dep content ⇒ different FullTraceHash ⇒ different Traces row.
    EXPECT_NE(a->traceHash, b->traceHash);
    EXPECT_NE(a->traceId.value, b->traceId.value);
}

// ── Sanity: query does not mutate store counters / state ───────────────

TEST_F(TraceStoreTest, QueryEvalInfo_DoesNotBumpRecordOrVerifyCounters)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), int_t{NixInt{5}}, {});
    });

    PathCountersSnapshot snap;
    // Query twice; must not trigger record, verify, or recovery codepaths —
    // queryEvalInfoExclusive is a pure read that bypasses the orchestrator.
    auto r1 = db->queryEvalInfoExclusive(rootPath(), false);
    auto r2 = db->queryEvalInfoExclusive(rootPath(), false);
    ASSERT_TRUE(r1);
    ASSERT_TRUE(r2);
    EXPECT_EQ(snap.deltaRecords(), 0u);
    EXPECT_EQ(snap.deltaTraceCacheHits(), 0u);
    EXPECT_EQ(snap.deltaTraceCacheMisses(), 0u);
    EXPECT_EQ(snap.deltaRecoveryAttempts(), 0u);
    EXPECT_EQ(snap.deltaHistoryBootstraps(), 0u);
    // Results are equivalent across the two read calls.
    EXPECT_EQ(r1->traceId.value, r2->traceId.value);
    EXPECT_EQ(r1->traceHash, r2->traceHash);
}

} // namespace nix::eval_trace
