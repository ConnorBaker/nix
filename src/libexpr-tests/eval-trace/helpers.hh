#pragma once

#include "nix/expr/trace-cache.hh"
#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace nix::eval_trace::test {

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

// ── Oracle dep factory helpers (BSàlC: trace dep constructors) ───────

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

// ── CachedResult comparison (trace result equality) ─────────────────────

/**
 * Compare two CachedResults for equality. Supports all variant types.
 * Used to verify that verified trace results match expected values (BSàlC: result equivalence).
 */
void assertCachedResultEquals(const CachedResult & a, const CachedResult & b, SymbolTable & symbols);

} // namespace nix::eval_trace::test
