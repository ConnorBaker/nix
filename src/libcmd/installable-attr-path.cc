#include "nix/store/globals.hh"
#include "nix/cmd/installable-attr-path.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/util/util.hh"
#include "nix/cmd/command.hh"
#include "nix/expr/attr-path.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/store/derivations.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/store/store-api.hh"
#include "nix/main/shared.hh"
#include "nix/flake/flake.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/util/url.hh"
#include "nix/util/hash.hh"
#include "nix/util/environment-variables.hh"
#include "nix/fetchers/registry.hh"
#include "nix/util/serialise.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
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

/**
 * Compute a stable identity hash for --file or --expr evaluations.
 * Returns std::nullopt if the evaluation is not cacheable (stdin input).
 */
static std::optional<Hash> computeFileEvalIdentity(const SourceExprCommand & cmd, EvalState & state)
{
    std::string identity;
    if (cmd.file) {
        if (cmd.file->string() == "-")
            return std::nullopt; // stdin not cacheable
        identity = "file:" + absPath(cmd.file->string()).string();
    } else if (cmd.expr) {
        identity = "expr:" + hashString(HashAlgorithm::BLAKE3, *cmd.expr)
            .to_string(HashFormat::Base16, false);
    } else {
        return std::nullopt;
    }

    // Include auto-args in identity (different args → different cache)
    auto argsId = cmd.getAutoArgsIdentity();
    if (!argsId)
        return std::nullopt;
    if (!argsId->empty())
        identity += ";args=" + *argsId;  // already a BLAKE3 hex hash

    // Include -I lookup path args
    if (!cmd.lookupPath.elements.empty()) {
        std::string lookupStr;
        for (auto & elem : cmd.lookupPath.elements)
            lookupStr += elem.prefix.s + "=" + elem.path.s + '\0';
        identity += ";lookup=" + hashString(HashAlgorithm::BLAKE3, lookupStr)
            .to_string(HashFormat::Base16, false);
    }

    // Include store dir to differentiate builtins.storeDir across --store flags
    identity += ";store=" + state.store->storeDir;

    // Include system to differentiate builtins.currentSystem across --system flags
    identity += ";system=" + state.settings.getCurrentSystem();

    return hashString(HashAlgorithm::BLAKE3, identity);
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

ref<eval_trace::TraceCache> InstallableAttrPath::getOrCreateTraceCache(EvalState & state)
{
    std::optional<Hash> stableIdentity;
    if (state.settings.useTraceCache)
        stableIdentity = computeFileEvalIdentity(cmd, state);

    auto * autoArgs = cmd.getAutoArgs(state);
    auto vRoot = v; // RootValue (shared_ptr keeps GC root alive)

    // When using the eval trace, the rootLoader must perform fresh
    // evaluation from scratch so the dependency tracker (Adapton DDG)
    // captures oracle deps during trace recording. If we reused the
    // pre-evaluated vRoot, state.forceValue() would be a no-op and
    // the tracker would record zero deps.
    auto & cmdRef = cmd;
    auto rootLoader = [vRoot, autoArgs, &state, &cmdRef, stableIdentity]() -> Value * {
        if (getEnv("NIX_ALLOW_EVAL").value_or("1") == "0") {
            throw Error("not everything is cached, but evaluation is not allowed");
        }

        Value * base;
        if (stableIdentity && (cmdRef.file || cmdRef.expr)) {
            // Fresh evaluation from source so dependency tracker captures oracle deps
            base = state.allocValue();
            if (cmdRef.file) {
                auto dir = absPath(cmdRef.getCommandBaseDir());
                state.evalFile(lookupFileArg(state, cmdRef.file->string(), &dir), *base);
            } else {
                auto dir = absPath(cmdRef.getCommandBaseDir());
                auto e = state.parseExprFromString(*cmdRef.expr, state.rootPath(dir.string()));
                state.eval(e, *base);
            }
        } else {
            base = *vRoot;
        }

        auto result = state.allocValue();
        state.autoCallFunction(*autoArgs, *base, *result);

        // Record git identity as a coarse dep so cross-commit trace reuse
        // works (context_hash is now path-only, not git-aware).
        if (stableIdentity && cmdRef.file && state.traceCtx) {
            try {
                auto resolvedPath = absPath(cmdRef.file->string());
                if (auto repoRoot = findGitRepoRoot(resolvedPath)) {
                    if (auto hash = computeGitIdentityHash(*repoRoot)) {
                        DependencyTracker::record(
                            *state.traceCtx->pools,
                            DepType::GitIdentity, "", repoRoot->string(),
                            DepHashValue(*hash));
                    }
                }
            } catch (...) {
                debug("git identity dep recording failed for '%s'", cmdRef.file->string());
            }
        }

        return result;
    };

    if (stableIdentity) {
        // stableIdentity implies useTraceCache=true -> traceCtx is non-null
        auto search = state.traceCtx->evalCaches.find(*stableIdentity);
        if (search == state.traceCtx->evalCaches.end()) {
            search = state.traceCtx->evalCaches
                .emplace(*stableIdentity,
                    make_ref<eval_trace::TraceCache>(
                        stableIdentity, state, rootLoader))
                .first;
        }
        return search->second;
    }
    return make_ref<eval_trace::TraceCache>(std::nullopt, state, rootLoader);
}

std::pair<Value *, PosIdx> InstallableAttrPath::toValue(EvalState & state)
{
    auto evalCache = getOrCreateTraceCache(state);
    auto * root = evalCache->getRootValue();
    state.forceValue(*root, noPos);

    if (attrPath.empty()) {
        if (state.settings.verifyTraceCache)
            evalCache->verifyCold("", *root);
        return {root, noPos};
    }

    auto emptyArgs = state.buildBindings(0).finish();
    auto found = findAlongAttrPath(state, attrPath, *emptyArgs, *root);

    if (state.settings.verifyTraceCache) {
        state.forceValue(*found.first, noPos);
        evalCache->verifyCold(attrPath, *found.first);
    }

    return found;
}

DerivedPathsWithInfo InstallableAttrPath::toDerivedPaths()
{
    auto [vp, pos] = toValue(*state);
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
            auto * realRoot = evalCache->getOrEvaluateRoot();
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
