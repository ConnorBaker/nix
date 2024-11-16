// FIXME: integrate this with nix path-info?
// FIXME: rename to 'nix store derivation show' or 'nix debug derivation show'?

#include "attr-path.hh"
#include "command.hh"
#include "eval-error.hh"
#include "installables.hh"
#include "logging.hh"
#include "path.hh"
#include "store-api.hh"
#include "derivations.hh"
#include <nlohmann/json.hpp>

using namespace nix;
using json = nlohmann::json;

struct CmdShowDerivation : InstallablesCommand
{
    bool recursive = false;

    CmdShowDerivation()
    {
        addFlag({
            .longName = "recursive",
            .shortName = 'r',
            .description = "Include the dependencies of the specified derivations.",
            .handler = {&recursive, true}
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

    Category category() override { return catUtility; }

    void run(ref<Store> store, Installables && installables) override
    {
        auto drvPaths = StorePathSet();
        auto jsonRoot = json::object();
        size_t numErrors = 0;

        // This loop takes the longest time relative to the others.
        // TODO:
        // error: attribute 'http_parser' in selection path 'legacyPackages.x86_64-linux.rubyPackages.http_parser.rb' not found
        for (const auto & installable : installables) {
            try {
                drvPaths.merge(Installable::toDerivations(store, {installable}, true));
            } 
            // Really should only catch EvalError and AttrPathNotFound.
            catch (Error & e) {
                e.addTrace(nullptr, "while evaluating the installable '%s'", installable->what());
                if (settings.keepGoing) {
                    ignoreExceptionExceptInterrupt();
                    numErrors++;
                } else {
                    throw;
                }
            }
        }

        if (recursive) {
            StorePathSet closure;
            store->computeFSClosure(drvPaths, closure);
            drvPaths = std::move(closure);
        }

        for (const auto & drvPath : drvPaths) {
            if (!drvPath.isDerivation()) {
                continue;
            }

            jsonRoot[store->printStorePath(drvPath)] =
                store->readDerivation(drvPath).toJSON(*store);
        }

        logger->cout(jsonRoot.dump());

        if (numErrors > 0) {
            throw Error("some errors (%s) were encountered during the evaluation", numErrors);
        }
    }
};

static auto rCmdShowDerivation = registerCommand2<CmdShowDerivation>({"derivation", "show"});
