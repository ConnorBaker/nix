// Keyset-provenance differential test harness.
//
// Design memo: plans/keyset-provenance-differential-harness.md
// Obligation:  memory project_eval_trace_provenance_proof_obligation
//
// PURPOSE.  The provenance proof is a STRICT DOWNGRADE of an already
// fail-closed recorder: a complete-keyset observation of a structured source
// may be downgraded to per-observed-member deps IFF the keyset never reaches a
// result-visible sink.  The only thing a bug in that downgrade can do is
// OVER-REUSE (serve a stale result when the keyset actually mattered).  This
// file is the differential oracle that DETECTS over-reuse, so the downgrade can
// be prototyped safely.  Tests lead: this harness must exist and pass against
// the current (pre-downgrade) recorder before any downgrade code is written.
//
// THE ORACLE (per keyset mutation, cross two independent observations):
//   * ground-truth value: eval() the expression through the UNCACHED path
//     (LibExprTest::eval — parses + evaluates raw, never consults the
//     TraceSession), before and after the mutation.  This is the source of
//     truth for "did the result actually change?".
//   * cache decision: PathCountersSnapshot — primaryCacheServedOnly() for a
//     hit, deltaTraceCacheMisses() >= 1 for a miss.
//
//                         cache HIT            cache MISS
//   ground-truth value ┌─────────────────┬──────────────────┐
//      UNCHANGED        │  precision ✓     │  precision LOSS   │  ← tolerated; log
//                       ├─────────────────┼──────────────────┤
//      CHANGED          │  STALE SERVE ✗   │  sound ✓          │
//                       └─────────────────┴──────────────────┘
//
// The downgrade can only push a cell UPWARD (miss -> hit).  Enabled tests
// assert the SOUNDNESS invariant only (never a stale serve), so they remain
// correct both before and after the downgrade.  The PRECISION win the downgrade
// is for is encoded as a DISABLED_ pin (§ "precision pin" below) that flips
// green exactly when the proof lands — its red->green transition is the
// acceptance test for the downgrade.
//
// CROSS-TRACE SINK (OR-4) is intentionally NOT exercised here.  Per the memo §7
// research finding, the "keyset crosses into another cached trace" case is
// (a) unreachable from real evaluation (the ExprParseFile FileBytes backstop at
// eval.cc:1422-1435 catches the source edit first) and (b) already pinned by
// SYNTHETIC raw-record() tests: store/trace-parent-slot.cc,
// store/trace-value-context.cc, store/cross-session-subsumption.cc, and
// verify/integration.cc::Integration_ParentSlot_DoesNotCaptureKeySetRemoval.
// New cross-trace keyset-provenance cases belong there, NOT in this real-eval
// file and NOT behind a multi-EvalState fixture (neither needed nor sufficient).

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <nlohmann/json.hpp>

#include <format>
#include <functional>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// Fixture
// ═══════════════════════════════════════════════════════════════════════

// DepPrecisionTest gives us makeCache / forceRoot / invalidateFileCache /
// evalAndCollectDeps / hasJsonDep / shapePred / fj(), and inherits the raw
// uncached eval() from LibExprTest.  The corpus is deterministic, not
// RapidCheck-generated: the sink / non-sink discriminator must be encoded as
// DATA (it is the whole point), and a random generator's RC_PRE cannot reliably
// select an expression of a given sink class (see property/CLAUDE.md note on
// makeNixExprGen + RC_PRE shape selection).
class KeysetProvenanceTest : public DepPrecisionTest
{
public:
    KeysetProvenanceTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "keyset-provenance");
    }

protected:
    // ── Local value comparator ──────────────────────────────────────────
    //
    // assertValuesEqual (expr-gen.hh) uses RC_ASSERT and is for RapidCheck
    // bodies; this is a deterministic-test boolean equivalent covering the
    // kinds the corpus produces (scalars, list-of-scalar, shallow attrset).
    // Both Values must come from the same EvalState (they do — all evals run
    // through the fixture's `state`).
    static bool valuesEqual(const Value & a, const Value & b)
    {
        if (a.type() != b.type())
            return false;
        switch (a.type()) {
        case nInt:
            return a.integer().value == b.integer().value;
        case nFloat:
            return a.fpoint() == b.fpoint();
        case nBool:
            return a.boolean() == b.boolean();
        case nNull:
            return true;
        case nString:
            return std::string_view(a.string_view()) == std::string_view(b.string_view());
        case nList: {
            if (a.listSize() != b.listSize())
                return false;
            auto la = a.listView();
            auto lb = b.listView();
            for (size_t i = 0; i < a.listSize(); ++i) {
                if (!la[i] || !lb[i])
                    return false;
                if (!valuesEqual(*la[i], *lb[i]))
                    return false;
            }
            return true;
        }
        case nAttrs: {
            if (a.attrs()->size() != b.attrs()->size())
                return false;
            auto ia = a.attrs()->begin();
            auto ib = b.attrs()->begin();
            for (; ia != a.attrs()->end() && ib != b.attrs()->end(); ++ia, ++ib) {
                if (ia->name != ib->name)
                    return false;
                if (ia->value && ib->value && !valuesEqual(*ia->value, *ib->value))
                    return false;
            }
            return true;
        }
        case nPath:
        case nFunction:
        case nExternal:
        case nThunk:
        case nFailed:
            // Not produced by the corpus; treat as incomparable (forces a
            // "changed" verdict, the conservative direction for the soundness
            // assertion).
            return false;
        }
        return false;  // unreachable — all nType enumerators handled above
    }

    // ── Vacuity guard ───────────────────────────────────────────────────
    //
    // Keyset deps fire ONLY for TracedData attrsets (hasAnyTracedDataLayer
    // gate; see memory project_eval_trace_gap2_recording_ground_truth).  If the
    // corpus source were a plain Nix attrset literal instead of a fromJSON /
    // readDir source, NO keyset dep would be recorded and the whole test would
    // pass vacuously.  Assert the complete-keyset (StructuredProjection #keys)
    // dep is actually present so a vacuous pass is impossible.
    void assertCompleteKeysetDepRecorded(const std::string & expr)
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "VACUITY GUARD: expected a StructuredProjection #keys dep; without it "
               "this keyset-provenance test proves nothing.\n"
            << dumpDeps(deps);
    }

    // ── The differential oracle ─────────────────────────────────────────
    //
    // SOUNDNESS IS A VALUE COMPARISON, NOT A COUNTER.  The adversarial pass
    // (2026-05-29) established that the path counter alone is UNSOUND as a
    // stale-serve detector: for a non-scalar result (a list/attrset), a
    // primaryCacheServedOnly() "hit" on the root spine does NOT imply the served
    // value is stale — the element thunks are re-derived per-leaf when forced
    // (DEF-6 per-leaf lazy verification).  e.g. `attrValues j` with a VALUE
    // change reports primaryHit=1,miss=0 yet the deep-forced served value equals
    // the NEW ground truth (servedEqOld=0, servedEqNew=1).  A counter-only oracle
    // would FALSE-FAIL that as a stale serve — testing the part (which path ran),
    // not the seam (what value reached the user).
    //
    // So the soundness signal is `servedValue` deep-forced and compared to the
    // post-mutation ground truth.  The counter (`primaryHit`/`missed`) is kept as
    // the PRECISION signal: it measures whether we re-evaluated, which is what
    // the downgrade is trying to avoid.
    struct Oracle
    {
        bool groundTruthChanged;  // uncached value moved across the mutation
        bool servedStale;         // SOUNDNESS: warm-served value != post ground truth
        bool primaryHit;          // PRECISION: primary cache served (no recovery/bootstrap)
        bool missed;              // PRECISION: verifier re-evaluated
    };

    // Run the oracle for one (expr, file-mutation) pair.  `expr` must read
    // `file` (typically via fj(file.path)).  `mutatedJson` is the post-mutation
    // file content.  All three observed values are deep-forced so the comparison
    // is over fully-evaluated structure (eval()/forceRoot only reach WHNF; list
    // and attrset elements would otherwise remain thunks and compare unequal
    // spuriously).
    Oracle runOracle(const std::string & expr, TempJsonFile & file, const std::string & mutatedJson)
    {
        return runOracleWith(expr, [&] {
            file.modify(mutatedJson);
            invalidateFileCache(file.path);
        });
    }

    // Generic core: `mutate` performs the source mutation AND the cache
    // invalidation (it must, since readDir / multi-file sources are not a single
    // TempJsonFile).  Everything else — deep-forced ground truth pre/post, cold
    // record, warm serve, value-comparing soundness verdict — is shared.
    Oracle runOracleWith(const std::string & expr, const std::function<void()> & mutate)
    {
        // (1) Ground-truth pre-value, uncached.  No active TraceSession yet, so
        //     eval() reads the original source and does not touch the cache.
        Value pre = eval(expr);
        state.forceValueDeep(pre);

        // (2) Cold record: makeCache's loader eval()s the expression (reading
        //     the original source) and the session records the trace.
        {
            auto cache = makeCache(expr);
            (void) forceRoot(*cache);
        }

        // (3) Mutate the structured source and invalidate file caches (releases
        //     the active session + clears FS / content / memo caches).
        mutate();

        // (4) Ground-truth post-value, uncached.  Reads the mutated source.
        Value post = eval(expr);
        state.forceValueDeep(post);
        const bool changed = !valuesEqual(pre, post);

        // (5) Warm eval: capture BOTH the path counters (precision) AND the
        //     actual served value deep-forced (soundness).
        PathCountersSnapshot snap;
        Value served;
        {
            auto cache = makeCache(expr);
            served = forceRoot(*cache);
        }
        state.forceValueDeep(served);

        return Oracle{
            .groundTruthChanged = changed,
            .servedStale = !valuesEqual(served, post),
            .primaryHit = snap.primaryCacheServedOnly(),
            .missed = snap.deltaTraceCacheMisses() >= 1,
        };
    }

    // The universal SOUNDNESS assertion: the warm-served value must EQUAL the
    // post-mutation ground truth, full stop.  This holds regardless of which
    // cache path served (primary hit, recovery, re-eval) — it is a property of
    // the VALUE the user sees, not of the machinery.  A precision LOSS (ground
    // truth unchanged but the cache re-evaluated) is TOLERATED and only logged:
    // the fail-closed baseline lives there by design and the downgrade is what
    // reclaims it.
    static void assertNoStaleServe(const Oracle & o, const char * what)
    {
        EXPECT_FALSE(o.servedStale)
            << "STALE SERVE (" << what << "): the warm-served value does not equal the "
               "post-mutation ground truth — the cache served a stale result.";
        if (!o.groundTruthChanged && o.missed) {
            // Tolerated: fail-closed over-recording.  Logged so the precision
            // headroom is visible, never failed.
            GTEST_LOG_(INFO) << "precision loss (" << what << "): ground-truth value "
                                "unchanged but the cache missed (fail-closed baseline).";
        }
    }

    // ── JSON keyset-mutation helpers ────────────────────────────────────
    //
    // The existing DepSlot::generateMutation (expr-gen.cc:89-122) is
    // keyset-PRESERVING by construction (it changes values/types, never key
    // names).  These are the inverse: they change the KEY SET while keeping the
    // content valid JSON.  Kept local — the production keyset-preserving
    // mutation must not change (it is load-bearing for invalidation.cc /
    // precision.cc).
    static std::string addKey(const std::string & json, const std::string & newKey, const std::string & value)
    {
        auto obj = nlohmann::json::parse(json);
        obj[newKey] = value;
        return obj.dump();
    }
    static std::string removeKey(const std::string & json, const std::string & key)
    {
        auto obj = nlohmann::json::parse(json);
        obj.erase(key);
        return obj.dump();
    }
    static std::string renameKey(
        const std::string & json, const std::string & oldKey, const std::string & newKey)
    {
        auto obj = nlohmann::json::parse(json);
        auto v = obj.at(oldKey);
        obj.erase(oldKey);
        obj[newKey] = v;
        return obj.dump();
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Sink cases — keyset reaches the result ⇒ any keyset change MUST miss.
//
// These are the soundness FLOOR.  They must pass against the current recorder
// (proving the fail-closed baseline is sound) and must KEEP passing after the
// downgrade (proving the downgrade did not over-reach).  A downgrade that turns
// any of these green->red is rejected.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, SinkAttrNames_KeyAdded_Invalidates)
{
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": "alpha", "b": "beta"})", "c", "gamma"));
    // attrNames materializes the keyset into the result: adding a key MUST
    // change the ground truth and MUST miss.
    EXPECT_TRUE(o.groundTruthChanged) << "attrNames result should include the new key";
    EXPECT_TRUE(o.missed) << "keyset materialized into result — must invalidate";
    assertNoStaleServe(o, "attrNames/add");
}

TEST_F(KeysetProvenanceTest, SinkAttrNames_KeyRemoved_Invalidates)
{
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, removeKey(R"({"a": "alpha", "b": "beta"})", "b"));
    EXPECT_TRUE(o.groundTruthChanged);
    EXPECT_TRUE(o.missed);
    assertNoStaleServe(o, "attrNames/remove");
}

TEST_F(KeysetProvenanceTest, SinkAttrNames_KeyRenamed_Invalidates)
{
    // Rename keeps cardinality equal — a length-only dep would wrongly hit.
    // This pins that the dep tracks key NAMES, not just count.
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, renameKey(R"({"a": "alpha", "b": "beta"})", "b", "z"));
    EXPECT_TRUE(o.groundTruthChanged) << "renamed key changes the names list";
    EXPECT_TRUE(o.missed);
    assertNoStaleServe(o, "attrNames/rename");
}

TEST_F(KeysetProvenanceTest, SinkLengthOfAttrNames_KeyAdded_Invalidates)
{
    // Cardinality into the result.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.length (builtins.attrNames ({}))", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": 1, "b": 2})", "c", "3"));
    EXPECT_TRUE(o.groundTruthChanged) << "count changes from 2 to 3";
    EXPECT_TRUE(o.missed);
    assertNoStaleServe(o, "length(attrNames)/add");
}

TEST_F(KeysetProvenanceTest, SinkConcatStringsSepOfAttrNames_KeyAdded_Invalidates)
{
    // Keyset stringified into the result.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format(
        R"(builtins.concatStringsSep "," (builtins.attrNames ({})))", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": 1, "b": 2})", "c", "3"));
    EXPECT_TRUE(o.groundTruthChanged) << "stringified keyset changes";
    EXPECT_TRUE(o.missed);
    assertNoStaleServe(o, "concatStringsSep(attrNames)/add");
}

// ═══════════════════════════════════════════════════════════════════════
// Non-sink / point-observed cases — soundness must always hold; precision is
// where the downgrade headroom lives.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, PointAccess_UnrelatedSiblingAdded_NoStaleServe)
{
    // Plain point access.  The accessed value at "a" does not change when an
    // unobserved sibling "c" is added, so ground truth is unchanged.  Whether
    // the cache hits today depends on whether plain access records a broad
    // keyset dep; either way SOUNDNESS must hold.  (This is the positive
    // control: it should already hit via the SC point-dep override, mirroring
    // attrkeys.cc::AttrNamesOnly_ValueChange_CacheHit.)
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format(R"(({}).a)", fj(file.path));

    auto o = runOracle(expr, file, addKey(R"({"a": "alpha", "b": "beta"})", "c", "gamma"));
    EXPECT_FALSE(o.groundTruthChanged) << "value at .a is independent of sibling c";
    assertNoStaleServe(o, "point-access/sibling-add");
}

TEST_F(KeysetProvenanceTest, PointAccessOfRemoveAttrs_UnobservedSiblingAdded_NoStaleServe)
{
    // Transform case, EMPIRICALLY ALREADY PRECISE.  `(removeAttrs j ["b"]).a`
    // records NO StructuredProjection #keys dep on j (verified: the only deps
    // are FileBytes + the point-access SP dep), so adding an unobserved sibling
    // "c" already HITS.  This is NOT a precision-headroom case — it documents
    // that the minimal removeAttrs-then-point-access shape does not exhibit the
    // work-log's broader-source-keyset over-recording at the unit level (that
    // over-recording is gated behind the Nixpkgs-callsite / command-JSON path,
    // not active in these fixtures).  The construction-only precision headroom
    // lives in the seq() shape below.  Asserted as SOUND either way.
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format(R"((builtins.removeAttrs ({}) ["b"]).a)", fj(file.path));

    auto o = runOracle(expr, file, addKey(R"({"a": "alpha", "b": "beta"})", "c", "gamma"));
    EXPECT_FALSE(o.groundTruthChanged) << "removeAttrs result at .a unchanged by sibling c";
    assertNoStaleServe(o, "removeAttrs/sibling-add");
}

// ── Precision pin (acceptance test for the downgrade) ────────────────────
//
// DISABLED until the provenance proof (the strict downgrade) lands.
//
// Shape: `builtins.seq (builtins.attrNames j) (j.a)`.  attrNames is FORCED for
// seq's side effect — so the recorder emits a StructuredProjection #keys
// (complete-keyset) dep on j — but its value is DISCARDED; only `j.a` reaches
// the result.  The keyset is therefore CONSTRUCTION-ONLY: it never reaches a
// result-visible sink.  This is precisely the obligation's downgrade target.
//
// EMPIRICALLY VERIFIED (DISABLED_DiagnoseOverRecordingShapes diagnostic, run
// 2026-05-29 against the pre-downgrade recorder):
//   * deps recorded: FileBytes, ImplicitStructure #keys, StructuredProjection
//     #keys (the complete-keyset dep), StructuredProjection ["a"] (the point
//     dep).  recordsSP#keys == 1.
//   * CURRENT behavior: adding an unobserved sibling "c" → ground truth
//     UNCHANGED (result is still j.a == "alpha") but the cache MISSES, because
//     the SP#keys dep invalidates on the keyset change.  This is the
//     fail-closed over-recording at the exact dep the downgrade removes.
//   * SOUNDNESS counterpart (verified): changing j.a's value → ground truth
//     CHANGED → MISS.  So the point dep correctly keeps the result honest.
//
// POST-DOWNGRADE behavior: this HITS.  The SP#keys dep on j is downgraded to
// per-observed-member deps (only "a" is point-observed), so a keyset-only
// change to an unobserved member no longer invalidates.
//
// This test's red->green flip is the acceptance criterion: enable it (drop the
// DISABLED_ prefix) when the downgrade lands.  If it ever passes BEFORE the
// downgrade, the soundness floor (the Sink* tests) has likely been weakened —
// investigate, do not just enable.  The vacuity guard below ensures the SP#keys
// dep is actually present, so a "pass" cannot come from the dep silently
// disappearing for an unrelated reason.
TEST_F(KeysetProvenanceTest, DISABLED_SeqDiscardedAttrNames_UnobservedSiblingAdded_StillHits)
{
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format(
        R"(builtins.seq (builtins.attrNames ({0})) (({0}).a))", fj(file.path));

    // The complete-keyset dep MUST be present, or the pin is vacuous.
    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": "alpha", "b": "beta"})", "c", "gamma"));
    EXPECT_FALSE(o.groundTruthChanged) << "result is j.a; discarded attrNames does not reach it";
    EXPECT_TRUE(o.primaryHit)
        << "PRECISION WIN: the attrNames keyset is construction-only (discarded by "
           "seq, never reaches a sink); an unobserved-sibling change should serve "
           "from cache after the downgrade.";
}

// Soundness guard tied to the precision pin: when the keyset IS construction-
// only (seq-discarded), the result still depends on j.a, so changing j.a's
// VALUE must always miss — both before AND after the downgrade.  Enabled: this
// must hold permanently (the downgrade removes the keyset dep, not the point
// dep).
TEST_F(KeysetProvenanceTest, SeqDiscardedAttrNames_AccessedValueChanged_Invalidates)
{
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format(
        R"(builtins.seq (builtins.attrNames ({0})) (({0}).a))", fj(file.path));

    // Change the value at the accessed key "a" (keyset unchanged).
    auto o = runOracle(expr, file, R"({"a": "ALPHA2", "b": "beta"})");
    EXPECT_TRUE(o.groundTruthChanged) << "j.a value changed";
    EXPECT_TRUE(o.missed) << "point dep on j.a must invalidate regardless of keyset downgrade";
    assertNoStaleServe(o, "seq-discarded/value-change");
}

// ═══════════════════════════════════════════════════════════════════════
// Keyset-SPECIFICITY — the sink miss is about the KEYSET, not blanket content.
//
// GAP 1 (adversarial pass).  If `attrNames` simply re-evaluated on any content
// change, the Sink* tests above would pass for the WRONG reason (the FileBytes
// content dep, not the keyset dep) — and would keep passing even if the
// downgrade deleted the keyset dep entirely.  This pins that a keyset-PRESERVING
// value change HITS: the invalidation in the Sink* tests is keyset-specific.
// EMPIRICAL: SP#keys=1, value-change → gtUnchanged, primary HIT.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, SinkAttrNames_ValueChangeKeepingKeys_StillHits)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    // Change values, keep the keyset: attrNames result is unchanged.
    auto o = runOracle(expr, file, R"({"a": 99, "b": 88})");
    EXPECT_FALSE(o.groundTruthChanged) << "attrNames is keyset-only; values don't reach it";
    EXPECT_TRUE(o.primaryHit)
        << "the attrNames miss must be KEYSET-specific — a value-only change must HIT, "
           "proving the Sink* invalidations are not blanket FileBytes content invalidation";
    assertNoStaleServe(o, "attrNames/value-change");
}

// ═══════════════════════════════════════════════════════════════════════
// v7 FLOOR — complete-keyset sink AND a positive point hasAttr on the SAME set.
//
// GAP 7 (adversarial pass).  This is the case v7 exists for: the result both
// materializes the keyset (concatStringsSep ∘ attrNames — a sink) AND probes a
// point key (hasAttr "a").  Adding an UNOBSERVED key "c" changes the ground
// truth (the stringified keyset gains "c").  The downgrade must NOT let the
// positive hasAttr("a") suppress the complete-keyset sink — doing so was exactly
// the v7 mistake.  This is a permanent SOUNDNESS floor (enabled, must hold
// before AND after the downgrade).  Distinct from the seq-discarded precision
// pin below, where attrNames does NOT reach the result.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, SinkAttrNamesPlusHasAttr_UnobservedKeyAdded_Invalidates)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format(
        R"(let j = ({0}); in )"
        R"((builtins.concatStringsSep "," (builtins.attrNames j)) + (if j ? "a" then "!" else "?"))",
        fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": 1, "b": 2})", "c", "3"));
    EXPECT_TRUE(o.groundTruthChanged) << "stringified keyset gains 'c'";
    EXPECT_TRUE(o.missed) << "complete-keyset sink must invalidate even with a positive point hasAttr";
    assertNoStaleServe(o, "v7-floor/attrNames+hasAttr/add");
}

// ═══════════════════════════════════════════════════════════════════════
// NEGATIVE hasAttr — an OBSERVED ABSENCE that later becomes present.
//
// GAP 6 (adversarial pass).  `if (j ? "z") ...` observes that "z" is ABSENT.
// Adding "z" flips the branch, changing the ground truth.  EMPIRICAL: this
// records NO complete-keyset dep (SP#keys=0) — only a point `#has(z)` dep — yet
// it correctly invalidates.  Pins that absence observations are point deps that
// the downgrade must preserve (a member-deps-only downgrade keeps point #has
// deps; this guards that it does not accidentally drop the NEGATIVE one).
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, NegativeHasAttr_ProbedAbsentKeyAppears_Invalidates)
{
    TempJsonFile file(R"({"a": 1})");
    auto expr = std::format(R"(if (({}) ? "z") then "present" else "absent")", fj(file.path));

    auto o = runOracle(expr, file, addKey(R"({"a": 1})", "z", "9"));
    EXPECT_TRUE(o.groundTruthChanged) << "absent 'z' becomes present → branch flips";
    EXPECT_TRUE(o.missed) << "observed absence is a point dep that must invalidate when the key appears";
    assertNoStaleServe(o, "neg-hasAttr/absent-becomes-present");
}

// ═══════════════════════════════════════════════════════════════════════
// ENUMERATION SINKS — the keyset selects WHICH members flow into the result.
//
// GAP 8/9/10 (adversarial pass).  These are the glob-enumeration shapes the
// research review (project_eval_trace_glob_research_review) flagged as the
// downgrade's sharpest trap: every member is read pointwise, but the KEYSET is
// genuinely result-determining (adding a key adds a value to the output).  A
// member-deps-only downgrade that treated these as construction-only would
// STALE-SERVE.  EMPIRICAL: all three record SP#keys=1 and correctly invalidate
// on a key add.  These are permanent SOUNDNESS floors.
//
// CRITICAL HARNESS NOTE.  Soundness here is asserted by VALUE comparison
// (assertNoStaleServe over the deep-forced served value), NOT by the counter.
// See the Oracle comment: a counter-only oracle FALSE-FAILS the enumeration
// value-change case below, because a root-spine primary hit still re-derives the
// element thunks per-leaf (DEF-6).  The value comparison is the seam; the
// counter is only the precision part.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, EnumSinkAttrValues_KeyAdded_Invalidates)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrValues ({})", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": 1, "b": 2})", "c", "3"));
    EXPECT_TRUE(o.groundTruthChanged) << "attrValues gains the new key's value";
    EXPECT_TRUE(o.missed) << "enumeration keyset is result-determining — must invalidate";
    assertNoStaleServe(o, "attrValues/key-add");
}

TEST_F(KeysetProvenanceTest, EnumSinkMapOverAttrNames_KeyAdded_Invalidates)
{
    // Hand-rolled enumeration: attrNames drives the map, each j.${k} is a
    // dynamic point access.  The shape most likely to fool a member-deps-only
    // downgrade (every access looks like a point dep).
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format(
        R"(let j = ({0}); in builtins.map (k: j.${{k}}) (builtins.attrNames j))", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": 1, "b": 2})", "c", "3"));
    EXPECT_TRUE(o.groundTruthChanged) << "the mapped list gains an element";
    EXPECT_TRUE(o.missed) << "attrNames-driven enumeration must invalidate on key add";
    assertNoStaleServe(o, "map-attrNames/key-add");
}

TEST_F(KeysetProvenanceTest, EnumSinkLengthOfAttrValues_KeyAdded_Invalidates)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.length (builtins.attrValues ({}))", fj(file.path));

    assertCompleteKeysetDepRecorded(expr);

    auto o = runOracle(expr, file, addKey(R"({"a": 1, "b": 2})", "c", "3"));
    EXPECT_TRUE(o.groundTruthChanged) << "count changes from 2 to 3";
    EXPECT_TRUE(o.missed);
    assertNoStaleServe(o, "length(attrValues)/key-add");
}

// THE HEADLINE of the adversarial pass.  attrValues with a VALUE change (keyset
// preserved): the result list of VALUES changes, so the served value MUST be
// the new one.  EMPIRICAL: the counter reports primaryHit=1, miss=0 — yet the
// deep-forced served value equals the NEW ground truth (servedEqOld=0,
// servedEqNew=1).  The root-spine hit re-derives the element thunks per-leaf, so
// the value is correct despite the "hit".  A counter-only oracle would FALSE-
// FAIL this as a stale serve; the value-comparing oracle correctly passes it.
// This test is both (a) a soundness floor (the served value is never stale) and
// (b) the regression guard for the harness's own value-vs-counter distinction.
TEST_F(KeysetProvenanceTest, EnumSinkAttrValues_ValueChangedKeepingKeys_NoStaleServe)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrValues ({})", fj(file.path));

    auto o = runOracle(expr, file, R"({"a": 9, "b": 8})");
    EXPECT_TRUE(o.groundTruthChanged) << "attrValues yields the changed values";
    // No assertion on primaryHit/missed: per-leaf laziness makes the COUNTER
    // path implementation-defined here.  The seam is the value:
    assertNoStaleServe(o, "attrValues/value-change");
}

// ═══════════════════════════════════════════════════════════════════════
// DERIVED-KEYSET SINK — the keyset of a derived set reaches the result.
//
// GAP 11 (adversarial pass).  `attrNames (removeAttrs j ["b"])`: the keyset of
// the derived (post-removal) set is the result.  EMPIRICAL: this records NO
// complete-keyset SP#keys dep on j (so no vacuity guard — it would fail) but is
// still backstopped: a key ADD invalidates (GAP 11), and a value-only change
// HITS (GAP 11b — the derived keyset is value-independent).  Pins that the
// derived-keyset sink is sound WITHOUT relying on a complete-keyset dep on the
// original source — i.e. the downgrade (which targets SP#keys) does not touch
// the mechanism that protects this shape.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, DerivedSinkAttrNamesOfRemoveAttrs_KeyAdded_Invalidates)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames (builtins.removeAttrs ({}) [\"b\"])", fj(file.path));

    // NOTE: deliberately NOT vacuity-guarded — this shape records no SP#keys on
    // the source (verified in the adversarial pass); its soundness comes from
    // ImplicitStructure #keys + content, not the complete-keyset dep.
    auto o = runOracle(expr, file, addKey(R"({"a": 1, "b": 2})", "c", "3"));
    EXPECT_TRUE(o.groundTruthChanged) << "derived keyset gains 'c' (not removed)";
    EXPECT_TRUE(o.missed);
    assertNoStaleServe(o, "attrNames(removeAttrs)/key-add");
}

TEST_F(KeysetProvenanceTest, DerivedSinkAttrNamesOfRemoveAttrs_ValueChangeKeepingKeys_StillHits)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames (builtins.removeAttrs ({}) [\"b\"])", fj(file.path));

    auto o = runOracle(expr, file, R"({"a": 9, "b": 8})");
    EXPECT_FALSE(o.groundTruthChanged) << "derived keyset is value-independent";
    EXPECT_TRUE(o.primaryHit)
        << "the derived-keyset sink is keyset-protected — a value-only change must HIT";
    assertNoStaleServe(o, "attrNames(removeAttrs)/value-change");
}

// ═══════════════════════════════════════════════════════════════════════
// NON-JSON STRUCTURED SOURCES — readDir (no FileBytes content backstop) and
// multi-source // merge.  Use the generic runOracleWith() core since the source
// is not a single TempJsonFile.
//
// GAP 4 / GAP 5 (adversarial pass).  readDir is the by-name MOTIVATING case for
// the whole keyset-provenance work: there is no source FILE whose bytes
// backstop the enumeration, so the DirectoryEntries keyset dep is the ONLY thing
// standing between a key add and a stale serve.  EMPIRICAL: both invalidate
// correctly.  Permanent SOUNDNESS floors.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KeysetProvenanceTest, SinkReadDirAttrNames_EntryAdded_Invalidates)
{
    TempDir dir;
    dir.addFile("keep.txt", "x");
    auto expr = std::format("builtins.attrNames ({})", rd(dir.path()));

    auto o = runOracleWith(expr, [&] {
        dir.addFile("added.txt", "y");
        INVALIDATE_DIR(dir);
    });
    EXPECT_TRUE(o.groundTruthChanged) << "readDir keyset gains 'added.txt'";
    EXPECT_TRUE(o.missed) << "DirectoryEntries keyset is the only backstop — must invalidate";
    assertNoStaleServe(o, "readDir/attrNames/entry-add");
}

TEST_F(KeysetProvenanceTest, SinkMergeAttrNames_KeyAddedToOperand_Invalidates)
{
    TempJsonFile fa(R"({"a": 1})");
    TempJsonFile fb(R"({"b": 2})");
    auto expr = std::format("builtins.attrNames (({}) // ({}))", fj(fa.path), fj(fb.path));

    auto o = runOracleWith(expr, [&] {
        fa.modify(R"({"a": 1, "c": 3})");
        invalidateFileCache(fa.path);
    });
    EXPECT_TRUE(o.groundTruthChanged) << "merged keyset gains 'c'";
    EXPECT_TRUE(o.missed);
    assertNoStaleServe(o, "merge//attrNames/key-add");
}

} // namespace nix::eval_trace
