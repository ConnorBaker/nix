#include "nix/cmd/command-installable-value.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings-inline.hh"
#include "nix/util/util.hh"

#include <bits/chrono.h>
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/interprocess/timed_utils.hpp>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace nix;
using namespace boost::interprocess;

const nlohmann::json
_getJSON(const EvalState & state, const std::vector<SymbolStr> & attrPath, const PackageInfo & packageInfo)
{
    nlohmann::json result;

    // TODO: Either remove cpuTime from statistics or find a way to do a before/after forcing the derivation path
    // to get some sort of marginal cost for the evaluation.
    result["attr"] = packageInfo.attrPath;
    result["attrPath"] = attrPath;
    result["drvPath"] = packageInfo.queryDrvPath()->to_string();
    result["name"] = packageInfo.queryName();
    // TODO: outputs
    result["stats"] = state.getStatistics();
    result["system"] = packageInfo.querySystem();

    return result;
}

void _doFork(
    const std::function<void()> & doFailure,
    const std::function<int()> & doChild,
    const std::function<void(pid_t)> & doParent)
{
    const auto pid = fork();
    switch (pid) {
    [[unlikely]] case -1: // fork failed
        doFailure();
        return;
    case 0:
        _exit(doChild());
    default:
        doParent(pid);
        return;
    }
}

struct CmdEvalDrvs : InstallableValueCommand, MixReadOnlyOption, MixPrintJSON
{
    bool json = true;
    bool outputPretty = false;
    bool forceRecurse = false;
    uint32_t maxProcesses = 32;

    CmdEvalDrvs()
        : InstallableValueCommand()
    {
        // TODO: As implemented, this is a misnomer.
        addFlag({
            .longName = "max-processes",
            .shortName = 'P',
            .description =
                "Maximum number of processes to use for simultaneous evaluation (actual number may be higher)",
            .labels = {"n"},
            .handler = {[&](const std::string s) { maxProcesses = string2IntWithUnitPrefix<uint32_t>(s); }},
        });

        addFlag({
            .longName = "force-recurse",
            .shortName = 'R',
            .description =
                "When set, forces recursion into attribute sets even if they do not set `recurseForDerivations`",
            .handler = {&forceRecurse, true},
        });

        // TODO: Add "ignore at root level" flag, to ignore names which appear at the root level
        // TODO: Add "ignore at any level" flag, to ignore names which appear at any level
    }

    std::string description() override
    {
        return "evaluate an attribute set of derivations";
    }

    std::string doc() override
    {
        return
#include "eval-drvs.md"
            ;
    }

    Category category() override
    {
        return catSecondary;
    }

    // Mutates state and forcedValue.
    // It is assumed the value has already been forced, like by _testDerivation.
    void _baseCase(
        interprocess_mutex & loggerMutex,
        EvalState & state,
        const std::vector<SymbolStr> & attrPath,
        PackageInfo & packageInfo)
    {
        const auto result = _getJSON(state, attrPath, packageInfo);

        // Acquire the logger mutex and write the result to stdout.
        loggerMutex.lock();
        logger->writeToStdout(result.dump());
        loggerMutex.unlock();
    }

    void _recursiveCase(
        interprocess_mutex & loggerMutex,
        interprocess_semaphore & evalTokens,
        EvalState & state,
        std::vector<SymbolStr> & attrPath,
        const Bindings * const attrs)
    {
        // Keep track of child processes.
        std::unordered_map<pid_t, SymbolStr> pidToSymbolStr;
        pidToSymbolStr.reserve(attrs->size());

        const auto failureFunc = [&]() -> void {
            throw std::runtime_error(concatStringsSep(".", attrPath) + ": fork failed");
        };

        const auto childFunc = [&](const auto & value) -> int {
            int exitCode = 0;
            try {
                // TODO:
                // This is gross and probably doesn't work in the way I hope it does.
                // The goal is to force re-creation of the file descriptors/sockets used for the build and eval
                // store.
                state.store->init();
                state.buildStore->init();
                _step(loggerMutex, evalTokens, state, attrPath, *value);
            } catch (std::exception & e) {
                loggerMutex.lock();
                // TODO: e.what() doesn't post the Nix stack trace.
                logger->log(
                    Verbosity::lvlError,
                    "processing " + concatStringsSep(".", attrPath) + " failed: " + std::string(e.what()));
                loggerMutex.unlock();
                exitCode = 1;
            }
            return exitCode;
        };

        // Updates pidToSymbolStr with the pid and the last element of attrPath.
        const auto parentFunc = [&](const auto symbolStr, const auto pid) -> void {
            pidToSymbolStr.emplace(pid, symbolStr);
        };

        for (const auto attr : *attrs) {
            const auto symbolStr = state.symbols[attr.name];

            // Add the attribute name to the path.
            attrPath.push_back(symbolStr);

            // Must take eval token in the parent before calling _step in the child.
            evalTokens.wait();

            // Fork a child process to evaluate the attribute.
            _doFork(
                failureFunc, std::bind(childFunc, attr.value), std::bind(parentFunc, symbolStr, std::placeholders::_1));

            // Remove the last element from the attribute path.
            attrPath.pop_back();
        }

        // Wait for all of the children, looping through them so long as there is at least one which has not been
        // awaited.
        while (!pidToSymbolStr.empty()) {
            pidToSymbolStr.erase(std::erase_if(pidToSymbolStr, [&](const auto tup2) -> bool {
                const auto [pid, symbolStr] = tup2;

                int status = 0;
                const auto result = waitpid(pid, &status, WNOHANG | WUNTRACED);
                if (result == 0)
                    // Need to check later
                    return false;
                else if (result > 0) {
                    // Log if it exited poorly.
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        attrPath.push_back(symbolStr);
                        loggerMutex.lock();
                        logger->log(
                            Verbosity::lvlError,
                            "child processing " + concatStringsSep(".", attrPath) + " did not exit cleanly");
                        loggerMutex.unlock();
                        attrPath.pop_back();
                    }
                    // Mark for removal.
                    return true;
                } else {
                    attrPath.push_back(symbolStr);
                    loggerMutex.lock();
                    logger->log(
                        Verbosity::lvlError, "waitpid failed for child processing " + concatStringsSep(".", attrPath));
                    loggerMutex.unlock();
                    attrPath.pop_back();
                    // Check later
                    return false;
                }
            }));

            // NOTE: It helps (noticeably) to have a small sleep here so the parent isn't in a hot loop.
            {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(1s);
            }
        }
    }

    // NOTE: _step must only ever be called from the child process of a fork.
    // NOTE: It is assumed that prior to _step being called, a eval token is taken.
    void _step(
        interprocess_mutex & loggerMutex,
        interprocess_semaphore & evalTokens,
        EvalState & state,
        std::vector<SymbolStr> & attrPath,
        Value & value)
    {
        checkInterrupt();

        // TODO: Find a way to push evaluation warnings and errors into the JSON output.

        // TODO: forceValue can throw.
        state.forceValue(value, value.determinePos(noPos));
        if (nAttrs == value.type() && !value.attrs()->empty()) {
            // TODO: isDerivation can throw.
            if (state.isDerivation(value)) {
                PackageInfo packageInfo(state, concatStringsSep(".", attrPath), value.attrs());
                try {
                    _baseCase(loggerMutex, state, attrPath, packageInfo);
                } catch (std::exception & e) {
                    evalTokens.post(); // Release as part of getting ready for cleanup and exit.
                    throw e;
                }
                evalTokens.post(); // Release as part of getting ready for cleanup and exit.
            } else {
                // Get the attribute set we'll be iterating over.
                const auto attrs = value.attrs();

                // NOTE: Performing the check for whether we should recurse or not here, rather than in _recursiveCase,
                // allows us to force recursion into the root attribute set since the first iteration is special-cased
                // in run.

                // If we are not forcing recursion, we need to check if the attribute set has recurseForDerivations set
                // to true.
                if (!forceRecurse) {
                    // Try to get the recurseForDerivations attribute.
                    const auto recurseForDerivationsAttr = attrs->get(state.sRecurseForDerivations);

                    // If recurseForDerivations is not present, we do not recurse.
                    if (!recurseForDerivationsAttr)
                        return;

                    // Get the value of recurseForDerivations.
                    const auto recurseForDerivationsValue = recurseForDerivationsAttr->value;
                    state.forceValue(*recurseForDerivationsValue, recurseForDerivationsValue->determinePos(attrs->pos));
                    // TODO: check if recurseForDerivationsValue is a boolean, and throw an error if it is not.

                    // If recurseForDerivations is not set to true, we do not recurse; otherwise continue.
                    if (!recurseForDerivationsValue->boolean())
                        return;
                }

                evalTokens.post(); // Release prior to recursing.
                _recursiveCase(loggerMutex, evalTokens, state, attrPath, attrs);
            }
        }
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        if (outputPretty)
            throw std::runtime_error("The --output-pretty flag is not supported by this command.");

        const auto state = installable->state;
        const auto cursor = installable->getCursor(*state);
        logger->stop();

        try {
            // Create anonymous shared memory segments.
            // They are unmapped when we go out of scope.
            mapped_region loggerMutexRegion(anonymous_shared_memory(sizeof(interprocess_mutex)));
            mapped_region evalTokensRegion(anonymous_shared_memory(sizeof(interprocess_semaphore)));

            // Create inter-process semaphore to synchronize access
            interprocess_mutex * loggerMutex = new (loggerMutexRegion.get_address()) interprocess_mutex();
            interprocess_semaphore * evalTokens =
                new (evalTokensRegion.get_address()) interprocess_semaphore(maxProcesses);

            // Get the attribute path
            auto attrPath = state->symbols.resolve(cursor->getAttrPath());

            // Copied from _step but without the token stuff
            auto forcedValue = cursor->forceValue();
            if (nAttrs == forcedValue.type() && !forcedValue.attrs()->empty()) {
                if (state->isDerivation(forcedValue)) {
                    PackageInfo packageInfo(*state, concatStringsSep(".", attrPath), forcedValue.attrs());
                    _baseCase(*loggerMutex, *state, attrPath, packageInfo);
                } else {
                    const auto attrs = forcedValue.attrs();
                    _recursiveCase(*loggerMutex, *evalTokens, *state, attrPath, attrs);
                }
            }
        } catch (interprocess_exception & ex) {
            // If we cannot create the shared memory segment, we throw an error.
            throw std::runtime_error("Failed to create shared memory segment(s): " + std::string(ex.what()));
        }
    }
};

static auto rCmdEval = registerCommand<CmdEvalDrvs>("eval-drvs");
