#pragma once

#include "nix/expr/eval-cache.hh"
#include "nix/expr/file-load-tracker.hh"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace nix::eval_cache::test {

/**
 * RAII environment variable setter. Restores the original value on destruction.
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
 */
struct TempTestFile
{
    std::filesystem::path path;

    explicit TempTestFile(std::string_view content)
    {
        auto dir = std::filesystem::temp_directory_path() / "nix-test-eval-cache";
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
 * Use this in test fixtures that create EvalCacheDb or EvalCache,
 * which all need a writable cache directory for their SQLite databases.
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

// ── Dep factory helpers ──────────────────────────────────────────────

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

// ── AttrValue comparison ─────────────────────────────────────────────

/**
 * Compare two AttrValues for equality. Supports all variant types.
 * Throws on mismatch with descriptive message.
 */
void assertAttrValueEquals(const AttrValue & a, const AttrValue & b, SymbolTable & symbols);

} // namespace nix::eval_cache::test
