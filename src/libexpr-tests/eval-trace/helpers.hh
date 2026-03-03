#pragma once

#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/canon-path.hh"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

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
        auto dir = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
        std::filesystem::create_directories(dir);

        // Generate unique filename
        static int counter = 0;
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
        std::filesystem::remove(path);
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
        auto dir = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
        std::filesystem::create_directories(dir);
        static int counter = 0;
        path = dir / ("test-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + "." + std::string(ext));
        std::ofstream ofs(path);
        ofs << content;
    }

    void modify(std::string_view newContent)
    {
        std::ofstream ofs(path, std::ios::trunc);
        ofs << newContent;
    }

    ~TempExtFile() { std::filesystem::remove(path); }
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
        auto base = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
        std::filesystem::create_directories(base);
        static int counter = 0;
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
        std::filesystem::remove_all(dir_ / name);
    }
    void changeToSymlink(const std::string & name, const std::string & target)
    {
        std::filesystem::remove_all(dir_ / name);
        std::filesystem::create_symlink(target, dir_ / name);
    }
    void changeToSubdir(const std::string & name)
    {
        std::filesystem::remove_all(dir_ / name);
        std::filesystem::create_directory(dir_ / name);
    }

    ~TempDir() { std::filesystem::remove_all(dir_); }
    TempDir(const TempDir &) = delete;
    TempDir & operator=(const TempDir &) = delete;
};

/**
 * RAII helper that creates a temporary cache directory and sets NIX_CACHE_HOME.
 * Use this in test fixtures that create TraceStore or TraceCache,
 * which need a writable cache directory for their SQLite trace databases.
 * Required for sandbox builds where $HOME is /homeless-shelter (not writable).
 */
struct ScopedCacheDir
{
    std::filesystem::path dir;
    ScopedEnvVar envVar;

    ScopedCacheDir()
        : dir(std::filesystem::temp_directory_path()
              / ("nix-test-cache-" + std::to_string(getpid()) + "-" + std::to_string(nextId())))
        , envVar("NIX_CACHE_HOME", dir.string())
    {
        std::filesystem::create_directories(dir);
    }

    ~ScopedCacheDir()
    {
        std::filesystem::remove_all(dir);
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

/// Invalidate directory cache entries (for use inside test methods)
#define INVALIDATE_DIR(td) \
    do { \
        getFSSourceAccessor()->invalidateCache(CanonPath((td).path().string())); \
        StatHashStore::instance().clear(); \
    } while (0)

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

inline Dep makeContentDep(InterningPools & p, std::string_view key, std::string_view content)
{
    return {{DepType::Content, p.intern<DepSourceId>(""), p.intern<DepKeyId>(key)}, depHash(content)};
}

inline Dep makeEnvVarDep(InterningPools & p, std::string_view key, std::string_view value)
{
    return {{DepType::EnvVar, p.intern<DepSourceId>(""), p.intern<DepKeyId>(key)}, depHash(value)};
}

inline Dep makeExistenceDep(InterningPools & p, std::string_view key, bool exists)
{
    return {{DepType::Existence, p.intern<DepSourceId>(""), p.intern<DepKeyId>(key)},
        DepHashValue(exists ? std::string("type:1") : std::string("missing"))};
}

inline Dep makeSystemDep(InterningPools & p, std::string_view system)
{
    return {{DepType::System, p.intern<DepSourceId>(""), p.intern<DepKeyId>("")}, depHash(system)};
}

inline Dep makeCurrentTimeDep(InterningPools & p)
{
    return {{DepType::CurrentTime, p.intern<DepSourceId>(""), p.intern<DepKeyId>("")},
        DepHashValue(std::string("volatile"))};
}

inline Dep makeExecDep(InterningPools & p)
{
    return {{DepType::Exec, p.intern<DepSourceId>(""), p.intern<DepKeyId>("")},
        DepHashValue(std::string("volatile"))};
}

inline Dep makeCopiedPathDep(InterningPools & p, std::string_view key, std::string_view storePath)
{
    return {{DepType::CopiedPath, p.intern<DepSourceId>(""), p.intern<DepKeyId>(key)},
        DepHashValue(std::string(storePath))};
}

inline Dep makeNARContentDep(InterningPools & p, std::string_view key, const Blake3Hash & hash)
{
    return {{DepType::NARContent, p.intern<DepSourceId>(""), p.intern<DepKeyId>(key)}, hash};
}

inline Dep makeDirectoryDep(InterningPools & p, std::string_view key, const Blake3Hash & hash)
{
    return {{DepType::Directory, p.intern<DepSourceId>(""), p.intern<DepKeyId>(key)}, hash};
}

/// Create a ParentContext Dep from an AttrPathId and trace hash.
inline Dep makeParentContextDep(AttrPathId pathId, const Hash & traceHash)
{
    return Dep::makeParentContext(pathId, DepHashValue(Blake3Hash::fromHash(traceHash)));
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
class EvalTraceTest : public LibExprTest
{
public:
    EvalTraceTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {
        // Reset process-lifetime pools so each test starts with a clean
        // eval trace state. In production these outlive EvalState, but in
        // tests multiple EvalState instances exist in the same process.
        auto & pools = *state.traceCtx->pools;
        pools.strings.clear();
        pools.filePathPool.clear();
        pools.dataPathPool.clear();
        pools.sessionSymbols = nullptr;
        DependencyTracker::clearSessionTraces();
    }

protected:
    ScopedCacheDir cacheDir;
};

/**
 * Fixture for tests that use TraceCache (most eval-trace tests).
 * Provides makeCache(), forceRoot(), invalidateFileCache().
 */
class TraceCacheFixture : public EvalTraceTest
{
protected:
    Hash testFingerprint = hashString(HashAlgorithm::SHA256, "trace-cache-fixture");

    std::unique_ptr<TraceCache> makeCache(
        const std::string & nixExpr,
        int * loaderCalls = nullptr)
    {
        auto loader = [this, nixExpr, loaderCalls]() -> Value * {
            if (loaderCalls) (*loaderCalls)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        return std::make_unique<TraceCache>(
            testFingerprint, state, std::move(loader));
    }

    Value forceRoot(TraceCache & cache)
    {
        auto * v = cache.getRootValue();
        state.forceValue(*v, noPos);
        return *v;
    }

    void invalidateFileCache(const std::filesystem::path & path)
    {
        getFSSourceAccessor()->invalidateCache(CanonPath(path.string()));
        StatHashStore::instance().clear();
    }
};

/**
 * Fixture for tests that use TraceStore directly.
 * Provides makeDb() with a fixed test context hash.
 */
class TraceStoreFixture : public EvalTraceTest
{
protected:
    static constexpr int64_t testContextHash = 0x1234567890ABCDEF;

    AttrVocabStore & testVocab() {
        return state.traceCtx->getVocabStore(state.symbols);
    }

    /// Build an AttrPathId from string components.
    AttrPathId vpath(std::initializer_list<std::string_view> parts) {
        return vocabPath(testVocab(), parts);
    }

    /// Root path sentinel.
    AttrPathId rootPath() { return AttrVocabStore::rootPath(); }

    InterningPools & pools() { return *state.traceCtx->pools; }

    TraceStore makeDb()
    {
        return TraceStore(state.symbols, *state.traceCtx->pools,
            testVocab(), testContextHash);
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
    using ResolvedDep = TraceStore::ResolvedDep;

    static std::vector<ResolvedDep> resolveDeps(InterningPools & pools, const std::vector<Dep> & deps)
    {
        std::vector<ResolvedDep> result;
        result.reserve(deps.size());
        for (auto & d : deps) {
            result.push_back(ResolvedDep{
                std::string(pools.resolve(d.key.sourceId)),
                std::string(pools.resolve(d.key.keyId)),
                d.hash,
                d.key.type});
        }
        return result;
    }

    std::vector<ResolvedDep> evalAndCollectDeps(const std::string & nixExpr)
    {
        DependencyTracker tracker(*state.traceCtx->pools);
        state.traceActiveDepth++;
        (void) eval(nixExpr, /* forceValue */ true);
        state.traceActiveDepth--;
        return resolveDeps(*state.traceCtx->pools, tracker.collectTraces());
    }

    /**
     * Try to parse a dep key as JSON. Returns nullopt for non-JSON keys
     * (Content, Directory, EnvVar, etc. use plain strings).
     */
    static std::optional<nlohmann::json> parseDepKey(const ResolvedDep & d)
    {
        if (d.key.empty() || d.key[0] != '{') return std::nullopt;
        try { return nlohmann::json::parse(d.key); }
        catch (...) { return std::nullopt; }
    }

    /**
     * Count StructuredContent/ImplicitShape deps matching a JSON predicate.
     * The predicate receives the parsed JSON dep key.
     */
    static size_t countJsonDeps(const std::vector<ResolvedDep> & deps, DepType type,
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
    static bool hasJsonDep(const std::vector<ResolvedDep> & deps, DepType type,
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
    static size_t countDeps(const std::vector<ResolvedDep> & deps, DepType type, const std::string & keySubstr)
    {
        size_t n = 0;
        for (auto & d : deps)
            if (d.type == type && d.key.find(keySubstr) != std::string::npos)
                n++;
        return n;
    }

    /// Check if any dep of a given type has a key containing a substring.
    static bool hasDep(const std::vector<ResolvedDep> & deps, DepType type, const std::string & keySubstr)
    {
        return countDeps(deps, type, keySubstr) > 0;
    }

    /// Count all deps of a given type.
    static size_t countDepsByType(const std::vector<ResolvedDep> & deps, DepType type)
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
            result += depTypeName(d.type);
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
 * for querying stored traces directly via a fresh TraceStore connection.
 */
class MaterializationDepTest : public DepPrecisionTest
{
protected:
    TraceStore makeQueryDb()
    {
        int64_t contextHash;
        std::memcpy(&contextHash, testFingerprint.hash, sizeof(contextHash));
        return TraceStore(state.symbols, *state.traceCtx->pools,
            state.traceCtx->getVocabStore(state.symbols), contextHash);
    }

    /// Convert a dot-separated attr path string to an AttrPathId.
    AttrPathId pathFromDotted(const std::string & dotPath) {
        auto & v = state.traceCtx->getVocabStore(state.symbols);
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
        auto result = db.verify(pathId, {}, state);
        if (!result) return {};
        auto interned = db.loadFullTrace(result->traceId);
        std::vector<ResolvedDep> deps;
        deps.reserve(interned.size());
        for (auto & idep : interned)
            deps.push_back(db.resolveDep(idep));
        return deps;
    }

    std::optional<CachedResult> getStoredResult(const std::string & attrPath)
    {
        auto db = makeQueryDb();
        auto pathId = pathFromDotted(attrPath);
        auto result = db.verify(pathId, {}, state);
        if (!result) return std::nullopt;
        return result->value;
    }
};

} // namespace nix::eval_trace::test
