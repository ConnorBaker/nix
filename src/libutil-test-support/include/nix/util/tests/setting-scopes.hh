#pragma once
///@file
/**
 * Global settings set for a scope and restored on exit, for tests that
 * must not depend on the platform's or the process's defaults.
 */

#include "nix/util/config-global.hh"
#include "nix/util/configuration.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/error.hh"
#include "nix/util/file-system.hh"

#include <filesystem>
#include <map>
#include <string>

namespace nix {

/**
 * The `use-case-hack` setting (a registered global setting, archive.cc) for
 * the scope: on by default on Apple only, so a test of the case hack sets
 * it rather than depend on the platform.
 *
 * @throws Error if the setting is not registered.
 */
struct WithCaseHack
{
    std::string saved;

    explicit WithCaseHack(bool on)
    {
        std::map<std::string, Config::SettingInfo> settings;
        globalConfig.getSettings(settings);
        saved = settings.at("use-case-hack").value;
        if (!globalConfig.set("use-case-hack", on ? "true" : "false"))
            throw Error("test scope: 'use-case-hack' is not a registered setting");
    }

    ~WithCaseHack()
    {
        globalConfig.set("use-case-hack", saved);
    }
};

/**
 * A cache directory of the fixture's own: the fetchers' memos live in
 * `getCacheDir()` (`NIX_CACHE_HOME` first), so with this a test neither
 * reads the developer's memos nor writes into them, and it holds where no
 * home directory is writable (the sandboxed package build).  A base class
 * or first member, so that it is set before the fixture's evaluator or
 * `fetchers::Settings` is constructed.
 */
struct FreshCacheHome
{
    std::filesystem::path dir = createTempDir();
    AutoDelete deleteDir{dir, true};

    FreshCacheHome()
    {
        setEnv("NIX_CACHE_HOME", dir.string().c_str());
    }
};

} // namespace nix
