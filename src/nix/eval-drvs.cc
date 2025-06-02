#include "nix/cmd/command-installable-value.hh"
#include "nix/cmd/command.hh"
#include "nix/cmd/installable-value.hh"
#include "nix/cmd/installable-value.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/main/shared.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings-inline.hh"
#include "nix/util/util.hh"

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

auto getJSON(
    const bool showStats,
    const EvalState & state,
    const std::vector<SymbolStr> & attrPath,
    const PackageInfo & packageInfo) -> const nlohmann::json
{
    nlohmann::json result;

    // TODO: Either remove cpuTime from statistics or find a way to do a before/after forcing the derivation path
    // to get some sort of marginal cost for the evaluation.
    result["attr"] = packageInfo.attrPath;
    result["attrPath"] = attrPath;
    result["drvPath"] = state.store->printStorePath(packageInfo.requireDrvPath());
    result["name"] = packageInfo.queryName();
    // TODO: outputs
    if (showStats)
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

// Forks the current process and runs the child function in the child process.
// If the fork fails, the failure function is called in the parent process.
// The child process exits with the return value of the child function.
// NOTE: Because control flow returns to the parent process after this function, there is no
// `onParent` argument.
template<class FFailure, class FChild>
auto doFork(
    FFailure && onFailure, // :: () -> void
    FChild && onChild      // :: () -> int
    ) -> pid_t
{
    const auto pid = fork();
    [[unlikely]] if (pid < 0) {
        std::forward<FFailure>(onFailure)();
        return pid;
    } else if (pid == 0)
        _exit(std::forward<FChild>(onChild)());
    else
        return pid;
}

// Returns true if the child process was successfully waited for, false otherwise.
template<class FFailure, class FNotReady, class FSuccess>
auto doWait(
    const pid_t pid,
    FFailure && onFailure,   // :: (pid_t, int) -> void
    FNotReady && onNotReady, // :: (pid_t, int) -> void
    FSuccess && onSuccess    // :: (pid_t, int) -> void
    ) -> bool
{
    int status = 0;
    const auto awaitedPid = waitpid(pid, &status, WNOHANG | WUNTRACED);

    [[unlikely]] if (awaitedPid < 0) {
        std::forward<FFailure>(onFailure)(awaitedPid, status);
        return false;
    } else if (awaitedPid == 0) {
        std::forward<FNotReady>(onNotReady)(awaitedPid, status);
        return false;
    } else {
        std::forward<FSuccess>(onSuccess)(awaitedPid, status);
        return true;
    }
}

template<std::ranges::input_range Range, class FEach, class FWait>
void doForEachParallel(
    Range && r,      // :: Range a
    FEach && onEach, // :: a -> (b, c)
    FWait && onWait  // :: (b, c) -> bool
)
{
    using namespace std::chrono_literals;

    // Populate the pidMap.
    // NOTE: Keys are required to be unique.
    auto pidMap = std::forward<Range>(r) | std::views::transform(std::forward<FEach>(onEach))
                  | std::ranges::to<std::unordered_map>();

    // Drain the pidMap.
    const auto waitAndShouldErasePredicate = std::forward<FWait>(onWait);
    while (!pidMap.empty()) {
        // Remove all of the children which have finished.
        pidMap.erase(std::erase_if( // NOLINT(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
            pidMap,
            waitAndShouldErasePredicate));

        // NOTE: It helps (noticeably) to have a small sleep here so the parent isn't in a hot loop.
        std::this_thread::sleep_for(500ms);
    }
}

struct PushDownParallel
{
    // This is a helper struct to push down parallelism into the `doForEachParallel` function.
    // It is used to avoid capturing the `this` pointer in lambdas, which can cause issues with
    // shared ownership and lifetime management.

    // TODO: Access to the logger, or just a function we can use to log?
    // TODO: Access to write output, or just a function we can use to write output?
    PushDownParallel(
        // Required for evaluation
        std::vector<SymbolStr> attrPath,
        ref<EvalState> state,
        Value & value,

        // Configuration
        const uint32_t maxProcesses,
        const bool forceRecurse,
        const bool showStats)
        : logger(makeSimpleLogger())
        , attrPath(std::move(attrPath))
        , attrPathStr(concatStringsSep(".", this->attrPath))
        , state(std::move(state))
        , value(value)
        , forceRecurse(forceRecurse)
        , showStats(showStats)
        , loggerMutexRegion(anonymous_shared_memory(sizeof(interprocess_mutex)))
        , loggerMutex(static_cast<interprocess_mutex *>(loggerMutexRegion.get_address()))
        , evalTokensRegion(anonymous_shared_memory(sizeof(interprocess_semaphore)))
        , evalTokens(static_cast<interprocess_semaphore *>(evalTokensRegion.get_address()))
    // TODO: For things which are known as a result of configuration (e.g., forceRecurse, showStats),
    // we should select implementations of the methods generated through template specialization.
    // This would allow us to avoid the overhead of the `if` checks in the methods.
    {
        // Construct in place using placement new:
        new (loggerMutex) interprocess_mutex();
        new (evalTokens) interprocess_semaphore(maxProcesses);
    }

public:

    // TODO: Should we try to do something similar to what findAlongAttrPath does and use
    // autoCalling to force the value?

    void run()
    {
        state->forceValue(value, value.determinePos(noPos));
        if (nAttrs == value.type() && !value.attrs()->empty()) {
            const auto & attrs = *value.attrs();
            if (state->isDerivation(value))
                _baseCase(PackageInfo(*state, attrPathStr, &attrs));
            else
                _recursiveCase(attrs);
        }
    }

private:
    // Logger
    std::unique_ptr<Logger> logger;

    // Required for evaluation
    std::vector<SymbolStr> attrPath;
    std::string attrPathStr; // Cached instead of recomputing it every time.
    ref<EvalState> state;
    Value value; // Cached forced value, if applicable.

    // Configuration
    bool forceRecurse;
    bool showStats;

    // Required for parallelism
    mapped_region loggerMutexRegion;
    interprocess_mutex * loggerMutex; // placement-new in shared memory, do not delete

    mapped_region evalTokensRegion;
    interprocess_semaphore * evalTokens; // placement-new in shared memory, do not delete

    void _log(const Verbosity lvl, const std::string & msg)
    {
        loggerMutex->lock();
        logger->log(lvl, msg);
        loggerMutex->unlock();
    }

    // It is assumed the value has already been forced, like by _testDerivation.
    void _baseCase(const PackageInfo & packageInfo)
    {
        const auto result = getJSON(showStats, *state, attrPath, packageInfo);

        // Acquire the logger mutex and write the result to stdout.
        loggerMutex->lock();
        logger->writeToStdout(result.dump());
        loggerMutex->unlock();
    }

    void _recursiveCase(const Bindings & attrs)
    {
        doForEachParallel(
            attrs,

            // onEach
            [&](const Attr attr) -> std::tuple<const pid_t, const SymbolStr> {
                const auto symbolStr = state->symbols[attr.name];
                evalTokens->wait(); // Must take eval token in the parent before calling _step in the child.
                const auto pid = doFork(
                    // onFailure
                    [&]() -> void { throw std::runtime_error(attrPathStr + "." + symbolStr + ": fork failed"); },
                    // onChild
                    [&]() -> int {
                        try {
                            // TODO:
                            // This is gross and probably doesn't work in the way I hope it does.
                            // The goal is to force re-creation of the file descriptors/sockets used for the build and
                            // eval store.
                            // TODO: Create a pool of file descriptors/sockets which can be reused across children.
                            // TODO: This is unecessary if the store is owned by the user rather than the daemon.
                            *const_cast<ref<Store> *>(&state->store) = // NOLINT(cppcoreguidelines-pro-type-const-cast)
                                state->store->config.openStore();
                            // TODO: We need the build store in case of IFD, right?
                            *const_cast<ref<Store> *>( // NOLINT(cppcoreguidelines-pro-type-const-cast)
                                &state->buildStore) = state->buildStore->config.openStore();

                            // Update the object now that we're in the child process (and so these changes are visible
                            // only to the child).
                            // NOTE: These steps mirror the dependencies in the constructor.
                            attrPath.push_back(symbolStr);
                            attrPathStr += "." + symbolStr;
                            value = *attr.value;

                            // NOTE: _step catches exceptions and releases the eval token before re-throwing.
                            _step();
                            return 0; // Success.
                        } catch (std::exception & e) {
                            _log(Verbosity::lvlError, e.what());
                            return 1; // Failure.
                        }
                    });
                return std::make_tuple(pid, symbolStr);
            },

            // onWait
            [&](std::tuple<const pid_t, const SymbolStr> pidAndSymbolStr) -> bool {
                // NOTE: If we do tuple unpacking, clang-tidy complains about symbolStr being a null pointer.
                return doWait(
                    std::get<0>(pidAndSymbolStr),
                    // onFailure
                    [&](const auto...) -> void {
                        _log(
                            Verbosity::lvlError,
                            "waitpid failed for child processing " + attrPathStr + "." + std::get<1>(pidAndSymbolStr)
                                + ": " + std::string(strerror(errno)));
                    },
                    // onNotReady
                    [](const auto...) -> void {},
                    // onSuccess
                    [](const auto...) -> void {});
            });
    }

    // NOTE: _step must only ever be called from the child process of a fork.
    // NOTE: It is assumed that prior to _step being called, an eval token is taken.
    void _step()
    {
        checkInterrupt();

        // TODO: Find a way to push evaluation warnings and errors into the JSON output.

        // TODO: Ensure every path through _step releases the eval token.
        // Generally, we want to hold on to it as long as possible (for as much evaluation as possible) but we
        // release it before recursing into the attribute set.

        try {
            state->forceValue(value, value.determinePos(noPos));

            // Only case where we have anything to do.
            if (nAttrs == value.type() && !value.attrs()->empty()) {
                const auto & attrs = *value.attrs();

                if (state->isDerivation(value)) {
                    _baseCase(PackageInfo(*state, attrPathStr, &attrs));
                    evalTokens->post(); // Release for case with derivation.
                }

                // NOTE: Performing the check for whether we should recurse or not here, rather than in _recursiveCase,
                // allows us to force recursion into the root attribute set since the first iteration is special-cased
                // in run.
                else if (shouldRecurse(forceRecurse, *state, attrs)) {
                    evalTokens->post(); // Release prior to recursing, but after eval required for shouldRecurse.
                    _recursiveCase(attrs);
                }

                // Release for case without recursion.
                else
                    evalTokens->post();
            }

            // Release for case with non-attribute set value.
            else
                evalTokens->post();
        } catch (std::exception & e) {
            evalTokens->post(); // Release for case with an exception.
            state->error<nix::EvalError>("evaluation of %s failed: %s", attrPathStr, e.what()).debugThrow();
        }
    }
};

struct CmdEvalDrvs : InstallableValueCommand
{
private:
    std::vector<std::string> commonAttrPaths;
    bool forceRecurse = false;
    uint32_t maxProcesses = 1;
    std::optional<std::string> evalAttrPath;
    bool showStats = false;

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
            .longName = "common-attr-paths",
            .shortName = 'C',
            .description = "Common attribute paths to evaluate before the installable (must be specified last)",
            .labels = {"attr-paths"},
            .handler = Handler(&commonAttrPaths),
        });

        addFlag({
            .longName = "force-recurse",
            .shortName = 'R',
            .description = "Recurse into attribute sets regardless of `recurseForDerivations`",
            .handler = Handler(&forceRecurse, true),
        });

        // TODO: As implemented, this is a misnomer.
        addFlag({
            .longName = "max-processes",
            .shortName = 'P',
            .description =
                "Maximum number of processes to use for simultaneous evaluation (actual number may be higher)",
            .labels = {"n"},
            .handler = Handler(&maxProcesses),
        });

        addFlag({
            .longName = "show-stats",
            .shortName = 'S',
            .description = "Output stats for each derivation evaluated",
            .handler = Handler(&showStats, true),
        });

        // TODO: Add support for regex for attribute paths to ignore.
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
        if (maxProcesses <= 0)
            throw UsageError("max-processes must be greater than 0");

        auto state = installable->state;
        auto & rootValue = *installable->toValue(*state).first;
        auto & autoArgs = *getAutoArgs(*state);
        auto attrPath = state->symbols.resolve(installable->getCursor(*state)->getAttrPath());

        // Process all of the common attribute paths first.
        processCommonAttrPaths(*state, autoArgs, rootValue, commonAttrPaths);

        // If an attrPath is provided, we use it to index into the installable.
        if (evalAttrPath.has_value()) {
            rootValue = *findAlongAttrPath(*state, *evalAttrPath, autoArgs, rootValue).first;
            // Update the attrPath to include the evaluated attribute path.
            for (const auto & attrPathComponent : parseAttrPath(*state, evalAttrPath.value()))
                attrPath.push_back(state->symbols[attrPathComponent]);
        }

        try {
            auto pdp = PushDownParallel(
                // Required for evaluation
                attrPath,
                state,
                rootValue,

                // Configuration
                maxProcesses,
                forceRecurse,
                showStats);

            // Run the evaluation in parallel.
            pdp.run();
        } catch (interprocess_exception & ex) {
            // If we cannot create the shared memory segment, we throw an error.
            throw std::runtime_error("Failed to create shared memory segment(s): " + std::string(ex.what()));
        }
    }

private:
    void processCommonAttrPaths(
        EvalState & state, Bindings & autoArgs, Value & rootValue, const std::vector<std::string> & commonAttrPaths)
    {
        // All we need to do for the common installables is to force their values, and, if they are derivations,
        // force their derivations.
        // We need to do this through the value rather than the cursor because evaluation caching doesn't force
        // state like we need.
        // NOTE: InstallableValue's toValue() uses findAlongAttrPath() to find the value, which uses autoCalling;
        // cursor->forceValue() does not.
        for (const auto & commonAttrPathStr : commonAttrPaths) {
            auto & value = *findAlongAttrPath(state, commonAttrPathStr, autoArgs, rootValue).first;
            if (state.isDerivation(value)) {
                const auto drvPath =
                    state.store->printStorePath(PackageInfo(state, commonAttrPathStr, value.attrs()).requireDrvPath());
                logger->log(
                    Verbosity::lvlError,
                    "Forced derivation (" + drvPath + ") for common installable: " + commonAttrPathStr.c_str());
            } else
                logger->log(
                    Verbosity::lvlError,
                    "Forced value (" + showType(value) + ") for common installable: " + commonAttrPathStr.c_str());
        }
    }
};

static auto rCmdEval = // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp)
    registerCommand<CmdEvalDrvs>("eval-drvs");
