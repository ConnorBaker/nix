#include "nix/cmd/installable-attr-path.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/util.hh"
#include "nix/cmd/command.hh"
#include "nix/expr/attr-path.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-environment/environment.hh"
#include "nix/expr/eval-environment/request-types.hh"
#include "nix/util/canon-path.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/flake/flake.hh"
#include "nix/util/url.hh"
#include "nix/util/hash.hh"
#include "nix/util/environment-variables.hh"
#include "nix/fetchers/registry.hh"
#include "nix/util/serialise.hh"
#include "nix/expr/eval-trace/store/session-policy.hh"
#include "nix/store/build-result.hh"

#include <filesystem>
#include <regex>
#include <queue>

#include <nlohmann/json.hpp>

namespace nix {

/**
 * Walk parent directories looking for a .git directory or file (worktrees).
 * Returns the repo root path, or nullopt if not inside a git repo.
 */
static std::optional<std::filesystem::path> findGitRepoRoot(const std::filesystem::path & path)
{
    auto dir = std::filesystem::is_directory(path) ? path : path.parent_path();
    while (true) {
        if (std::filesystem::exists(dir / ".git"))
            return dir;
        auto parent = dir.parent_path();
        if (parent == dir)
            return std::nullopt;
        dir = parent;
    }
}

namespace {

class FileEvalRootLoaderHolder final : public RootLoaderHolder
{
    EvalState & state_;
    SourceExprCommand & cmd_;
    Bindings * autoArgs_;
    RootValue vRoot_;
    bool freshEval_;

public:
    FileEvalRootLoaderHolder(
        EvalState & state,
        SourceExprCommand & cmd,
        Bindings * autoArgs,
        RootValue vRoot,
        bool freshEval)
        : state_(state)
        , cmd_(cmd)
        , autoArgs_(autoArgs)
        , vRoot_(std::move(vRoot))
        , freshEval_(freshEval)
    {
    }

    Value * loadRoot() override
    {
        if (getEnv("NIX_ALLOW_EVAL").value_or("1") == "0")
            throw Error("not everything is cached, but evaluation is not allowed");

        Value * base;
        if (freshEval_ && (cmd_.file || cmd_.expr)) {
            base = state_.allocValue();
            if (cmd_.file) {
                auto dir = absPath(cmd_.getCommandBaseDir());
                state_.evalFile(lookupFileArg(state_, cmd_.file->string(), &dir), *base);
            } else {
                auto dir = absPath(cmd_.getCommandBaseDir());
                auto e = state_.parseExprFromString(*cmd_.expr, state_.rootPath(dir.string()));
                state_.eval(e, *base);
            }
        } else {
            base = *vRoot_;
        }

        auto result = state_.allocValue();
        state_.autoCallFunction(*autoArgs_, *base, *result);

        return result;
    }
};

}

InstallableAttrPath::InstallableAttrPath(
    ref<EvalState> state,
    SourceExprCommand & cmd,
    Value * v,
    const std::string & attrPath,
    ExtendedOutputsSpec extendedOutputsSpec)
    : InstallableValue(state)
    , cmd(cmd)
    , v(allocRootValue(v))
    , attrPath(attrPath)
    , extendedOutputsSpec(std::move(extendedOutputsSpec))
{
}

ref<eval_trace::TraceSession> InstallableAttrPath::getOrCreateTraceCache(EvalState & state) const
{
    if (activeTraceSession_)
        return ref<eval_trace::TraceSession>(activeTraceSession_);

    auto * autoArgs = cmd.getAutoArgs(state);
    auto vRoot = v; // RootValue (shared_ptr keeps GC root alive)
    EvalEnvironment environment(makeSessionEvalEnvironmentAuthority(state));
    auto detached = environment.openDetachedEffectScope();

    // Eagerly resolve git repo root + identity hash so the result is shared
    // between session assembly and root-load dep recording metadata.
    std::optional<CanonPath> repoRoot;
    if (state.settings.useTraceCache && cmd.file) {
        try {
            auto resolvedPath = absPath(cmd.file->string());
            if (auto repoRootPath = findGitRepoRoot(resolvedPath)) {
                repoRoot = CanonPath(repoRootPath->string());
            }
        } catch (...) {
            debug("git identity computation failed for '%s'", cmd.file->string());
        }
    }
    const bool freshEval = state.settings.useTraceCache
        && (cmd.file || cmd.expr)
        && !(cmd.file && cmd.file->string() == "-")
        && cmd.getAutoArgsIdentity().has_value();

    std::optional<GitIdentityObservation> gitObservation;
    if (repoRoot) {
        gitObservation = environment.observeGitIdentity(
            detached,
            GitIdentityRequest::fromRepoRoot(
                state.rootPath(*repoRoot),
                std::nullopt));
    }

    auto sessionGitIdentity = gitObservation
        ? buildFileEvalSessionConfigInputs(*gitObservation)
        : std::optional<FileEvalGitIdentitySnapshot>{};

    std::vector<RootLoadDepObservation> rootLoadDeps;
    if (gitObservation && gitObservation->hash) {
        rootLoadDeps.push_back(RootLoadDepObservation{
            .kind = CanonicalQueryKind::GitRevisionIdentity,
            .source = DepSource::makeAbsolute(),
            .key = SimpleDepKeyAtom{gitObservation->observedRepoRoot.path.abs()},
            .hash = DepHashValue(DepHash{gitObservation->hash->value}),
        });
    }

    auto reuseSource = [&]() -> FileEvalReuseSource {
        if (cmd.file) {
            if (cmd.file->string() == "-")
                return std::monostate{};

            return FileEvalAbsoluteFilePath{
                CanonPath(absPath(cmd.file->string()).string())
            };
        }

        if (cmd.expr) {
            return FileEvalExpressionHash{
                EvalTraceHash::fromHash(hashString(
                    eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm()),
                    *cmd.expr)),
            };
        }

        return std::monostate{};
    }();

    auto reuseKeyInputs = FileEvalTraceSessionReuseKeyInputs{
        .source = std::move(reuseSource),
        .autoArgsIdentity = [&]() -> std::optional<FileEvalAutoArgsHash> {
            auto argsIdentity = cmd.getAutoArgsIdentity();
            if (!argsIdentity)
                return std::nullopt;
            return FileEvalAutoArgsHash{
                EvalTraceHash::fromHash(Hash::parseNonSRIUnprefixed(
                    *argsIdentity,
                    eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm()))),
            };
        }(),
        .storeDir = StoreDirIdentity{CanonPath(state.store->storeDir)},
        .currentSystem = SessionCurrentSystem{state.settings.getCurrentSystem()},
    };

    auto rootLoader = RootLoaderCapability::create(
        std::make_unique<FileEvalRootLoaderHolder>(
            state,
            cmd,
            autoArgs,
            vRoot,
            freshEval));

    auto authorityInputs = CommonTraceSessionAuthorityInputs::create(
        std::move(rootLoader),
        {},
        {},
        std::move(rootLoadDeps),
        {});

    auto sessionInputs = environment.captureSessionOpenInputs(detached, state.getLookupPath());
    auto sessionOpen = assembleFileEvalTraceSessionOpen(
        std::move(sessionInputs),
        std::move(authorityInputs),
        sessionGitIdentity,
        reuseKeyInputs);
    auto session = environment.openEvalSession(std::move(sessionOpen));

    auto traceSession = environment.traceSession(session);
    activeTraceSession_ = traceSession.get_ptr();
    return traceSession;
}

EvaluatedInstallableValue InstallableAttrPath::toValue(EvalState & state)
{
    auto evalCache = getOrCreateTraceCache(state);
    auto * root = evalCache->getRootValue();
    state.forceValue(*root, noPos);

    if (attrPath.empty())
        return EvaluatedInstallableValue::withKeepalive(root, noPos, evalCache);

    auto emptyArgs = state.buildBindings(0).finish();
    auto [value, pos] = findAlongAttrPath(state, attrPath, *emptyArgs, *root);
    /* Force the resolved value so that top-level evaluation errors escape this
       function — mirrors master's InstallableAttrPath::toValue. Without this,
       the returned TracedExpr thunk is first forced by ValuePrinter, whose
       default `errors = Print` catches the exception and renders it inline as
       `«error: ...»` while `nix eval` exits 0. Upstream tests rely on a non-zero
       exit when the thing referenced by the attr path throws. */
    state.forceValue(*value, pos);
    return EvaluatedInstallableValue::withKeepalive(value, pos, evalCache);
}

DerivedPathsWithInfo InstallableAttrPath::toDerivedPaths()
{
    auto evaluated = toValue(*state);
    auto * vp = evaluated.value;
    auto pos = evaluated.pos;
    state->forceValue(*vp, pos);

    // Tier 1: Single derivation (most common)
    if (vp->type() == nAttrs && state->isDerivation(*vp)) {
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
            auto evalCache = getOrCreateTraceCache(*state);
            auto * realRoot = evalCache->getRealRoot();
            state->forceValue(*realRoot, noPos);
            Value * target = realRoot;
            if (!attrPath.empty()) {
                auto emptyArgs = state->buildBindings(0).finish();
                auto [v2, pos2] = findAlongAttrPath(*state, attrPath, *emptyArgs, *realRoot);
                target = v2;
            }
            state->forceValue(*target, noPos);
            if (target->type() == nAttrs) {
                auto * aDP2 = target->attrs()->get(state->s.drvPath);
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
            .path = DerivedPath::Built{
                .drvPath = makeConstantStorePathRef(std::move(drvPath)),
                .outputs = std::visit(overloaded{
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
                                if (auto * aOutputsToInstall = aMeta->value->attrs()->get(state->symbols.create("outputsToInstall"))) {
                                    state->forceValue(*aOutputsToInstall->value, aOutputsToInstall->pos);
                                    if (aOutputsToInstall->value->type() == nList) {
                                        for (auto elem : aOutputsToInstall->value->listView()) {
                                            state->forceValue(*elem, noPos);
                                            outputsToInstall.insert(std::string(elem->string_view()));
                                        }
                                    }
                                }
                            }
                        }

                        // Fall back to all outputs (matches queryOutputs(false, true) behavior)
                        if (outputsToInstall.empty()) {
                            if (auto * aOutputs = vp->attrs()->get(state->symbols.create("outputs"))) {
                                state->forceValue(*aOutputs->value, aOutputs->pos);
                                if (aOutputs->value->type() == nList) {
                                    for (auto elem : aOutputs->value->listView()) {
                                        state->forceValue(*elem, noPos);
                                        outputsToInstall.insert(std::string(elem->string_view()));
                                    }
                                }
                            }
                        }

                        if (outputsToInstall.empty())
                            outputsToInstall.insert("out");

                        return OutputsSpec::Names{std::move(outputsToInstall)};
                    },
                    [&](const ExtendedOutputsSpec::Explicit & e) -> OutputsSpec { return e; },
                }, extendedOutputsSpec.raw),
            },
            .info = make_ref<ExtraPathInfoValue>(ExtraPathInfoValue::Value{
                .priority = priority,
                .attrPath = attrPath,
                .extendedOutputsSpec = extendedOutputsSpec,
            }),
        }};
    }

    // Tier 2: Single path/string
    if (auto derivedPathWithInfo = trySinglePathToDerivedPaths(
            *vp, pos, fmt("while evaluating '%s'", attrPath)))
        return {*derivedPathWithInfo};

    // Tier 3: Collections via getDerivations() (not cached)
    Bindings & autoArgs = *cmd.getAutoArgs(*state);

    PackageInfos packageInfos;
    getDerivations(*state, *vp, "", autoArgs, packageInfos, false);

    // Backward compatibility hack: group results by drvPath. This
    // helps keep .all output together.
    std::map<StorePath, OutputsSpec> byDrvPath;

    for (auto & packageInfo : packageInfos) {
        auto drvPath = packageInfo.queryDrvPath();
        if (!drvPath)
            throw Error("'%s' is not a derivation", what());

        auto newOutputs = std::visit(
            overloaded{
                [&](const ExtendedOutputsSpec::Default & d) -> OutputsSpec {
                    StringSet outputsToInstall;
                    for (auto & output : packageInfo.queryOutputs(false, true))
                        outputsToInstall.insert(output.first);
                    if (outputsToInstall.empty())
                        outputsToInstall.insert("out");
                    return OutputsSpec::Names{std::move(outputsToInstall)};
                },
                [&](const ExtendedOutputsSpec::Explicit & e) -> OutputsSpec { return e; },
            },
            extendedOutputsSpec.raw);

        auto [iter, didInsert] = byDrvPath.emplace(*drvPath, newOutputs);

        if (!didInsert)
            iter->second = iter->second.union_(newOutputs);
    }

    DerivedPathsWithInfo res;
    for (auto & [drvPath, outputs] : byDrvPath)
        res.push_back({
            .path =
                DerivedPath::Built{
                    .drvPath = makeConstantStorePathRef(drvPath),
                    .outputs = outputs,
                },
            .info = make_ref<ExtraPathInfoValue>(ExtraPathInfoValue::Value{
                .extendedOutputsSpec = outputs,
                /* FIXME: reconsider backwards compatibility above
                   so we can fill in this info. */
            }),
        });

    return res;
}

InstallableAttrPath InstallableAttrPath::parse(
    ref<EvalState> state,
    SourceExprCommand & cmd,
    Value * v,
    std::string_view prefix,
    ExtendedOutputsSpec extendedOutputsSpec)
{
    return {
        state,
        cmd,
        v,
        prefix == "." ? "" : std::string{prefix},
        std::move(extendedOutputsSpec),
    };
}

} // namespace nix
