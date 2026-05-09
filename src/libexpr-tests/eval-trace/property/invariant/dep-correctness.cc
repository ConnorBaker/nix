// Dep-correctness properties: verify exact dependency tracking using the
// generator's DepSlot::Contribution oracle (Result / Recorded / Conditional).
//
// Each generator annotates its dep slots:
//   Result:      content flows into the result. Dep MUST be in the stored trace.
//                For Kind::File, mutation MUST invalidate the cache.
//   Recorded:    evaluator reads the file as part of producing the result (dep IS
//                recorded) but the slot's content may not directly determine the
//                result value. Dep MUST be in the stored trace. Mutation may NOT
//                invalidate (SC override may serve cache).
//   Absent:      the slot is forced as a side effect but the dep is NOT recorded
//                in the stored trace. The dep MUST NOT appear in stored deps.
//   Conditional: may or may not be read depending on runtime state.
//
// Tests:
//   1. AllTrackedSlots_Present  — Result + Recorded slots appear in stored deps
//   2. AbsentSlots_NotPresent   — Absent slots do NOT appear in stored deps
//   3. ResultSlots_Invalidate   — mutating a Result/File slot causes cache miss
//   4. RecordedSlots_NoInvalidate — mutating a Recorded slot doesn't invalidate
//   5. KindCQK_Consistency      — Kind→CQK type mapping for present slots
//   6. NoPhantomDeps            — every stored dep traces to a known slot
//   7. CrossSession_Identity    — stored deps identical across session boundary
//   8. SelectiveForcing         — each attr forced independently; only its deps appear
//   9. CrossSession_Recovery    — 3-session chain: record v1→mutate→record v2→restore→recover v1
//  10. CrossSession_Precision   — unrelated file change across session boundary → cache hit
//  11. CrossSession_RecordedSlotPrecision — SC override survives session boundary
//  12. CrossSession_MultiSlotInvalidation — per-slot mutation→miss→restore→recover across sessions
//  13. Annotation_Consistency — verify Contribution annotations match actual stored dep presence (200 iters)
//  14. Conditional_DepPresence — guard exists → data dep present; guard missing → data dep absent
//  15. ForceDoesNotLeak — forcing one attr doesn't leak deps for unforced attrs

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include "nix/expr/eval-trace/deps/types.hh"

#include <nlohmann/json.hpp>
#include <set>
#include <unordered_set>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;
using ResolvedDep = SqliteTraceStorage::ResolvedDep;
using Contribution = DepSlot::Contribution;

// ═══════════════════════════════════════════════════════════════════════
// Fixture
// ═══════════════════════════════════════════════════════════════════════

class EvalTraceProperty_DepCorrectness : public MaterializationDepTest {
public:
    EvalTraceProperty_DepCorrectness() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-dep-correctness");
    }

    /// Collect ALL resolved deps from the stored trace tree rooted at `attrPath`,
    /// plus child paths listed in `childPaths` (for attrset/list results whose
    /// children are stored at independent attr-path entries, not linked via
    /// TraceValueContext).
    std::vector<ResolvedDep> collectAllStoredDeps(
        const std::string & attrPath,
        const std::vector<std::string> & childPaths = {})
    {
        releaseActiveSession();
        // Use the per-expression fingerprint (§N.1). See
        // `TraceCacheFixture::lastPerExprFingerprint_`.
        auto & fp = lastPerExprFingerprint_.has_value()
                      ? *lastPerExprFingerprint_ : testFingerprint;
        auto bootstrapKey = SemanticSessionKey::fromSerialized(
            "bootstrap:" + fp.to_string(HashFormat::Base16, false));
        SqliteTraceStorage db(state.symbols, state.tracingPools(),
            state.vocabStore(), std::move(bootstrapKey));

        std::vector<ResolvedDep> allDeps;
        std::unordered_set<AttrPathId, AttrPathId::Hash> visited;
        SemanticRegistry emptyRegistry;
        VerificationSession session;

        // Collect from root.
        collectDepsRecursive(db, pathFromDotted(attrPath), allDeps, visited,
            emptyRegistry, session);

        // Collect from explicitly listed child paths.
        for (auto & child : childPaths) {
            auto childPath = attrPath.empty() ? child : attrPath + "." + child;
            collectDepsRecursive(db, pathFromDotted(childPath), allDeps, visited,
                emptyRegistry, session);
        }

        return allDeps;
    }

private:
    void collectDepsRecursive(
        SqliteTraceStorage & db, AttrPathId pathId,
        std::vector<ResolvedDep> & allDeps,
        std::unordered_set<AttrPathId, AttrPathId::Hash> & visited,
        SemanticRegistry & registry,
        VerificationSession & session)
    {
        if (!visited.insert(pathId).second) return;
        auto result = test::TraceStorageTestAccess::verify(
            db, pathId, registry, state, session);
        if (!result) return;
        auto rawDeps = withExclusiveStore(db, [&](const auto & ea) {
            return db.loadFullTrace(ea, result->traceId);
        });
        for (const auto & dep : *rawDeps) {
            auto resolved = db.resolveDep(dep);
            if (resolved.traceContext
                && (resolved.type == CanonicalQueryKind::TraceValueContext
                    || resolved.type == CanonicalQueryKind::TraceParentSlot))
                collectDepsRecursive(db, resolved.traceContext->pathId, allDeps,
                    visited, registry, session);
            allDeps.push_back(std::move(resolved));
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════

static std::set<std::string> extractDepPaths(const std::vector<ResolvedDep> & deps)
{
    std::set<std::string> paths;
    for (auto & d : deps) {
        if (d.structured) {
            if (!d.structured->filePath.empty())
                paths.insert(d.structured->filePath);
            continue;
        }
        if (d.type == CanonicalQueryKind::FileBytes
            || d.type == CanonicalQueryKind::ExistenceCheck
            || d.type == CanonicalQueryKind::DirectoryEntries)
            if (!d.key.empty())
                paths.insert(d.key);
    }
    return paths;
}

static std::set<std::string> extractDepEnvVars(const std::vector<ResolvedDep> & deps)
{
    std::set<std::string> vars;
    for (auto & d : deps)
        if (d.type == CanonicalQueryKind::EnvironmentLookup)
            vars.insert(d.key);
    return vars;
}

static size_t countByType(const std::vector<ResolvedDep> & deps, CanonicalQueryKind type)
{
    size_t n = 0;
    for (auto & d : deps) if (d.type == type) n++;
    return n;
}

static bool anyKeyContains(
    const std::vector<ResolvedDep> & deps, CanonicalQueryKind type,
    const std::string & substr)
{
    for (auto & d : deps)
        if (d.type == type && d.key.find(substr) != std::string::npos)
            return true;
    return false;
}

static bool hasStructuredDepForPath(
    const std::vector<ResolvedDep> & deps, const std::string & pathStr)
{
    for (auto & d : deps) {
        if (!d.structured) continue;
        if (d.type != CanonicalQueryKind::StructuredProjection
            && d.type != CanonicalQueryKind::ImplicitStructure) continue;
        if (d.structured->filePath.find(pathStr) != std::string::npos)
            return true;
    }
    return false;
}

static bool pathInDeps(const std::vector<ResolvedDep> & deps, const std::string & pathStr)
{
    for (auto & dp : extractDepPaths(deps))
        if (dp.find(pathStr) != std::string::npos || pathStr.find(dp) != std::string::npos)
            return true;
    return false;
}

/// Force root, then walk each declared dot-separated attr path and force
/// the value at each step.  Also forces declared list indices.
/// This records deps for exactly the paths the expression accesses — no more.
static void forceRootAndChildren(
    EvalState & state,
    TraceSession & cache,
    const TestExpr & expr)
{
    auto * root = cache.getRootValue();
    state.forceValue(*root, noPos);

    // Walk each dot-separated attr path from the root.
    for (auto & dotPath : expr.attrsToForce) {
        Value * cur = root;
        std::string::size_type pos = 0;
        while (pos < dotPath.size()) {
            if (cur->type() != nAttrs) break;
            auto dot = dotPath.find('.', pos);
            auto component = dotPath.substr(pos, dot == std::string::npos ? dot : dot - pos);
            auto * attr = cur->attrs()->get(state.symbols.create(component));
            if (!attr || !attr->value) break;
            state.forceValue(*attr->value, noPos);
            cur = attr->value;
            pos = dot == std::string::npos ? dotPath.size() : dot + 1;
        }
    }

    // Force declared list indices.
    if (root->isList()) {
        auto listView = root->listView();
        for (auto idx : expr.indicesToForce) {
            if (idx < root->listSize())
                state.forceValue(*listView[idx], noPos);
        }
    }
}

/// Build the list of child attr-path strings to query for stored deps.
static std::vector<std::string> childPathsForExpr(const TestExpr & expr)
{
    std::vector<std::string> paths;
    for (auto & name : expr.attrsToForce)
        paths.push_back(name);
    for (auto idx : expr.indicesToForce)
        paths.push_back(std::to_string(idx));
    return paths;
}

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    params.maxDiscardRatio = 200;
    return params;
}

// Higher iteration count for tests that need to hit every generator.
static rc::detail::TestParams makeThoroughParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 200;
    params.maxDiscardRatio = 200;
    return params;
}

// ═══════════════════════════════════════════════════════════════════════
// Test 1: AllTrackedSlots_Present
//
// For every expression: every Result and Recorded slot MUST appear in
// the stored trace tree. Conditional slots are checked for type
// consistency when present.
// Runs across the full makeNixExprGen() — all 37+ generators.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, AllTrackedSlots_Present)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-tracked-present-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!expr.depSlots.empty());
            RC_PRE(!state.settings.pureEval);

            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            auto allDeps = collectAllStoredDeps("", childPathsForExpr(expr));

            for (auto & slot : expr.depSlots) {
                // Conditional and Absent slots: skip presence assertion.
                if (slot.contribution == Contribution::Conditional
                    || slot.contribution == Contribution::Absent)
                    continue;

                // Result and Recorded slots MUST appear.
                std::string sp = slot.path.string();

                switch (slot.kind) {
                case DepSlot::Kind::File:
                    RC_ASSERT(anyKeyContains(allDeps, CanonicalQueryKind::FileBytes, sp));
                    break;
                case DepSlot::Kind::EnvVar:
                    RC_ASSERT(anyKeyContains(allDeps, CanonicalQueryKind::EnvironmentLookup, slot.envVarName));
                    break;
                case DepSlot::Kind::JsonFile:
                case DepSlot::Kind::JsonArray:
                case DepSlot::Kind::TomlFile:
                    RC_ASSERT(anyKeyContains(allDeps, CanonicalQueryKind::FileBytes, sp));
                    break;
                case DepSlot::Kind::FileExistence:
                    RC_ASSERT(anyKeyContains(allDeps, CanonicalQueryKind::ExistenceCheck, sp));
                    break;
                case DepSlot::Kind::DirectoryEntries:
                    RC_ASSERT(anyKeyContains(allDeps, CanonicalQueryKind::DirectoryEntries, sp));
                    break;
                }
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 2: AbsentSlots_NotPresent
//
// For expressions with Absent slots (multiBindingLet): those slots
// MUST NOT appear in the stored trace tree. This proves the trace
// system is precise about what the result depends on — side-effect-
// forced bindings that don't contribute to the result are excluded.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, AbsentSlots_NotPresent)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            // Draw from all generators that have Absent slots.
            auto expr = *rc::gen::oneOf(
                makeMultiBindingLetGen(),
                makeNestedAttrsetAccessGen(),
                makeDeepAttrsetAccessGen());

            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-absent-slots-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!state.settings.pureEval);

            bool hasAbsent = false;
            for (auto & slot : expr.depSlots)
                if (slot.contribution == Contribution::Absent)
                    hasAbsent = true;
            RC_PRE(hasAbsent);

            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            auto allDeps = collectAllStoredDeps("", childPathsForExpr(expr));

            for (auto & slot : expr.depSlots) {
                if (slot.contribution != Contribution::Absent)
                    continue;

                std::string sp = slot.path.string();
                RC_ASSERT(!pathInDeps(allDeps, sp));
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 3 (was 2): ResultSlots_Invalidate
//
// For single-slot Result/File expressions: mutation MUST invalidate.
// This is the soundness direction for Result deps.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, ResultSlots_Invalidate)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-result-inval-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() == 1);
            RC_PRE(!state.settings.pureEval);

            auto & slot = expr.depSlots[0];
            RC_PRE(slot.contribution == Contribution::Result);
            RC_PRE(slot.kind == DepSlot::Kind::File);

            auto newValue = *slot.generateMutation();
            RC_PRE(newValue != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate.
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Must invalidate.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 3: RecordedSlots_NoInvalidate
//
// For multi-slot expressions with Recorded slots: mutating a Recorded
// Kind::File slot MUST NOT invalidate the cache. The dep is recorded
// but the SC override correctly serves the cached result because the
// slot's content doesn't flow into the accessed key's value.
//
// This is the precision direction: the trace system correctly
// distinguishes deps that affect the result from deps that don't.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, RecordedSlots_NoInvalidate)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            auto expr = *rc::gen::oneOf(
                makeMultiBindingLetGen(),
                makeMultiSourceAttrGen(),
                makeTripleSourceMergeGen(),
                makeIntersectAccessGen());

            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-recorded-noinval-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!state.settings.pureEval);

            // Must have at least one Recorded slot.
            bool hasRecorded = false;
            for (auto & slot : expr.depSlots)
                if (slot.contribution == Contribution::Recorded)
                    hasRecorded = true;
            RC_PRE(hasRecorded);

            // Cold eval.
            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            // Warm baseline.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Mutate each Recorded slot → must NOT invalidate.
            // For Kind::JsonFile, the mutation must preserve the JSON key set
            // so the ImplicitStructure #keys dep still passes.
            for (auto & slot : expr.depSlots) {
                if (slot.contribution != Contribution::Recorded)
                    continue;

                auto newValue = *slot.generateMutation();
                RC_PRE(newValue != slot.currentValue);

                // Guard: mutation must preserve JSON key names. If RC shrinking
                // produced a fallback mutation with different keys, discard.
                if (slot.kind == DepSlot::Kind::JsonFile
                    || slot.kind == DepSlot::Kind::JsonArray)
                {
                    auto origJson = nlohmann::json::parse(slot.currentValue, nullptr, false);
                    auto newJson = nlohmann::json::parse(newValue, nullptr, false);
                    if (origJson.is_object() && newJson.is_object()) {
                        std::set<std::string> origKeys, newKeys;
                        for (auto & [k, _] : origJson.items()) origKeys.insert(k);
                        for (auto & [k, _] : newJson.items()) newKeys.insert(k);
                        RC_PRE(origKeys == newKeys);
                    }
                }

                slot.mutate(newValue);
                if (slot.kind != DepSlot::Kind::EnvVar)
                    invalidateFileCache(slot.path);

                {
                    PathCountersSnapshot snap;
                    auto cache = makeCache(expr.nixCode);
                    (void) forceRoot(*cache);
                    RC_ASSERT(snap.primaryCacheServedOnly());
                }

                slot.restore();
                if (slot.kind != DepSlot::Kind::EnvVar)
                    invalidateFileCache(slot.path);
                // Re-record clean trace.
                {
                    auto cache = makeCache(expr.nixCode);
                    (void) forceRoot(*cache);
                }
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 4: KindCQK_Consistency
//
// For every expression: for every slot present in stored deps, the
// Kind→CQK type mapping must be correct:
//   File → no structured deps
//   JsonFile/JsonArray/TomlFile → has structured deps
//   DirectoryEntries → has structured deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, KindCQK_Consistency)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-kind-cqk-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!expr.depSlots.empty());
            RC_PRE(!state.settings.pureEval);

            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            auto allDeps = collectAllStoredDeps("", childPathsForExpr(expr));

            for (auto & slot : expr.depSlots) {
                std::string sp = slot.path.string();
                if (!pathInDeps(allDeps, sp))
                    continue;

                switch (slot.kind) {
                case DepSlot::Kind::File:
                    RC_ASSERT(!hasStructuredDepForPath(allDeps, sp));
                    break;
                case DepSlot::Kind::JsonFile:
                case DepSlot::Kind::JsonArray:
                case DepSlot::Kind::TomlFile:
                    RC_ASSERT(hasStructuredDepForPath(allDeps, sp));
                    break;
                case DepSlot::Kind::DirectoryEntries:
                    RC_ASSERT(hasStructuredDepForPath(allDeps, sp));
                    break;
                case DepSlot::Kind::EnvVar:
                case DepSlot::Kind::FileExistence:
                    break;
                }
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 5: NoPhantomDeps
//
// Every file-path-bearing stored dep must trace to a DepSlot path or
// a path literal in nixCode.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, NoPhantomDeps)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-no-phantom-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!expr.depSlots.empty());
            RC_PRE(!state.settings.pureEval);

            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            auto allDeps = collectAllStoredDeps("", childPathsForExpr(expr));
            auto depPaths = extractDepPaths(allDeps);

            std::set<std::string> knownPaths;
            for (auto & slot : expr.depSlots)
                if (slot.kind != DepSlot::Kind::EnvVar)
                    knownPaths.insert(slot.path.string());

            for (auto & dp : depPaths) {
                bool known = false;
                for (auto & kp : knownPaths)
                    if (dp.find(kp) != std::string::npos || kp.find(dp) != std::string::npos) {
                        known = true;
                        break;
                    }
                if (!known)
                    known = expr.nixCode.find(dp) != std::string::npos;
                RC_ASSERT(known);
            }

            for (auto & d : allDeps) {
                if (d.type != CanonicalQueryKind::EnvironmentLookup) continue;
                bool known = false;
                for (auto & slot : expr.depSlots)
                    if (slot.kind == DepSlot::Kind::EnvVar
                        && d.key.find(slot.envVarName) != std::string::npos)
                        known = true;
                RC_ASSERT(known);
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 6: CrossSession_Identity
//
// Stored deps from the full trace tree must be identical across a
// session boundary: same count, same per-CQK counts, same file paths,
// same env var names.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, CrossSession_Identity)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-cross-session-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!expr.depSlots.empty());
            RC_PRE(!state.settings.pureEval);

            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }
            auto deps1 = collectAllStoredDeps("", childPathsForExpr(expr));
            auto paths1 = extractDepPaths(deps1);
            auto vars1 = extractDepEnvVars(deps1);

            simulateWarmRestart();

            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                // Case D: after simulateWarmRestart() the primary session
                // slot is dropped, so the warm eval legitimately goes
                // through History-recovery. Accept either path.
                RC_ASSERT(snap.deltaTraceCacheHits() >= 1);
            }

            auto deps2 = collectAllStoredDeps("", childPathsForExpr(expr));
            auto paths2 = extractDepPaths(deps2);
            auto vars2 = extractDepEnvVars(deps2);

            RC_ASSERT(deps1.size() == deps2.size());
            RC_ASSERT(paths1 == paths2);
            RC_ASSERT(vars1 == vars2);

            for (int cqk = 0; cqk <= static_cast<int>(CanonicalQueryKind::VolatileTime); ++cqk) {
                auto kind = static_cast<CanonicalQueryKind>(cqk);
                RC_ASSERT(countByType(deps1, kind) == countByType(deps2, kind));
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 8: SelectiveForcing_EachAttrIndependently
//
// Using selectiveAttrsetGen (3 attrs: plain/File, structured/JsonFile,
// env/EnvVar): for each attr, force ONLY that attr and verify that ONLY
// its dep slot appears. The other attrs' deps MUST NOT appear.
//
// Tests all three selective forcing patterns, not just one.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, SelectiveForcing_EachAttrIndependently)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            auto expr = *makeSelectiveAttrsetGen();

            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() == 3);
            RC_PRE(!state.settings.pureEval);

            std::string plainPath = expr.depSlots[0].path.string();
            std::string jsonPath = expr.depSlots[1].path.string();
            std::string envName = expr.depSlots[2].envVarName;

            // Helper: force one attr, collect deps, verify.
            auto forceOneAndCheck = [&](const std::string & attrName) {
                simulateWarmRestart();
                testFingerprint = hashString(
                    HashAlgorithm::SHA256,
                    "prop-selective-" + attrName + "-" + std::to_string(iteration++));

                {
                    auto cache = makeCache(expr.nixCode);
                    auto * root = cache->getRootValue();
                    state.forceValue(*root, noPos);
                    auto * attr = root->attrs()->get(state.symbols.create(attrName));
                    RC_PRE(attr && attr->value);
                    state.forceValue(*attr->value, noPos);
                }

                return collectAllStoredDeps("", {attrName});
            };

            // ── Force only "plain" ──
            {
                auto deps = forceOneAndCheck("plain");
                RC_ASSERT(anyKeyContains(deps, CanonicalQueryKind::FileBytes, plainPath));
                RC_ASSERT(!pathInDeps(deps, jsonPath));
                auto envVars = extractDepEnvVars(deps);
                RC_ASSERT(envVars.find(envName) == envVars.end());
            }

            // ── Force only "structured" ──
            {
                auto deps = forceOneAndCheck("structured");
                RC_ASSERT(anyKeyContains(deps, CanonicalQueryKind::FileBytes, jsonPath));
                RC_ASSERT(hasStructuredDepForPath(deps, jsonPath));
                RC_ASSERT(!anyKeyContains(deps, CanonicalQueryKind::FileBytes, plainPath));
                auto envVars = extractDepEnvVars(deps);
                RC_ASSERT(envVars.find(envName) == envVars.end());
            }

            // ── Force only "env" ──
            {
                auto deps = forceOneAndCheck("env");
                auto envVars = extractDepEnvVars(deps);
                RC_ASSERT(envVars.find(envName) != envVars.end());
                RC_ASSERT(!anyKeyContains(deps, CanonicalQueryKind::FileBytes, plainPath));
                RC_ASSERT(!pathInDeps(deps, jsonPath));
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 9: CrossSession_InvalidationAndRecovery
//
// Three-session chain that models the real nixpkgs workflow:
//   Session 1: cold eval with v1, record trace
//   (mutate a dep)
//   Session 2: warm verify → miss (dep changed), cold eval with v2, re-record
//   (restore dep to v1)
//   Session 3: warm verify → recover v1 from history, loaderCalls == 0
//
// Verifies that constructive recovery works across session boundaries
// and that the recovered result matches the original v1 result.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, CrossSession_InvalidationAndRecovery)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-cross-recovery-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() == 1);
            RC_PRE(!state.settings.pureEval);

            auto & slot = expr.depSlots[0];
            RC_PRE(slot.contribution == Contribution::Result);
            RC_PRE(slot.kind == DepSlot::Kind::File);

            auto newValue = *slot.generateMutation();
            RC_PRE(newValue != slot.currentValue);

            // ── Session 1: cold eval with v1 ──
            Value r1;
            {
                auto cache = makeCache(expr.nixCode);
                r1 = forceRoot(*cache);
            }

            // Verify deps are stored.
            auto deps1 = collectAllStoredDeps("", childPathsForExpr(expr));
            std::string sp = slot.path.string();
            RC_ASSERT(anyKeyContains(deps1, CanonicalQueryKind::FileBytes, sp));

            // ── Mutate to v2 ──
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // ── Session 2: warm verify → miss, then cold eval with v2 ──
            simulateWarmRestart();

            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Verify v2 deps are also stored.
            auto deps2 = collectAllStoredDeps("", childPathsForExpr(expr));
            RC_ASSERT(anyKeyContains(deps2, CanonicalQueryKind::FileBytes, sp));

            // ── Restore to v1 ──
            slot.restore();
            invalidateFileCache(slot.path);

            // ── Session 3: warm verify → recover v1 from history ──
            simulateWarmRestart();

            Value rRecovered;
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                rRecovered = forceRoot(*cache);
                // After restoring v1 across a session boundary, a cached
                // result must serve — either directly (primary hit if the
                // restored trace still verifies) or via recovery. The
                // value-equality check below is the real correctness
                // assertion; forbid silent loader re-invocation.
                RC_ASSERT(snap.deltaTraceCacheHits() >= 1);
            }
            assertValuesEqual(r1, rRecovered);

            // Deps after recovery must match session 1's deps.
            auto deps3 = collectAllStoredDeps("", childPathsForExpr(expr));
            auto paths1 = extractDepPaths(deps1);
            auto paths3 = extractDepPaths(deps3);
            RC_ASSERT(paths1 == paths3);
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 10: CrossSession_Precision
//
// Verify that precision survives session boundaries:
//   Session 1: cold eval with expression depending on file A
//   (create an unrelated file B, modify it)
//   Session 2: warm verify → must hit cache (file B is not a dep)
//
// This catches bugs where session-boundary cache reconstruction drops
// precision info, causing spurious invalidation for unrelated changes.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, CrossSession_Precision)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr, std::string unrelatedContent) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-cross-precision-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!expr.depSlots.empty());
            RC_PRE(!state.settings.pureEval);

            // ── Session 1: cold eval ──
            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            // Verify stored deps exist.
            auto deps1 = collectAllStoredDeps("", childPathsForExpr(expr));
            RC_PRE(!extractDepPaths(deps1).empty()
                || !extractDepEnvVars(deps1).empty());

            // ── Create and mutate an unrelated file ──
            TempTextFile unrelated(unrelatedContent);
            unrelated.modify(unrelatedContent + "_changed");
            invalidateFileCache(unrelated.path);

            // ── Session 2: warm verify after session boundary ──
            simulateWarmRestart();

            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                // Must still hit — unrelated file is not in the dep set.
                // Case D: after simulateWarmRestart() the primary slot is
                // dropped, so History-recovery is a legitimate path here.
                RC_ASSERT(snap.deltaTraceCacheHits() >= 1);
            }

            // Stored deps must be unchanged.
            auto deps2 = collectAllStoredDeps("", childPathsForExpr(expr));
            RC_ASSERT(extractDepPaths(deps1) == extractDepPaths(deps2));
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 11: CrossSession_RecordedSlotPrecision
//
// Mutate a Recorded slot ACROSS a session boundary — must still not
// invalidate. The SC override precision must survive session
// reconstruction.
//
// Session 1: cold eval → warm hit (baseline)
// Session 2: mutate Recorded slot (key-preserving) → warm verify hits
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, CrossSession_RecordedSlotPrecision)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            auto expr = *rc::gen::oneOf(
                makeMultiSourceAttrGen(),
                makeTripleSourceMergeGen(),
                makeIntersectAccessGen());

            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-cross-recorded-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!state.settings.pureEval);

            bool hasRecorded = false;
            for (auto & slot : expr.depSlots)
                if (slot.contribution == Contribution::Recorded)
                    hasRecorded = true;
            RC_PRE(hasRecorded);

            // Session 1: cold eval.
            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            // Session 1: warm hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Mutate each Recorded slot with key-preserving mutation.
            for (auto & slot : expr.depSlots) {
                if (slot.contribution != Contribution::Recorded) continue;

                auto newValue = *slot.generateMutation();
                RC_PRE(newValue != slot.currentValue);

                if (slot.kind == DepSlot::Kind::JsonFile
                    || slot.kind == DepSlot::Kind::JsonArray) {
                    auto origJson = nlohmann::json::parse(slot.currentValue, nullptr, false);
                    auto newJson = nlohmann::json::parse(newValue, nullptr, false);
                    if (origJson.is_object() && newJson.is_object()) {
                        std::set<std::string> origKeys, newKeys;
                        for (auto & [k, _] : origJson.items()) origKeys.insert(k);
                        for (auto & [k, _] : newJson.items()) newKeys.insert(k);
                        RC_PRE(origKeys == newKeys);
                    }
                }

                slot.mutate(newValue);
                if (slot.kind != DepSlot::Kind::EnvVar)
                    invalidateFileCache(slot.path);
            }

            // Session 2: warm verify after session boundary.
            simulateWarmRestart();

            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                // SC override precision must survive the session boundary.
                // Case D: after simulateWarmRestart() the primary slot is
                // dropped, so either a primary-slot re-bootstrap or
                // History-recovery may serve the hit.
                RC_ASSERT(snap.deltaTraceCacheHits() >= 1);
            }

            // Restore all Recorded slots.
            for (auto & slot : expr.depSlots) {
                if (slot.contribution != Contribution::Recorded) continue;
                slot.restore();
                if (slot.kind != DepSlot::Kind::EnvVar)
                    invalidateFileCache(slot.path);
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 12: CrossSession_MultiSlotInvalidation
//
// For multi-slot expressions: mutate each Result/File slot across a
// session boundary → must invalidate. Then restore and verify recovery.
// Tests every File dep in the trace tree including mixed-kind expressions.
// JsonFile/JsonArray/TomlFile Result slots are skipped for invalidation
// (SC override may serve cache on value-only changes).
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, CrossSession_MultiSlotInvalidation)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            auto expr = *rc::gen::oneOf(
                makeCallPackageGen(),
                makeImportTreeGen(),
                makeConcatStringsGen(),
                makeMixedDepStringGen(),
                makeFunctionChainGen(),
                makeDeepPipelineGen(),
                makeNestedImportPipelineGen(),
                makeReadDirCountGen());

            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-cross-multi-inval-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() >= 2);
            RC_PRE(!state.settings.pureEval);

            // Must have at least one Result slot with File or
            // DirectoryEntries kind — both support safe mutation.
            bool hasMutableResultSlot = false;
            for (auto & slot : expr.depSlots)
                if (slot.contribution == Contribution::Result
                    && (slot.kind == DepSlot::Kind::File
                        || slot.kind == DepSlot::Kind::DirectoryEntries))
                    hasMutableResultSlot = true;
            RC_PRE(hasMutableResultSlot);

            // Session 1: cold eval.
            Value r1;
            {
                auto cache = makeCache(expr.nixCode);
                r1 = forceRoot(*cache);
            }

            // Test each mutable Result slot independently across sessions.
            // Kind::File slots tagged ContentConstraint::NixSource are
            // mutated via a syntax-preserving comment append (see
            // generateMutation), so importing the file continues to
            // parse.  That in turn makes this test not a soundness test
            // for those slots — the mutation doesn't change the result
            // value.  We still exercise cache invalidation on the
            // FileBytes dep, which must fire on any byte-level change.
            for (auto & slot : expr.depSlots) {
                if (slot.contribution != Contribution::Result) continue;
                if (slot.kind != DepSlot::Kind::File
                    && slot.kind != DepSlot::Kind::DirectoryEntries)
                    continue;

                auto newValue = *slot.generateMutation();
                RC_PRE(newValue != slot.currentValue);

                // Mutate.
                slot.mutate(newValue);
                invalidateFileCache(slot.path);

                // Session N: must miss.
                simulateWarmRestart();

                {
                    PathCountersSnapshot snap;
                    auto cache = makeCache(expr.nixCode);
                    (void) forceRoot(*cache);
                    RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
                }

                // Restore.
                slot.restore();
                invalidateFileCache(slot.path);

                // Session N+1: must recover original.
                simulateWarmRestart();

                Value rRecovered;
                {
                    PathCountersSnapshot snap;
                    auto cache = makeCache(expr.nixCode);
                    rRecovered = forceRoot(*cache);
                    // After restoring to v1, either the restored trace
                    // verifies against current state (primary hit) or
                    // recovery finds v1 in history. The value-equality
                    // check below is the real correctness assertion.
                    RC_ASSERT(snap.deltaTraceCacheHits() >= 1);
                }
                assertValuesEqual(r1, rRecovered);
            }
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 12b: CrossSession_DirectoryEntries_Invalidation
//
// Deterministic test: readDir expression accesses entry "a".
// Session 1: cold eval → records DirectoryEntries dep.
// Session 2: add new unrelated entry "b" → SC override → cache hit (precision).
// Session 3: remove accessed entry "a" → SC dep fails → cache miss (soundness).
// Session 4: restore "a" → recovery finds Session 1's trace → cache hit.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, CrossSession_DirectoryEntries_Invalidation)
{
    TempDir dir;
    dir.addFile("a", "content-a");
    dir.addFile("other", "content-other");

    auto nixCode =
        "let d = builtins.readDir " + dir.path().string() + ";"
        " in d.\"a\"";

    testFingerprint = hashString(HashAlgorithm::SHA256,
        "prop-direntry-invalidation");

    // Session 1: cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "regular");
    }

    // Warm hit baseline.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheHits(), 1u)
            << "warm hit expected: nothing changed";
    }

    // Session 2: add unrelated entry "b" → SC override → cache hit (precision).
    dir.addFile("b", "content-b");
    invalidateFileCache(dir.path());

    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheHits(), 1u)
            << "cache hit expected: unrelated entry added, SC override covers accessed key";
    }

    // Session 3: remove accessed entry "a" → SC dep for "a" fails → miss.
    dir.removeEntry("a");
    invalidateFileCache(dir.path());

    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        // Re-eval will throw (accessing missing attr "a") — that's expected.
        // The important assertion is that re-eval happened (cache miss).
        try {
            (void) forceRoot(*cache);
        } catch (...) {
            // Expected: attribute 'a' missing in readDir result.
        }
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u)
            << "cache miss expected: accessed entry 'a' removed";
    }

    // Session 4: restore "a" → recovery finds Session 1's trace.
    dir.addFile("a", "content-a");
    invalidateFileCache(dir.path());

    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        // After restoring "a", a cached result must serve — either the
        // session-3 miss re-wrote the primary cache with a fresh trace
        // whose deps now match (primary hit), or recovery finds the
        // session-1 trace in history. Either way, no loader re-invocation.
        EXPECT_GE(snap.deltaTraceCacheHits(), 1u)
            << "cache serve expected: original state restored";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "regular");
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test 13: Annotation_Consistency
//
// Cross-cutting: for every expression, observe which slots are actually
// present in stored deps, then verify the observation matches the
// Contribution annotation. Does NOT use the annotation as a filter —
// checks every slot regardless.
//
//   Result or Recorded → slot MUST be present in deps
//   Absent → slot MUST NOT be present in deps
//   Conditional → skip (either is valid)
//
// This catches wrong annotations AND trace system bugs. If a generator
// marks a slot as Result but the trace system doesn't record it, this
// test fails. If a generator marks a slot as Absent but the trace system
// records it anyway, this test fails.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, Annotation_Consistency)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-annotation-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(!expr.depSlots.empty());
            RC_PRE(!state.settings.pureEval);

            {
                auto cache = makeCache(expr.nixCode);
                forceRootAndChildren(state, *cache, expr);
            }

            auto allDeps = collectAllStoredDeps("", childPathsForExpr(expr));

            for (auto & slot : expr.depSlots) {
                if (slot.contribution == Contribution::Conditional)
                    continue;

                bool present;
                if (slot.kind == DepSlot::Kind::EnvVar) {
                    auto vars = extractDepEnvVars(allDeps);
                    present = vars.find(slot.envVarName) != vars.end();
                } else {
                    present = pathInDeps(allDeps, slot.path.string());
                }

                if (slot.contribution == Contribution::Result
                    || slot.contribution == Contribution::Recorded) {
                    // Annotation says present → must be present.
                    RC_ASSERT(present);
                } else {
                    // Absent → must not be present.
                    RC_ASSERT(!present);
                }
            }
        },
        makeThoroughParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 14: Conditional_DepPresence
//
// For conditionalDepGen: the guard always starts as "exists", so the
// then-branch is taken and slot[1] (data file) IS read.
//
// Verify: when guard exists, the Conditional slot[1]'s dep IS present
// in stored deps (FileBytes for the data file).  This proves that
// Conditional slots produce deps when their condition holds.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, Conditional_DepPresence)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            auto expr = *makeConditionalDepGen();

            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-conditional-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() == 2);
            RC_PRE(!state.settings.pureEval);

            auto & guardSlot = expr.depSlots[0];
            auto & dataSlot = expr.depSlots[1];

            RC_PRE(guardSlot.kind == DepSlot::Kind::FileExistence);
            RC_PRE(dataSlot.kind == DepSlot::Kind::File);
            RC_PRE(dataSlot.contribution == Contribution::Conditional);

            // Guard always starts as "exists" → then-branch taken.
            RC_PRE(guardSlot.currentValue == "exists");

            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            auto deps = collectAllStoredDeps("", childPathsForExpr(expr));

            std::string guardPath = guardSlot.path.string();
            std::string dataPath = dataSlot.path.string();

            // Guard dep (ExistenceCheck) must be present — pathExists is always checked.
            RC_ASSERT(anyKeyContains(deps, CanonicalQueryKind::ExistenceCheck, guardPath));

            // Conditional slot's dep (FileBytes) must be present when condition holds.
            RC_ASSERT(anyKeyContains(deps, CanonicalQueryKind::FileBytes, dataPath));
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 14b: Conditional_ElseBranch_DepAbsence
//
// For conditionalDepElseBranchGen: the guard starts MISSING, so the
// else-branch is taken and slot[1] (data file) is NOT read.
//
// Verify:
//   1. ExistenceCheck dep for the guard IS present (even for absent files).
//   2. FileBytes dep for the data file is NOT present (else-branch: data
//      file never read, lazy dep property).
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, Conditional_ElseBranch_DepAbsence)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            auto expr = *makeConditionalDepElseBranchGen();

            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-conditional-else-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() == 2);
            RC_PRE(!state.settings.pureEval);

            auto & guardSlot = expr.depSlots[0];
            auto & dataSlot = expr.depSlots[1];

            RC_PRE(guardSlot.kind == DepSlot::Kind::FileExistence);
            RC_PRE(dataSlot.kind == DepSlot::Kind::File);
            RC_PRE(dataSlot.contribution == Contribution::Absent);

            // Guard starts as "missing" → else-branch taken.
            RC_PRE(guardSlot.currentValue == "missing");

            {
                auto cache = makeCache(expr.nixCode);
                auto v = forceRoot(*cache);
                // Result should be "default" (else-branch).
                RC_ASSERT(v.type() == nString);
                RC_ASSERT(std::string_view(v.string_view()) == "default");
            }

            auto deps = collectAllStoredDeps("", childPathsForExpr(expr));

            std::string guardPath = guardSlot.path.string();
            std::string dataPath = dataSlot.path.string();

            // Guard dep (ExistenceCheck) must be present even when file is absent.
            RC_ASSERT(anyKeyContains(deps, CanonicalQueryKind::ExistenceCheck, guardPath));

            // Data file's dep (FileBytes) must NOT be present — else-branch
            // was taken, so readFile was never called.
            RC_ASSERT(!anyKeyContains(deps, CanonicalQueryKind::FileBytes, dataPath));
        },
        makeParams);
}

// ═══════════════════════════════════════════════════════════════════════
// Test 15: ForceDoesNotLeak
//
// Structural test: evaluate selectiveAttrsetGen, force only "plain",
// then query stored deps for ALL child paths (including "structured"
// and "env" which were NOT forced). Verify that the unforced children
// have no stored trace at all — not just that their deps are absent
// from the forced child's trace.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(EvalTraceProperty_DepCorrectness, ForceDoesNotLeak)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration]() {
            auto expr = *makeSelectiveAttrsetGen();

            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-force-no-leak-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() == 3);
            RC_PRE(!state.settings.pureEval);

            // Force ONLY "plain".
            {
                auto cache = makeCache(expr.nixCode);
                auto * root = cache->getRootValue();
                state.forceValue(*root, noPos);
                auto * attr = root->attrs()->get(state.symbols.create("plain"));
                RC_PRE(attr && attr->value);
                state.forceValue(*attr->value, noPos);
            }

            // Query ALL three child paths — including the ones we didn't force.
            auto allDeps = collectAllStoredDeps("",
                {"plain", "structured", "env"});

            std::string plainPath = expr.depSlots[0].path.string();
            std::string jsonPath = expr.depSlots[1].path.string();
            std::string envName = expr.depSlots[2].envVarName;

            // "plain" was forced — its FileBytes dep must appear.
            RC_ASSERT(anyKeyContains(allDeps, CanonicalQueryKind::FileBytes, plainPath));

            // "structured" was NOT forced — even when querying its child path,
            // the dep must not appear (no stored trace at that path).
            RC_ASSERT(!pathInDeps(allDeps, jsonPath));

            // "env" was NOT forced — EnvironmentLookup must not appear.
            auto envVars = extractDepEnvVars(allDeps);
            RC_ASSERT(envVars.find(envName) == envVars.end());
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
