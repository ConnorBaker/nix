/// trace-store-verify.cc — Verification, recovery, and dep resolution
///
/// Split from trace-store.cc (Phase 4b). Contains:
///   - resolveCurrentDepHash / resolveParentContextHash
///   - verifyTrace (two-pass verification with structural override)
///   - verify (high-level orchestration)
///   - recovery (direct hash + structural variant)

#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval.hh"

#include "trace-serialize.hh"
#include "trace-verify-deps.hh"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <unordered_set>

namespace nix::eval_trace {

// ── Timing helpers (no-op when NIX_SHOW_STATS is unset) ──────────────

static auto timerStart()
{
    return Counter::enabled ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
}

static uint64_t elapsedUs(std::chrono::steady_clock::time_point start)
{
    if (!Counter::enabled) return 0;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

// ── Dep hash resolution (verification support) ──────────────────────

std::optional<DepHashValue> TraceStore::resolveCurrentDepHash(
    const Dep & dep,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state, VerificationScope & scope)
{
    auto cacheIt = currentDepHashes.find(dep.key);
    if (cacheIt != currentDepHashes.end())
        return cacheIt->second;
    auto resolved = resolveDep(dep);
    auto current = computeCurrentHash(state, resolved, inputAccessors, scope, pools.dirSets);
    currentDepHashes[dep.key] = current;
    return current;
}

std::optional<Blake3Hash> TraceStore::resolveParentContextHash(
    const Dep::Key & key,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    VerificationScope & scope)
{
    auto parentPathId = AttrPathId(key.keyId.value);
    auto parentRow = lookupTraceRow(parentPathId);
    if (!parentRow || !verifyTrace(parentRow->traceId, inputAccessors, state, scope))
        return std::nullopt;
    auto parentTraceHash = getCurrentTraceHash(parentPathId);
    if (!parentTraceHash) return std::nullopt;
    return parentTraceHash->raw();
}

// ── Trace verification types ─────────────────────────────────────────

/**
 * File identity for coverage set lookups: (source, filePath).
 * Content/Directory deps use dep.key directly as the file path.
 * StructuredContent/ImplicitShape deps extract "f" from their JSON key.
 */
struct FileIdentity {
    std::string source;
    std::string filePath;

    bool operator==(const FileIdentity &) const = default;

    struct Hash {
        size_t operator()(const FileIdentity & fi) const noexcept {
            return hashValues(fi.source, fi.filePath);
        }
    };
};

static FileIdentity scFileIdentity(const TraceStore::ResolvedDep & dep) {
    auto j = nlohmann::json::parse(dep.key);
    if (j.contains("ds"))
        return {dep.source, "ds:" + j["ds"].get<std::string>()};
    return {dep.source, j["f"].get<std::string>()};
}

static FileIdentity contentFileIdentity(const TraceStore::ResolvedDep & dep) {
    return {dep.source, dep.key};
}

/**
 * Classification of trace verification outcome. Replaces the ad-hoc boolean
 * combination (allValid, hasContentFailure, hasImplicitShapeOnlyOverride).
 * -Wswitch ensures every consumer handles all cases.
 */
enum class VerifyOutcome {
    /** All deps match current state. No hash recomputation needed. */
    Valid,
    /** Content dep(s) failed but StructuredContent deps cover all failures.
     *  Value-aware: accessed scalars verified. No hash recomputation needed. */
    ValidViaStructuralOverride,
    /** Content dep(s) failed, covered by ImplicitShape-only (no SC coverage).
     *  Value-blind: key set unchanged but values may differ. Requires
     *  trace_hash recomputation so ParentContext deps detect potential change. */
    ValidViaImplicitShapeOverride,
    /** Unrecoverable verification failure. */
    Invalid,
};

/// What the caller must do after classifyDep().
enum class DepAction {
    Done,               ///< Already classified; no hash resolution needed.
    CheckGitIdentity,   ///< Resolve hash, call recordGitIdentity(matched).
    CheckParentContext,  ///< Resolve hash, call recordParentContext(matched).
    CheckNormal,        ///< Resolve hash, call recordNormal(matched, ...).
};

/**
 * Accumulated state from Pass 1 (dep classification) of verifyTrace.
 * Groups the 5 classification flags + 2 deferred index vectors + failed
 * file set into a testable struct. classifyDep() routes deps by kind;
 * record*() methods capture hash comparison results; determineOutcome()
 * produces the final VerifyOutcome from accumulated state.
 */
struct VerificationState {
    bool hasNonContentFailure = false;
    bool hasContentFailure = false;
    bool gitIdentityMatched = false;

    /// Indices into fullDeps for deferred StructuredContent deps.
    std::vector<size_t> structuralDepIndices;
    /// Indices into fullDeps for deferred ImplicitShape deps.
    std::vector<size_t> implicitShapeDepIndices;
    /// Content/Directory deps that failed hash comparison.
    std::unordered_set<FileIdentity, FileIdentity::Hash> failedContentFiles;

    bool hasStructuralDeps() const { return !structuralDepIndices.empty(); }
    bool hasImplicitShapeDeps() const { return !implicitShapeDepIndices.empty(); }

    /// True when GitIdentity matched and no other failures or deferred deps exist.
    bool canShortCircuit() const {
        return gitIdentityMatched && !hasNonContentFailure && !hasContentFailure
            && !hasStructuralDeps() && !hasImplicitShapeDeps();
    }

    /// Pass 1: Classify a dep by kind. Returns what the caller should do.
    /// For Volatile/Structural/ImplicitShape: updates state directly, returns Done.
    /// For deps needing hash resolution: returns the appropriate check action.
    DepAction classifyDep(size_t index, DepType type) {
        if (isVolatile(type)) {
            hasNonContentFailure = true;
            return DepAction::Done;
        }
        if (depKind(type) == DepKind::ImplicitStructural) {
            if (type == DepType::GitIdentity)
                return DepAction::CheckGitIdentity;
            implicitShapeDepIndices.push_back(index);
            return DepAction::Done;
        }
        if (depKind(type) == DepKind::Structural) {
            structuralDepIndices.push_back(index);
            return DepAction::Done;
        }
        if (depKind(type) == DepKind::ParentContext)
            return DepAction::CheckParentContext;
        return DepAction::CheckNormal;
    }

    /// Record result of GitIdentity hash check.
    void recordGitIdentity(bool matched) {
        if (matched) gitIdentityMatched = true;
    }

    /// Record result of ParentContext hash check.
    void recordParentContext(bool matched) {
        if (!matched) hasNonContentFailure = true;
    }

    /// Record result of normal dep hash check.
    void recordNormal(bool matched, bool contentOverrideable, const FileIdentity * failedFile) {
        if (!matched) {
            if (contentOverrideable) {
                hasContentFailure = true;
                if (failedFile) failedContentFiles.insert(*failedFile);
            } else {
                hasNonContentFailure = true;
            }
        }
    }

    /// Determine outcome from accumulated Pass 1 state and Pass 2 verification results.
    /// @param structuralDepsVerified  All deferred StructuredContent deps match current hashes.
    /// @param implicitDepsVerified    All deferred ImplicitShape deps match current hashes.
    /// @param allFailuresCovered      Every failed content file has structural or implicit coverage.
    /// @param hasImplicitOnlyCoverage At least one failed file covered only by implicit (not structural).
    VerifyOutcome determineOutcome(bool structuralDepsVerified, bool implicitDepsVerified,
                                    bool allFailuresCovered, bool hasImplicitOnlyCoverage) const {
        if (hasNonContentFailure)
            return VerifyOutcome::Invalid;
        if (!hasContentFailure) {
            bool depsOk = (!hasStructuralDeps() && !hasImplicitShapeDeps())
                       || (structuralDepsVerified && implicitDepsVerified);
            return depsOk ? VerifyOutcome::Valid : VerifyOutcome::Invalid;
        }
        // hasContentFailure && need structural/implicit coverage
        if (!hasStructuralDeps() && !hasImplicitShapeDeps())
            return VerifyOutcome::Invalid;
        if (!allFailuresCovered || !structuralDepsVerified || !implicitDepsVerified)
            return VerifyOutcome::Invalid;
        return hasImplicitOnlyCoverage
            ? VerifyOutcome::ValidViaImplicitShapeOverride
            : VerifyOutcome::ValidViaStructuralOverride;
    }
};

// ── Trace verification (BSàlC VT check) ─────────────────────────────

bool TraceStore::verifyTrace(
    TraceId traceId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto scopeOwner = createVerificationScope();
    return verifyTrace(traceId, inputAccessors, state, *scopeOwner);
}

bool TraceStore::verifyTrace(
    TraceId traceId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    VerificationScope & scope)
{
    if (verifiedTraceIds.count(traceId))
        return true;

    auto vtStart = timerStart();

    // Load the full trace (single DB read via JOIN) — vector<Dep>
    auto fullDeps = loadFullTrace(traceId);

    // ── Pass 1: Classify each dep ────────────────────────────────────
    VerificationState vs;

    for (size_t i = 0; i < fullDeps.size(); ++i) {
        auto & idep = fullDeps[i];
        nrDepsChecked++;

        switch (vs.classifyDep(i, idep.key.type)) {
        case DepAction::Done:
            break;
        case DepAction::CheckGitIdentity: {
            auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);
            vs.recordGitIdentity(current && *current == idep.hash);
            break;
        }
        case DepAction::CheckParentContext: {
            auto parentHash = resolveParentContextHash(idep.key, inputAccessors, state, scope);
            bool matched = false;
            if (parentHash) {
                auto * expected = std::get_if<Blake3Hash>(&idep.hash);
                matched = expected && std::memcmp(expected->bytes.data(), parentHash->bytes.data(), 32) == 0;
            }
            if (!matched) nrVerificationsFailed++;
            vs.recordParentContext(matched);
            break;
        }
        case DepAction::CheckNormal: {
            auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);
            bool matched = current && *current == idep.hash;
            if (!matched) {
                nrVerificationsFailed++;
                bool overrideable = isContentOverrideable(idep.key.type);
                if (overrideable) {
                    auto dep = resolveDep(idep);
                    auto fi = contentFileIdentity(dep);
                    vs.recordNormal(false, true, &fi);
                } else {
                    vs.recordNormal(false, false, nullptr);
                }
            }
            break;
        }
        }
    }

    // ── GitIdentity fast-path: skip Pass 2 if git identity matches,
    //    all other deps passed, and no structural/implicit deps need checking.
    if (vs.canShortCircuit()) {
        verifiedTraceIds.insert(traceId);
        nrVerifyTraceTimeUs += elapsedUs(vtStart);
        return true;
    }

    // ── Pass 2: Resolve overrides and determine outcome ─────────────

    // Helper: verify a set of structural/implicit deps (by index) against current hashes.
    auto verifyDeps = [&](const std::vector<size_t> & indices,
                          const std::unordered_set<FileIdentity, FileIdentity::Hash> * skipFiles = nullptr,
                          const std::unordered_set<FileIdentity, FileIdentity::Hash> * onlyFiles = nullptr) -> bool {
        for (auto idx : indices) {
            auto & idep = fullDeps[idx];
            if (skipFiles || onlyFiles) {
                auto dep = resolveDep(idep);
                auto fileKey = scFileIdentity(dep);
                if (skipFiles && skipFiles->count(fileKey)) continue;
                if (onlyFiles && !onlyFiles->count(fileKey)) continue;
            }
            nrDepsChecked++;
            auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);
            if (!current || *current != idep.hash) {
                nrVerificationsFailed++;
                return false;
            }
        }
        return true;
    };

    // Files whose Content/Directory deps passed — populated in hasContentFailure branch,
    // used both for skipFiles filtering in verifyDeps and for old-hash shortcutting
    // in the Invalid pre-compute loop.
    std::unordered_set<FileIdentity, FileIdentity::Hash> passedContentFiles;

    // Pass 2 results fed to determineOutcome().
    bool structuralDepsVerified = true;
    bool implicitDepsVerified = true;
    bool allFailuresCovered = true;
    bool hasImplicitOnlyCoverage = false;

    if (!vs.hasNonContentFailure && !vs.hasContentFailure) {
        // No content failures: verify standalone structural/implicit deps
        if (vs.hasStructuralDeps() || vs.hasImplicitShapeDeps()) {
            std::unordered_set<FileIdentity, FileIdentity::Hash> coveredFiles;
            for (auto & idep : fullDeps) {
                if (isContentOverrideable(idep.key.type)) {
                    auto dep = resolveDep(idep);
                    coveredFiles.insert(contentFileIdentity(dep));
                }
            }
            structuralDepsVerified = verifyDeps(vs.structuralDepIndices, &coveredFiles);
            implicitDepsVerified = structuralDepsVerified
                && verifyDeps(vs.implicitShapeDepIndices, &coveredFiles);
        }
    } else if (!vs.hasNonContentFailure && vs.hasContentFailure
               && (vs.hasStructuralDeps() || vs.hasImplicitShapeDeps())) {
        // Content failures with structural/implicit coverage available.
        // Populate passedContentFiles: unchanged files, safe to skip.
        for (auto & idep : fullDeps) {
            if (isContentOverrideable(idep.key.type)) {
                auto dep = resolveDep(idep);
                auto fi = contentFileIdentity(dep);
                if (!vs.failedContentFiles.count(fi))
                    passedContentFiles.insert(fi);
            }
        }

        std::unordered_set<FileIdentity, FileIdentity::Hash> structuralCoveredFiles;
        for (auto idx : vs.structuralDepIndices) {
            auto dep = resolveDep(fullDeps[idx]);
            auto fi = scFileIdentity(dep);
            structuralCoveredFiles.insert(fi);
            if (fi.filePath.starts_with("ds:")) {
                try {
                    auto j = nlohmann::json::parse(dep.key);
                    auto dsHash = j.value("ds", "");
                    auto it = pools.dirSets.find(dsHash);
                    if (it == pools.dirSets.end()) continue;
                    auto dirs = nlohmann::json::parse(it->second);
                    for (auto & dir : dirs) {
                        if (dir.is_array() && dir.size() == 2)
                            structuralCoveredFiles.insert({dir[0].get<std::string>(), dir[1].get<std::string>()});
                    }
                } catch (...) {}
            }
        }

        std::unordered_set<FileIdentity, FileIdentity::Hash> implicitCoveredFiles;
        for (auto idx : vs.implicitShapeDepIndices) {
            auto dep = resolveDep(fullDeps[idx]);
            implicitCoveredFiles.insert(scFileIdentity(dep));
        }

        // Coverage analysis: are all failed files covered by structural or implicit deps?
        for (auto & failedFile : vs.failedContentFiles) {
            if (!structuralCoveredFiles.count(failedFile)
                && !implicitCoveredFiles.count(failedFile)) {
                allFailuresCovered = false;
                break;
            }
            if (!structuralCoveredFiles.count(failedFile)
                && implicitCoveredFiles.count(failedFile)) {
                hasImplicitOnlyCoverage = true;
            }
        }

        structuralDepsVerified = verifyDeps(vs.structuralDepIndices, &passedContentFiles);
        implicitDepsVerified = structuralDepsVerified
            && verifyDeps(vs.implicitShapeDepIndices, &structuralCoveredFiles, &vs.failedContentFiles);
    }

    auto outcome = vs.determineOutcome(structuralDepsVerified, implicitDepsVerified,
                                        allFailuresCovered, hasImplicitOnlyCoverage);

    // ── Apply outcome ────────────────────────────────────────────────

    switch (outcome) {
    case VerifyOutcome::Valid:
    case VerifyOutcome::ValidViaStructuralOverride:
        verifiedTraceIds.insert(traceId);
        break;

    case VerifyOutcome::ValidViaImplicitShapeOverride: {
        // Build Dep directly — no round-trip needed
        std::vector<Dep> currentDeps;
        currentDeps.reserve(fullDeps.size());
        for (auto & idep : fullDeps) {
            if (depKind(idep.key.type) == DepKind::ParentContext) {
                auto b3 = resolveParentContextHash(idep.key, inputAccessors, state, scope);
                currentDeps.push_back(b3 ? Dep{idep.key, DepHashValue(*b3)} : idep);
            } else {
                auto cacheIt = currentDepHashes.find(idep.key);
                currentDeps.push_back(cacheIt != currentDepHashes.end() && cacheIt->second
                    ? Dep{idep.key, *cacheIt->second} : idep);
            }
        }
        auto newTraceHash = computePresortedTraceHash(currentDeps);
        auto * data = ensureTraceHashes(traceId);
        if (data) {
            data->traceHash = newTraceHash;
        }
        verifiedTraceIds.insert(traceId);
        break;
    }

    case VerifyOutcome::Invalid:
        // Pre-compute remaining deferred dep hashes so recovery
        // gets L1 cache hits instead of expensive recomputation.
        // When only content deps failed, unchanged files' structural dep
        // hashes equal the recorded values — skip expensive recomputation.
        for (auto * indices : {&vs.structuralDepIndices, &vs.implicitShapeDepIndices}) {
            for (auto idx : *indices) {
                auto & idep = fullDeps[idx];
                if (currentDepHashes.find(idep.key) == currentDepHashes.end()) {
                    if (!passedContentFiles.empty()) {
                        auto dep = resolveDep(idep);
                        if (passedContentFiles.count(scFileIdentity(dep))) {
                            currentDepHashes[idep.key] = idep.hash;
                            continue;
                        }
                    }
                    resolveCurrentDepHash(idep, inputAccessors, state, scope);
                }
            }
        }
        break;
    }

    nrVerifyTraceTimeUs += elapsedUs(vtStart);
    return outcome != VerifyOutcome::Invalid;
}

// ── Verify path (BSàlC verifying trace) ──────────────────────────────

std::optional<TraceStore::VerifyResult> TraceStore::verify(
    AttrPathId pathId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto verifyStart = timerStart();

    // 1. Lookup attribute
    auto row = lookupTraceRow(pathId);
    if (!row) {
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return std::nullopt;
    }

    nrTraceVerifications++;

    // Shared scope: DOM caches (JSON, TOML, dir listing, Nix AST) persist
    // across verifyTrace → recovery, avoiding redundant file parsing.
    auto scopeOwner = createVerificationScope();
    auto & scope = *scopeOwner;

    // 2. Verify trace — on Invalid outcome, verifyTrace pre-computes all
    //    deferred dep hashes (structural + implicit shape) so that recovery
    //    gets L1 cache hits instead of expensive recomputation.
    if (verifyTrace(row->traceId, inputAccessors, state, scope)) {
        nrVerificationsPassed++;
        nrVerifyTimeUs += elapsedUs(verifyStart);
        return VerifyResult{decodeCachedResult(*row), row->traceId};
    }

    // 3. Verification failed → constructive recovery.
    //    All dep hashes are pre-computed in currentDepHashes (from step 2).
    //    Direct hash recovery is O(1): sort+hash the pre-computed values, lookup.
    //    No additional hash computation needed unless structural variant recovery
    //    encounters a trace with different dep keys (rare).
    debug("verify: trace validation failed for '%s', attempting constructive recovery", vocab.displayPath(pathId));
    auto result = recovery(row->traceId, pathId, inputAccessors, state, scope);
    nrVerifyTimeUs += elapsedUs(verifyStart);
    return result;
}

// ── Recovery (BSàlC constructive trace recovery) ─────────────────────
//    Two-phase: direct hash recovery + structural variant recovery

std::optional<TraceStore::VerifyResult> TraceStore::recovery(
    TraceId oldTraceId,
    AttrPathId pathId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    auto scopeOwner = createVerificationScope();
    return recovery(oldTraceId, pathId, inputAccessors, state, *scopeOwner);
}

std::optional<TraceStore::VerifyResult> TraceStore::recovery(
    TraceId oldTraceId,
    AttrPathId pathId,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    VerificationScope & scope)
{
    auto recoveryStart = timerStart();
    nrRecoveryAttempts++;

    // Load old trace's full deps — now vector<Dep>
    auto oldDeps = loadFullTrace(oldTraceId);

    // Check for volatile deps → immediate abort
    for (auto & idep : oldDeps) {
        if (isVolatile(idep.key.type)) {
            debug("recovery: aborting for '%s' -- contains volatile dep", vocab.displayPath(pathId));
            nrRecoveryFailures++;
            nrRecoveryTimeUs += elapsedUs(recoveryStart);
            return std::nullopt;
        }
    }

    // Build Dep directly with current hash values
    std::vector<Dep> currentDeps;
    bool allComputable = true;
    for (auto & idep : oldDeps) {
        if (depKind(idep.key.type) == DepKind::ParentContext) {
            auto parentHash = resolveParentContextHash(idep.key, inputAccessors, state, scope);
            if (!parentHash) { allComputable = false; break; }
            currentDeps.push_back({idep.key, DepHashValue(*parentHash)});
            continue;
        }

        auto current = resolveCurrentDepHash(idep, inputAccessors, state, scope);

        if (!current) {
            allComputable = false;
            break;
        }
        currentDeps.push_back({idep.key, *current});
    }

    debug("recovery: recomputed %d/%d dep hashes for '%s'",
          currentDeps.size(), oldDeps.size(), vocab.displayPath(pathId));

    boost::unordered_flat_set<TraceId, TraceId::Hash> triedTraceIds;

    // === Pre-load: scan history with widened query ===
    struct HistoryEntry {
        DepKeySetId depKeySetId;
        StructHash structHash;
        TraceId traceId;
        ResultId resultId;
        TraceHash traceHash;
        ResultKind type;
        std::string value;
        std::string context;
    };
    std::vector<HistoryEntry> historyEntries;
    {
        auto st(_state->lock());
        auto use(st->scanHistoryForAttr.use()(contextHash)(static_cast<int64_t>(pathId.value)));
        while (use.next()) {
            auto [shData, shSize] = use.getBlob(1);
            auto [thData, thSize] = use.getBlob(4);
            historyEntries.push_back({
                DepKeySetId(static_cast<uint32_t>(use.getInt(0))),
                StructHash::fromBlob(shData, shSize),
                TraceId(static_cast<uint32_t>(use.getInt(2))),
                ResultId(static_cast<uint32_t>(use.getInt(3))),
                TraceHash::fromBlob(thData, thSize),
                static_cast<ResultKind>(use.getInt(5)),
                use.isNull(6) ? "" : use.getStr(6),
                use.isNull(7) ? "" : use.getStr(7),
            });
        }
    }

    // Build in-memory trace_hash → entry index lookup.
    boost::unordered_flat_map<TraceHash, size_t, TraceHash::Hasher> traceHashToEntry;
    for (size_t i = 0; i < historyEntries.size(); i++)
        traceHashToEntry.emplace(historyEntries[i].traceHash, i);

    auto lookupCandidate = [&](const TraceHash & candidateHash) -> const HistoryEntry * {
        auto it = traceHashToEntry.find(candidateHash);
        if (it == traceHashToEntry.end()) return nullptr;
        return &historyEntries[it->second];
    };

    auto acceptRecoveredTrace = [&](const HistoryEntry & entry) -> std::optional<VerifyResult> {
        if (triedTraceIds.count(entry.traceId))
            return std::nullopt;
        triedTraceIds.insert(entry.traceId);

        {
            auto st(_state->lock());
            st->upsertAttr.use()
                (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(entry.traceId.value))(static_cast<int64_t>(entry.resultId.value)).exec();
        }

        TraceRow newRow{entry.traceId, entry.resultId, entry.type, entry.value, entry.context};
        traceRowCache[pathId] = newRow;

        verifiedTraceIds.insert(entry.traceId);
        return VerifyResult{decodeCachedResult(newRow), entry.traceId};
    };

    // === Direct hash recovery (BSàlC CT) ===
    if (allComputable) {
        auto directHashStart = timerStart();
        auto newFullHash = computePresortedTraceHash(currentDeps);

        if (auto * entry = lookupCandidate(newFullHash)) {
            if (auto r = acceptRecoveredTrace(*entry)) {
                debug("recovery: direct hash recovery succeeded for '%s'", vocab.displayPath(pathId));
                nrRecoveryDirectHashHits++;
                nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
        nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
    }

    std::optional<StructHash> directHashStructHash;
    if (allComputable) {
        directHashStructHash = getTraceStructHash(oldTraceId);
    }

    // === Structural variant recovery ===
    auto structVariantStart = timerStart();

    debug("recovery: structural variant scan for '%s' -- scanning %d history entries",
          vocab.displayPath(pathId), historyEntries.size());

    boost::unordered_flat_map<DepKeySetId, TraceId, DepKeySetId::Hash> structGroups;
    boost::unordered_flat_map<DepKeySetId, StructHash, DepKeySetId::Hash> groupStructHashes;
    for (auto & e : historyEntries) {
        if (triedTraceIds.count(e.traceId))
            continue;
        structGroups.emplace(e.depKeySetId, e.traceId);
        groupStructHashes.emplace(e.depKeySetId, e.structHash);
    }

    std::vector<Dep> repDeps;
    for (auto & [depKeySetId, repTraceId] : structGroups) {
        if (triedTraceIds.count(repTraceId))
            continue;
        auto structHashIt = groupStructHashes.find(depKeySetId);
        if (directHashStructHash && structHashIt != groupStructHashes.end()
            && structHashIt->second == *directHashStructHash)
            continue;

        auto repKeys = loadKeySet(depKeySetId);

        // Build Dep directly using Dep::Key
        repDeps.clear();
        bool repComputable = true;
        for (auto & key : repKeys) {
            if (isVolatile(key.type)) {
                repComputable = false;
                break;
            }

            if (depKind(key.type) == DepKind::ParentContext) {
                auto parentHash = resolveParentContextHash(key, inputAccessors, state, scope);
                if (!parentHash) { repComputable = false; break; }
                repDeps.push_back({key, DepHashValue(*parentHash)});
                continue;
            }

            auto current = resolveCurrentDepHash(
                Dep{key, DepHashValue{Blake3Hash{}}}, inputAccessors, state, scope);

            if (!current) {
                repComputable = false;
                break;
            }
            repDeps.push_back({key, *current});
        }
        if (!repComputable)
            continue;

        auto candidateFullHash = computePresortedTraceHash(repDeps);

        if (auto * entry = lookupCandidate(candidateFullHash)) {
            if (auto r = acceptRecoveredTrace(*entry)) {
                debug("recovery: structural variant recovery succeeded for '%s'", vocab.displayPath(pathId));
                nrRecoveryStructVariantHits++;
                nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);
                nrRecoveryTimeUs += elapsedUs(recoveryStart);
                return r;
            }
        }
    }
    nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);

    debug("recovery: all strategies failed for '%s'", vocab.displayPath(pathId));
    nrRecoveryFailures++;
    nrRecoveryTimeUs += elapsedUs(recoveryStart);
    return std::nullopt;
}

} // namespace nix::eval_trace
