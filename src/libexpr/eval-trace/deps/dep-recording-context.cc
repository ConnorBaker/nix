/// dep-recording-context.cc — Dependency recording context implementation.

#include "nix/expr/eval-trace/deps/dep-recording-context.hh"

#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/counters.hh"

namespace nix {

/// Deduplication helper: returns true if the dep should be appended
/// to the scope's ownDeps (new key or overriding has-key entry).
///
/// Instability signal: when the same dep KEY is recorded with two
/// different HASHES within one scope, `scope.unstable = true` and the
/// trace is rejected downstream. This historically caught
/// file-changes-during-eval for FileBytes deps. Note: with the
/// session-wide content-hash cache
/// (`EvalEnvironmentSharedState::fileContentHashCache`), FileBytes
/// deps always see the first-observed hash, so the FileBytes path of
/// this detector is no longer reachable. This is intentional — see
/// "Known Limitations" in eval-trace/CLAUDE.md.
bool DepRecordingContext::observeRecordedDep(DepRecordingContext::Scope & scope, const Dep & dep)
{
    auto [it, inserted] = scope.seenDeps.emplace(dep.key, dep.hash);
    if (inserted)
        return true;

    if (it->second == dep.hash)
        return false;

    if (dep.key.kind == CanonicalQueryKind::StructuredProjection && dep.key.hasKeyId) {
        auto & kOne = sentinel(SentinelHash::One);
        auto & kZero = sentinel(SentinelHash::Zero);
        auto * existingDigest = std::get_if<DepHash>(&it->second);
        auto * newDigest = std::get_if<DepHash>(&dep.hash);
        if (existingDigest && newDigest) {
            if (*existingDigest == kZero && *newDigest == kOne) {
                it->second = dep.hash;
                return false;
            }
            if (*existingDigest == kOne && *newDigest == kZero)
                return false;
        }
    }

    scope.unstable = true;
    return false;
}

void DepRecordingContext::recordInterned(
    CanonicalQueryKind type,
    DepSourceId sourceId,
    SimpleDepKeyId keyId,
    DepHashValue hash,
    RepoRootId governingRepoId)
{
    auto key = Dep::Key::makeSimple(type, sourceId, keyId);
    key.governingRepoId = governingRepoId;
    Dep dep{std::move(key), std::move(hash)};

    auto * scope = currentScope();
    bool dedupPassed = !scope || observeRecordedDep(*scope, dep);

    EpochLogWriteCertifier::ifDedupPassed(dedupPassed, [&](const auto & proof) {
        if (scope)
            scope->ownDeps.push_back(dep);
        epochLog.append(proof, dep);
    });
}

void DepRecordingContext::record(
    CanonicalQueryKind type,
    const DepSource & source,
    const SimpleDepKeyAtom & key,
    DepHashValue hash,
    RepoRootId governingRepoId)
{
    auto srcId = pools.intern<DepSourceId>(source);
    auto keyId = pools.intern(key);
    recordInterned(type, srcId, keyId, std::move(hash), governingRepoId);
}

void DepRecordingContext::record(
    const DepSource & source,
    const DerivedStorePathDepKey & key,
    DepHashValue hash,
    RepoRootId governingRepoId)
{
    auto srcId = pools.intern<DepSourceId>(source);
    auto keyId = pools.intern(key);
    auto depKey = Dep::Key::makeDerivedStorePath(srcId, keyId);
    depKey.governingRepoId = governingRepoId;
    record(Dep{
        std::move(depKey),
        std::move(hash),
    });
}

void DepRecordingContext::record(
    const DepSource & source,
    const StorePathAvailabilityDepKey & key,
    DepHashValue hash)
{
    auto srcId = pools.intern<DepSourceId>(source);
    auto keyId = pools.intern(key);
    record(Dep{
        Dep::Key::makeStorePathAvailability(srcId, keyId),
        std::move(hash),
    });
}

void DepRecordingContext::record(
    const DepSource & source,
    const RuntimeFetchIdentityDepKey & key,
    DepHashValue hash)
{
    auto srcId = pools.intern<DepSourceId>(source);
    auto keyId = pools.intern(key);
    record(Dep{
        Dep::Key::makeRuntimeFetchIdentity(srcId, keyId),
        std::move(hash),
    });
}

void DepRecordingContext::record(const Dep & dep)
{
    auto * scope = currentScope();
    bool dedupPassed = !scope || observeRecordedDep(*scope, dep);

    EpochLogWriteCertifier::ifDedupPassed(dedupPassed, [&](const auto & proof) {
        if (scope)
            scope->ownDeps.push_back(dep);
        epochLog.append(proof, dep);
    });
}

void DepRecordingContext::replaceWindowWithEdge(
    const std::vector<Dep> & producerInnerDeps, const Dep & edge)
{
    auto * scope = currentScope();
    if (!scope)
        return;

    // Build a key-set of the producer's deps (Dep::Key::operator== compares the
    // key identity; hashes are irrelevant for membership). Keep it local — this
    // is per-derivation, not a hot per-dep structure.
    boost::unordered_flat_set<Dep::Key, Dep::Key::Hash> producerKeys;
    producerKeys.reserve(producerInnerDeps.size());
    for (const auto & d : producerInnerDeps)
        producerKeys.insert(d.key);

    // Erase the producer's deps from the consumer's ownDeps. std::erase_if
    // preserves the relative order of the survivors.
    std::erase_if(scope->ownDeps, [&](const Dep & d) {
        return producerKeys.contains(d.key);
    });

    // Record the replacing edge through the normal path (scope dedup +
    // epochLog append). If the same producer edge was already recorded in this
    // scope (a second consumer of the same producer in one scope), the dedup
    // collapses it — correct.
    record(edge);
}

} // namespace nix
