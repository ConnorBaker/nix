#pragma once

#include "nix/expr/trace-cache.hh"
#include "nix/expr/trace-store.hh"
#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/canon-path.hh"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

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
        clearStatHashMemoryCache(); \
    } while (0)

// ── Attr path helpers ───────────────────────────────────────────────

/**
 * Build a null-byte-separated attr path from components.
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
 * Convert an attr path (\0-separated) to a ParentContext dep key (\t-separated).
 * Matches the conversion in trace-cache.cc.
 */
inline std::string toDepKey(const std::string & attrPath)
{
    std::string key = attrPath;
    std::replace(key.begin(), key.end(), '\0', '\t');
    return key;
}

// ── Oracle dep factory helpers ──────────────────────────────────────

inline Dep makeContentDep(std::string_view key, std::string_view content)
{
    return Dep{"", std::string(key), depHash(content), DepType::Content};
}

inline Dep makeEnvVarDep(std::string_view key, std::string_view value)
{
    return Dep{"", std::string(key), depHash(value), DepType::EnvVar};
}

inline Dep makeExistenceDep(std::string_view key, bool exists)
{
    return Dep{
        "", std::string(key),
        DepHashValue(exists ? std::string("type:1") : std::string("missing")),
        DepType::Existence};
}

inline Dep makeSystemDep(std::string_view system)
{
    return Dep{"", "", depHash(system), DepType::System};
}

inline Dep makeCurrentTimeDep()
{
    return Dep{"", "", DepHashValue(std::string("volatile")), DepType::CurrentTime};
}

inline Dep makeExecDep()
{
    return Dep{"", "", DepHashValue(std::string("volatile")), DepType::Exec};
}

inline Dep makeCopiedPathDep(std::string_view key, std::string_view storePath)
{
    return Dep{"", std::string(key), DepHashValue(std::string(storePath)), DepType::CopiedPath};
}

inline Dep makeNARContentDep(std::string_view key, const Blake3Hash & hash)
{
    return Dep{"", std::string(key), hash, DepType::NARContent};
}

inline Dep makeDirectoryDep(std::string_view key, const Blake3Hash & hash)
{
    return Dep{"", std::string(key), hash, DepType::Directory};
}

inline Dep makeParentContextDep(std::string_view depKey, const Hash & traceHash)
{
    Blake3Hash b3;
    static_assert(sizeof(b3.bytes) == 32);
    std::memcpy(b3.bytes.data(), traceHash.hash, 32);
    return Dep{"", std::string(depKey), DepHashValue(b3), DepType::ParentContext};
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
    {}

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
        clearStatHashMemoryCache();
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

    TraceStore makeDb()
    {
        return TraceStore(state.symbols, testContextHash);
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

} // namespace nix::eval_trace::test
