#include "nix/cmd/command-installable-value.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings-inline.hh"
#include "nix/util/util.hh"

#include <algorithm>
#include <bits/chrono.h>
#include <bits/elements_of.h>
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/interprocess/timed_utils.hpp>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <nlohmann/json.hpp>
#include <ranges>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace nix;
using namespace boost::interprocess;

// NOTE: EvalState attributes to be concerned about:
//
// - storeFS
// - rootFS
// - corepkgsFS
// - internalFS
// - derivationInternal
// - store
// - buildStore
// - inputCache
// - evalCaches
// - srcToStore
// - fileParseCache
// - fileEvalCache
// - positionToDocComment
// - lookupPathResolved
//
// In terms of direct dependencies on the store attributes:
//
// - storeFS is initialized with store
// - rootFS is initialized with store (and storeFS!)

auto getJSON(const EvalState & state, const std::vector<SymbolStr> & attrPath, const PackageInfo & packageInfo)
    -> const nlohmann::json
{
    nlohmann::json result;

    // TODO: Either remove cpuTime from statistics or find a way to do a before/after forcing the derivation path
    // to get some sort of marginal cost for the evaluation.
    result["attr"] = packageInfo.attrPath;
    result["attrPath"] = attrPath;
    result["drvPath"] = state.store->printStorePath(packageInfo.requireDrvPath());
    result["name"] = packageInfo.queryName();
    // TODO: outputs
    result["stats"] = state.getStatistics();
    result["system"] = packageInfo.querySystem();

    return result;
}

auto shouldRecurse(const bool forceRecurse, EvalState & state, const Bindings & attrs) -> bool
{
    if (forceRecurse)
        return true;

    // If recurseForDerivations is not present, we do not recurse.
    const auto recurseForDerivationsAttr = attrs.get(state.sRecurseForDerivations);
    if (nullptr == recurseForDerivationsAttr)
        return false;

    // Get the value of recurseForDerivations.
    auto recurseForDerivationsValue = *recurseForDerivationsAttr->value;
    return state.forceBool(
        recurseForDerivationsValue,
        recurseForDerivationsValue.determinePos(attrs.pos),
        "while evaluating the `recurseForDerivations` attribute");
}

template<class FFailure, class FChild, class FParent>
void doFork(FFailure && onFailure, FChild && onChild, FParent && onParent)
{
    const auto pid = fork();
    switch (pid) {
    [[unlikely]] case -1: // fork failed
        std::forward<FFailure>(onFailure)();
        return;
    case 0:
        _exit(std::forward<FChild>(onChild)());
    default:
        std::forward<FParent>(onParent)(pid);
        return;
    }
}

template<class FWait, class FFailure, class FNotReady, class FSuccess>
void doWait(FWait && onWait, FFailure && onFailure, FNotReady && onNotReady, FSuccess && onSuccess)
{
    const auto [pid, status] = std::forward<FWait>(onWait)();
    [[unlikely]] if (pid < 0) // It's unlikely waitpid failed, but it can happen.
        std::forward<FFailure>(onFailure)(pid, status);
    else if (pid == 0)
        std::forward<FNotReady>(onNotReady)(pid, status);
    else
        std::forward<FSuccess>(onSuccess)(pid, status);
}

// Specialized for unordered_map, as erase and erase_if performance differ across containers.
template<typename K, typename V, class FPred>
void doUnorderedWaits(std::unordered_map<K, V> & tasks, FPred && taskIsComplete)
{
    using namespace std::chrono_literals;
    // NOTE: Cannot forward in the loop, as that would be a use-after-move.
    auto pred = std::forward<FPred>(taskIsComplete);
    while (!tasks.empty()) {
        tasks.erase(std::erase_if( // NOLINT(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
            tasks,
            pred));
        // NOTE: It helps (noticeably) to have a small sleep here so the parent isn't in a hot loop.
        std::this_thread::sleep_for(500ms);
    }
}

struct CmdEvalDrvs : InstallableValueCommand, MixPrintJSON
{
    bool json = true;
    bool outputPretty = false;
    bool forceRecurse = false;

    // TODO: See if we can re-use the logic for `cores`.
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
            .handler = {[&](const std::string & s) { maxProcesses = string2IntWithUnitPrefix<uint32_t>(s); }},
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

    // It is assumed the value has already been forced, like by _testDerivation.
    void _baseCase(
        interprocess_mutex & loggerMutex,
        EvalState & state,
        const std::vector<SymbolStr> & attrPath,
        const PackageInfo & packageInfo)
    {
        const auto result = getJSON(state, attrPath, packageInfo);

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
        const Bindings & attrs)
    {
        std::unordered_map<pid_t, SymbolStr> pidToSymbolStr;
        pidToSymbolStr.reserve(attrs.size()); // Reserve space for the number of attributes.

        std::ranges::for_each(attrs, [&](auto attr) {
            const auto symbolStr = state.symbols[attr.name];
            pid_t pid = 0;     // Initialize pid to 0, so we can use it in the lambda below.
            evalTokens.wait(); // Must take eval token in the parent before calling _step in the child.
            doFork(
                // onFailure
                [&]() -> void {
                    throw std::runtime_error(concatStringsSep(".", attrPath) + "." + symbolStr + ": fork failed");
                },
                // onChild
                [&]() -> int {
                    attrPath.push_back(symbolStr); // Only visible to the child.
                    try {
                        // TODO:
                        // This is gross and probably doesn't work in the way I hope it does.
                        // The goal is to force re-creation of the file descriptors/sockets used for the build and
                        // eval store.
                        // TODO: Create a pool of file descriptors/sockets which can be reused across children.
                        *const_cast<ref<Store> *>(&state.store) = // NOLINT(cppcoreguidelines-pro-type-const-cast)
                            state.store->config.openStore();
                        *const_cast<ref<Store> *>( // NOLINT(cppcoreguidelines-pro-type-const-cast)
                            &state.buildStore) = state.buildStore->config.openStore();
                        _step(loggerMutex, evalTokens, state, attrPath, *attr.value);
                    } catch (std::exception & e) {
                        loggerMutex.lock();
                        logger->log(Verbosity::lvlError, e.what());
                        loggerMutex.unlock();
                        return 1; // Failure.
                    }
                    return 0; // Success.
                },
                // onParent
                [&](const pid_t pid_) -> void { pid = pid_; });

            pidToSymbolStr.emplace(pid, symbolStr); // Store the pid and symbolStr in the map.
        });

        // Wait for all of the children to finish.
        doUnorderedWaits(pidToSymbolStr, [&](const std::tuple<const pid_t, const SymbolStr> tup2) -> bool {
            const auto [pid, symbolStr] = tup2;
            bool completed = false;
            doWait(
                // onWait
                [pid]() -> std::tuple<const pid_t, const int> {
                    int status = 0;
                    const auto pid_ = waitpid(
                        // For some reason, the clang-analyzer can't figure out that `pid` is neither
                        // being dereferenced nor is null.
                        pid, // NOLINT(clang-analyzer-core.NullDereference)
                        &status,
                        WNOHANG | WUNTRACED);
                    return {pid_, status};
                },
                // onFailure
                [&](const pid_t, const int) -> void {
                    attrPath.push_back(symbolStr);
                    loggerMutex.lock();
                    logger->log(
                        Verbosity::lvlError,
                        "waitpid failed for child processing " + concatStringsSep(".", attrPath) + ": "
                            + std::string(strerror(errno)));
                    loggerMutex.unlock();
                    attrPath.pop_back();
                    completed = false;
                },
                // onNotReady
                [&](const pid_t, const int) -> void { completed = false; },
                // onSuccess
                [&](const pid_t, const int) -> void { completed = true; });
            return completed;
        });
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

        // TODO: Ensure every path through _step releases the eval token.
        // Generally, we want to hold on to it as long as possible (for as much evaluation as possible) but we
        // release it before recursing into the attribute set.

        try {
            state.forceValue(value, value.determinePos(noPos));
            if (nAttrs == value.type() && !value.attrs()->empty()) {
                const auto & attrs = *value.attrs();

                // TODO: isDerivation can throw.
                if (state.isDerivation(value)) {
                    _baseCase(
                        loggerMutex, state, attrPath, PackageInfo(state, concatStringsSep(".", attrPath), &attrs));
                    evalTokens.post(); // Release for case with derivation.
                }
                // NOTE: Performing the check for whether we should recurse or not here, rather than in _recursiveCase,
                // allows us to force recursion into the root attribute set since the first iteration is special-cased
                // in run.
                else if (shouldRecurse(forceRecurse, state, attrs)) {
                    evalTokens.post(); // Release prior to recursing, but after eval required for shouldRecurse.
                    _recursiveCase(loggerMutex, evalTokens, state, attrPath, attrs);
                } else
                    evalTokens.post(); // Release for case without recursion.
            } else
                evalTokens.post(); // Release for case with non-attribute set value.
        } catch (std::exception & e) {
            evalTokens.post(); // Release for case with an exception.
            state.error<nix::EvalError>("evaluation of %s failed: %s", concatStringsSep(".", attrPath), e.what())
                .debugThrow();
        }
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        if (outputPretty)
            throw std::runtime_error("The --output-pretty flag is not supported by this command.");

        auto & state = *installable->state;
        auto & cursor = *installable->getCursor(state);
        logger->stop();

        try {
            // Create anonymous shared memory segments.
            // They are unmapped when we go out of scope.
            mapped_region loggerMutexRegion(anonymous_shared_memory(sizeof(interprocess_mutex)));
            mapped_region evalTokensRegion(anonymous_shared_memory(sizeof(interprocess_semaphore)));

            // Create inter-process semaphore to synchronize access
            auto & loggerMutex = *new (loggerMutexRegion.get_address()) interprocess_mutex();
            auto & evalTokens = *new (evalTokensRegion.get_address()) interprocess_semaphore(maxProcesses);

            // Get the attribute path
            auto attrPath = state.symbols.resolve(cursor.getAttrPath());

            // Copied from _step but without the token stuff
            // TODO: The output attrPath does not include the root?
            // For example, if run with .#hydraJobs, all of the output attrPaths are rooted at children of `hydraJobs`,
            // rather than at `hydraJobs` itself.
            auto forcedValue = cursor.forceValue();
            if (nAttrs == forcedValue.type() && !forcedValue.attrs()->empty()) {
                const auto & attrs = *forcedValue.attrs();
                if (state.isDerivation(forcedValue))
                    _baseCase(
                        loggerMutex, state, attrPath, PackageInfo(state, concatStringsSep(".", attrPath), &attrs));
                else
                    _recursiveCase(loggerMutex, evalTokens, state, attrPath, attrs);
            }
        } catch (interprocess_exception & ex) {
            // If we cannot create the shared memory segment, we throw an error.
            throw std::runtime_error("Failed to create shared memory segment(s): " + std::string(ex.what()));
        }
    }
};

static auto rCmdEval = // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp)
    registerCommand<CmdEvalDrvs>("eval-drvs");
