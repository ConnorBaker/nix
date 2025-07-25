#include "nix/cmd/command-installable-value.hh"
#include "nix/cmd/command.hh"
#include "nix/cmd/installable-value.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/main/shared.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/strings-inline.hh"

#include <nlohmann/json.hpp>

using namespace nix;

struct CmdEvalDrvs : InstallableValueCommand
{
private:
    bool forceRecurse = false;
    bool retryFailed = false;
    std::optional<std::string> evalAttrPath;

public:
    CmdEvalDrvs()
    {
        addFlag({
            .longName = "attr-path",
            .shortName = 'A',
            .description = "Attribute path to evaluate relative to the provided installable",
            .labels = {"attr-path"},
            .handler = Handler(&evalAttrPath),
        });

        addFlag({
            .longName = "force-recurse",
            .shortName = 'R',
            .description = "Recurse into attribute sets regardless of `recurseForDerivations`",
            .handler = Handler(&forceRecurse, true),
        });

        addFlag({
            .longName = "retry-failed",
            .shortName = 'F',
            .description = "Retry failed derivations",
            .handler = Handler(&retryFailed, true),
        });
    }

    auto description() -> std::string override
    {
        return "evaluate an attribute set of derivations";
    }

    auto doc() -> std::string override
    {
        return
#include "eval-drvs.md"
            ;
    }

    auto category() -> Category override
    {
        return catSecondary;
    }

    // No default attribute paths for this command.
    auto getDefaultFlakeAttrPaths() -> Strings override
    {
        return {""};
    };

    // No default attribute paths for this command.
    auto getDefaultFlakeAttrPathPrefixes() -> Strings override
    {
        return {""};
    };

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        auto state = installable->state;
        FutureVector futures(*state->executor);

        auto cursor = installable->getCursor(*state);

        // If an attrPath is provided, we use it to index into the installable.
        if (evalAttrPath.has_value()) {
            for (const auto attr : parseAttrPath(*state, evalAttrPath.value())) {
                logger->log(Verbosity::lvlDebug, "Cursor is at: " + cursor->getAttrPathStr());
                logger->log(
                    Verbosity::lvlDebug,
                    "Discovered attrs: " + concatStringsSep(", ", state->symbols.resolve(cursor->getAttrs())));

                cursor = cursor->getAttr(attr);
            }
        }

        auto shouldRecurse = [this, state](const ref<eval_cache::AttrCursor> cursor) -> bool {
            if (forceRecurse)
                return true;

            const auto maybeRecurse = cursor->maybeGetAttr(state->sRecurseForDerivations);
            return maybeRecurse && maybeRecurse->getBool();
        };

        auto logDrvAsJson = [state](const Symbol attr, const ref<eval_cache::AttrCursor> cursor) -> void {
            nlohmann::json j;

            j["attr"] = state->symbols[attr];
            j["attrPath"] = state->symbols.resolve(cursor->getAttrPath());
            j["drvPath"] = state->store->printStorePath(cursor->forceDerivation());

            // TODO: inputDrvs
            // j["inputDrvs"] = nlohmann::json::object();

            const auto maybeName = cursor->maybeGetAttr(state->sName);
            if (maybeName)
                j["name"] = maybeName->getString();

            // TODO: Is it possible that a derivation has no outputs?
            j["outputs"] = nlohmann::json::object();
            const auto maybeOutputs = cursor->maybeGetAttr(state->sOutputs);
            if (maybeOutputs) {
                for (const auto & output : maybeOutputs->getListOfStrings()) {
                    j["outputs"][output] = cursor->getAttr(output)->getAttr(state->sOutPath)->getString();
                }
            }

            const auto maybeSystem = cursor->maybeGetAttr(state->sSystem);
            if (maybeSystem)
                j["system"] = maybeSystem->getString();

            // Write the JSON to stdout.
            {
                const auto suspension = logger->suspend();
                logger->writeToStdout(j.dump());
            }
        };

        std::function<void(const ref<eval_cache::AttrCursor>)> visit = [&](const auto cursor) -> void {
            std::vector<std::pair<Executor::work_t, uint8_t>> work;
            for (const auto attr : cursor->getAttrs()) {
                work.emplace_back(
                    [this, attr, cursor, &logDrvAsJson, &shouldRecurse, &visit]() {
                        const auto newCursor = cursor->getAttr(attr);
                        const auto newCursorPath = newCursor->getAttrPathStr();
                        auto isDerivation = false;
                        auto doRecurse = false;

                        try {
                            isDerivation = newCursor->isDerivation();
                        } catch (const std::exception & e) {
                            logger->log(
                                Verbosity::lvlDebug,
                                "Failed to determine if " + newCursorPath + " is a derivation: " + e.what());
                            return;
                        }

                        // Each of these cases is mutually exclusive -- the condition we would branch on
                        // can throw, so we wrap each in a try-catch block.
                        if (isDerivation) {
                            try {
                                logDrvAsJson(attr, newCursor);
                            } catch (eval_cache::CachedEvalError & e) {
                                // Retry the evaluation of the attribute that failed to report it to the user.
                                if (retryFailed) {
                                    logger->log(Verbosity::lvlDebug, "Retrying failed evaluation of " + newCursorPath);
                                    try {
                                        e.force();
                                        logDrvAsJson(attr, newCursor);
                                    } catch (const std::exception & e) {
                                        logger->log(
                                            Verbosity::lvlError,
                                            "Failed to log derivation for " + newCursorPath + ": " + e.what());
                                    }
                                    return;
                                }

                                logger->log(
                                    Verbosity::lvlError,
                                    "Failed to log derivation for " + newCursorPath + ": " + e.what());
                            } catch (const std::exception & e) {
                                logger->log(
                                    Verbosity::lvlError,
                                    "Failed to log derivation for " + newCursorPath + ": " + e.what());
                            }
                            return;
                        }

                        try {
                            doRecurse = shouldRecurse(newCursor);
                        } catch (const std::exception & e) {
                            logger->log(
                                Verbosity::lvlError,
                                "Failed to determine if " + newCursorPath + " should recurse: " + e.what());
                            return;
                        }

                        if (doRecurse) {
                            logger->log(Verbosity::lvlDebug, "Found attribute set to recurse into: " + newCursorPath);
                            visit(newCursor);
                            return;
                        }

                        logger->log(Verbosity::lvlDebug, "Found non-derivation: " + newCursorPath);
                    },
                    0);
            };
            futures.spawn(std::move(work));
        };

        futures.spawn(1, [&]() { visit(cursor); });
        futures.finishAll();
    };
};

static auto rCmdEval = // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp)
    registerCommand<CmdEvalDrvs>("eval-drvs");
