#include "nix/store/globals.hh"
#include "nix/cmd/installable-flake.hh"
#include "nix/cmd/installable-derived-path.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/util.hh"
#include "nix/cmd/command.hh"
#include "nix/expr/attr-path.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/store/derivations.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/store/store-api.hh"
#include "nix/main/shared.hh"
#include "nix/flake/flake.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/util/url.hh"
#include "nix/fetchers/registry.hh"
#include "nix/store/build-result.hh"

#include <regex>
#include <queue>

#include <nlohmann/json.hpp>

namespace nix {

std::vector<std::string> InstallableFlake::getActualAttrPaths()
{
    std::vector<std::string> res;
    if (attrPaths.size() == 1 && attrPaths.front().starts_with(".")) {
        attrPaths.front().erase(0, 1);
        res.push_back(attrPaths.front());
        return res;
    }

    for (auto & prefix : prefixes)
        res.push_back(prefix + *attrPaths.begin());

    for (auto & s : attrPaths)
        res.push_back(s);

    return res;
}

static std::string showAttrPaths(const std::vector<std::string> & paths)
{
    std::string s;
    for (const auto & [n, i] : enumerate(paths)) {
        if (n > 0)
            s += n + 1 == paths.size() ? " or " : ", ";
        s += '\'';
        s += i;
        s += '\'';
    }
    return s;
}

InstallableFlake::InstallableFlake(
    SourceExprCommand * cmd,
    ref<EvalState> state,
    FlakeRef && flakeRef,
    std::string_view fragment,
    ExtendedOutputsSpec extendedOutputsSpec,
    Strings attrPaths,
    Strings prefixes,
    const flake::LockFlags & lockFlags)
    : InstallableValue(state)
    , flakeRef(flakeRef)
    , attrPaths(fragment == "" ? attrPaths : Strings{(std::string) fragment})
    , prefixes(fragment == "" ? Strings{} : prefixes)
    , extendedOutputsSpec(std::move(extendedOutputsSpec))
    , lockFlags(lockFlags)
{
    if (cmd && cmd->getAutoArgs(*state)->size())
        throw UsageError("'--arg' and '--argstr' are incompatible with flakes");
}

DerivedPathsWithInfo InstallableFlake::toDerivedPaths()
{
    Activity act(*logger, lvlTalkative, actUnknown, fmt("evaluating derivation '%s'", what()));

    auto evalCache = openTraceCache(*state, getLockedFlake());
    auto * root = evalCache->getRootValue();
    state->forceValue(*root, noPos);

    auto emptyArgs = state->buildBindings(0).finish();
    auto attrPaths = getActualAttrPaths();
    Suggestions suggestions;

    Value * vp = nullptr;
    std::string resolvedPath;
    for (auto & ap : attrPaths) {
        debug("trying flake output attribute '%s'", ap);
        try {
            auto [v, pos] = findAlongAttrPath(*state, ap, *emptyArgs, *root);
            vp = v;
            resolvedPath = ap;
            break;
        } catch (Error & e) {
            suggestions += e.info().suggestions;
        }
    }

    if (!vp)
        throw Error(suggestions, "flake '%s' does not provide attribute %s", flakeRef, showAttrPaths(attrPaths));

    state->forceValue(*vp, noPos);

    if (state->settings.verifyTraceCache)
        evalCache->verifyCold(resolvedPath, *vp);

    if (vp->type() != nAttrs || !state->isDerivation(*vp)) {
        if (std::optional derivedPathWithInfo = trySinglePathToDerivedPaths(
                *vp, noPos, fmt("while evaluating the flake output attribute '%s'", resolvedPath))) {
            return {*derivedPathWithInfo};
        } else {
            throw Error(
                "expected flake output attribute '%s' to be a derivation or path but found %s: %s",
                resolvedPath,
                showType(*vp),
                ValuePrinter(*this->state, *vp, errorPrintOptions));
        }
    }

    // Ensure .drv exists in store
    auto * aDrvPath = vp->attrs()->get(state->s.drvPath);
    if (!aDrvPath)
        throw Error("derivation does not have a 'drvPath' attribute");
    state->forceValue(*aDrvPath->value, aDrvPath->pos);
    auto drvPath = state->store->parseStorePath(aDrvPath->value->string_view());
    drvPath.requireDerivation();
    if (!state->store->isValidPath(drvPath)) {
        /* The eval trace may have returned a traced drvPath from a previous
           session, but the .drv file was garbage-collected. Perform fresh
           evaluation via the rootLoader to regenerate it (BSàlC: rebuild).
           This also re-copies any source paths without derivers (e.g.,
           .patch files added via builtins.path or path coercion) that were
           GC'd along with the .drv that referenced them. */
        auto * realRoot = evalCache->getOrEvaluateRoot();
        state->forceValue(*realRoot, noPos);
        auto [v2, pos2] = findAlongAttrPath(*state, resolvedPath, *emptyArgs, *realRoot);
        state->forceValue(*v2, noPos);
        if (v2->type() == nAttrs) {
            auto * aDP2 = v2->attrs()->get(state->s.drvPath);
            if (aDP2) {
                state->forceValue(*aDP2->value, aDP2->pos);
                drvPath = state->store->parseStorePath(aDP2->value->string_view());
            }
        }
        if (!state->store->isValidPath(drvPath))
            throw Error(
                "don't know how to recreate store derivation '%s'!",
                state->store->printStorePath(drvPath));
    }

    std::optional<NixInt::Inner> priority;

    if (vp->attrs()->get(state->s.outputSpecified)) {
    } else if (auto * aMeta = vp->attrs()->get(state->s.meta)) {
        state->forceValue(*aMeta->value, aMeta->pos);
        if (aMeta->value->type() == nAttrs) {
            if (auto * aPriority = aMeta->value->attrs()->get(state->symbols.create("priority"))) {
                state->forceValue(*aPriority->value, aPriority->pos);
                priority = aPriority->value->integer().value;
            }
        }
    }

    return {{
        .path =
            DerivedPath::Built{
                .drvPath = makeConstantStorePathRef(std::move(drvPath)),
                .outputs = std::visit(
                    overloaded{
                        [&](const ExtendedOutputsSpec::Default &) -> OutputsSpec {
                            StringSet outputsToInstall;
                            if (auto * aOutputSpecified = vp->attrs()->get(state->s.outputSpecified)) {
                                state->forceValue(*aOutputSpecified->value, aOutputSpecified->pos);
                                if (aOutputSpecified->value->type() == nBool && aOutputSpecified->value->boolean()) {
                                    if (auto * aOutputName = vp->attrs()->get(state->symbols.create("outputName"))) {
                                        state->forceValue(*aOutputName->value, aOutputName->pos);
                                        outputsToInstall = {std::string(aOutputName->value->string_view())};
                                    }
                                }
                            } else if (auto * aMeta = vp->attrs()->get(state->s.meta)) {
                                state->forceValue(*aMeta->value, aMeta->pos);
                                if (aMeta->value->type() == nAttrs) {
                                    if (auto * aOTI = aMeta->value->attrs()->get(state->symbols.create("outputsToInstall"))) {
                                        state->forceValue(*aOTI->value, aOTI->pos);
                                        if (aOTI->value->type() == nList) {
                                            for (auto elem : aOTI->value->listView()) {
                                                state->forceValue(*elem, noPos);
                                                outputsToInstall.insert(std::string(elem->string_view()));
                                            }
                                        }
                                    }
                                }
                            }

                            if (outputsToInstall.empty())
                                outputsToInstall.insert("out");

                            return OutputsSpec::Names{std::move(outputsToInstall)};
                        },
                        [&](const ExtendedOutputsSpec::Explicit & e) -> OutputsSpec { return e; },
                    },
                    extendedOutputsSpec.raw),
            },
        .info = make_ref<ExtraPathInfoFlake>(
            ExtraPathInfoValue::Value{
                .priority = priority,
                .attrPath = resolvedPath,
                .extendedOutputsSpec = extendedOutputsSpec,
            },
            ExtraPathInfoFlake::Flake{
                .originalRef = flakeRef,
                .lockedRef = getLockedFlake()->flake.lockedRef,
            }),
    }};
}

std::pair<Value *, PosIdx> InstallableFlake::toValue(EvalState & state)
{
    auto evalCache = openTraceCache(state, getLockedFlake());
    auto * root = evalCache->getRootValue();
    state.forceValue(*root, noPos);

    auto attrPaths = getActualAttrPaths();
    Suggestions suggestions;
    auto emptyArgs = state.buildBindings(0).finish();

    std::pair<Value *, PosIdx> found{nullptr, noPos};
    for (auto & attrPath : attrPaths) {
        debug("trying flake output attribute '%s'", attrPath);
        try {
            found = findAlongAttrPath(state, attrPath, *emptyArgs, *root);
            resolvedAttrPath_ = attrPath;
            break;
        } catch (Error & e) {
            suggestions += e.info().suggestions;
        }
    }

    if (!found.first)
        throw Error(suggestions, "flake '%s' does not provide attribute %s", flakeRef, showAttrPaths(attrPaths));

    if (state.settings.verifyTraceCache) {
        state.forceValue(*found.first, noPos);
        evalCache->verifyCold(resolvedAttrPath_, *found.first);
    }

    return found;
}


ref<flake::LockedFlake> InstallableFlake::getLockedFlake() const
{
    if (!_lockedFlake) {
        flake::LockFlags lockFlagsApplyConfig = lockFlags;
        // FIXME why this side effect?
        lockFlagsApplyConfig.applyNixConfig = true;
        _lockedFlake = make_ref<flake::LockedFlake>(lockFlake(flakeSettings, *state, flakeRef, lockFlagsApplyConfig));
    }
    // _lockedFlake is now non-null but still just a shared_ptr
    return ref<flake::LockedFlake>(_lockedFlake);
}

FlakeRef InstallableFlake::nixpkgsFlakeRef() const
{
    auto lockedFlake = getLockedFlake();

    if (auto nixpkgsInput = lockedFlake->lockFile.findInput({"nixpkgs"})) {
        if (auto lockedNode = std::dynamic_pointer_cast<const flake::LockedNode>(nixpkgsInput)) {
            if (lockedNode->isFlake) {
                debug("using nixpkgs flake '%s'", lockedNode->lockedRef);
                return std::move(lockedNode->lockedRef);
            }
        }
    }

    return defaultNixpkgsFlakeRef();
}

} // namespace nix
