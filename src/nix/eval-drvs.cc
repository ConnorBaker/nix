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

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace nix;

void _wait_parent_action(const pid_t pid)
{
    waitpid(pid, nullptr, 0);
}

void _do(std::function<void()> childAction, std::function<void(const pid_t)> parentAction)
{
    const pid_t pid = fork();
    switch (pid) {
    case -1: // fork failed
        [[unlikely]] throw std::runtime_error("fork failed");
    case 0:
        childAction();
        break;
    default:
        parentAction(pid);
        break;
    };
    std::exit(0);
}

namespace nix::fs {
using namespace std::filesystem;
}

struct CmdEvalDrvs : InstallableValueCommand, MixReadOnlyOption, MixPrintJSON
{
    bool json = true;
    bool outputPretty = false;

    CmdEvalDrvs()
        : InstallableValueCommand()
    {
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
            // res["stats"] = state.getStatistics();

            // printJSON(res);

            // Return true to indicate no further recursion.
            return true;
        }
    }

    std::function<void()> _mk_recurse_child_action(
        const std::vector<SymbolStr> & attrPath,
        EvalState & state,
        const std::vector<const Attr *> & attrs,
        const size_t & idx)
    {
        return [&]() -> void {
            if (idx >= attrs.size())
                throw std::runtime_error("Index for attrs is out of bounds");

            // TODO: Was it mutation of the vector which caused inscrutable errors?
            // NOPE! Seems to be the daemon being overwhelmed/busy; when we fork, certain resources aren't duplicated/
            // we hold the same locks as the parent. Need to find a clean way to reset them/get new locks.
            std::vector<SymbolStr> newAttrPath;
            newAttrPath.reserve(attrPath.size() + 1);
            newAttrPath.insert(newAttrPath.end(), attrPath.cbegin(), attrPath.cend());
            newAttrPath.push_back(state.symbols[attrs[idx]->name]);

            _recurse(newAttrPath, state, *attrs[idx]->value);
        };
    };

    std::function<void(const pid_t)> _mk_recurse_parent_action(
        const std::vector<SymbolStr> & attrPath,
        EvalState & state,
        const std::vector<const Attr *> & attrs,
        const size_t & idx)
    {
        return [&](const pid_t pid) -> void {
            if (idx < attrs.size()) {
                _do(_mk_recurse_child_action(attrPath, state, attrs, idx),
                    _mk_recurse_parent_action(attrPath, state, attrs, idx + 1));
            }
            _wait_parent_action(pid);
        };
    }

    void _recurse(const std::vector<SymbolStr> & attrPath, EvalState & state, Value & v)
    {
        checkInterrupt();

        if (_try_base_case(attrPath, state, v))
            return;

        // Value is known to be an attribute set and not a derivation in this branch.
        // TODO: 2. Switch to breadth-first traversal.
        // Can I do that without forcing all attributes at the top-level of an attribute set and seeing which I need to recurse into?

        // TODO: Do I need to iterate over it in lexicographic order?
        const auto attrs = v.attrs()->lexicographicOrder(state.symbols);
        // If there are no elements to process, exit.
        if (attrs.empty())
            return;

        // Otherwise, we have at least one element to process, have the child to process it and have the parent try to
        // process the second and further element. Essentially a foldl over the vector.
        _do(_mk_recurse_child_action(attrPath, state, attrs, 0UL),
            _mk_recurse_parent_action(attrPath, state, attrs, 1UL));
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        auto state = installable->state;
        auto cursor = installable->getCursor(*state);
        logger->stop();

        // Can't help the initial need to force the value, not sure how else to get a value.
        _recurse(state->symbols.resolve(cursor->getAttrPath()), *state, cursor->forceValue());
    }
};

static auto rCmdEval = registerCommand<CmdEvalDrvs>("eval-drvs");
