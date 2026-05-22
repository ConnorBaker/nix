#include "nix/util/users.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"

#ifndef _WIN32
#  include "unix/xdg-dirs.hh"
#else
#  include "nix/util/windows-known-folders.hh"
#endif

namespace nix {

namespace {

/* Platform-specific defaults for the five Nix per-user/system
   directories, computed once at startup. The `NIX_*_HOME` environment
   overrides are still applied per call in the wrappers below; this
   table only holds the fallback paths that would otherwise be selected
   by a per-call `#ifdef _WIN32` switch.

   On Windows the five logical directories collapse onto two known
   folders: `cache`, `data`, and `state` all live under `LocalAppData`
   (machine-local, not roamed across machines), and `config` lives under
   `RoamingAppData` (roamed). `extraConfigDirs` is the system-wide config
   search path; it is empty on Windows because the known-folders model
   has no analogue of `XDG_CONFIG_DIRS`. */
struct NixDirDefaults
{
    std::filesystem::path cacheDir;
    std::filesystem::path configDir;
    std::filesystem::path dataDir;
    std::filesystem::path stateDir;
    std::vector<std::filesystem::path> extraConfigDirs;
};

const NixDirDefaults & nixDirDefaults()
{
    static const NixDirDefaults defaults = [] {
        NixDirDefaults d;
#ifndef _WIN32
        d.cacheDir = unix::xdg::getCacheHome() / "nix";
        d.configDir = unix::xdg::getConfigHome() / "nix";
        d.dataDir = unix::xdg::getDataHome() / "nix";
        d.stateDir = unix::xdg::getStateHome() / "nix";
        for (auto & dir : unix::xdg::getConfigDirs())
            d.extraConfigDirs.push_back(dir / "nix");
#else
        auto localAppData = windows::known_folders::getLocalAppData();
        d.cacheDir = localAppData / "nix" / "cache";
        d.configDir = windows::known_folders::getRoamingAppData() / "nix" / "config";
        d.dataDir = localAppData / "nix" / "data";
        d.stateDir = localAppData / "nix" / "state";
#endif
        return d;
    }();
    return defaults;
}

} // namespace

std::filesystem::path getCacheDir()
{
    auto dir = getEnvOs(OS_STR("NIX_CACHE_HOME"));
    if (dir)
        return *dir;
    return nixDirDefaults().cacheDir;
}

std::filesystem::path getConfigDir()
{
    auto dir = getEnvOs(OS_STR("NIX_CONFIG_HOME"));
    if (dir)
        return *dir;
    return nixDirDefaults().configDir;
}

std::vector<std::filesystem::path> getConfigDirs()
{
    std::vector<std::filesystem::path> result;
    result.push_back(getConfigDir());
    auto & extras = nixDirDefaults().extraConfigDirs;
    result.insert(result.end(), extras.begin(), extras.end());
    return result;
}

std::filesystem::path getDataDir()
{
    auto dir = getEnvOs(OS_STR("NIX_DATA_HOME"));
    if (dir)
        return *dir;
    return nixDirDefaults().dataDir;
}

std::filesystem::path getStateDir()
{
    auto dir = getEnvOs(OS_STR("NIX_STATE_HOME"));
    if (dir)
        return *dir;
    return nixDirDefaults().stateDir;
}

std::filesystem::path createNixStateDir()
{
    std::filesystem::path dir = getStateDir();
    createDirs(dir);
    return dir;
}

std::string expandTilde(std::string_view path)
{
    // TODO: expand ~user ?
    auto tilde = path.substr(0, 2);
    if (tilde == "~/" || tilde == "~") {
        auto suffix = path.size() >= 2 ? std::string(path.substr(2)) : std::string{};
        return (getHome() / suffix).string();
    } else
        return std::string(path);
}

} // namespace nix
