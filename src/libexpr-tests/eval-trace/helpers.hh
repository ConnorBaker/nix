#pragma once

#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/trace-activation-scope.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/canon-path.hh"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test {

inline GitRepoRoot testGitRepoRoot(std::string_view repoRoot)
{
    return GitRepoRoot{CanonPath(std::string(repoRoot))};
}

/// Test-only helper to push a scope on a DepRecordingContext.
/// Production code must go through DepCaptureScope (GDP-guarded).
struct TestScopeAccess : private gdp::Certifier<DepRecordingContext::DepCaptureScopeTag> {
    static void pushScope(DepRecordingContext & ctx) {
        Certifier::withProof([&](const auto & proof) { ctx.pushScope(proof); });
    }
    static void popScope(DepRecordingContext & ctx) {
        Certifier::withProof([&](const auto & proof) { ctx.popScope(proof); });
    }
    static std::vector<Dep> takeDeps(DepRecordingContext & ctx) {
        std::vector<Dep> deps;
        Certifier::withProof([&](const auto & proof) { deps = ctx.takeDeps(proof); });
        return deps;
    }
};

/// Friend access struct for test code to call private SqliteTraceStorage methods.
/// Tests must provide a VerificationSession — there is no convenience overload
/// for verifyTrace/recovery. The verify() convenience creates its own session.
struct TraceStorageTestAccess : private gdp::Certifier<BlockingTag> {
    /// All test methods use withProof to create a scoped blocking proof.
    /// No escape hatches — the proof is stack-scoped in each method.

    static EncodedResultPayload encodeCachedResult(
        SqliteTraceStorage & store, const CachedResult & value)
    {
        return store.encodeCachedResult(value);
    }

    static CachedResult decodeCachedResult(
        SqliteTraceStorage & store, const SqliteTraceStorage::ResultPayload & payload)
    {
        return store.decodeCachedResult(payload);
    }

    static bool verifyTrace(
        SqliteTraceStorage & store, TraceId traceId,
        const SemanticRegistry & registry,
        EvalState & state, VerificationSession & session)
    {
        return Certifier<BlockingTag>::withProof([&](const gdp::Proof<BlockingTag> & bs) {
            return store.withExclusiveAccess(bs, [&](const auto & ea) {
                return store.verifyTrace(ea, traceId, registry, state, session);
            });
        });
    }
    static std::optional<SqliteTraceStorage::VerifyResult> recovery(
        SqliteTraceStorage & store, TraceId oldTraceId, AttrPathId pathId,
        const SemanticRegistry & registry,
        EvalState & state, VerificationSession & session)
    {
        return Certifier<BlockingTag>::withProof([&](const gdp::Proof<BlockingTag> & bs) {
            return store.withExclusiveAccess(bs, [&](const auto & ea) {
                return store.recovery(ea, oldTraceId, pathId, registry, state, session);
            });
        });
    }
    /// Convenience: verifyTrace with auto-created session.
    static bool verifyTrace(
        SqliteTraceStorage & store, TraceId traceId,
        const SemanticRegistry & registry,
        EvalState & state)
    {
        VerificationSession session;
        return Certifier<BlockingTag>::withProof([&](const gdp::Proof<BlockingTag> & bs) {
            return store.withExclusiveAccess(bs, [&](const auto & ea) {
                return store.verifyTrace(ea, traceId, registry, state, session);
            });
        });
    }
    /// Convenience: recovery with auto-created session.
    static std::optional<SqliteTraceStorage::VerifyResult> recovery(
        SqliteTraceStorage & store, TraceId oldTraceId, AttrPathId pathId,
        const SemanticRegistry & registry,
        EvalState & state)
    {
        VerificationSession session;
        return Certifier<BlockingTag>::withProof([&](const gdp::Proof<BlockingTag> & bs) {
            return store.withExclusiveAccess(bs, [&](const auto & ea) {
                return store.recovery(ea, oldTraceId, pathId, registry, state, session);
            });
        });
    }
    /// Convenience: runs the orchestrator's inlined verify flow.
    static std::optional<SqliteTraceStorage::VerifyResult> verify(
        SqliteTraceStorage & store, AttrPathId pathId,
        const SemanticRegistry & registry,
        EvalState & state)
    {
        VerificationSession session;
        return verify(store, pathId, registry, state, session);
    }
    static std::optional<SqliteTraceStorage::VerifyResult> verify(
        SqliteTraceStorage & store, AttrPathId pathId,
        const SemanticRegistry & registry,
        EvalState & state, VerificationSession & session)
    {
        return Certifier<BlockingTag>::withProof(
            [&](const gdp::Proof<BlockingTag> & bs) {
                return store.withExclusiveAccess(bs, [&](const auto & ea) {
                    return store.verify(ea, pathId, registry, state, session);
                });
            });
    }

    // --- Convenience overloads for tests: empty SemanticRegistry ---
    // These replace the old pattern of passing {} (empty map) as inputAccessors.

    static bool verifyTrace(SqliteTraceStorage & store, TraceId traceId, EvalState & state, VerificationSession & session) {
        SemanticRegistry r; return verifyTrace(store, traceId, r, state, session);
    }
    static bool verifyTrace(SqliteTraceStorage & store, TraceId traceId, EvalState & state) {
        SemanticRegistry r; return verifyTrace(store, traceId, r, state);
    }
    static std::optional<SqliteTraceStorage::VerifyResult> recovery(
        SqliteTraceStorage & store, TraceId oldTraceId, AttrPathId pathId, EvalState & state, VerificationSession & session) {
        SemanticRegistry r; return recovery(store, oldTraceId, pathId, r, state, session);
    }
    static std::optional<SqliteTraceStorage::VerifyResult> recovery(
        SqliteTraceStorage & store, TraceId oldTraceId, AttrPathId pathId, EvalState & state) {
        SemanticRegistry r; return recovery(store, oldTraceId, pathId, r, state);
    }
    static std::optional<SqliteTraceStorage::VerifyResult> verify(
        SqliteTraceStorage & store, AttrPathId pathId, EvalState & state) {
        SemanticRegistry r; return verify(store, pathId, r, state);
    }
    static std::optional<SqliteTraceStorage::VerifyResult> verify(
        SqliteTraceStorage & store, AttrPathId pathId, EvalState & state, VerificationSession & session) {
        SemanticRegistry r; return verify(store, pathId, r, state, session);
    }

    // NOTE: §N.7 orchestrator access.
    //
    // `verify(store, ...)` above calls `SqliteTraceStorage::verify` directly and
    // therefore CANNOT observe the orchestrator's
    // `scanHistory(stableRecoveryKey, pathId)` bootstrap at
    // `verification-orchestrator.cc:verifyAttrImpl` — the path OR-5-class
    // bugs live on.  Tests that need to exercise that path must route
    // through `TraceSession::forceRoot` via a `TraceCacheFixture`
    // subclass:
    //
    //     auto session = makeCacheWithSessionConfig(
    //         expr, bootstrapSeed,
    //         SessionConfig::forTest(policyDigest, stableRecoveryKey));
    //     (void) forceRoot(*session);
    //
    // That path constructs a `TraceBackend` which owns an
    // `AsyncRuntime`, and `forceRoot` → `backend->verify()` runs the
    // orchestrator's `verifyAttrImpl` (including `scanHistory`) via
    // `TraceBackend::verify`.
    //
    // Example using this pattern:
    // `verify/integration.cc::Integration_OR5Reproducer_ScanHistoryServesCrossSession`
    // asserts on `PathCountersSnapshot::deltaHistoryBootstraps()` to
    // prove the orchestrator hit its `scanHistory` fallback.
    //
    // A raw `SqliteTraceStorage`-level helper that built its own AsyncRuntime +
    // VerificationOrchestrator would need access to the private
    // `async-runtime.hh` header (not exported from libexpr).  Rather
    // than publish that header, the canonical unit-level shape is
    // through `makeCacheWithSessionConfig`.
};

} // namespace nix::eval_trace::test

namespace nix::eval_trace::test {

// ── RAII environment helpers ────────────────────────────────────────

/**
 * RAII environment variable setter. Restores the original value on destruction.
 * Used to simulate oracle state changes in verification tests.
 */
struct ScopedEnvVar
{
    std::string key;
    std::optional<std::string> oldValue;

    ScopedEnvVar(std::string k, std::string v)
        : key(std::move(k))
    {
        if (auto * old = std::getenv(key.c_str()))
            oldValue = old;
        setenv(key.c_str(), v.c_str(), 1);
    }

    ~ScopedEnvVar()
    {
        if (oldValue)
            setenv(key.c_str(), oldValue->c_str(), 1);
        else
            unsetenv(key.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar &) = delete;
    ScopedEnvVar & operator=(const ScopedEnvVar &) = delete;
};

// ── RAII temporary file/dir helpers ─────────────────────────────────

/**
 * RAII temporary file helper. Creates a file with given content
 * in a temporary directory, and removes it on destruction.
 * Represents a filesystem oracle input for trace dep recording and verification.
 */
struct TempTestFile
{
    std::filesystem::path path;

    explicit TempTestFile(std::string_view content)
    {
        auto dir = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
        createDirs(dir);

        // Generate unique filename
        static std::atomic<int> counter = 0;
        path = dir / ("test-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".nix");

        std::ofstream ofs(path);
        ofs << content;
    }

    void modify(std::string_view newContent)
    {
        std::ofstream ofs(path, std::ios::trunc);
        ofs << newContent;
    }

    ~TempTestFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempTestFile(const TempTestFile &) = delete;
    TempTestFile & operator=(const TempTestFile &) = delete;
};

/**
 * RAII temporary file with configurable extension.
 * Unifies TempJsonFile, TempTomlFile, TempTextFile.
 */
struct TempExtFile
{
    std::filesystem::path path;

    TempExtFile(std::string_view ext, std::string_view content)
    {
        auto dir = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
        createDirs(dir);
        static std::atomic<int> counter = 0;
        path = dir / ("test-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + "." + std::string(ext));
        std::ofstream ofs(path);
        ofs << content;
    }

    void modify(std::string_view newContent)
    {
        std::ofstream ofs(path, std::ios::trunc);
        ofs << newContent;
    }

    ~TempExtFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempExtFile(const TempExtFile &) = delete;
    TempExtFile & operator=(const TempExtFile &) = delete;
};

/// Convenience aliases
struct TempJsonFile : TempExtFile {
    explicit TempJsonFile(std::string_view content) : TempExtFile("json", content) {}
};
struct TempTomlFile : TempExtFile {
    explicit TempTomlFile(std::string_view content) : TempExtFile("toml", content) {}
};
struct TempTextFile : TempExtFile {
    explicit TempTextFile(std::string_view content) : TempExtFile("txt", content) {}
};

/**
 * RAII temporary directory with helpers for adding/removing entries.
 */
class TempDir {
    std::filesystem::path dir_;
public:
    TempDir()
    {
        auto base = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
        createDirs(base);
        static std::atomic<int> counter = 0;
        dir_ = base / ("dir-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directory(dir_);
    }
    const std::filesystem::path & path() const { return dir_; }

    void addFile(const std::string & name, const std::string & content = "")
    {
        std::ofstream ofs(dir_ / name);
        ofs << content;
    }
    void addSubdir(const std::string & name)
    {
        std::filesystem::create_directory(dir_ / name);
    }
    void addSymlink(const std::string & name, const std::string & target)
    {
        std::filesystem::create_symlink(target, dir_ / name);
    }
    void removeEntry(const std::string & name)
    {
        deletePath(dir_ / name);
    }
    void changeToSymlink(const std::string & name, const std::string & target)
    {
        deletePath(dir_ / name);
        std::filesystem::create_symlink(target, dir_ / name);
    }
    void changeToSubdir(const std::string & name)
    {
        deletePath(dir_ / name);
        std::filesystem::create_directory(dir_ / name);
    }

    ~TempDir()
    {
        try {
            deletePath(dir_);
        } catch (...) {
        }
    }
    TempDir(const TempDir &) = delete;
    TempDir & operator=(const TempDir &) = delete;
};

/**
 * RAII temporary git repository for testing GitRevisionIdentity deps.
 * Creates a temp directory, initializes a git repo, and provides methods
 * to add files, commit, get HEAD hash, and make dirty modifications.
 */
struct TempGitRepo
{
    std::filesystem::path repoPath;

    /// Creates temp dir + git init + initial commit.
    TempGitRepo();
    /// Removes the directory recursively.
    ~TempGitRepo();

    /// Add a file to the repo (does not commit).
    void addFile(const std::string & name, const std::string & content);

    /// Commit all staged changes. Returns the new HEAD hash.
    std::string commit(const std::string & message = "test commit");

    /// Get the current HEAD commit hash (full 40-char hex).
    std::string headHash() const;

    /// Modify a tracked file without committing (dirty working tree).
    void dirtyModify(const std::string & name, const std::string & newContent);

    /// Get the path to a file in the repo.
    std::filesystem::path filePath(const std::string & name) const;

    TempGitRepo(const TempGitRepo &) = delete;
    TempGitRepo & operator=(const TempGitRepo &) = delete;
};

/**
 * RAII snapshot of eval-trace performance counters for path-kind
 * discrimination in tests. Enables the counters on construction (saving
 * the prior `Counter::enabled` state) and records baseline values for
 * each counter of interest. `delta*()` accessors return the change
 * since construction. Destructor restores the prior enabled state.
 *
 * Use to assert WHICH cache path served a warm hit — primary session,
 * DirectHash recovery, GitIdentity recovery, or structural-variant
 * recovery — without parsing `NIX_SHOW_STATS` JSON. Addresses test
 * drift pattern §N.8: before this helper, no unit test could
 * discriminate among recovery strategies.
 *
 * Counter declarations are in `nix/expr/eval-trace/counters.hh`. In-
 * process precedent for `Counter::enabled = true` around a measurement:
 * `PerfCounters_ColdEval_IncrementsTracesVerified`
 * (`store/record-verify.cc:639-657`).
 *
 * Counters are `std::atomic<uint64_t>`; reads are lock-free and safe.
 *
 * Thread safety invariant: `Counter::enabled` is a non-atomic global.
 * Nested `PathCountersSnapshot` instances are safe because each saves
 * the prior `enabled` state and restores it on destruction. But:
 * - Parallel tests that both construct a snapshot would race on the
 *   `Counter::enabled = true` store.
 * - Any test code that explicitly sets `Counter::enabled = false` mid-
 *   snapshot (e.g. `PerfCounters_ColdEval_IncrementsTracesVerified` at
 *   `store/record-verify.cc`) will desync counts — the snapshot's
 *   increments after that point are dropped.
 * gtest runs sequentially by default; this helper relies on that.
 * If parallel test execution is ever adopted, `Counter::enabled`
 * must become per-thread (or the counters must shift to a thread-
 * local implementation) before this helper stays correct.
 */
struct PathCountersSnapshot
{
    PathCountersSnapshot()
        : wasEnabled_(Counter::enabled)
        , traceCacheHitsBaseline_(nrTraceCacheHits.load())
        , traceCacheMissesBaseline_(nrTraceCacheMisses.load())
        , recoveryAttemptsBaseline_(nrRecoveryAttempts.load())
        , recoveryDirectHashHitsBaseline_(nrRecoveryDirectHashHits.load())
        , recoveryStructVariantHitsBaseline_(nrRecoveryStructVariantHits.load())
        , recoveryGitIdentityHitsBaseline_(nrRecoveryGitIdentityHits.load())
        , recoveryFailuresBaseline_(nrRecoveryFailures.load())
        , recordsBaseline_(nrRecords.load())
        , historyBootstrapsBaseline_(nrHistoryBootstraps.load())
    {
        Counter::enabled = true;
    }

    ~PathCountersSnapshot()
    {
        Counter::enabled = wasEnabled_;
    }

    PathCountersSnapshot(const PathCountersSnapshot &) = delete;
    PathCountersSnapshot & operator=(const PathCountersSnapshot &) = delete;

    Counter::value_type deltaTraceCacheHits() const
    { return nrTraceCacheHits.load() - traceCacheHitsBaseline_; }
    Counter::value_type deltaTraceCacheMisses() const
    { return nrTraceCacheMisses.load() - traceCacheMissesBaseline_; }
    Counter::value_type deltaRecoveryAttempts() const
    { return nrRecoveryAttempts.load() - recoveryAttemptsBaseline_; }
    Counter::value_type deltaRecoveryDirectHashHits() const
    { return nrRecoveryDirectHashHits.load() - recoveryDirectHashHitsBaseline_; }
    Counter::value_type deltaRecoveryStructVariantHits() const
    { return nrRecoveryStructVariantHits.load() - recoveryStructVariantHitsBaseline_; }
    Counter::value_type deltaRecoveryGitIdentityHits() const
    { return nrRecoveryGitIdentityHits.load() - recoveryGitIdentityHitsBaseline_; }
    Counter::value_type deltaRecoveryFailures() const
    { return nrRecoveryFailures.load() - recoveryFailuresBaseline_; }
    Counter::value_type deltaRecords() const
    { return nrRecords.load() - recordsBaseline_; }
    /// Orchestrator's scanHistory-based bootstrap at
    /// `verification-orchestrator.cc` (in the
    /// `bootstrappedFromHistory=true` branch). Zero means the primary
    /// `lookupCurrentNode` hit directly (or the attr wasn't cached);
    /// non-zero means the primary lookup missed and history was
    /// consulted. Critically distinct from `deltaRecoveryAttempts`,
    /// which only tracks the 3-strategy `recovery()` fallback on
    /// `verifyTrace` failure.
    Counter::value_type deltaHistoryBootstraps() const
    { return nrHistoryBootstraps.load() - historyBootstrapsBaseline_; }

    /// Convenience: was this delta served STRICTLY by the primary
    /// session cache (no history bootstrap, no recovery fallback) with
    /// at least one verify hit? Forbids both recovery-served and
    /// history-bootstrap-served paths — the semantic that §N.4
    /// "_CacheHit" tests intend.
    bool primaryCacheServedOnly() const
    {
        return deltaTraceCacheHits() >= 1
            && deltaRecoveryAttempts() == 0
            && deltaHistoryBootstraps() == 0;
    }

private:
    bool wasEnabled_;
    Counter::value_type traceCacheHitsBaseline_;
    Counter::value_type traceCacheMissesBaseline_;
    Counter::value_type recoveryAttemptsBaseline_;
    Counter::value_type recoveryDirectHashHitsBaseline_;
    Counter::value_type recoveryStructVariantHitsBaseline_;
    Counter::value_type recoveryGitIdentityHitsBaseline_;
    Counter::value_type recoveryFailuresBaseline_;
    Counter::value_type recordsBaseline_;
    Counter::value_type historyBootstrapsBaseline_;
};

/**
 * RAII helper that creates a temporary cache directory and sets NIX_CACHE_HOME.
 * Use this in test fixtures that create SqliteTraceStorage or TraceSession,
 * which need a writable cache directory for their SQLite trace databases.
 * Required for sandbox builds where $HOME is /homeless-shelter (not writable).
 */
struct ScopedCacheDir
{
    std::filesystem::path dir;
    ScopedEnvVar envVar;

    ScopedCacheDir()
        : dir(std::filesystem::canonical(std::filesystem::temp_directory_path())
              / ("nix-test-cache-" + std::to_string(getpid()) + "-" + std::to_string(nextId())))
        , envVar("NIX_CACHE_HOME", dir.string())
    {
        createDirs(dir);
    }

    ~ScopedCacheDir()
    {
        // `deletePath` is throwing; swallow in dtor so unwinding under another
        // in-flight exception doesn't fire `std::terminate`.
        try { deletePath(dir); } catch (...) {}
    }

    ScopedCacheDir(const ScopedCacheDir &) = delete;
    ScopedCacheDir & operator=(const ScopedCacheDir &) = delete;

private:
    static int nextId()
    {
        static int c = 0;
        return c++;
    }
};

// ── Invalidation helpers ────────────────────────────────────────────

/// Invalidate directory cache entries. Expands to
/// invalidateFileCache(td.path()), which clears the FS accessor lstat
/// cache and the file content hash cache.
///
/// Call sites must be inside a method of a class that inherits
/// TraceCacheFixture (which provides invalidateFileCache).
#define INVALIDATE_DIR(td) invalidateFileCache((td).path())

// ── Attr path helpers ───────────────────────────────────────────────

/**
 * Build a null-byte-separated attr path from components.
 * DEPRECATED: Use vocabPath() for new code (returns AttrPathId).
 */
inline std::string makePath(std::initializer_list<std::string_view> parts)
{
    std::string path;
    bool first = true;
    for (auto & part : parts) {
        if (!first) path.push_back('\0');
        path.append(part);
        first = false;
    }
    return path;
}

/**
 * Build an AttrPathId from components using the vocab store.
 */
inline AttrPathId vocabPath(AttrVocabStore & vocab, std::initializer_list<std::string_view> parts)
{
    auto id = AttrVocabStore::rootPath();
    for (auto & part : parts)
        id = vocab.internPath(id, vocab.internName(part));
    return id;
}


// ── Oracle dep factory helpers ──────────────────────────────────────

inline Dep::Key makeSimpleKeyForTest(CanonicalQueryKind type, uint32_t sourceId, uint32_t keyId)
{
    if (type == CanonicalQueryKind::StorePathAvailability)
        return Dep::Key::makeStorePathAvailability(
            DepSourceId(sourceId),
            StorePathAvailabilityDepKeyId{DepKeyId(keyId)});
    if (type == CanonicalQueryKind::RuntimeFetchIdentity)
        return Dep::Key::makeRuntimeFetchIdentity(
            DepSourceId(sourceId),
            RuntimeFetchIdentityDepKeyId{DepKeyId(keyId)});
    return Dep::Key::makeSimple(type, DepSourceId(sourceId), SimpleDepKeyId(keyId));
}

inline Dep::Key makeStructuredKeyForTest(
    CanonicalQueryKind type,
    uint32_t sourceId,
    uint32_t filePathId,
    StructuredFormat format = StructuredFormat::Json,
    DataPathId dataPathId = DataPathId())
{
    return Dep::Key::makeStructured(
        type, DepSourceId(sourceId), FilePathId(filePathId), format, dataPathId);
}

inline Dep::Key makeTraceContextKeyForTest(CanonicalQueryKind type, uint32_t pathId)
{
    return Dep::Key::makeTraceContext(type, AttrPathId(pathId));
}

inline std::string renderStructuredKeyDisplay(const SqliteTraceStorage::ResolvedDep::StructuredKey & key)
{
    if (!key.dirSetHash.empty()) {
        auto result = std::string("dirset(") + key.dirSetHash + ")@"
            + std::string(1, structuredFormatChar(key.format));
        if (!key.hasKey.empty())
            result += "#has(" + key.hasKey + ")";
        return result;
    }

    auto result = key.filePath + "@" + std::string(1, structuredFormatChar(key.format))
        + displayStructuredPath(key.dataPath);
    if (!key.hasKey.empty())
        result += "#has(" + key.hasKey + ")";
    else if (key.suffix != ShapeSuffix::None)
        result += "#" + std::string(shapeSuffixName(key.suffix));
    return result;
}

inline SqliteTraceStorage::ResolvedDep resolveDepForTest(InterningPools & pools, const Dep & dep)
{
    using ResolvedDep = SqliteTraceStorage::ResolvedDep;

    if (dep.key.isTraceContext()) {
        return ResolvedDep{
            .source = "",
            .key = std::to_string(dep.key.attrPathId.value),
            .expectedHash = dep.hash,
            .type = dep.key.kind,
            .structured = std::nullopt,
            .traceContext = ResolvedDep::TraceContextKey{
                .pathId = dep.key.attrPathId,
            },
        };
    }

    if (dep.key.isStructured()) {
        auto structured = ResolvedDep::StructuredKey{
            .filePath = std::string(pools.resolve(dep.key.filePathId)),
            .format = dep.key.structuredFormat(),
            .dataPath = resolveStructuredPath(pools, dep.key.dataPathId),
            .suffix = dep.key.suffix,
            .hasKey = dep.key.hasKeyId ? std::string(pools.resolve(dep.key.hasKeyId)) : "",
            .dirSetHash = dep.key.dirSetHashId ? std::string(pools.resolve(dep.key.dirSetHashId)) : "",
        };
        return ResolvedDep{
            .source = std::string(pools.resolve(dep.key.sourceId)),
            .key = renderStructuredKeyDisplay(structured),
            .expectedHash = dep.hash,
            .type = dep.key.kind,
            .structured = std::move(structured),
            .traceContext = std::nullopt,
        };
    }

    return ResolvedDep{
        .source = std::string(pools.resolve(dep.key.sourceId)),
        .key = renderSimpleDepKeyDisplay(pools, dep.key),
        .expectedHash = dep.hash,
        .type = dep.key.kind,
        .structured = std::nullopt,
        .traceContext = std::nullopt,
    };
}

inline std::string depKeyDisplayForTest(InterningPools & pools, const Dep & dep)
{
    return resolveDepForTest(pools, dep).key;
}

inline Dep makeStructuredDepForTest(
    InterningPools & pools,
    CanonicalQueryKind type,
    DepSource source,
    std::string_view filePath,
    StructuredFormat format,
    StructuredPath dataPath,
    DepHashValue hash,
    ShapeSuffix suffix = ShapeSuffix::None,
    std::string_view hasKey = {},
    std::string_view dirSetHash = {})
{
    return {
        Dep::Key::makeStructured(
            type,
            pools.intern<DepSourceId>(source),
            pools.intern<FilePathId>(filePath),
            format,
            internStructuredPath(pools, dataPath),
            suffix,
            hasKey.empty() ? StringId() : pools.intern<StringId>(hasKey),
            dirSetHash.empty() ? StringId() : pools.intern<StringId>(dirSetHash)),
        std::move(hash),
    };
}

inline Dep makeContentDep(InterningPools & p, std::string_view key, std::string_view content)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::FileBytes,
                p.intern<DepSourceId>(DepSource::makeAbsolute()),
                p.intern<SimpleDepKeyId>(key)),
        depHash(content)};
}

/// Like `makeContentDep`, but also attaches the given governing repo root
/// id to the key.  Use this in tests that exercise the git-identity-skip
/// coverage fast path.  Production code sets governingRepoId at recording
/// time (`InterningPools::internGoverningRepo`); tests can hand-pick a
/// repo root without needing a real .git directory.
inline Dep makeContentDepInRepo(
    InterningPools & p, std::string_view key, std::string_view content,
    std::string_view repoRoot)
{
    auto k = Dep::Key::makeSimple(
        CanonicalQueryKind::FileBytes,
        p.intern<DepSourceId>(DepSource::makeAbsolute()),
        p.intern<SimpleDepKeyId>(key));
    k.governingRepoId = p.intern<RepoRootId>(repoRoot);
    return {std::move(k), depHash(content)};
}

inline Dep makeSimpleRecordedDep(
    InterningPools & p,
    CanonicalQueryKind type,
    const DepSource & source,
    std::string_view key,
    DepHashValue hash,
    std::string_view governingRepoRoot = {})
{
    auto sourceId = p.intern<DepSourceId>(source);
    if (type == CanonicalQueryKind::StorePathAvailability) {
        auto keyId = p.intern(StorePathAvailabilityDepKey{
            .storePath = StorePath{std::filesystem::path(std::string(key)).filename().string()},
        });
        return {
            Dep::Key::makeStorePathAvailability(sourceId, keyId),
            std::move(hash),
        };
    }
    if (type == CanonicalQueryKind::RuntimeFetchIdentity) {
        fetchers::Settings fetchSettings;
        auto input = fetchers::Input::fromURL(fetchSettings, std::string(key));
        auto keyId = p.intern(RuntimeFetchIdentityDepKey{.inputAttrs = input.toAttrs()});
        return {
            Dep::Key::makeRuntimeFetchIdentity(sourceId, keyId),
            std::move(hash),
        };
    }
    auto depKey = Dep::Key::makeSimple(
        type,
        sourceId,
        p.intern<SimpleDepKeyId>(key));
    // Attach governing-repo id when given — tests that exercise the
    // git-identity coverage fast path or `allDepsGitRecoverable` need to
    // declare which synthetic repo a file dep belongs to.  Production
    // sets this automatically via `InterningPools::internGoverningRepo`
    // during `recordDep`.
    if (!governingRepoRoot.empty()
        && repoRootAddressingKind(type) == RepoRootAddressingKind::DirectPath)
        depKey.governingRepoId = p.intern<RepoRootId>(governingRepoRoot);
    return {std::move(depKey), std::move(hash)};
}

inline Dep makeEnvVarDep(InterningPools & p, std::string_view key, std::string_view value)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::EnvironmentLookup, p.intern<DepSourceId>(DepSource::makeAbsolute()), p.intern<SimpleDepKeyId>(key)),
        depHash(value)};
}

inline Dep makeExistenceDep(InterningPools & p, std::string_view key, bool exists)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::ExistenceCheck,
                p.intern<DepSourceId>(DepSource::makeAbsolute()),
                p.intern<SimpleDepKeyId>(key)),
        DepHashValue(exists ? std::string("type:1") : std::string("missing"))};
}

inline Dep makeSystemDep(InterningPools & p, std::string_view system)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::SessionSystemValue, p.intern<DepSourceId>(DepSource::makeAbsolute()), p.intern<SimpleDepKeyId>("")),
        depHash(system)};
}

inline Dep makeCurrentTimeDep(InterningPools & p)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::VolatileTime, p.intern<DepSourceId>(DepSource::makeAbsolute()), p.intern<SimpleDepKeyId>("")),
        DepHashValue(std::string("volatile"))};
}

inline Dep makeExecDep(InterningPools & p)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::VolatileExec, p.intern<DepSourceId>(DepSource::makeAbsolute()), p.intern<SimpleDepKeyId>("")),
        DepHashValue(std::string("volatile"))};
}

/// Create a DerivedStorePath dep using the canonical typed dep-key encoding.
inline Dep makeCopiedPathDep(InterningPools & p, std::string_view sourcePath,
                             std::string_view storeName, std::string_view storePath)
{
    auto key = DerivedStorePathDepKey{
        .pathKey = CanonPath(std::string(sourcePath)),
        .storeName = SimpleDepKeyAtom{std::string(storeName)},
    };
    return {Dep::Key::makeDerivedStorePath(
                p.intern<DepSourceId>(DepSource::makeAbsolute()),
                p.intern(key)),
        DepHashValue(std::string(storePath))};
}

inline Dep makeNARContentDep(InterningPools & p, std::string_view key, const DepHash & hash)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::NarIdentity,
                p.intern<DepSourceId>(DepSource::makeAbsolute()),
                p.intern<SimpleDepKeyId>(key)),
        DepHashValue(hash)};
}

inline Dep makeDirectoryDep(InterningPools & p, std::string_view key, const DepHash & hash)
{
    return {Dep::Key::makeSimple(
                CanonicalQueryKind::DirectoryEntries,
                p.intern<DepSourceId>(DepSource::makeAbsolute()),
                p.intern<SimpleDepKeyId>(key)),
        DepHashValue(hash)};
}

inline Dep makeGitIdentityDep(InterningPools & p, std::string_view repoPath, std::string_view fingerprint)
{
    auto key = Dep::Key::makeSimple(
        CanonicalQueryKind::GitRevisionIdentity,
        p.intern<DepSourceId>(DepSource::makeAbsolute()),
        p.intern<SimpleDepKeyId>(repoPath));
    // Mirror production: a GitRevisionIdentity dep's governingRepo is itself,
    // so the verifier can cross-reference file-content deps that carry the
    // same governingRepoId.
    key.governingRepoId = p.intern<RepoRootId>(repoPath);
    return {std::move(key), depHash(fingerprint)};
}

// ── CachedResult comparison ─────────────────────────────────────────

/**
 * Compare two CachedResults for equality. Supports all variant types.
 */
void assertCachedResultEquals(const CachedResult & a, const CachedResult & b, SymbolTable & symbols);

// ── Test fixture base classes ───────────────────────────────────────

/**
 * Base fixture for eval-trace tests that need a Nix evaluator + writable cache.
 */
class EvalTraceTest : public LibExprTest, private gdp::Certifier<BlockingTag>
{
public:
    /// Create a scoped blocking proof for test code. Authorized because tests
    /// run single-threaded with no event loop — blocking is always safe.
    template<typename F>
    static decltype(auto) withBs(F && f) { return withProof(std::forward<F>(f)); }

    /// Convenience: compose `withBs` + `store.withExclusiveAccess`.
    /// Passes a scoped `ExclusiveTraceStoreAccess` (`ea`) to `f`.
    /// Use this when test code needs to call SqliteTraceStorage methods that take
    /// the capability (Phase 2 migration target). For migration timing,
    /// both the old `withBs([&](bs) { store.method(bs, ...); })` and the
    /// new `withExclusiveStore(store, [&](ea) { store.method(ea, ...); })`
    /// patterns work; prefer the latter for new code.
    template<typename F>
    static decltype(auto) withExclusiveStore(SqliteTraceStorage & store, F && f)
    {
        return withBs([&](const auto & bs) -> decltype(auto) {
            return store.withExclusiveAccess(bs, std::forward<F>(f));
        });
    }

    EvalTraceTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {
        // EvalState (and its TraceRuntime/InterningPools) is fresh per
        // test — gtest creates a new fixture instance for each TEST_F.
        // No pool reset needed.
    }

protected:
    // M-5 hazard note: In C++ the base class (LibExprTest) constructor runs
    // before this member initializer. If LibExprTest (or EvalState) ever
    // reads NIX_CACHE_HOME during construction, it would see the wrong value.
    // Currently safe: NIX_CACHE_HOME is only read by SqliteTraceStorage's constructor
    // (trace-store-lifecycle.cc), which is called from makeCache() — well after
    // both the base constructor and cacheDir have finished initializing.
    ScopedCacheDir cacheDir;
};

/**
 * Fixture for tests that use TraceSession (most eval-trace tests).
 * Provides makeCache(), forceRoot(), invalidateFileCache().
 */
class TraceCacheFixture : public EvalTraceTest
{
public:
    ~TraceCacheFixture()
    {
        // Release the last session's backend (and its AsyncRuntime threads)
        // so they don't accumulate across tests. Without this, each test
        // leaves an active TraceBackend (4 threads) alive via GC
        // TracedExpr thunks → ref<TraceSession>. After ~250 tests,
        // ~1000 threads accumulate, causing deadlocks.
        //
        // `releaseActiveSession` → `TraceSession::releaseBackend` →
        // `backend->flush()` can throw on SQLite I/O failure. A throw from
        // a dtor during stack unwinding (another test-body exception in
        // flight) would fire `std::terminate`. Swallow to preserve the
        // original exception and let gtest report it.
        try {
            releaseActiveSession();
        } catch (...) {
        }
    }

protected:
    /// Per-suite fingerprint namespace. The 2-arg `makeCache(nixExpr, …)`
    /// below mixes `nixExpr` into this value so that each distinct
    /// expression lands at its own `(session_key, AttrPathId(0))` slot.
    /// Mirrors production's `FileEvalExpressionHash =
    /// active-backend-digest(exprText)` (see `installable-attr-path.cc`) —
    /// tests that varied the Nix
    /// expression but reused a constant fingerprint used to collide on
    /// the same slot, masking regressions.
    ///
    /// Suites that INTENTIONALLY share a slot across differing
    /// expressions (e.g. `Session_DifferentFingerprints_Isolated`) must
    /// use the explicit-Hash `TraceCacheTest::makeCache(nixExpr, Hash,
    /// int*)` overload in `store/cache.cc`, which bypasses the mixing.
    Hash testFingerprint = hashString(HashAlgorithm::SHA256, "trace-cache-fixture");

    /// Track the active session so we can release its backend (and
    /// SQLite connection) before opening the same DB file again.
    /// With ref<TraceSession>, GC-allocated TracedExpr thunks keep
    /// old sessions alive — without releasing the backend, the old
    /// SqliteTraceStorage holds the DB file open, causing "database busy"
    /// errors when the next makeCache() or makeQueryDb() call tries
    /// to open the same file. After release, the session becomes a
    /// zombie (null backend). Zombie TracedExpr thunks fall
    /// through to evaluateDirect().
    std::shared_ptr<TraceSession> activeSession_;

    /// Release the active session's backend if one exists.
    /// Call before creating a new SqliteTraceStorage connection to the same DB.
    void releaseActiveSession()
    {
        if (activeSession_) {
            activeSession_->releaseBackend();
            activeSession_.reset();
        }
    }

    /**
     * Construct a TraceSession with an EXPLICIT SessionConfig, bypassing
     * the 2-arg makeCache's per-expression fingerprint mixing (§N.1).
     *
     * Use for tests that need to control `semanticSessionKey` and
     * `stableRecoveryKey` directly — typically to exercise cross-session
     * recovery behavior (two sessions with distinct semantic keys but
     * the same stable key, triggering scanHistory bootstrap on the
     * second eval).
     *
     * `bootstrapFingerprintForBackend` parameterises the in-memory
     * bootstrap key the backend starts with BEFORE `setSessionConfig`
     * runs. It does NOT select the SQLite file -- SqliteTraceStorage chooses the
     * versioned, hash-algorithm-suffixed cache file under ScopedCacheDir. A
     * common valid choice is a per-test hashString seed so distinct
     * tests don't race on the bootstrap slot.
     */
    ref<TraceSession> makeCacheWithSessionConfig(
        const std::string & nixExpr,
        const Hash & bootstrapFingerprintForBackend,
        SessionConfig sessionConfig,
        int * loaderCalls = nullptr)
    {
        releaseActiveSession();
        auto loader = [this, nixExpr, loaderCalls]() -> Value * {
            if (loaderCalls) (*loaderCalls)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        TraceSession::BackendParams backendParams{
            bootstrapFingerprintForBackend,
            std::optional<SessionConfig>{std::move(sessionConfig)},
        };
        auto session = make_ref<TraceSession>(
            std::optional<TraceSession::BackendParams>{std::move(backendParams)},
            state, std::move(loader),
            boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash>{},
            boost::unordered_flat_map<CanonPath, std::vector<std::pair<DepSource, RegistryMountSubdir>>>{},
            std::vector<TraceSession::RootLoadDep>{},
            SemanticRegistry{});
        activeSession_ = session.get_ptr();
        // Do NOT update lastPerExprFingerprint_ — callers that need
        // makeQueryDb() with a custom bootstrap must pass the key
        // through the SessionConfig, not through this field.
        return session;
    }

    ref<TraceSession> makeCache(
        const std::string & nixExpr,
        int * loaderCalls = nullptr)
    {
        releaseActiveSession();
        auto loader = [this, nixExpr, loaderCalls]() -> Value * {
            if (loaderCalls) (*loaderCalls)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        // Mix the Nix expression into the fingerprint so that distinct
        // expressions land at distinct `(session_key, AttrPathId(0))`
        // slots — mirroring production's per-expression session-key
        // derivation. See `testFingerprint` comment above.
        lastPerExprFingerprint_ = hashString(
            HashAlgorithm::SHA256,
            testFingerprint.to_string(HashFormat::Base16, false) + ":" + nixExpr);
        auto session = make_ref<TraceSession>(
            std::optional<TraceSession::BackendParams>{
                TraceSession::BackendParams{*lastPerExprFingerprint_, std::nullopt}},
            state, std::move(loader));
        activeSession_ = session.get_ptr();
        return session;
    }

    /// The per-expression fingerprint from the most recent 2-arg
    /// `makeCache(nixExpr, …)` call. Subclasses (e.g. `MaterializationDepTest`)
    /// that open a second SqliteTraceStorage to query stored rows must use this
    /// value as the bootstrap key; the un-mixed `testFingerprint` lands
    /// at a different in-memory session slot and will not see rows
    /// recorded through the mixed fingerprint. Set to `nullopt` before
    /// any 2-arg call.
    std::optional<Hash> lastPerExprFingerprint_;

    Value forceRoot(TraceSession & cache)
    {
        auto * v = cache.getRootValue();
        state.forceValue(*v, noPos);
        return *v;
    }

    void invalidateFileCache([[maybe_unused]] const std::filesystem::path & path)
    {
        // Three caches must be invalidated after a file mutation:
        //
        // 1. `CachingSourceAccessor` (wraps `rootFS`) — memoizes
        //    `maybeLstat`/`readLink` results; stale ancestor directory
        //    entries there leak pre-mutation shape into post-mutation reads.
        //    (Master's accessor rewrite replaced the concrete
        //    `PosixSourceAccessor` and its per-instance lstat cache with the
        //    `CachingSourceAccessor` wrapper; the `getFSSourceAccessor()`
        //    singleton still delegates via this chain, so the invalidation
        //    shape is unchanged.)
        // 2. TraceRuntime fileContentHashes — in-memory EvalState hash cache
        //    used by import chain replay.
        // 3. fileTraceCache / srcToStore / importResolutionCache —
        //    EvalEnvironmentSharedState memoizes evalFile + source-copy
        //    results; a real new process has these empty, so tests mutating
        //    files on disk must clear them via `resetFileCache`.
        releaseActiveSession();
        getFSSourceAccessor()->invalidateCache();
        if (state.traceCtx)
            state.traceCtx->clearFileContentHashes();
        state.resetFileCache();
    }

    /// Simulate a warm process restart against the same DB: all cross-
    /// session caches an out-of-process subprocess would start empty are
    /// wiped, but PosTable and traceCtx (which cannot be cleared mid-
    /// iteration without breaking active callers) are preserved.
    ///
    /// Releases the active session (flushes to SQLite), clears the FS
    /// accessor lstat cache, the file-content hash cache, and the
    /// evalFile memo tables (importResolutionCache + fileTraceCache +
    /// inputCache).
    ///
    /// Residual in-process state a real new process wouldn't share:
    /// - InterningPools (interned string IDs survive — semantically
    ///   correct since bulkLoadAll repopulates from SQLite)
    /// - `positions` (PosTable) and traceCtx are NOT cleared; for a
    ///   FULL cold-process simulation use `simulateColdProcess()`.
    ///
    /// Renamed from `simulateNewSession()` (§N.2). Safe to call while an
    /// RC iteration still holds references into PosTable/traceCtx.
    void simulateWarmRestart()
    {
        releaseActiveSession();
        getFSSourceAccessor()->invalidateCache();
        if (state.traceCtx)
            state.traceCtx->clearFileContentHashes();
        state.clearCrossSessionCaches();
    }

    /// Simulate a cold process boundary. Strictly stronger than
    /// `simulateWarmRestart()`: also wipes `positions` (PosTable) and
    /// resets the traceCtx, matching what an out-of-process subprocess
    /// actually starts with. The trade-off is that pre-existing parsed
    /// expressions and trace contexts held by the caller become invalid,
    /// so it is only safe to call between logical iterations — never
    /// mid-evaluation.
    ///
    /// Use when a test must observe recovery/history-bootstrap behavior
    /// a warm restart would miss because a residual cache happens to
    /// short-circuit the code path (§N.2).
    void simulateColdProcess()
    {
        releaseActiveSession();
        getFSSourceAccessor()->invalidateCache();
        if (state.traceCtx)
            state.traceCtx->clearFileContentHashes();
        state.resetFileCache();
    }
};

/**
 * Fixture for tests that use SqliteTraceStorage directly.
 * Provides makeDb() with a fixed bootstrap session key.
 */
class TraceStoreFixture : public EvalTraceTest
{
protected:
    /// Per-test bootstrap fingerprint.
    ///
    /// Mixes the current gtest test case name (`TestSuite.TestName`) into
    /// the hash so every TEST_F lands at its own `Sessions.session_key`
    /// slot. Previously a fixed literal shared across the entire suite —
    /// see §N.1 in `CLAUDE.md`. Without per-test rotation, two tests in
    /// the same suite sharing `(session_key, AttrPathId(0))` could have
    /// one silently verify the other's recorded trace.
    ///
    /// Returns a test-independent fallback when called outside an active
    /// gtest (which happens only during fixture construction orderings
    /// where `current_test_info()` is null).
    Hash testBootstrapFingerprint() const
    {
        const auto * info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string seed = "trace-store-fixture-bootstrap";
        if (info) {
            seed += ":";
            seed += info->test_suite_name();
            seed += ".";
            seed += info->name();
        }
        return hashString(HashAlgorithm::SHA256, seed);
    }

    AttrVocabStore & testVocab() {
        return state.vocabStore();
    }

    /// Build an AttrPathId from string components.
    AttrPathId vpath(std::initializer_list<std::string_view> parts) {
        return vocabPath(testVocab(), parts);
    }

    /// Root path sentinel.
    AttrPathId rootPath() { return AttrVocabStore::rootPath(); }

    InterningPools & pools() { return state.tracingPools(); }

    std::unique_ptr<SqliteTraceStorage> makeDb()
    {
        auto bootstrapKey = SemanticSessionKey::fromSerialized(
            "test-bootstrap:"
            + testBootstrapFingerprint().to_string(HashFormat::Base16, false));
        return std::make_unique<SqliteTraceStorage>(state.symbols, state.tracingPools(),
            testVocab(), std::move(bootstrapKey));
    }

    /// Destroy the old store (flushing pending writes to DB), then create
    /// a fresh one against the same DB. Ensures the old store's destructor
    /// completes before the new store opens the DB, avoiding SQLite busy
    /// errors from concurrent connections.
    void recreateDb(std::unique_ptr<SqliteTraceStorage> & db)
    {
        db.reset();
        db = makeDb();
    }
};

// ── Shared test fixture classes (used across split TUs) ─────────────

/**
 * Fixture for traced-data tests (JSON/TOML/readDir StructuredContent).
 * Used by traced-data-*.cc files.
 */
class TracedDataTest : public TraceCacheFixture
{
public:
    TracedDataTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "traced-data-test");
    }
};

/**
 * Fixture for trace-store tests (SQLite backend).
 * Used by trace-store-*.cc files.
 */
class TraceStoreTest : public TraceStoreFixture
{
};

/**
 * Fixture for dep-precision tests.
 * Provides evalAndCollectDeps(), hasDep(), countDeps(), dumpDeps().
 * Inherits TracedDataTest so tests also have makeCache/forceRoot/invalidateFileCache.
 */
class DepPrecisionTest : public TracedDataTest
{
protected:
    using ResolvedDep = SqliteTraceStorage::ResolvedDep;

    static std::vector<ResolvedDep> resolveDeps(InterningPools & pools, const std::vector<Dep> & deps)
    {
        std::vector<ResolvedDep> result;
        result.reserve(deps.size());
        for (auto & d : deps)
            result.push_back(resolveDepForTest(pools, d));
        return result;
    }

    // Empty guarded mounts for test dep capture scopes.
    eval_trace::SemanticRegistry testRegistry;

    std::vector<ResolvedDep> evalAndCollectDeps(const std::string & nixExpr)
    {
        auto & pools = state.tracingPools();
        DepCaptureScope depCapture(pools, testRegistry);
        TraceActivationScope traceActivation(state);
        (void) eval(nixExpr, /* forceValue */ true);
        return resolveDeps(pools, depCapture.finalizeAndTakeDeps());
    }

    /// Synthesize the legacy JSON test view for a structured dep key.
    static std::optional<nlohmann::json> parseDepKey(const ResolvedDep & d)
    {
        if (!d.structured)
            return std::nullopt;

        nlohmann::json result = nlohmann::json::object();
        result["t"] = std::string(1, structuredFormatChar(d.structured->format));
        if (!d.structured->dirSetHash.empty()) {
            result["ds"] = d.structured->dirSetHash;
            result["h"] = d.structured->hasKey;
            return result;
        }

        result["f"] = d.structured->filePath;
        nlohmann::json path = nlohmann::json::array();
        for (auto & component : d.structured->dataPath) {
            if (component.isIndex())
                path.push_back(component.index);
            else
                path.push_back(component.key);
        }
        result["p"] = std::move(path);
        if (!d.structured->hasKey.empty())
            result["h"] = d.structured->hasKey;
        else if (d.structured->suffix != ShapeSuffix::None)
            result["s"] = std::string(shapeSuffixName(d.structured->suffix));
        return result;
    }

    /**
     * Count StructuredContent/ImplicitShape deps matching a JSON predicate.
     * The predicate receives the parsed JSON dep key.
     */
    static size_t countJsonDeps(const std::vector<ResolvedDep> & deps, CanonicalQueryKind type,
                                std::function<bool(const nlohmann::json &)> pred)
    {
        size_t n = 0;
        for (auto & d : deps) {
            if (d.type != type) continue;
            auto j = parseDepKey(d);
            if (j && pred(*j)) n++;
        }
        return n;
    }

    /// Check if any dep of a given type has a JSON key matching a predicate.
    static bool hasJsonDep(const std::vector<ResolvedDep> & deps, CanonicalQueryKind type,
                           std::function<bool(const nlohmann::json &)> pred)
    {
        return countJsonDeps(deps, type, std::move(pred)) > 0;
    }

    /// Match a #has:key dep: {"h": keyName, ...}
    static auto hasKeyPred(const std::string & keyName)
    {
        return [=](const nlohmann::json & j) {
            return j.contains("h") && j["h"].get<std::string>() == keyName;
        };
    }

    /// Match a shape suffix dep: {"s": suffixName, ...}
    static auto shapePred(const std::string & suffixName)
    {
        return [=](const nlohmann::json & j) {
            return j.contains("s") && j["s"].get<std::string>() == suffixName;
        };
    }

    /// Match any dep whose JSON path array contains these components (in order).
    static auto pathContainsPred(const nlohmann::json & expectedPath)
    {
        return [=](const nlohmann::json & j) {
            return j.contains("p") && j["p"] == expectedPath;
        };
    }

    /**
     * Count deps of a given type whose key contains a substring.
     * For non-JSON keys (Content, EnvVar, etc.), does plain substring matching.
     */
    static size_t countDeps(const std::vector<ResolvedDep> & deps, CanonicalQueryKind type, const std::string & keySubstr)
    {
        size_t n = 0;
        for (auto & d : deps)
            if (d.type == type && d.key.find(keySubstr) != std::string::npos)
                n++;
        return n;
    }

    /// Check if any dep of a given type has a key containing a substring.
    static bool hasDep(const std::vector<ResolvedDep> & deps, CanonicalQueryKind type, const std::string & keySubstr)
    {
        return countDeps(deps, type, keySubstr) > 0;
    }

    /// Count all deps of a given type.
    static size_t countDepsByType(const std::vector<ResolvedDep> & deps, CanonicalQueryKind type)
    {
        size_t n = 0;
        for (auto & d : deps)
            if (d.type == type)
                n++;
        return n;
    }

    /// Dump deps for diagnostic output in EXPECT failures.
    static std::string dumpDeps(const std::vector<ResolvedDep> & deps)
    {
        std::string result = "Deps (" + std::to_string(deps.size()) + "):\n";
        for (auto & d : deps) {
            result += "  [";
            result += queryKindName(d.type);
            result += "] " + d.key + "\n";
        }
        return result;
    }

    // ── Expression builder helpers ──────────────────────────────────

    /// Build `builtins.fromJSON (builtins.readFile PATH)` expression fragment.
    static std::string fj(const std::filesystem::path & path)
    {
        return "builtins.fromJSON (builtins.readFile " + path.string() + ")";
    }

    /// Build `builtins.fromTOML (builtins.readFile PATH)` expression fragment.
    static std::string ft(const std::filesystem::path & path)
    {
        return "builtins.fromTOML (builtins.readFile " + path.string() + ")";
    }

    /// Build `builtins.readDir PATH` expression fragment.
    static std::string rd(const std::filesystem::path & path)
    {
        return "builtins.readDir " + path.string();
    }
};

/**
 * Fixture for cross-scope materialization tests.
 * Extends DepPrecisionTest with getStoredDeps(attrPath) and getStoredResult(attrPath)
 * for querying stored traces directly via a fresh SqliteTraceStorage connection.
 */
class MaterializationDepTest : public DepPrecisionTest
{
protected:
    /**
     * Open a read-only SqliteTraceStorage pointed at the same SQLite file the
     * active session was writing to. Bootstraps with the per-expression
     * fingerprint (§N.1) of the MOST-RECENT `TraceCacheFixture::makeCache`
     * 2-arg call — stored in `lastPerExprFingerprint_` — so queries land
     * at the same `(session_key, AttrPathId(0))` slot the record path
     * used. Falls back to `testFingerprint` when no 2-arg call has
     * happened yet.
     *
     * CALLER CONSTRAINT: if a test calls `makeCache(exprA)`, then
     * `makeCache(exprB)`, then `makeQueryDb()`, the returned store
     * queries using `exprB`'s fingerprint and will NOT find rows
     * recorded under `exprA`'s session. Tests that need to query
     * multiple expressions must re-invoke `makeCache` with each
     * expression before the corresponding `makeQueryDb`. No runtime
     * enforcement — the failure mode is silent "row not found,"
     * which typically surfaces as an ASSERT_TRUE(result.has_value())
     * failure downstream.
     */
    SqliteTraceStorage makeQueryDb()
    {
        // Release the active session's backend first — its SQLite
        // connection must be closed before we can open a query connection.
        releaseActiveSession();
        auto & fp = lastPerExprFingerprint_.has_value()
                      ? *lastPerExprFingerprint_ : testFingerprint;
        auto bootstrapKey = SemanticSessionKey::fromSerialized(
            "bootstrap:"
            + fp.to_string(HashFormat::Base16, false));
        return SqliteTraceStorage(state.symbols, state.tracingPools(),
            state.vocabStore(), std::move(bootstrapKey));
    }

    /// Convert a dot-separated attr path string to an AttrPathId.
    AttrPathId pathFromDotted(const std::string & dotPath) {
        auto & v = state.vocabStore();
        if (dotPath.empty()) return AttrVocabStore::rootPath();
        auto id = AttrVocabStore::rootPath();
        size_t pos = 0;
        while (pos < dotPath.size()) {
            auto dot = dotPath.find('.', pos);
            auto component = dotPath.substr(pos, dot == std::string::npos ? dot : dot - pos);
            id = v.internPath(id, v.internName(component));
            pos = dot == std::string::npos ? dotPath.size() : dot + 1;
        }
        return id;
    }

    std::vector<ResolvedDep> getStoredDeps(const std::string & attrPath)
    {
        auto db = makeQueryDb();
        auto pathId = pathFromDotted(attrPath);
        auto result = test::TraceStorageTestAccess::verify(db, pathId, state);
        if (!result) return {};
        auto interned = withBs([&](const gdp::Proof<BlockingTag> & bs) {
            return db.withExclusiveAccess(bs, [&](const auto & ea) {
                return db.loadFullTrace(ea, result->traceId);
            });
        });
        std::vector<ResolvedDep> deps;
        deps.reserve(interned->size());
        for (const auto & idep : *interned)
            deps.push_back(db.resolveDep(idep));
        return deps;
    }

    std::optional<CachedResult> getStoredResult(const std::string & attrPath)
    {
        auto db = makeQueryDb();
        auto pathId = pathFromDotted(attrPath);
        auto result = test::TraceStorageTestAccess::verify(db, pathId, state);
        if (!result) return std::nullopt;
        return result->value;
    }
};

} // namespace nix::eval_trace::test
