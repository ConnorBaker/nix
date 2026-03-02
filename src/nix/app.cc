#include "nix/cmd/installables.hh"
#include "nix/cmd/installable-derived-path.hh"
#include "nix/cmd/installable-value.hh"
#include "nix/store/store-api.hh"
#include "nix/store/globals.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval.hh"
#include "nix/store/names.hh"
#include "nix/cmd/command.hh"
#include "nix/store/derivations.hh"
#include "nix/store/downstream-placeholder.hh"

namespace nix {

/**
 * Return the rewrites that are needed to resolve a string whose context is
 * included in `dependencies`.
 */
StringPairs resolveRewrites(Store & store, const std::vector<BuiltPathWithResult> & dependencies)
{
    StringPairs res;
    if (!experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
        return res;
    }
    for (auto & dep : dependencies) {
        auto drvDep = std::get_if<BuiltPathBuilt>(&dep.path);
        if (!drvDep) {
            continue;
        }

        for (const auto & [outputName, outputPath] : drvDep->outputs) {
            res.emplace(
                DownstreamPlaceholder::fromSingleDerivedPathBuilt(
                    SingleDerivedPath::Built{
                        .drvPath = make_ref<SingleDerivedPath>(drvDep->drvPath->discardOutputPath()),
                        .output = outputName,
                    })
                    .render(),
                store.printStorePath(outputPath));
        }
    }
    return res;
}

/**
 * Resolve the given string assuming the given context.
 */
std::string
resolveString(Store & store, const std::string & toResolve, const std::vector<BuiltPathWithResult> & dependencies)
{
    auto rewrites = resolveRewrites(store, dependencies);
    return rewriteStrings(toResolve, rewrites);
}

/**
 * Force a derivation and ensure its .drv file exists in the store.
 */
static StorePath ensureDerivationInStore(EvalState & state, Value & v)
{
    auto * aDrvPath = v.attrs()->get(state.s.drvPath);
    if (!aDrvPath)
        throw Error("derivation does not have a 'drvPath' attribute");
    state.forceValue(*aDrvPath->value, aDrvPath->pos);
    auto drvPath = state.store->parseStorePath(aDrvPath->value->string_view());
    drvPath.requireDerivation();
    if (!state.store->isValidPath(drvPath) && !settings.readOnlyMode)
        throw Error(
            "don't know how to recreate store derivation '%s'!", state.store->printStorePath(drvPath));
    return drvPath;
}

static std::string forceStringAttr(EvalState & state, Value & parent, Symbol name)
{
    auto * attr = parent.attrs()->get(name);
    if (!attr)
        throw Error("attribute '%s' missing", state.symbols[name]);
    state.forceValue(*attr->value, attr->pos);
    return std::string(attr->value->string_view());
}

UnresolvedApp InstallableValue::toApp(EvalState & state)
{
    auto [vp, pos] = toValue(state);
    auto & v = *vp;
    state.forceValue(v, pos);

    if (v.type() != nAttrs)
        throw Error("app value is not an attribute set");

    auto type = forceStringAttr(state, v, state.symbols.create("type"));

    auto attrPath = what();

    // Validate type matches the schema position (apps.* → "app", packages.* → "derivation")
    auto resolved = resolvedAttrPath();
    if (!resolved.empty()) {
        std::string expectedType;
        if (resolved.starts_with("apps.") || resolved.starts_with("defaultApp"))
            expectedType = "app";
        else
            expectedType = "derivation";
        if (type != expectedType)
            throw Error("attribute '%s' should have type '%s'", attrPath, expectedType);
    }

    if (type == "app") {
        auto * aProg = v.attrs()->get(state.symbols.create("program"));
        if (!aProg)
            throw Error("app '%s' does not have a 'program' attribute", attrPath);
        state.forceValue(*aProg->value, aProg->pos);
        auto program = std::string(aProg->value->string_view());
        NixStringContext context;
        copyContext(*aProg->value, context);

        std::vector<DerivedPath> context2;
        for (auto & c : context) {
            context2.emplace_back(
                std::visit(
                    overloaded{
                        [&](const NixStringContextElem::DrvDeep & d) -> DerivedPath {
                            /* We want all outputs of the drv */
                            return DerivedPath::Built{
                                .drvPath = makeConstantStorePathRef(d.drvPath),
                                .outputs = OutputsSpec::All{},
                            };
                        },
                        [&](const NixStringContextElem::Built & b) -> DerivedPath {
                            return DerivedPath::Built{
                                .drvPath = b.drvPath,
                                .outputs = OutputsSpec::Names{b.output},
                            };
                        },
                        [&](const NixStringContextElem::Opaque & o) -> DerivedPath {
                            return DerivedPath::Opaque{
                                .path = o.path,
                            };
                        },
                    },
                    c.raw));
        }

        return UnresolvedApp{App{
            .context = std::move(context2),
            .program = program,
        }};
    }

    else if (type == "derivation") {
        auto drvPath = ensureDerivationInStore(state, v);
        auto outPath = forceStringAttr(state, v, state.s.outPath);
        auto outputName = forceStringAttr(state, v, state.s.outputName);
        auto name = forceStringAttr(state, v, state.s.name);
        auto * aPname = v.attrs()->get(state.symbols.create("pname"));
        auto * aMeta = v.attrs()->get(state.s.meta);
        std::string mainProgram;
        if (aMeta) {
            state.forceValue(*aMeta->value, aMeta->pos);
            if (aMeta->value->type() == nAttrs) {
                auto * aMainProgram = aMeta->value->attrs()->get(state.symbols.create("mainProgram"));
                if (aMainProgram) {
                    state.forceValue(*aMainProgram->value, aMainProgram->pos);
                    mainProgram = std::string(aMainProgram->value->string_view());
                }
            }
        }
        if (mainProgram.empty() && aPname) {
            state.forceValue(*aPname->value, aPname->pos);
            mainProgram = std::string(aPname->value->string_view());
        }
        if (mainProgram.empty())
            mainProgram = DrvName(name).name;
        auto program = outPath + "/bin/" + mainProgram;
        return UnresolvedApp{App{
            .context = {DerivedPath::Built{
                .drvPath = makeConstantStorePathRef(drvPath),
                .outputs = OutputsSpec::Names{outputName},
            }},
            .program = program,
        }};
    }

    else
        throw Error("attribute '%s' has unsupported type '%s'", attrPath, type);
}

std::vector<BuiltPathWithResult> UnresolvedApp::build(ref<Store> evalStore, ref<Store> store)
{
    Installables installableContext;

    for (auto & ctxElt : unresolved.context)
        installableContext.push_back(make_ref<InstallableDerivedPath>(store, DerivedPath{ctxElt}));

    return Installable::build(evalStore, store, Realise::Outputs, installableContext);
}

// FIXME: move to libcmd
App UnresolvedApp::resolve(ref<Store> evalStore, ref<Store> store)
{
    auto res = unresolved;

    auto builtContext = build(evalStore, store);
    res.program = resolveString(*store, unresolved.program.string(), builtContext);
    if (!store->isInStore(res.program.string()))
        throw Error("app program '%s' is not in the Nix store", res.program.string());

    return res;
}

} // namespace nix
