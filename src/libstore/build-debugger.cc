#include "nix/store/build-debugger.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

namespace nix {

std::filesystem::path writeDebuggerRedirectAttachInfo(
    Store & store,
    const StorePath & drvPath,
    std::string_view remoteHost)
{
    auto printed = store.printStorePath(drvPath);

    auto dir = std::filesystem::path(settings.nixStateDir) / "debugger";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Enforce 0700 on the directory explicitly (create_directories
    // respects only the umask). Mirrors the local attach-info publisher.
    if (!ec)
        (void) ::chmod(dir.c_str(), 0700);

    auto target = dir / (std::string(drvPath.hashPart()) + ".attach");

    nlohmann::json info = {
        {"schemaVersion", kDebuggerAttachInfoSchemaVersion},
        {"drvPath", printed},
        {"remoteHost", std::string(remoteHost)},
    };

    auto tmp = target;
    tmp += ".tmp";
    try {
        writeFile(tmp.string(), info.dump(), /*mode=*/0600);
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::filesystem::remove(tmp);
            throw SysError(
                "publishing build-debugger redirect to `%s`: %s",
                target.string(), ec.message());
        }
        return target;
    } catch (std::exception & e) {
        std::filesystem::remove(tmp);
        // Non-fatal: losing the redirect just forces the user into an
        // explicit `nix debug-attach --on <host>` invocation. Surface a
        // warning so they can see what happened.
        logWarning({.msg = HintFmt(
            "failed to publish build-debugger redirect: %s (falling "
            "back to manual `nix debug-attach --on %s %s`)",
            e.what(),
            std::string(remoteHost),
            printed)});
        return {};
    }
}

} // namespace nix
