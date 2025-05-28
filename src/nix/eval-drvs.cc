#include "nix/cmd/command-installable-value.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings-inline.hh"
#include <boost/interprocess/anonymous_shared_memory.hpp>
#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <algorithm>
#include <boost/interprocess/timed_utils.hpp>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <sched.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace nix;
using namespace boost::interprocess;

void _do(
    interprocess_semaphore & sem,
    const std::function<void()> childAction,
    const std::function<void()> parentPreWaitAction,
    const std::function<void()> parentPostWaitAction)
{
    std::chrono::time_point<std::chrono::system_clock> waitExpires =
        std::chrono::system_clock::now() + std::chrono::seconds(10); // Set a timeout of 10 seconds

    if (!sem.timed_wait(waitExpires)) {
        // If the semaphore timed out, we throw an error.
        // This is a safeguard against deadlocks or other issues.
        [[unlikely]] throw std::runtime_error("Semaphore timed out while waiting for child process to finish");
    }
    const pid_t pid = fork();
    switch (pid) {
    case -1: // fork failed
        [[unlikely]] throw std::runtime_error("fork failed");
    case 0: {
        int exitCode = 0;
        try {
            childAction();
        } catch (std::exception & e) {
            logger->cout("Error in child process: " + std::string(e.what()));
            exitCode = 1;
        }
        sem.post();
        _exit(exitCode);
    }
    default: {
        parentPreWaitAction(); // Execute any parent pre-wait actions

        int status;
        if (waitpid(pid, &status, 0) == -1) [[unlikely]]
            throw std::runtime_error("waitpid failed");

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) [[unlikely]]
            throw std::runtime_error("Child process did not exit cleanly");

        parentPostWaitAction(); // Execute any parent post-wait actions
        std::exit(0);           // Exit the parent process cleanly
    }
    };
}

namespace nix::fs {
using namespace std::filesystem;
}

struct CmdEvalDrvs : InstallableValueCommand, MixReadOnlyOption, MixPrintJSON
{
    bool json = true;
    bool outputPretty = false;
    int numProcesses = 32; // Default number of processes to use for evaluation

    CmdEvalDrvs()
        : InstallableValueCommand()
    {
        addFlag({
            .longName = "num-processes",
            .description = "Maximum number of processes to use for evaluation",
            .handler = {&numProcesses, 32},
        });
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

    // Returns a bool:
    // true => do not recurse, the value was a derivation or other primitive case
    // false => recurse, the value was a non-derivation attribute set
    bool _try_base_case(const std::vector<SymbolStr> & attrPath, EvalState & state, Value & v)
    {
        state.forceValue(v, noPos);

        // Early return if the value is not an attribute set.
        // Return true because we cannot recurse into it.
        if (nAttrs != v.type())
            return true;

        // Early return if the value is not a derivation.
        // Return false because we know the value is an attribute set and we will recurse into it.
        else if (!state.isDerivation(v))
            return false;

        else {
            // Taken from AttrCursor::forceDerivation()
            // auto drvPathValue = v.attrs()->get(state.sDrvPath)->value;
            // state.forceValue(*drvPathValue, noPos);
            // const auto drvPath = state.store->parseStorePath(drvPathValue->string_view());
            // drvPath.requireDerivation();

            // Create the JSON object
            nlohmann::json res;
            res["attrPath"] = attrPath;
            // res["drvPath"] = drvPath.to_string();
            res["stats"] = state.getStatistics();

            printJSON(res);

            // Return true to indicate no further recursion.
            return true;
        }
    }

    std::function<void()> _mk_recurse_child_action(
        const std::vector<SymbolStr> & attrPath,
        interprocess_semaphore & sem,
        EvalState & state,
        const std::vector<const Attr *> & attrs,
        const size_t & idx)
    {
        return [&]() -> void {
            if (idx >= attrs.size())
                throw std::runtime_error("Index for attrs is out of bounds");

            // TODO: Was it mutation of the vector which caused inscrutable errors?
            // NOPE! Seems to be the daemon being overwhelmed/busy; when we fork, certain resources aren't
            // duplicated/we hold the same locks as the parent. Need to find a clean way to reset them/get new locks.
            std::vector<SymbolStr> newAttrPath;
            newAttrPath.reserve(attrPath.size() + 1);
            newAttrPath.insert(newAttrPath.end(), attrPath.cbegin(), attrPath.cend());
            newAttrPath.push_back(state.symbols[attrs[idx]->name]);

            _recurse(newAttrPath, sem, state, *attrs[idx]->value);
        };
    };

    std::function<void()> _mk_recurse_parent_action(
        const std::vector<SymbolStr> & attrPath,
        interprocess_semaphore & sem,
        EvalState & state,
        const std::vector<const Attr *> & attrs,
        const size_t & idx)
    {
        return [&]() -> void {
            if (idx < attrs.size())
                _do(sem,
                    _mk_recurse_child_action(attrPath, sem, state, attrs, idx),
                    _mk_recurse_parent_action(attrPath, sem, state, attrs, idx + 1UL),
                    []() {});
        };
    }

    //
    void _recurse(const std::vector<SymbolStr> & attrPath, interprocess_semaphore & sem, EvalState & state, Value & v)
    {
        checkInterrupt();

        if (_try_base_case(attrPath, state, v) || v.attrs()->empty())
            return;

        // Value is known to be a non-empty, non-derivation attribute set at this point.
        // TODO: 2. Switch to breadth-first traversal.
        // Can I do that without forcing all attributes at the top-level of an attribute set and seeing which I need to
        // recurse into?
        const auto attrs = v.attrs()->lexicographicOrder(state.symbols);

        // Essentially a foldl over the vector.
        _do(sem,
            _mk_recurse_child_action(attrPath, sem, state, attrs, 0UL),
            _mk_recurse_parent_action(attrPath, sem, state, attrs, 1UL),
            []() {});
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        auto state = installable->state;
        auto cursor = installable->getCursor(*state);
        logger->stop();

        try {
            // Create an anonymous shared memory segment
            mapped_region region(anonymous_shared_memory(sizeof(interprocess_semaphore)));

            // Create an inter-process semaphore to synchronize access
            interprocess_semaphore * sem = new (region.get_address()) interprocess_semaphore(numProcesses);

            // In this case, we've got a non-empty attribute set to recurse into.
            _recurse(state->symbols.resolve(cursor->getAttrPath()), *sem, *state, cursor->forceValue());

            // The segment is unmapped when "region" goes out of scope
        } catch (interprocess_exception & ex) {
            // If we cannot create the shared memory segment, we throw an error.
            [[unlikely]] throw std::runtime_error("Failed to create shared memory segment: " + std::string(ex.what()));
        }
    }
};

static auto rCmdEval = registerCommand<CmdEvalDrvs>("eval-drvs");
