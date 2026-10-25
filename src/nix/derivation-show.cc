// FIXME: integrate this with `nix path-info`?
// FIXME: rename to 'nix store derivation show'?

#include "nix/cmd/command.hh"
#include "nix/main/common-args.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace nix {

struct CmdShowDerivation : InstallablesCommand, MixPrintJSON
{
    bool recursive = false;

    CmdShowDerivation()
    {
        addFlag({
            .longName = "recursive",
            .shortName = 'r',
            .description = "Include the dependencies of the specified derivations.",
            .handler = {&recursive, true},
        });
    }

    std::string description() override
    {
        return "show the contents of a store derivation";
    }

    std::string doc() override
    {
        return
#include "derivation-show.md"
            ;
    }

    void run(ref<Store> store, Installables && installables) override
    {
        auto evalStore = getEvalStore();
        auto drvPaths = Installable::toDerivations(evalStore, store, installables, true);
        auto drvStore = Installable::getStoreForDerivations(evalStore, store, drvPaths);

        if (recursive) {
            StorePathSet closure;
            drvStore->computeFSClosure(drvPaths, closure);
            drvPaths = std::move(closure);
        }

        json jsonRoot = json::object();

        for (auto & drvPath : drvPaths) {
            if (!drvPath.isDerivation())
                continue;

            jsonRoot[drvPath.to_string()] = drvStore->readDerivation(drvPath);
        }
        printJSON(
            nlohmann::json{
                {"version", expectedJsonVersionDerivation},
                {"derivations", std::move(jsonRoot)},
            });
    }
};

static auto rCmdShowDerivation = registerCommand2<CmdShowDerivation>({"derivation", "show"});

} // namespace nix
