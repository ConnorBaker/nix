#include <nlohmann/json.hpp>
#include <assert.h>
#include <stdint.h>
#include <boost/container/detail/std_fwd.hpp>
#include <boost/core/pointer_traits.hpp>
#include <boost/unordered/detail/foa/table.hpp>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "nix/util/terminal.hh"
#include "nix/util/ref.hh"
#include "nix/util/environment-variables.hh"
#include "nix/flake/flake.hh"
#include "nix/flake/eval-trace-session-open-adapter.hh"
#include "canonical-fetcher-attrs.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-environment/environment.hh"
#include "nix/expr/eval-environment/request-types.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-trace/deps/authority.hh"
#include "nix/expr/eval-trace/store/session-identity.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/store/session-policy.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/json-to-value.hh"
#include "nix/flake/lockfile.hh"
#include "nix/util/url.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/finally.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/flake/settings.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/json-to-value.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/posix-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/fetch-tree.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/expr/value/context.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/registry.hh"
#include "nix/flake/flakeref.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/store/path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/configuration.hh"
#include "nix/util/error.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fmt.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/source-path.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"

namespace nix {
struct SourceAccessor;

namespace flake {

static bool isWithinSameSourceTree(const SourcePath & path, const SourcePath & root)
{
    if (path.path == root.path || path.path.isWithin(root.path))
        return true;

    if (path.accessor == root.accessor)
        return false;

    auto pathPhysical = path.getPhysicalPath();
    auto rootPhysical = root.getPhysicalPath();
    if (pathPhysical && rootPhysical) {
        auto relative = pathPhysical->lexically_relative(*rootPhysical);
        return relative.empty() ? *pathPhysical == *rootPhysical : *relative.begin() != "..";
    }

    return false;
}

static std::optional<CanonPath> tryNormalizeAbsoluteLocalPath(const std::optional<std::filesystem::path> & path)
{
    if (!path)
        return std::nullopt;

    if (!path->is_absolute())
        return std::nullopt;

    return CanonPath(path->lexically_normal().string());
}

static DepSource depSourceForFlakeGraphNode(const FlakeGraphNodeKey & key)
{
    return DepSource::fromNodeKey(GraphNodeDepSourceKey{key.value});
}

static DepSource depSourceForFlakeGraphNode(const ResolvedFlakeGraphNodeKey & key)
{
    return depSourceForFlakeGraphNode(asFlakeGraphNodeKey(key));
}

static ref<LockedNode> cloneLockedNodeDeep(const LockedNode & node);

static Node::Edge cloneLockEdgeDeep(const Node::Edge & edge)
{
    if (auto child = edgeChild(edge))
        return cloneLockedNodeDeep(**child);
    return *edgeFollows(edge);
}

static ref<LockedNode> cloneLockedNodeDeep(const LockedNode & node)
{
    auto cloned = make_ref<LockedNode>(
        node.lockedRef,
        node.originalRef,
        node.isFlake,
        node.parentInputAttrPath);
    for (const auto & [id, edge] : node.inputs)
        cloned->inputs.emplace(id, cloneLockEdgeDeep(edge));
    return cloned;
}

static LockedVersionIdentity computeLockedVersionIdentity(
    Store & store,
    const fetchers::Input & lockedInput)
{
    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::FlakeLockedVersionIdentity>();

    if (lockedInput.isRelative()) {
        builder.field("kind", "relative-path");
        builder.field("value", lockedInput.getStableIdentity().value_or(lockedInput.to_string()));
        return LockedVersionIdentity{builder.finish()};
    }

    if (auto rev = lockedInput.getRev()) {
        builder.field("kind", "git-rev");
        builder.field("value", rev->gitRev());
        return LockedVersionIdentity{builder.finish()};
    }

    if (auto dirtyRev = fetchers::maybeGetStrAttr(lockedInput.attrs, "dirtyRev")) {
        auto dashPos = dirtyRev->rfind('-');
        auto normalized = dashPos != std::string::npos ? dirtyRev->substr(0, dashPos) : *dirtyRev;
        builder.field("kind", "dirty-rev");
        builder.field("value", normalized);
        return LockedVersionIdentity{builder.finish()};
    }

    if (auto stableIdentity = lockedInput.getStableIdentity()) {
        builder.field("kind", "stable-identity");
        builder.field("value", *stableIdentity);
        return LockedVersionIdentity{builder.finish()};
    }

    if (auto fingerprint = lockedInput.getFingerprint(store)) {
        builder.field("kind", "fingerprint");
        builder.field("value", *fingerprint);
        return LockedVersionIdentity{builder.finish()};
    }

    builder.field("kind", "attrs");
    appendFetcherAttrs(builder, lockedInput.attrs);
    return LockedVersionIdentity{builder.finish()};
}

static SourcePath makeMountedStorePath(
    EvalState & state,
    const StorePath & storePath,
    std::string_view subdir = "");

static std::optional<std::pair<StorePath, CanonPath>> tryToStorePath(
    EvalState & state,
    const SourcePath & path);

/// Get the relative path from a locked node, checking originalRef first
/// then falling back to lockedRef. During computeLocks, the originalRef
/// may have been resolved to absolute while the lockedRef preserves
/// the relative path from the lock file.
static std::optional<std::filesystem::path> getRelativePath(const LockedNode & node)
{
    if (auto p = node.originalRef.value.input.isRelative())
        return p;
    return node.lockedRef.value.input.isRelative();
}

static bool isRelativeLockedInput(const LockedNode & node)
{
    return getRelativePath(node).has_value();
}

struct Phase1FlakeRoots
{
    LogicalFlakeRootPath logicalRoot;
    CarrierRootPath carrierRoot;
    EvaluationFlakeRootPath evaluationRoot;
    ParseFlakeRootPath parseRoot;
    bool preserveLiveEvaluationRoot = false;
};

static bool shouldPreserveLiveEvaluationRoot(
    EvalState & state,
    const FlakeRef & ref)
{
    if (ref.input.isRelative())
        return true;

    auto sourcePath = ref.input.getSourcePath();
    if (!sourcePath)
        return false;
    if (state.store->isInStore(sourcePath->string()))
        return false;
    if (ref.input.getRef() || ref.input.getRev() || ref.input.isRelative())
        return false;

    auto localPath = std::filesystem::path(*sourcePath);
    return localPath.is_absolute() && !localPath.empty();
}

static bool shouldUseLivePhase1EvaluationRoot(
    EvalState & state,
    const FlakeRef & ref)
{
    return shouldPreserveLiveEvaluationRoot(state, ref);
}

static std::optional<SourcePath> makeLiveDisplayPhase1Root(
    EvalState & state,
    const FlakeRef & ref)
{
    auto sourcePath = ref.input.getSourcePath();
    if (!sourcePath)
        return std::nullopt;

    auto physicalPath = std::filesystem::path(*sourcePath);
    if (!physicalPath.is_absolute())
        return std::nullopt;

    return state.rootPath(CanonPath(physicalPath.string()));
}

static ParseFlakeRootPath makePhase1ParseRoot(
    const SourcePath & phase1Root,
    std::string_view subdir,
    bool preserveLiveEvaluationRoot)
{
    if (!preserveLiveEvaluationRoot)
        return makeParseFlakeRoot(phase1Root, subdir);

    // Preserve the real phase-1 path. Re-rooting the accessor at '/'
    // collapses relative child resolution and diagnostics to '/flake.nix',
    // '/sub0', etc., which is not the live local tree semantics we want.
    return makeParseFlakeRoot(phase1Root, subdir);
}

static void forceTrivialValue(EvalState & state, Value & value, const PosIdx pos)
{
    if (value.isThunk() && value.isTrivial())
        state.forceValue(value, pos);
}

static void expectType(EvalState & state, ValueType type, Value & value, const PosIdx pos)
{
    forceTrivialValue(state, value, pos);
    if (value.type() != type)
        throw Error("expected %s but got %s at %s", showType(type), showType(value.type()), state.positions[pos]);
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs> parseFlakeInputs(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir,
    const SourcePath & carrierRoot,
    bool allowSelf);

static void parseFlakeInputAttr(EvalState & state, const Attr & attr, fetchers::Attrs & attrs)
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (attr.value->type()) {
    case nString:
        attrs.emplace(state.symbols[attr.name], std::string(attr.value->string_view()));
        break;
    case nBool:
        attrs.emplace(state.symbols[attr.name], Explicit<bool>{attr.value->boolean()});
        break;
    case nInt: {
        auto intValue = attr.value->integer().value;
        if (intValue < 0)
            state
                .error<EvalError>(
                    "negative value given for flake input attribute %1%: %2%", state.symbols[attr.name], intValue)
                .debugThrow();
        attrs.emplace(state.symbols[attr.name], uint64_t(intValue));
        break;
    }
    default:
        if (attr.name == state.symbols.create("publicKeys")) {
            experimentalFeatureSettings.require(Xp::VerifiedFetches);
            NixStringContext emptyContext = {};
            attrs.emplace(
                state.symbols[attr.name], printValueAsJSON(state, true, *attr.value, attr.pos, emptyContext).dump());
        } else
            state
                .error<TypeError>(
                    "flake input attribute '%s' is %s while a string, Boolean, or integer is expected",
                    state.symbols[attr.name],
                    showType(*attr.value))
                .debugThrow();
    }
#pragma GCC diagnostic pop
}

static FlakeInput parseFlakeInput(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir,
    const SourcePath & carrierRoot)
{
    expectType(state, nAttrs, *value, pos);

    FlakeInput input;

    auto sInputs = state.symbols.create("inputs");
    auto sUrl = state.symbols.create("url");
    auto sFlake = state.symbols.create("flake");
    auto sFollows = state.symbols.create("follows");

    fetchers::Attrs attrs;
    std::optional<std::string> url;

    for (auto & attr : *value->attrs()) {
        try {
            if (attr.name == sUrl) {
                forceTrivialValue(state, *attr.value, pos);
                if (attr.value->type() == nString)
                    url = attr.value->string_view();
                else if (attr.value->type() == nPath) {
                    auto path = attr.value->path();
                    if (!isWithinSameSourceTree(path, carrierRoot))
                        throw Error(
                            "input attribute path '%s' at %s must be in the same source tree as %s",
                            path,
                            state.positions[attr.pos],
                            carrierRoot);
                    auto rel = flakeDir.path.makeRelative(path.path);
                    url = rel.empty() ? "path:." : "path:" + rel;
                } else
                    throw Error(
                        "expected a string or a path but got %s at %s",
                        showType(attr.value->type()),
                        state.positions[attr.pos]);
                attrs.emplace("url", *url);
            } else if (attr.name == sFlake) {
                expectType(state, nBool, *attr.value, attr.pos);
                input.isFlake = attr.value->boolean();
            } else if (attr.name == sInputs) {
                input.overrides =
                    parseFlakeInputs(state, attr.value, attr.pos, lockRootAttrPath, flakeDir, carrierRoot, false)
                        .first;
            } else if (attr.name == sFollows) {
                expectType(state, nString, *attr.value, attr.pos);
                auto follows(parseInputAttrPath(attr.value->string_view()));
                follows.insert(follows.begin(), lockRootAttrPath.begin(), lockRootAttrPath.end());
                input.follows = follows;
            } else
                parseFlakeInputAttr(state, attr, attrs);
        } catch (Error & e) {
            e.addTrace(
                state.positions[attr.pos], HintFmt("while evaluating flake attribute '%s'", state.symbols[attr.name]));
            throw;
        }
    }

    if (attrs.count("type"))
        try {
            input.ref = FlakeRef::fromAttrs(state.fetchSettings, attrs);
        } catch (Error & e) {
            e.addTrace(state.positions[pos], HintFmt("while evaluating flake input"));
            throw;
        }
    else {
        attrs.erase("url");
        if (!attrs.empty())
            throw Error("unexpected flake input attribute '%s', at %s", attrs.begin()->first, state.positions[pos]);
        if (url)
            input.ref = parseFlakeRef(state.fetchSettings, *url, {}, true, input.isFlake, true);
    }

    if (input.ref && input.follows)
        throw Error("flake input has both a flake reference and a follows attribute, at %s", state.positions[pos]);

    return input;
}

static std::pair<std::map<FlakeId, FlakeInput>, fetchers::Attrs> parseFlakeInputs(
    EvalState & state,
    Value * value,
    const PosIdx pos,
    const InputAttrPath & lockRootAttrPath,
    const SourcePath & flakeDir,
    const SourcePath & carrierRoot,
    bool allowSelf)
{
    std::map<FlakeId, FlakeInput> inputs;
    fetchers::Attrs selfAttrs;

    expectType(state, nAttrs, *value, pos);

    for (auto & inputAttr : *value->attrs()) {
        auto inputName = state.symbols[inputAttr.name];
        if (inputName == "self") {
            if (!allowSelf)
                throw Error("'self' input attribute not allowed at %s", state.positions[inputAttr.pos]);
            expectType(state, nAttrs, *inputAttr.value, inputAttr.pos);
            for (auto & attr : *inputAttr.value->attrs())
                parseFlakeInputAttr(state, attr, selfAttrs);
        } else {
            inputs.emplace(
                inputName,
                parseFlakeInput(
                    state,
                    inputAttr.value,
                    inputAttr.pos,
                    lockRootAttrPath,
                    flakeDir,
                    carrierRoot));
        }
    }

    return {inputs, selfAttrs};
}

static Flake readFlake(
    EvalState & state,
    OriginalFlakeRef originalRef,
    ResolvedFlakeRef resolvedRef,
    EvaluationLockedFlakeRef lockedRef,
    LogicalFlakeRootPath logicalRoot,
    CarrierRootPath carrierRoot,
    EvaluationFlakeRootPath evaluationRoot,
    ParseFlakeRootPath parseRoot,
    const InputAttrPath & lockRootAttrPath)
{
    // Phase-2 imports must resolve through the authoritative evaluation root.
    // Keep diagnostics anchored to the caller-facing parse root, but do not
    // let relative imports or path coercions fall back to the logical/live
    // root view.
    auto displayFlakePath = parseRootToFlakeNixPath(parseRoot);
    auto physicalFlakePath = evaluationRoot.value / "flake.nix";
    auto lockFileRef = PersistedLockFileFlakeRef{lockedRef.value};

    // NOTE evalFile forces vInfo to be an attrset because mustBeTrivial is true.
    Value vInfo;
    state.evalFile(displayFlakePath, physicalFlakePath, evaluationRoot.value, vInfo, true);

    Flake flake{
        .originalRef = std::move(originalRef),
        .resolvedRef = std::move(resolvedRef),
        .lockedRef = std::move(lockedRef),
        .lockFileRef = std::move(lockFileRef),
        .logicalRoot = std::move(logicalRoot),
        .carrierRoot = std::move(carrierRoot),
        .parseRoot = std::move(parseRoot),
        .evaluationRoot = std::move(evaluationRoot),
    };

    if (auto description = vInfo.attrs()->get(state.s.description)) {
        expectType(state, nString, *description->value, description->pos);
        flake.description = description->value->string_view();
    }

    auto sInputs = state.symbols.create("inputs");

    if (auto inputs = vInfo.attrs()->get(sInputs)) {
        auto [flakeInputs, selfAttrs] =
            parseFlakeInputs(
                state,
                inputs->value,
                inputs->pos,
                lockRootAttrPath,
                flake.logicalRoot.value,
                flake.carrierRoot.value,
                true);
        flake.inputs = std::move(flakeInputs);
        flake.selfAttrs = std::move(selfAttrs);
    }

    auto sOutputs = state.symbols.create("outputs");

    if (auto outputs = vInfo.attrs()->get(sOutputs)) {
        expectType(state, nFunction, *outputs->value, outputs->pos);

        if (outputs->value->isLambda()) {
            if (auto formals = outputs->value->lambda().fun->getFormals()) {
                for (auto & formal : formals->formals) {
                    if (formal.name != state.s.self)
                        flake.inputs.emplace(
                            state.symbols[formal.name],
                            FlakeInput{
                                .ref = parseFlakeRef(state.fetchSettings, std::string(state.symbols[formal.name]))});
                }
            }
        }

    } else
        throw Error("flake '%s' lacks attribute 'outputs'", flake.resolvedRef.value);

    auto sNixConfig = state.symbols.create("nixConfig");

    if (auto nixConfig = vInfo.attrs()->get(sNixConfig)) {
        expectType(state, nAttrs, *nixConfig->value, nixConfig->pos);
        EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));

        for (auto & setting : *nixConfig->value->attrs()) {
            forceTrivialValue(state, *setting.value, setting.pos);
            if (setting.value->type() == nString)
                flake.config.settings.emplace(
                    state.symbols[setting.name], std::string(state.forceStringNoCtx(*setting.value, setting.pos, "")));
            else if (setting.value->type() == nPath) {
                auto request = StorePathPublishRequest{
                    .coercedPath = CoercedPathRequest{
                        .path = setting.value->path(),
                    },
                };
                auto published = environment.publishStorePath(request);
                flake.config.settings.emplace(state.symbols[setting.name], published.renderedPath());
            } else if (setting.value->type() == nInt)
                flake.config.settings.emplace(
                    state.symbols[setting.name], state.forceInt(*setting.value, setting.pos, "").value);
            else if (setting.value->type() == nBool)
                flake.config.settings.emplace(
                    state.symbols[setting.name], Explicit<bool>{state.forceBool(*setting.value, setting.pos, "")});
            else if (setting.value->type() == nList) {
                std::vector<std::string> ss;
                for (auto elem : setting.value->listView()) {
                    if (elem->type() != nString)
                        state
                            .error<TypeError>(
                                "list element in flake configuration setting '%s' is %s while a string is expected",
                                state.symbols[setting.name],
                                showType(*setting.value))
                            .debugThrow();
                    ss.emplace_back(state.forceStringNoCtx(*elem, setting.pos, ""));
                }
                flake.config.settings.emplace(state.symbols[setting.name], ss);
            } else
                state
                    .error<TypeError>(
                        "flake configuration setting '%s' is %s", state.symbols[setting.name], showType(*setting.value))
                    .debugThrow();
        }
    }

    for (auto & attr : *vInfo.attrs()) {
        if (attr.name != state.s.description && attr.name != sInputs && attr.name != sOutputs
            && attr.name != sNixConfig)
            throw Error(
                "flake '%s' has an unsupported attribute '%s', at %s",
                resolvedRef.value,
                state.symbols[attr.name],
                state.positions[attr.pos]);
    }

    return flake;
}

static FlakeRef applySelfAttrs(const FlakeRef & ref, const Flake & flake)
{
    auto newRef(ref);
    bool mutated = false;

    StringSet allowedAttrs{"submodules", "lfs"};

    for (auto & attr : flake.selfAttrs) {
        if (!allowedAttrs.contains(attr.first))
            throw Error("flake 'self' attribute '%s' is not supported", attr.first);
        newRef.input.attrs.insert_or_assign(attr.first, attr.second);
        mutated = true;
    }

    if (mutated)
        newRef.input.cachedFingerprint.reset();

    return newRef;
}

static Flake getFlake(
    EvalState & state,
    const FlakeRef & originalRef,
    const FlakeRef & evaluationRef,
    fetchers::UseRegistries useRegistries,
    const InputAttrPath & lockRootAttrPath)
{
    EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
    auto detached = environment.openDetachedEffectScope();

    struct ResolvedMountedFlakeInput
    {
        ResolvedFlakeRef resolvedRef;
        EvaluationLockedFlakeRef lockedRef;
        SourcePath phase1Root;
        DetachedStandaloneMountedInput mounted;
    };

    auto resolveAndMount =
        [&](const fetchers::Input & input, std::string_view fallbackSubdir, fetchers::UseRegistries registries)
            -> ResolvedMountedFlakeInput
    {
        auto resolved = environment.resolveFetchIdentity(
            detached,
            FetchIdentityRequest::make(input, registries));
        auto subdir = fetchers::maybeGetStrAttr(resolved.extraAttrs, "dir").value_or(std::string(fallbackSubdir));
        auto resolvedRef = ResolvedFlakeRef{FlakeRef(fetchers::Input(resolved.resolvedInput), subdir)};
        auto fetched = environment.materializeFetch(detached, std::move(resolved.identity));
        auto mounted = environment.mountFetchedInput(detached, std::move(fetched));
        auto lockedRef = EvaluationLockedFlakeRef{FlakeRef(fetchers::Input(*mounted.lockedInput.value), subdir)};
        return ResolvedMountedFlakeInput{
            .resolvedRef = std::move(resolvedRef),
            .lockedRef = std::move(lockedRef),
            .phase1Root = std::move(resolved.phase1Root),
            .mounted = std::move(mounted),
        };
    };

    auto resolvedInput = resolveAndMount(evaluationRef.input, evaluationRef.subdir, useRegistries);
    auto resolvedRef = resolvedInput.resolvedRef;
    auto lockedRef = resolvedInput.lockedRef;
    auto lockFileBaseRef = lockedRef;

    auto initialFlakeRoots =
        [&](const ResolvedMountedFlakeInput & mountedInput, const ResolvedFlakeRef & ref) {
        const bool preserveLiveEvaluationRoot =
            lockRootAttrPath.empty()
            && shouldUseLivePhase1EvaluationRoot(state, originalRef);
        std::optional<SourcePath> displayPhase1Root;
        if (preserveLiveEvaluationRoot)
        {
            displayPhase1Root = makeLiveDisplayPhase1Root(state, originalRef);
            if (!displayPhase1Root)
                displayPhase1Root = mountedInput.phase1Root;
        }
        auto physicalPhase1Root =
            makeMountedStorePath(state, mountedInput.mounted.storePath);
        return Phase1FlakeRoots{
            .logicalRoot = makeLogicalFlakeRoot(physicalPhase1Root, ref.value.subdir),
            .carrierRoot = makeCarrierRoot(physicalPhase1Root),
            // Phase-2 flake imports must stay mounted/store-backed so
            // `call-flake.nix` never imports a live absolute path in pure eval.
            .evaluationRoot = makeEvaluationFlakeRoot(physicalPhase1Root, ref.value.subdir),
            .parseRoot = makePhase1ParseRoot(
                displayPhase1Root.value_or(physicalPhase1Root),
                ref.value.subdir,
                displayPhase1Root.has_value()),
            .preserveLiveEvaluationRoot = preserveLiveEvaluationRoot,
        };
    };

    auto initialRoots = initialFlakeRoots(resolvedInput, resolvedRef);

    // Parse/eval flake.nix to get at the input.self attributes.
    auto flake = readFlake(
        state,
        OriginalFlakeRef{originalRef},
        resolvedRef,
        lockedRef,
        initialRoots.logicalRoot,
        initialRoots.carrierRoot,
        initialRoots.evaluationRoot,
        initialRoots.parseRoot,
        lockRootAttrPath);

    // Re-fetch the tree if necessary.
    auto newResolvedRef = ResolvedFlakeRef{applySelfAttrs(resolvedRef.value, flake)};

    if (resolvedRef != newResolvedRef) {
        debug("refetching input '%s' due to self attribute", newResolvedRef.value);
        auto refetchInput = newResolvedRef.value.input;
        refetchInput.attrs.erase("narHash");
        refetchInput.cachedFingerprint.reset();
        resolvedInput = resolveAndMount(refetchInput, newResolvedRef.value.subdir, fetchers::UseRegistries::No);
        resolvedRef = resolvedInput.resolvedRef;
        lockedRef = resolvedInput.lockedRef;
        auto refetchedRoots = initialFlakeRoots(resolvedInput, resolvedRef);
        flake = readFlake(
            state,
            OriginalFlakeRef{originalRef},
            resolvedRef,
            lockedRef,
            refetchedRoots.logicalRoot,
            refetchedRoots.carrierRoot,
            refetchedRoots.evaluationRoot,
            refetchedRoots.parseRoot,
            lockRootAttrPath);
    }

    // Register the locked runtime source without collapsing the caller-visible
    // flake state onto a mounted store path. Phase 1 must retain the original
    // accessor identity so live Git/path semantics and lockfile-relative
    // behavior remain correct. Phase 2 constructs mounted roots separately from
    // the resolved graph.
    environment.ensureMountedStorePath(
        detached,
        EnsureMountedStorePathRequest{
            .storePath = resolvedInput.mounted.storePath,
            .lockedInput = std::make_shared<const fetchers::Input>(lockedRef.value.input),
        });
    auto result = flake;

    auto lockFileRef = PersistedLockFileFlakeRef{applySelfAttrs(lockFileBaseRef.value, result)};
    if (lockFileRef != PersistedLockFileFlakeRef{lockFileBaseRef.value}) {
        auto refetchInput = lockFileRef.value.input;
        refetchInput.attrs.erase("narHash");
        refetchInput.attrs.erase("__final");
        refetchInput.cachedFingerprint.reset();
        auto lockFileInput = resolveAndMount(refetchInput, lockFileRef.value.subdir, fetchers::UseRegistries::No);
        result.lockFileRef = PersistedLockFileFlakeRef{lockFileInput.lockedRef.value};
    } else {
        result.lockFileRef = PersistedLockFileFlakeRef{result.lockedRef.value};
    }

    return result;
}

static SourcePath makeMountedStorePath(
    EvalState & state,
    const StorePath & storePath,
    std::string_view subdir)
{
    auto mountPoint = CanonPath(state.store->printStorePath(storePath));
    auto storePathSource = SourcePath{
        state.storeFS.cast<SourceAccessor>(),
        mountPoint};
    return subdir.empty()
        ? storePathSource
        : SourcePath{
            storePathSource.accessor,
            storePathSource.path / CanonPath(subdir),
        };
}

// auth is accepted to document that this helper runs inside an authorized
// (phase-1) scope. It is not passed further because mountInput does not
// take a proof parameter — the sanctioned-caller contract is enforced by
// the doc comment on mountInput rather than by the type system.
static SourcePath ensureMountedStorePath(
    EvalState & state,
    const StorePath & storePath,
    const fetchers::Input & input,
    const gdp::Proof<eval_trace::AuthorityState> &)
{
    EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
    auto detached = environment.openDetachedEffectScope();
    auto mounted = environment.ensureMountedStorePath(
        detached,
        EnsureMountedStorePathRequest{
            .storePath = storePath,
            .lockedInput = std::make_shared<const fetchers::Input>(input),
        });
    return makeMountedStorePath(state, mounted.storePath, "");
}

// auth is accepted to document that this helper runs inside an authorized
// (phase-1) scope. It is not passed further because mountInput does not
// take a proof parameter — the sanctioned-caller contract is enforced by
// the doc comment on mountInput rather than by the type system.
static std::optional<SourcePath> tryMountLockedInputRoot(
    EvalState & state,
    const fetchers::Input & lockedInput,
    const gdp::Proof<eval_trace::AuthorityState> &)
{
    try {
        EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
        auto detached = environment.openDetachedEffectScope();
        auto resolved = environment.resolveFetchIdentity(
            detached,
            FetchIdentityRequest::make(lockedInput, fetchers::UseRegistries::No));
        auto fetched = environment.materializeFetch(detached, std::move(resolved.identity));
        auto mounted = environment.mountFetchedInput(detached, std::move(fetched));
        return makeMountedStorePath(state, mounted.storePath, "");
    } catch (Error &) {
        return std::nullopt;
    }
}

static std::optional<std::pair<StorePath, CanonPath>> tryToStorePath(EvalState & state, const SourcePath & path)
{
    try {
        return state.store->toStorePath(path.path.abs());
    } catch (Error &) {
        return std::nullopt;
    }
}

// Forward declaration — definition below, exposed via flake.hh for testing.

static ResolvedFlakeGraph buildResolvedFlakeGraph(
    EvalState & state,
    const Flake & flake,
    LockFile & lockFile,
    const std::map<ref<Node>, Phase1FlakeRoots> & fetchedNodePaths,
    const gdp::Proof<eval_trace::AuthorityState> & auth)
{
    auto lockFileData = lockFile.toJSON();
    auto keyMap = std::move(lockFileData.second);
    std::map<const Node *, FlakeGraphNodeKey> keyMapByPtr;
    std::map<FlakeGraphNodeKey, ref<const Node>> nodesByKey;
    for (auto & [node, key] : keyMap)
        keyMapByPtr.emplace(&*node, key);
    for (auto & [node, key] : keyMap)
        nodesByKey.emplace(key, node);

    std::map<const Node *, Phase1FlakeRoots> phase1Roots;
    for (auto & [node, roots] : fetchedNodePaths)
        phase1Roots.emplace(&*node, roots);
    phase1Roots.insert_or_assign(
        &*lockFile.root,
        Phase1FlakeRoots{
            .logicalRoot = flake.logicalRoot,
            .carrierRoot = flake.carrierRoot,
            .evaluationRoot = flake.evaluationRoot,
            .parseRoot = flake.parseRoot,
            .preserveLiveEvaluationRoot = shouldUseLivePhase1EvaluationRoot(
                state,
                flake.originalRef.value),
        });

    auto lookupNodeKey = [&](const ref<const Node> & node) -> FlakeGraphNodeKey {
        auto it = keyMapByPtr.find(&*node);
        if (it == keyMapByPtr.end())
            throw Error("internal error: flake node is missing from resolved graph key map");
        return it->second;
    };

    auto lookupNodeByKey = [&](FlakeGraphNodeKey key) -> ref<const Node> {
        auto it = nodesByKey.find(key);
        if (it == nodesByKey.end())
            throw Error("internal error: resolved graph key '%s' does not map to a lock file node", key.value);
        return it->second;
    };

    std::map<const Node *, ResolvedFlakeGraphNodeKey> sourceInfoKeys;

    auto resolveSourceInfoKey = [&](this auto & resolveSourceInfoKey, const ref<const Node> & node) -> ResolvedFlakeGraphNodeKey {
        auto * nodePtr = &*node;
        if (auto it = sourceInfoKeys.find(nodePtr); it != sourceInfoKeys.end())
            return it->second;

        auto sourceInfoKey = [&]() -> ResolvedFlakeGraphNodeKey {
            auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();
            if (!lockedNode)
                return makeResolvedFlakeGraphNodeKey(lookupNodeKey(node));

            if (isRelativeLockedInput(*lockedNode)) {
                if (!lockedNode->parentInputAttrPath)
                    throw Error("internal error: relative flake input '%s' has no parent path", lookupNodeKey(node).value);

                auto parentNode = lockFile.findInput(*lockedNode->parentInputAttrPath);
                if (!parentNode)
                    throw Error(
                        "internal error: relative flake input '%s' has dangling parent '%s'",
                        lookupNodeKey(node).value,
                        printInputAttrPath(*lockedNode->parentInputAttrPath));

                return resolveSourceInfoKey(ref<const Node>(parentNode));
            }

            return makeResolvedFlakeGraphNodeKey(lookupNodeKey(node));
        }();

        return sourceInfoKeys.emplace(nodePtr, std::move(sourceInfoKey)).first->second;
    };

    std::map<const Node *, LockedVersionIdentity> lockedVersionIdentities;

    auto resolveLockedVersionIdentity =
        [&](this auto & resolveLockedVersionIdentity, const ref<const Node> & node) -> LockedVersionIdentity {
        auto * nodePtr = &*node;
        if (auto it = lockedVersionIdentities.find(nodePtr); it != lockedVersionIdentities.end())
            return it->second;

        auto lockedVersionIdentity = [&]() -> LockedVersionIdentity {
            auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();
            if (!lockedNode)
                return computeLockedVersionIdentity(*state.store, flake.lockedRef.value.input);

            if (!isRelativeLockedInput(*lockedNode))
                return computeLockedVersionIdentity(*state.store, lockedNode->lockedRef.value.input);

            if (!lockedNode->parentInputAttrPath)
                throw Error(
                    "internal error: relative flake input '%s' has no parent path",
                    lookupNodeKey(node).value);

            auto parentNode = lockFile.findInput(*lockedNode->parentInputAttrPath);
            if (!parentNode)
                throw Error(
                    "internal error: relative flake input '%s' has dangling parent '%s'",
                    lookupNodeKey(node).value,
                    printInputAttrPath(*lockedNode->parentInputAttrPath));

            auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::FlakeRelativeLockedVersionIdentity>();
            builder.field(
                "parent-locked-version-identity",
                resolveLockedVersionIdentity(ref<const Node>(parentNode)));
            if (auto relativePath = lockedNode->originalRef.value.input.isRelative())
                builder.field("relative-path", relativePath->string());
            else if (auto relativePath = lockedNode->lockedRef.value.input.isRelative())
                builder.field("relative-path", relativePath->string());
            else
                throw Error(
                    "internal error: relative flake input '%s' lost its relative path",
                    lookupNodeKey(node).value);
            builder.field("subdir", lockedNode->lockedRef.value.subdir);
            return LockedVersionIdentity{builder.finish()};
        }();

        return lockedVersionIdentities.emplace(nodePtr, std::move(lockedVersionIdentity)).first->second;
    };

    struct CarrierResolution
    {
        CarrierRootPath sourceCarrierRoot;
        CarrierRootPath mountedCarrierRoot;
    };

    std::map<const Node *, CarrierResolution> resolvedCarrierRoots;
    std::map<CarrierRootPath, CarrierRootPath> mountedCarrierRoots;

    auto mountCarrierRoot = [&](const fetchers::Input & input, const CarrierRootPath & sourceCarrierRoot) -> CarrierRootPath {
        if (auto it = mountedCarrierRoots.find(sourceCarrierRoot); it != mountedCarrierRoots.end())
            return it->second;

        EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
        auto detached = environment.openDetachedEffectScope();
        auto resolved = environment.resolveFetchIdentity(
            detached,
            FetchIdentityRequest::make(input, fetchers::UseRegistries::No));
        auto fetched = environment.materializeFetch(detached, std::move(resolved.identity));
        auto mounted = environment.mountFetchedInput(detached, std::move(fetched));
        auto mountedCarrierRoot = CarrierRootPath{
            makeMountedStorePath(state, mounted.storePath, "")};
        return mountedCarrierRoots.emplace(sourceCarrierRoot, mountedCarrierRoot).first->second;
    };

    auto resolveCarrierRoot = [&](this auto & resolveCarrierRoot, const ref<const Node> & node) -> CarrierResolution {
        auto * nodePtr = &*node;
        if (auto it = resolvedCarrierRoots.find(nodePtr); it != resolvedCarrierRoots.end())
            return it->second;

        auto ownKey = makeResolvedFlakeGraphNodeKey(lookupNodeKey(node));
        auto sourceInfoKey = resolveSourceInfoKey(node);
        if (sourceInfoKey != ownKey) {
            auto carrier = resolveCarrierRoot(lookupNodeByKey(asFlakeGraphNodeKey(sourceInfoKey)));
            resolvedCarrierRoots.emplace(nodePtr, carrier);
            return carrier;
        }

        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();

        CarrierResolution carrier = [&]() -> CarrierResolution {
            if (auto it = phase1Roots.find(nodePtr); it != phase1Roots.end()) {
                auto phase1 = it->second;

                if (lockedNode && isRelativeLockedInput(*lockedNode)) {
                    if (!lockedNode->parentInputAttrPath)
                        throw Error(
                            "internal error: relative flake input '%s' has no parent path",
                            asFlakeGraphNodeKey(ownKey).value);
                    auto parentNode = lockFile.findInput(*lockedNode->parentInputAttrPath);
                    if (!parentNode)
                        throw Error(
                            "internal error: relative flake input '%s' has dangling parent '%s'",
                            asFlakeGraphNodeKey(ownKey).value,
                            printInputAttrPath(*lockedNode->parentInputAttrPath));
                    auto parentCarrier = resolveCarrierRoot(ref<const Node>(parentNode));
                    return CarrierResolution{
                        .sourceCarrierRoot = phase1.carrierRoot,
                        .mountedCarrierRoot = parentCarrier.mountedCarrierRoot,
                    };
                }

                if (tryToStorePath(state, phase1.carrierRoot.value)) {
                    // Phase 1 already mounted this carrier root against the
                    // authoritative fetched accessor. Re-deriving it from the
                    // locked input can collapse back onto raw local/path
                    // semantics for locked path inputs and custom stores.
                    return CarrierResolution{
                        .sourceCarrierRoot = phase1.carrierRoot,
                        .mountedCarrierRoot = phase1.carrierRoot,
                    };
                }

                auto carrierRoot = mountCarrierRoot(
                    lockedNode ? lockedNode->lockedRef.value.input : flake.lockedRef.value.input,
                    phase1.carrierRoot);

                return CarrierResolution{
                    .sourceCarrierRoot = phase1.carrierRoot,
                    .mountedCarrierRoot = carrierRoot,
                };
            }

            if (!lockedNode)
                throw Error("internal error: cannot resolve carrier root for root node without a fetched path");

            if (isRelativeLockedInput(*lockedNode))
                throw Error(
                    "internal error: relative flake input '%s' has no source-info carrier root",
                    asFlakeGraphNodeKey(ownKey).value);

            auto storePath = lockedNode->computeStorePath(*state.store);
            auto mountedRoot = [&]() -> SourcePath {
                if (state.store->isValidPath(storePath))
                    return ensureMountedStorePath(state, storePath, lockedNode->lockedRef.value.input, auth);
                if (auto reopened = tryMountLockedInputRoot(
                    state,
                    lockedNode->lockedRef.value.input,
                    auth))
                    return *reopened;
                // Lazy fallback: create a SourcePath over a potentially non-existent
                // store path.  This keeps the resolution cascade uniform for all node
                // types.  For non-flake inputs, the path may never be accessed during
                // phase 2 (Nix is lazy), so no error occurs.  For flake inputs,
                // call-flake.nix always does `import (appendPath flakePath "/flake.nix")`,
                // producing a clear filesystem error at point of use.
                //
                // Structural totality (every key/parent/sourceInfo/resolvedInput resolves)
                // is enforced by validateResolvedFlakeGraph.  Existential totality (store
                // path exists on disk) is intentionally deferred to point-of-use to avoid
                // failing on inputs the user's outputs function never accesses.
                //
                return makeMountedStorePath(
                    state,
                    storePath,
                    "");
            }();
            return CarrierResolution{
                .sourceCarrierRoot = CarrierRootPath{mountedRoot},
                .mountedCarrierRoot = CarrierRootPath{mountedRoot},
            };
        }();

        resolvedCarrierRoots.emplace(nodePtr, carrier);
        return carrier;
    };

    std::map<const Node *, EvaluationFlakeRootPath> resolvedFlakePaths;
    std::map<const Node *, DisplayFlakeRootPath> resolvedDisplayPaths;

    auto resolveFlakePath = [&](this auto & resolveFlakePath, const ref<const Node> & node) -> EvaluationFlakeRootPath {
        auto * nodePtr = &*node;
        if (auto it = resolvedFlakePaths.find(nodePtr); it != resolvedFlakePaths.end())
            return it->second;

        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();

        if (auto it = phase1Roots.find(nodePtr); it != phase1Roots.end()) {
            // For relative inputs, prefer the isRelative() resolution below
            // over the phase1 roots. The phase1 evaluationRoot may have been
            // computed from carrier + subdir (which produces wrong paths for
            // cross-flake "../" references). The isRelative() path resolves
            // correctly via parent node traversal.
            if (!lockedNode || !isRelativeLockedInput(*lockedNode)) {
                auto phase1 = it->second;
                if (!isWithinSameSourceTree(phase1.logicalRoot.value, phase1.carrierRoot.value))
                    throw Error(
                        "internal error: resolved logical flake root '%s' escapes carrier root '%s' for node '%s'",
                        phase1.logicalRoot.value,
                        phase1.carrierRoot.value,
                        lookupNodeKey(node).value);
                resolvedFlakePaths.emplace(nodePtr, phase1.evaluationRoot);
                return phase1.evaluationRoot;
            }
        }

        if (!lockedNode)
            throw Error("internal error: cannot resolve flake path for root node without a fetched path");

        EvaluationFlakeRootPath flakePath = [&]() {
            if (auto relativePath = getRelativePath(*lockedNode)) {
                if (!lockedNode->parentInputAttrPath)
                    throw Error("internal error: relative flake input '%s' has no parent path", lookupNodeKey(node).value);

                auto parentNode = lockFile.findInput(*lockedNode->parentInputAttrPath);
                if (!parentNode)
                    throw Error(
                        "internal error: relative flake input '%s' has dangling parent '%s'",
                        lookupNodeKey(node).value,
                        printInputAttrPath(*lockedNode->parentInputAttrPath));

                auto parentFlakePath = resolveFlakePath(ref<const Node>(parentNode));
                return appendEvaluationFlakeRelativePath(parentFlakePath, relativePath->string());
            }

            auto carrier = resolveCarrierRoot(node);
            auto flakePath = EvaluationFlakeRootPath{
                lockedNode->lockedRef.value.subdir.empty()
                    ? carrier.mountedCarrierRoot.value
                    : carrier.mountedCarrierRoot.value / CanonPath(lockedNode->lockedRef.value.subdir)};
            return flakePath;
        }();

        resolvedFlakePaths.emplace(nodePtr, flakePath);
        return flakePath;
    };

    auto resolveDisplayPath = [&](this auto & resolveDisplayPath, const ref<const Node> & node) -> DisplayFlakeRootPath {
        auto * nodePtr = &*node;
        if (auto it = resolvedDisplayPaths.find(nodePtr); it != resolvedDisplayPaths.end())
            return it->second;

        if (auto it = phase1Roots.find(nodePtr); it != phase1Roots.end()) {
            auto phase1 = it->second;
            auto displayPath = phase1.preserveLiveEvaluationRoot
                ? makeDisplayFlakeRoot(phase1.parseRoot.value)
                : makeDisplayFlakeRoot(resolveFlakePath(node).value);
            resolvedDisplayPaths.emplace(nodePtr, displayPath);
            return displayPath;
        }

        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();
        if (!lockedNode)
            throw Error("internal error: cannot resolve display path for root node without a fetched path");

        auto displayPath = [&]() -> DisplayFlakeRootPath {
            if (auto relativePath = getRelativePath(*lockedNode)) {
                if (!lockedNode->parentInputAttrPath)
                    throw Error("internal error: relative flake input '%s' has no parent path", lookupNodeKey(node).value);

                auto parentNode = lockFile.findInput(*lockedNode->parentInputAttrPath);
                if (!parentNode)
                    throw Error(
                        "internal error: relative flake input '%s' has dangling parent '%s'",
                        lookupNodeKey(node).value,
                        printInputAttrPath(*lockedNode->parentInputAttrPath));

                auto parentDisplayPath = resolveDisplayPath(ref<const Node>(parentNode));
                return makeDisplayFlakeRoot(
                    SourcePath{
                        parentDisplayPath.value.accessor,
                        CanonPath(relativePath->string(), parentDisplayPath.value.path)},
                    lockedNode->lockedRef.value.subdir);
            }

            return makeDisplayFlakeRoot(resolveFlakePath(node).value);
        }();

        resolvedDisplayPaths.emplace(nodePtr, displayPath);
        return displayPath;
    };

    std::map<const Node *, LogicalFlakeRootPath> resolvedLogicalRoots;

    auto resolveLogicalRoot = [&](this auto & resolveLogicalRoot, const ref<const Node> & node) -> LogicalFlakeRootPath {
        auto * nodePtr = &*node;
        if (auto it = resolvedLogicalRoots.find(nodePtr); it != resolvedLogicalRoots.end())
            return it->second;

        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();

        if (auto it = phase1Roots.find(nodePtr); it != phase1Roots.end()) {
            // Same guard as resolveFlakePath: relative inputs must resolve
            // via parent traversal, not from phase1 roots.
            if (!lockedNode || !isRelativeLockedInput(*lockedNode)) {
                resolvedLogicalRoots.emplace(nodePtr, it->second.logicalRoot);
                return it->second.logicalRoot;
            }
        }

        if (!lockedNode)
            throw Error("internal error: cannot resolve logical root for root node without a fetched path");

        auto logicalRoot = [&]() -> LogicalFlakeRootPath {
            if (auto relativePath = getRelativePath(*lockedNode)) {
                if (!lockedNode->parentInputAttrPath)
                    throw Error("internal error: relative flake input '%s' has no parent path", lookupNodeKey(node).value);

                auto parentNode = lockFile.findInput(*lockedNode->parentInputAttrPath);
                if (!parentNode)
                    throw Error(
                        "internal error: relative flake input '%s' has dangling parent '%s'",
                        lookupNodeKey(node).value,
                        printInputAttrPath(*lockedNode->parentInputAttrPath));

                auto parentLogicalRoot = resolveLogicalRoot(ref<const Node>(parentNode));
                return makeLogicalFlakeRoot(
                    SourcePath{
                        parentLogicalRoot.value.accessor,
                        CanonPath(relativePath->string(), parentLogicalRoot.value.path)},
                    lockedNode->lockedRef.value.subdir);
            }

            auto carrier = resolveCarrierRoot(node);
            return makeLogicalFlakeRoot(carrier.sourceCarrierRoot.value, lockedNode->lockedRef.value.subdir);
        }();

        resolvedLogicalRoots.emplace(nodePtr, logicalRoot);
        return logicalRoot;
    };

    ResolvedFlakeGraph graph{
        .rootKey = makeResolvedFlakeGraphRootKey(lookupNodeKey(ref<const Node>(lockFile.root))),
    };

    for (auto & [node, key] : keyMap) {
        auto carrier = resolveCarrierRoot(node);
        auto flakePath = resolveFlakePath(node);
        auto displayPath = resolveDisplayPath(node);
        auto logicalRoot = resolveLogicalRoot(node);
        auto carrierPath = carrier.mountedCarrierRoot;
        auto carrierStorePath = [&]() -> StorePath {
            auto storePath = tryToStorePath(state, carrier.mountedCarrierRoot.value);
            if (!storePath)
                throw Error(
                    "internal error: resolved flake node '%s' carrier root '%s' is not store-backed",
                    key.value,
                    carrier.mountedCarrierRoot.value);
            if (!storePath->second.isRoot())
                throw Error(
                    "internal error: resolved flake node '%s' carrier root '%s' is not mounted at a store root",
                    key.value,
                    carrier.mountedCarrierRoot.value);
            return storePath->first;
        }();
        auto relativePath = makeLogicalFlakeRelativePath(
            carrier.sourceCarrierRoot,
            EvaluationFlakeRootPath{logicalRoot.value});

        auto lockedNode = node.dynamic_pointer_cast<const LockedNode>();

        std::optional<ResolvedFlakeGraphNodeKey> parentKey;
        bool isFlake = true;
        FlakeInputSubdir subdir{""};
        fetchers::Input lockedInput;
        LockedVersionIdentity lockedVersionIdentity;

        if (lockedNode) {
            isFlake = lockedNode->isFlake;
            subdir = FlakeInputSubdir{lockedNode->lockedRef.value.subdir};
            lockedInput = lockedNode->lockedRef.value.input;

            if (lockedNode->parentInputAttrPath) {
                auto parentNode = lockFile.findInput(*lockedNode->parentInputAttrPath);
                if (!parentNode)
                    throw Error(
                        "internal error: flake input '%s' has dangling parent '%s'",
                        key.value,
                        printInputAttrPath(*lockedNode->parentInputAttrPath));
                parentKey = makeResolvedFlakeGraphNodeKey(lookupNodeKey(ref<const Node>(parentNode)));
            }
        } else {
            isFlake = true;
            subdir = FlakeInputSubdir{flake.lockedRef.value.subdir};
            lockedInput = flake.lockedRef.value.input;
        }

        lockedVersionIdentity = resolveLockedVersionIdentity(node);

        auto & resolvedNode = graph.nodes.emplace(
            makeResolvedFlakeGraphNodeKey(key),
            ResolvedFlakeNode{
                .parentKey = std::move(parentKey),
                .sourceInfoKey = resolveSourceInfoKey(node),
                .isFlake = isFlake,
                .carrierRoot = carrierPath,
                .evaluationRoot = flakePath,
                .displayRoot = displayPath,
                .carrierStorePath = std::move(carrierStorePath),
                .relativePath = std::move(relativePath),
                .subdir = std::move(subdir),
                .lockedInput = std::move(lockedInput),
                .lockedVersionIdentity = std::move(lockedVersionIdentity),
            }).first->second;

        for (auto & [inputName, inputSpec] : node->inputs) {
            if (auto child = edgeChild(inputSpec)) {
                auto childKey = makeResolvedFlakeGraphNodeKey(lookupNodeKey(*child));
                resolvedNode.inputSpecs.emplace(inputName, ResolvedFlakeInputSpec{.target = childKey});
                resolvedNode.resolvedInputs.emplace(inputName, childKey);
            } else if (auto follows = edgeFollows(inputSpec)) {
                auto resolvedInput = lockFile.findInput(*follows);
                if (!resolvedInput)
                    throw Error(
                        "internal error: flake input '%s/%s' follows a missing target '%s'",
                        key.value,
                        inputName,
                        printInputAttrPath(*follows));
                resolvedNode.inputSpecs.emplace(inputName, ResolvedFlakeInputSpec{.target = *follows});
                resolvedNode.resolvedInputs.emplace(
                    inputName,
                    makeResolvedFlakeGraphNodeKey(lookupNodeKey(ref<const Node>(resolvedInput))));
            }
        }
    }

    validateResolvedFlakeGraph(graph);

    return graph;
}

Flake getFlake(EvalState & state, const FlakeRef & originalRef, fetchers::UseRegistries useRegistries)
{
    return getFlake(state, originalRef, originalRef, useRegistries, {});
}

static LockFile readLockFile(const fetchers::Settings & fetchSettings, const SourcePath & lockFilePath)
{
    return lockFilePath.pathExists() ? LockFile(fetchSettings, lockFilePath.readFile(), fmt("%s", lockFilePath))
                                     : LockFile();
}

LockedFlake lockFlake(
    const Settings & settings, EvalState & state, const FlakeRef & topRef, const LockFlags & lockFlags, Flake flake)
{
    // Phase 1 authority: all fetching, registry lookups, store mutations,
    // and graph construction happen under this proof scope.  callFlake
    // (phase 2) and verification code structurally cannot obtain this
    // proof — they have no Certifier<AuthorityState> in scope.
    return eval_trace::AuthorityGate::withAuthority(
        [&](const gdp::Proof<eval_trace::AuthorityState> & auth) -> LockedFlake {
    experimentalFeatureSettings.require(Xp::Flakes);

    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesTop = useRegistries ? fetchers::UseRegistries::All : fetchers::UseRegistries::No;
    auto useRegistriesInputs = useRegistries ? fetchers::UseRegistries::Limited : fetchers::UseRegistries::No;

    if (lockFlags.applyNixConfig) {
        flake.config.apply(settings);
        state.store->setOptions();
    }

    try {
        if (!state.fetchSettings.allowDirty && lockFlags.referenceLockFilePath) {
            throw Error("reference lock file was provided, but the `allow-dirty` setting is set to false");
        }

        auto oldLockFile =
            readLockFile(state.fetchSettings, lockFlags.referenceLockFilePath.value_or(flake.lockFilePath()));

        debug("old lock file: %s", oldLockFile);

        struct OverrideTarget
        {
            FlakeInput input;
            LogicalFlakeRootPath logicalRoot;
            CarrierRootPath carrierRoot;
            ParseFlakeRootPath parseRoot;
            std::optional<InputAttrPath> parentInputAttrPath; // FIXME: rename to inputAttrPathPrefix?
        };

        std::map<NonEmptyInputAttrPath, OverrideTarget> overrides;
        std::set<NonEmptyInputAttrPath> explicitCliOverrides;
        std::set<NonEmptyInputAttrPath> overridesUsed;
        std::set<InputAttrPath> updatesUsed;
        std::map<ref<Node>, Phase1FlakeRoots> nodePaths;

        for (auto & i : lockFlags.inputOverrides) {
            overrides.emplace(
                i.first,
                    OverrideTarget{
                        .input = FlakeInput{.ref = i.second},
                        /* Note: any relative overrides
                           (e.g. `--override-input B/C "path:./foo/bar"`)
                           are interpreted relative to the top-level
                           flake. */
                        .logicalRoot = flake.logicalRoot,
                        .carrierRoot = flake.carrierRoot,
                        .parseRoot = flake.parseRoot,
                        .parentInputAttrPath = i.first.parent(),
                    });
            explicitCliOverrides.insert(i.first);
        }

        LockFile newLockFile;

        std::vector<FlakeRef> parents;

            std::function<void(
            const FlakeInputs & flakeInputs,
            ref<Node> node,
            const InputAttrPath & inputAttrPathPrefix,
            std::shared_ptr<const Node> oldNode,
            const InputAttrPath & followsPrefix,
            const LogicalFlakeRootPath & logicalRoot,
            const CarrierRootPath & carrierRoot,
            const ParseFlakeRootPath & parseRoot,
            bool trustLock)>
            computeLocks;

        computeLocks = [&](
                           /* The inputs of this node, either from flake.nix or
                              flake.lock. */
                           const FlakeInputs & flakeInputs,
                           /* The node whose locks are to be updated.*/
                           ref<Node> node,
                           /* The path to this node in the lock file graph. */
                           const InputAttrPath & inputAttrPathPrefix,
                           /* The old node, if any, from which locks can be
                              copied. */
                           std::shared_ptr<const Node> oldNode,
                           /* The prefix relative to which 'follows' should be
                              interpreted. When a node is initially locked, it's
                              relative to the node's flake; when it's already locked,
                              it's relative to the root of the lock file. */
                           const InputAttrPath & followsPrefix,
                           /* The source path of this node's flake. */
                           const LogicalFlakeRootPath & logicalRoot,
                           /* Carrier root that bounds relative path escapes. */
                           const CarrierRootPath & carrierRoot,
                           /* Exact live phase-1 import root for relative-path resolution. */
                           const ParseFlakeRootPath & parseRoot,
                           bool trustLock) {
            debug("computing lock file node '%s'", printInputAttrPath(inputAttrPathPrefix));

            /* Get the overrides (i.e. attributes of the form
               'inputs.nixops.inputs.nixpkgs.url = ...'). */
            auto addOverrides =
                [&](this const auto & addOverrides, const FlakeInput & input, const InputAttrPath & prefix) -> void {
                for (auto & [idOverride, inputOverride] : input.overrides) {
                    auto inputAttrPath = NonEmptyInputAttrPath::append(prefix, idOverride);
                    if (inputOverride.ref || inputOverride.follows)
                        overrides.emplace(
                            inputAttrPath,
                            OverrideTarget{
                                .input = inputOverride,
                                .logicalRoot = logicalRoot,
                                .carrierRoot = carrierRoot,
                                .parseRoot = parseRoot,
                                .parentInputAttrPath = inputAttrPathPrefix});
                    addOverrides(inputOverride, inputAttrPath);
                }
            };

            for (auto & [id, input] : flakeInputs) {
                auto inputAttrPath(inputAttrPathPrefix);
                inputAttrPath.push_back(id);
                addOverrides(input, inputAttrPath);
            }

            /* Check whether this input has overrides for a
               non-existent input. */
            for (auto [inputAttrPath, inputOverride] : overrides) {
                auto follow = inputAttrPath.inputName();
                auto inputAttrPath2 = inputAttrPath.parent();
                if (inputAttrPath2 == inputAttrPathPrefix && !flakeInputs.count(follow))
                    warn(
                        "input '%s' has an override for a non-existent input '%s'",
                        printInputAttrPath(inputAttrPathPrefix),
                        follow);
            }

            /* Go over the flake inputs, resolve/fetch them if
               necessary (i.e. if they're new or the flakeref changed
               from what's in the lock file). */
            for (auto & [id, input2] : flakeInputs) {
                auto nonEmptyInputAttrPath = NonEmptyInputAttrPath::append(inputAttrPathPrefix, id);
                auto inputAttrPath = nonEmptyInputAttrPath.get();
                auto inputAttrPathS = printInputAttrPath(inputAttrPath);
                debug("computing input '%s'", inputAttrPathS);

                try {

                    /* Do we have an override for this input from one of the
                       ancestors? */
                    auto i = overrides.find(nonEmptyInputAttrPath);
                    bool hasOverride = i != overrides.end();
                    bool hasCliOverride = explicitCliOverrides.contains(nonEmptyInputAttrPath);
                    if (hasOverride)
                        overridesUsed.insert(nonEmptyInputAttrPath);
                    auto input = hasOverride ? i->second.input : input2;

                    /* Resolve relative 'path:' inputs relative to
                       the source path of the overrider. */
                    auto overriddenLogicalRoot = hasOverride ? i->second.logicalRoot : logicalRoot;
                    auto overriddenCarrierRoot = hasOverride ? i->second.carrierRoot : carrierRoot;
                    auto overriddenParseRoot = hasOverride ? i->second.parseRoot : parseRoot;

                    /* Respect the "flakeness" of the input even if we
                       override it. */
                    if (hasOverride)
                        input.isFlake = input2.isFlake;

                    /* Resolve 'follows' later (since it may refer to an input
                       path we haven't processed yet. */
                    if (input.follows) {
                        InputAttrPath target;

                        target.insert(target.end(), input.follows->begin(), input.follows->end());

                        debug("input '%s' follows '%s'", inputAttrPathS, printInputAttrPath(target));
                        node->inputs.insert_or_assign(id, target);
                        continue;
                    }

                    if (!input.ref)
                        input.ref =
                            FlakeRef::fromAttrs(state.fetchSettings, {{"type", "indirect"}, {"id", std::string(id)}});

                    auto overriddenParentPath =
                        (input.ref->input.isRelative()
                            || (input2.ref && input2.ref->input.isRelative()))
                            ? std::optional<InputAttrPath>(
                                  hasOverride
                                      ? i->second.parentInputAttrPath.value_or(inputAttrPathPrefix)
                                      : inputAttrPathPrefix)
                            : std::nullopt;

                    struct ResolvedRelativeInputRoots
                    {
                        LogicalFlakeRootPath logicalRoot;
                        ParseFlakeRootPath parseRoot;
                    };

                    auto resolveRelativeInputRoots =
                        [&](const FlakeRef & relativeRef) -> std::optional<ResolvedRelativeInputRoots> {
                        if (auto relativePath = relativeRef.input.isRelative()) {
                            auto resolvedLogicalPath = CanonPath(relativePath->string(), overriddenLogicalRoot.value.path);
                            auto escapesCarrierRoot =
                                !(resolvedLogicalPath == overriddenCarrierRoot.value.path
                                    || resolvedLogicalPath.isWithin(overriddenCarrierRoot.value.path));
                            if (escapesCarrierRoot) {
                                if (state.settings.pureEval)
                                    throw Error(
                                        "path '%s' is forbidden in pure evaluation mode because it escapes from '%s'",
                                        resolvedLogicalPath,
                                        overriddenCarrierRoot.value.path);
                                /* Preserve the longstanding impure failure shape for
                                   escapeing relative path inputs. Historically this
                                   fell through store-path parsing after copying the
                                   parent flake into the store, which surfaced as a
                                   BadStorePath on the escaped basename. */
                                StorePath{std::string(resolvedLogicalPath.baseName().value_or("source"))};
                                unreachable();
                            }
                            auto resolvedParsePath = CanonPath(relativePath->string(), overriddenParseRoot.value.path);
                            return ResolvedRelativeInputRoots{
                                .logicalRoot = LogicalFlakeRootPath{
                                    SourcePath{
                                        overriddenLogicalRoot.value.accessor,
                                        std::move(resolvedLogicalPath)}},
                                .parseRoot = ParseFlakeRootPath{
                                    SourcePath{
                                        overriddenParseRoot.value.accessor,
                                        std::move(resolvedParsePath)}},
                            };
                        }

                        if (auto sourcePath = relativeRef.input.getSourcePath()) {
                            auto absolutePath = std::filesystem::path(*sourcePath);
                            if (absolutePath.is_absolute()
                                && !state.store->isInStore(sourcePath->string())) {
                                auto resolvedLogicalPath = CanonPath(absolutePath.string());
                                if (resolvedLogicalPath == overriddenCarrierRoot.value.path
                                    || resolvedLogicalPath.isWithin(overriddenCarrierRoot.value.path)) {
                                    auto resolvedParsePath = CanonPath(absolutePath.string());
                                    return ResolvedRelativeInputRoots{
                                        .logicalRoot = LogicalFlakeRootPath{
                                            SourcePath{
                                                overriddenLogicalRoot.value.accessor,
                                                std::move(resolvedLogicalPath)}},
                                        .parseRoot = ParseFlakeRootPath{
                                            SourcePath{
                                                overriddenParseRoot.value.accessor,
                                                std::move(resolvedParsePath)}},
                                    };
                                }
                            }
                        }

                        return std::nullopt;
                    };

                    /* Get the input flake, resolve 'path:./...'
                       flakerefs relative to the parent flake. */
                    auto getInputFlake = [&](const FlakeRef & ref, const fetchers::UseRegistries useRegistries) {
                        if (auto resolvedRoots = resolveRelativeInputRoots(ref)) {
                            return readFlake(
                                state,
                                OriginalFlakeRef{ref},
                                ResolvedFlakeRef{ref},
                                EvaluationLockedFlakeRef{ref},
                                resolvedRoots->logicalRoot,
                                overriddenCarrierRoot,
                                makeEvaluationFlakeRoot(resolvedRoots->logicalRoot.value),
                                resolvedRoots->parseRoot,
                                inputAttrPath);
                        } else {
                            return getFlake(state, ref, ref, useRegistries, inputAttrPath);
                        }
                    };

                    auto getReusedLockedInputFlake =
                        [&](const LockedNode & lockedNode, const fetchers::UseRegistries useRegistries) {
                        if (auto resolvedRoots = resolveRelativeInputRoots(lockedNode.originalRef.value)) {
                            // Relative inputs are re-resolved against the
                            // current parent roots. Reopening them through the
                            // previously locked ref freezes the child onto the
                            // old narHash/revision and misses live subdir
                            // updates until the lock file itself is refreshed.
                            return readFlake(
                                state,
                                lockedNode.originalRef,
                                ResolvedFlakeRef{lockedNode.originalRef.value},
                                EvaluationLockedFlakeRef{lockedNode.originalRef.value},
                                resolvedRoots->logicalRoot,
                                overriddenCarrierRoot,
                                makeEvaluationFlakeRoot(resolvedRoots->logicalRoot.value),
                                resolvedRoots->parseRoot,
                                inputAttrPath);
                        }

                        if (shouldUseLivePhase1EvaluationRoot(state, lockedNode.originalRef.value))
                            return getFlake(
                                state,
                                lockedNode.originalRef.value,
                                lockedNode.originalRef.value,
                                useRegistries,
                                inputAttrPath);

                        return getFlake(
                            state,
                            lockedNode.originalRef.value,
                            lockedNode.lockedRef.value,
                            useRegistries,
                            inputAttrPath);
                    };

                    auto getReusedLockedInputRoots = [&](const LockedNode & lockedNode) {
                        if (auto resolvedRoots = resolveRelativeInputRoots(lockedNode.originalRef.value)) {
                            return Phase1FlakeRoots{
                                .logicalRoot = resolvedRoots->logicalRoot,
                                .carrierRoot = overriddenCarrierRoot,
                                .evaluationRoot = makeEvaluationFlakeRoot(resolvedRoots->logicalRoot.value),
                                .parseRoot = resolvedRoots->parseRoot,
                                .preserveLiveEvaluationRoot = false,
                            };
                        }

                        auto inputFlake =
                            getReusedLockedInputFlake(lockedNode, fetchers::UseRegistries::No);
                        return Phase1FlakeRoots{
                            .logicalRoot = inputFlake.logicalRoot,
                            .carrierRoot = inputFlake.carrierRoot,
                            .evaluationRoot = inputFlake.evaluationRoot,
                            .parseRoot = inputFlake.parseRoot,
                            .preserveLiveEvaluationRoot = shouldUseLivePhase1EvaluationRoot(
                                state,
                                lockedNode.originalRef.value),
                        };
                    };

                    /* Do we have an entry in the existing lock file?
                       And the input is not in updateInputs? */
                    std::shared_ptr<LockedNode> oldLock;

                    updatesUsed.insert(inputAttrPath);

                    if (oldNode && !lockFlags.inputUpdates.count(nonEmptyInputAttrPath))
                        if (auto oldLock2 = get(oldNode->inputs, id))
                            if (auto oldLock3 = edgeChild(*oldLock2))
                                oldLock = *oldLock3;
                    if (!oldLock
                        && !lockFlags.recreateLockFile
                        && !lockFlags.inputUpdates.count(nonEmptyInputAttrPath))
                        if (auto oldLock2 = oldLockFile.findInput(inputAttrPath))
                            if (auto oldLock3 = std::dynamic_pointer_cast<LockedNode>(oldLock2))
                                oldLock = std::move(oldLock3);

                    auto normalizeRequestedComparisonRef = [](FlakeRef ref) {
                        if (auto url = fetchers::maybeGetStrAttr(ref.input.attrs, "url")) {
                            try {
                                auto parsed = parseURL(*url, /* lenient = */ true);
                                auto scheme = parseUrlScheme(parsed.scheme);
                                if (scheme.application == "git")
                                    parsed.scheme = scheme.transport;
                                if (auto dir = get(parsed.query, "dir")) {
                                    if (ref.subdir.empty())
                                        ref.subdir = *dir;
                                    if (ref.subdir == *dir) {
                                        parsed.query.erase("dir");
                                        ref.input.attrs.insert_or_assign("url", parsed.to_string());
                                    }
                                }
                            } catch (BadURL &) {
                            }
                        }

                        try {
                            ref = ref.canonicalize();
                        } catch (Error &) {
                        }

                        for (auto attr : {
                                 "ref",
                                 "rev",
                                 "revCount",
                                 "lastModified",
                                 "narHash",
                                 "dirtyRev",
                                 "dirtyShortRev",
                                 "__final",
                             })
                            ref.input.attrs.erase(attr);
                        return ref;
                    };

                    auto sameExistingInputRef = [&](const LockedNode & existing, const FlakeRef & requested) {
                        auto tryCanonicalize = [](const FlakeRef & ref) -> std::optional<FlakeRef> {
                            try {
                                return ref.canonicalize();
                            } catch (Error &) {
                                return std::nullopt;
                            }
                        };
                        auto sameStableIdentity = [&](const FlakeRef & lhs, const FlakeRef & rhs) {
                            auto lhsIdentity = lhs.input.getStableIdentity();
                            auto rhsIdentity = rhs.input.getStableIdentity();
                            return lhsIdentity
                                && rhsIdentity
                                && lhs.subdir == rhs.subdir
                                && *lhsIdentity == *rhsIdentity;
                        };
                        auto sameRequestedRefString = [](const FlakeRef & lhs, const FlakeRef & rhs) {
                            return lhs.to_string() == rhs.to_string();
                        };
                        auto sameSourcePath = [](const FlakeRef & lhs, const FlakeRef & rhs) {
                            auto lhsSource = tryNormalizeAbsoluteLocalPath(lhs.input.getSourcePath());
                            auto rhsSource = tryNormalizeAbsoluteLocalPath(rhs.input.getSourcePath());
                            if (!lhsSource || !rhsSource)
                                return false;
                            return lhs.subdir == rhs.subdir
                                && *lhsSource == *rhsSource;
                        };
                        auto sameCanonicalRequestedRefString = [&](const FlakeRef & lhs, const FlakeRef & rhs) {
                            auto lhsCanonical = tryCanonicalize(lhs);
                            auto rhsCanonical = tryCanonicalize(rhs);
                            return lhsCanonical
                                && rhsCanonical
                                && lhsCanonical->to_string() == rhsCanonical->to_string();
                        };
                        auto sameRequestedShape = [](const FlakeRef & lhs, const FlakeRef & rhs) {
                            return lhs.toPersistedAttrs() == rhs.toPersistedAttrs();
                        };
                        auto sameCanonicalRequestedShape = [&](const FlakeRef & lhs, const FlakeRef & rhs) {
                            auto lhsCanonical = tryCanonicalize(lhs);
                            auto rhsCanonical = tryCanonicalize(rhs);
                            return lhsCanonical
                                && rhsCanonical
                                && lhsCanonical->toPersistedAttrs() == rhsCanonical->toPersistedAttrs();
                        };
                        auto sameNormalizedRequestedShape = [&](const FlakeRef & lhs, const FlakeRef & rhs) {
                            return normalizeRequestedComparisonRef(lhs).toPersistedAttrs()
                                == normalizeRequestedComparisonRef(rhs).toPersistedAttrs();
                        };
                        auto sameNormalizedCanonicalRequestedShape = [&](const FlakeRef & lhs, const FlakeRef & rhs) {
                            auto lhsCanonical = tryCanonicalize(lhs);
                            auto rhsCanonical = tryCanonicalize(rhs);
                            return lhsCanonical
                                && rhsCanonical
                                && normalizeRequestedComparisonRef(*lhsCanonical).toPersistedAttrs()
                                    == normalizeRequestedComparisonRef(*rhsCanonical).toPersistedAttrs();
                        };
                        auto sameNormalizedCanonicalStableIdentity = [&](const FlakeRef & lhs, const FlakeRef & rhs) {
                            auto lhsCanonical = tryCanonicalize(lhs);
                            auto rhsCanonical = tryCanonicalize(rhs);
                            if (!lhsCanonical || !rhsCanonical)
                                return false;
                            auto lhsNormalized = normalizeRequestedComparisonRef(*lhsCanonical);
                            auto rhsNormalized = normalizeRequestedComparisonRef(*rhsCanonical);
                            auto lhsIdentity = lhsNormalized.input.getStableIdentity();
                            auto rhsIdentity = rhsNormalized.input.getStableIdentity();
                            return lhsIdentity
                                && rhsIdentity
                                && lhsNormalized.subdir == rhsNormalized.subdir
                                && *lhsIdentity == *rhsIdentity;
                        };
                        auto canonicalRequested = tryCanonicalize(requested);
                        auto sameCanonicalRef = [&](const FlakeRef & candidate) {
                            auto canonicalCandidate = tryCanonicalize(candidate);
                            return canonicalCandidate
                                && canonicalRequested
                                && *canonicalCandidate == *canonicalRequested;
                        };
                        auto containsRequested = [&](const FlakeRef & candidate) {
                            auto canonicalCandidate = tryCanonicalize(candidate);
                            return canonicalCandidate
                                && canonicalRequested
                                && canonicalCandidate->subdir == canonicalRequested->subdir
                                && (canonicalCandidate->input.contains(canonicalRequested->input)
                                    || canonicalRequested->input.contains(canonicalCandidate->input));
                        };
                        return sameRequestedRefString(existing.originalRef.value, requested)
                            || sameRequestedRefString(existing.lockedRef.value, requested)
                            || sameSourcePath(existing.originalRef.value, requested)
                            || sameSourcePath(existing.lockedRef.value, requested)
                            || sameStableIdentity(existing.originalRef.value, requested)
                            || sameStableIdentity(existing.lockedRef.value, requested)
                            || sameCanonicalRequestedRefString(existing.originalRef.value, requested)
                            || sameCanonicalRequestedRefString(existing.lockedRef.value, requested)
                            || sameRequestedShape(existing.originalRef.value, requested)
                            || sameRequestedShape(existing.lockedRef.value, requested)
                            || sameCanonicalRequestedShape(existing.originalRef.value, requested)
                            || sameCanonicalRequestedShape(existing.lockedRef.value, requested)
                            || sameNormalizedRequestedShape(existing.originalRef.value, requested)
                            || sameNormalizedRequestedShape(existing.lockedRef.value, requested)
                            || sameNormalizedCanonicalRequestedShape(existing.originalRef.value, requested)
                            || sameNormalizedCanonicalRequestedShape(existing.lockedRef.value, requested)
                            || sameNormalizedCanonicalStableIdentity(existing.originalRef.value, requested)
                            || sameNormalizedCanonicalStableIdentity(existing.lockedRef.value, requested)
                            || sameCanonicalRef(existing.originalRef.value)
                            || sameCanonicalRef(existing.lockedRef.value)
                            || containsRequested(existing.originalRef.value)
                            || containsRequested(existing.lockedRef.value);
                    };

                    std::vector<fetchers::Attrs> normalizedRequestedAttrs;

                    auto addNormalizedRequestedRef = [&](const FlakeRef & ref) {
                        auto attrs = normalizeRequestedComparisonRef(ref).toPersistedAttrs();
                        if (std::find(normalizedRequestedAttrs.begin(), normalizedRequestedAttrs.end(), attrs)
                            == normalizedRequestedAttrs.end())
                            normalizedRequestedAttrs.push_back(std::move(attrs));
                    };

                    addNormalizedRequestedRef(*input.ref);

                    auto sameNormalizedRequestedRef = [&](const FlakeRef & candidate) {
                        auto candidateAttrs = normalizeRequestedComparisonRef(candidate).toPersistedAttrs();
                        return std::find(
                                   normalizedRequestedAttrs.begin(),
                                   normalizedRequestedAttrs.end(),
                                   candidateAttrs)
                            != normalizedRequestedAttrs.end();
                    };

                    auto sameOriginalCanonicalRef = [&](const LockedNode & existing) {
                        return sameNormalizedRequestedRef(existing.originalRef.value);
                    };

                    auto sameRawRequestedRef = [&](const FlakeRef & candidate) {
                        return candidate.toPersistedAttrs() == input.ref->toPersistedAttrs();
                    };

                    auto sameRequestedInput = [&]() {
                        bool requestedIsRelative = input.ref->input.isRelative().has_value();
                        bool requestedHasExplicitPin =
                            input.ref->input.getRef()
                            || input.ref->input.getRev()
                            || input.ref->input.attrs.contains("revCount")
                            || input.ref->input.attrs.contains("lastModified")
                            || input.ref->input.attrs.contains("narHash");
                        auto existingIsRelative = isRelativeLockedInput(*oldLock);

                        if (requestedIsRelative != existingIsRelative)
                            return false;

                        if (requestedIsRelative)
                            return sameRawRequestedRef(oldLock->originalRef.value)
                                || sameRawRequestedRef(oldLock->lockedRef.value);

                        if (requestedHasExplicitPin)
                            return sameRawRequestedRef(oldLock->originalRef.value)
                                || sameRawRequestedRef(oldLock->lockedRef.value);

                        return sameRawRequestedRef(oldLock->originalRef.value)
                            || sameRawRequestedRef(oldLock->lockedRef.value)
                            || sameOriginalCanonicalRef(*oldLock)
                            || sameNormalizedRequestedRef(oldLock->lockedRef.value)
                            || (oldLock->isFlake == input.isFlake
                                && sameExistingInputRef(*oldLock, *input.ref));
                    };

                    if (oldLock && !hasCliOverride
                        && (!input.ref->input.isRelative()
                            || oldLock->parentInputAttrPath == overriddenParentPath)
                        && sameRequestedInput()) {
                        debug("keeping existing input '%s'", inputAttrPathS);

                        /* Copy the input from the old lock since its flakeref
                           didn't change and there is no override from a
                           higher level flake. */
                        auto childNode = cloneLockedNodeDeep(*oldLock);
                        childNode->isFlake = input.isFlake;

                        node->inputs.insert_or_assign(id, childNode);

                        // Non-flake inputs are terminal. Once the requested
                        // ref matches an existing locked node, there is no
                        // descendant graph to update lazily or refetch.
                        if (!childNode->isFlake)
                            continue;

                        /* If we have this input in updateInputs, then we
                           must fetch the flake to update it. */
                        auto lb = lockFlags.inputUpdates.lower_bound(nonEmptyInputAttrPath);

                        auto mustRefetch = lb != lockFlags.inputUpdates.end() && lb->get().size() > inputAttrPath.size()
                                           && std::equal(inputAttrPath.begin(), inputAttrPath.end(), lb->get().begin());

                        auto hasDescendantOverride = [&]() {
                            auto it = overrides.lower_bound(nonEmptyInputAttrPath);
                            return it != overrides.end()
                                && it->first.get().size() > inputAttrPath.size()
                                && std::equal(inputAttrPath.begin(), inputAttrPath.end(), it->first.get().begin());
                        }();

                        auto hasDescendantUpdate = lb != lockFlags.inputUpdates.end()
                            && lb->get().size() > inputAttrPath.size()
                            && std::equal(inputAttrPath.begin(), inputAttrPath.end(), lb->get().begin());

                        bool hasFollowsEdges = false;
                        for (auto & entry : oldLock->inputs)
                            if (edgeFollows(entry.second)) {
                                hasFollowsEdges = true;
                                break;
                            }

                        auto mustRefreshRelativeLockedFlake =
                            oldLock->isFlake && isRelativeLockedInput(*oldLock);

                        if (!mustRefetch && !hasDescendantOverride && !hasDescendantUpdate
                            && (trustLock || !hasFollowsEdges)
                            && !mustRefreshRelativeLockedFlake)
                            continue;

                        FlakeInputs fakeInputs;

                        if (!mustRefetch) {
                            if (oldLock->isFlake && isRelativeLockedInput(*oldLock)) {
                                auto inputFlake = getReusedLockedInputFlake(*oldLock, useRegistriesInputs);
                                auto inputOldLock = readLockFile(state.fetchSettings, inputFlake.lockFilePath()).root.get_ptr();
                                nodePaths.insert_or_assign(
                                    childNode,
                                    Phase1FlakeRoots{
                                        .logicalRoot = inputFlake.logicalRoot,
                                        .carrierRoot = inputFlake.carrierRoot,
                                        .evaluationRoot = inputFlake.evaluationRoot,
                                        .parseRoot = inputFlake.parseRoot,
                                        .preserveLiveEvaluationRoot = false,
                                    });
                                computeLocks(
                                    inputFlake.inputs,
                                    childNode,
                                    inputAttrPath,
                                    inputOldLock,
                                    followsPrefix,
                                    inputFlake.logicalRoot,
                                    inputFlake.carrierRoot,
                                    inputFlake.parseRoot,
                                    false);
                                continue;
                            }

                            /* No need to fetch this flake, we can be
                               lazy. However there may be new overrides on the
                               inputs of this flake, so we need to check
                               those. */
                            for (auto & i : oldLock->inputs) {
                                if (auto lockedNode = edgeChild(i.second)) {
                                    fakeInputs.emplace(
                                        i.first,
                                        FlakeInput{
                                            // Prefer the concrete locked ref for remote /
                                            // registry-mediated inputs so recursive traversal
                                            // remains concrete under --refresh and
                                            // --no-registries. Keep absolute local `path:`
                                            // inputs on their original ref so recreate-lock-file
                                            // does not try to reopen the locked
                                            // `path:/...?...` form as a live source tree.
                                            .ref =
                                                (*lockedNode)->originalRef.value.input.getType() == "path"
                                                && !(*lockedNode)->originalRef.value.input.isRelative()
                                                ? (*lockedNode)->originalRef.value
                                                : (*lockedNode)->lockedRef.value,
                                            .isFlake = (*lockedNode)->isFlake,
                                        });
                                } else if (auto follows = edgeFollows(i.second)) {
                                    fakeInputs.emplace(
                                        i.first,
                                        FlakeInput{
                                            .follows = *follows,
                                        });
                                }
                            }
                        }

                        auto deriveLazyChildRoots = [&]() -> std::optional<Phase1FlakeRoots> {
                            if (auto resolvedRoots = resolveRelativeInputRoots(*input.ref)) {
                                return Phase1FlakeRoots{
                                    .logicalRoot = resolvedRoots->logicalRoot,
                                    .carrierRoot = overriddenCarrierRoot,
                                    .evaluationRoot = makeEvaluationFlakeRoot(resolvedRoots->logicalRoot.value),
                                    .parseRoot = resolvedRoots->parseRoot,
                                    .preserveLiveEvaluationRoot = false,
                                };
                            }

                            if (!childNode->isFlake)
                                return std::nullopt;

                            return getReusedLockedInputRoots(*oldLock);
                        };

                        if (mustRefetch) {
                            auto inputFlake = getReusedLockedInputFlake(*oldLock, useRegistriesInputs);
                            auto inputOldLock = readLockFile(state.fetchSettings, inputFlake.lockFilePath()).root.get_ptr();
                            nodePaths.emplace(
                                childNode,
                                Phase1FlakeRoots{
                                    .logicalRoot = inputFlake.logicalRoot,
                                    .carrierRoot = inputFlake.carrierRoot,
                                    .evaluationRoot = inputFlake.evaluationRoot,
                                    .parseRoot = inputFlake.parseRoot,
                                    .preserveLiveEvaluationRoot = shouldUseLivePhase1EvaluationRoot(
                                        state,
                                        oldLock->originalRef.value),
                                });
                            computeLocks(
                                inputFlake.inputs,
                                childNode,
                                inputAttrPath,
                                inputOldLock,
                                followsPrefix,
                                inputFlake.logicalRoot,
                                inputFlake.carrierRoot,
                                inputFlake.parseRoot,
                                false);
                        } else {
                            if (auto roots = deriveLazyChildRoots())
                                nodePaths.insert_or_assign(childNode, std::move(*roots));

                            if (fakeInputs.empty())
                                continue;

                            auto roots = nodePaths.find(childNode);
                            if (roots == nodePaths.end())
                                throw Error(
                                    "internal error: missing phase-1 roots for reused flake input '%s'",
                                    inputAttrPathS);

                            computeLocks(
                                fakeInputs,
                                childNode,
                                inputAttrPath,
                                oldLock,
                                followsPrefix,
                                roots->second.logicalRoot,
                                roots->second.carrierRoot,
                                roots->second.parseRoot,
                                true);
                        }

                    } else {
                        /* We need to create a new lock file entry. So fetch
                           this input. */
                        debug("creating new input '%s'", inputAttrPathS);

                        if (!lockFlags.allowUnlocked && !input.ref->input.isLocked(state.fetchSettings)
                            && !input.ref->input.isRelative())
                            throw Error("cannot update unlocked flake input '%s' in pure mode", inputAttrPathS);

                        /* Note: in case of an --override-input, we use
                            the *original* ref (input2.ref) for the
                            "original" field, rather than the
                            override. This ensures that the override isn't
                            nuked the next time we update the lock
                            file. That is, overrides are sticky unless you
                            use --no-write-lock-file. */
                        auto inputIsOverride = explicitCliOverrides.contains(nonEmptyInputAttrPath);
                        auto ref = (input2.ref && inputIsOverride) ? *input2.ref : *input.ref;

                        if (input.isFlake) {
                            auto inputFlake = getInputFlake(
                                *input.ref, inputIsOverride ? fetchers::UseRegistries::All : useRegistriesInputs);

                            auto childNode =
                                make_ref<LockedNode>(
                                    EvaluationLockedFlakeRef{inputFlake.lockFileRef.value},
                                    OriginalFlakeRef{ref},
                                    true,
                                    overriddenParentPath);

                            node->inputs.insert_or_assign(id, childNode);

                            /* Guard against circular flake imports. */
                            for (auto & parent : parents)
                                if (parent == *input.ref)
                                    throw Error("found circular import of flake '%s'", parent);
                            parents.push_back(*input.ref);
                            Finally cleanup([&]() { parents.pop_back(); });

                            /* Recursively process the inputs of this
                               flake, using its own lock file. */
                            nodePaths.emplace(
                                childNode,
                                Phase1FlakeRoots{
                                    .logicalRoot = inputFlake.logicalRoot,
                                    .carrierRoot = inputFlake.carrierRoot,
                                    .evaluationRoot = inputFlake.evaluationRoot,
                                    .parseRoot = inputFlake.parseRoot,
                                    .preserveLiveEvaluationRoot = shouldUseLivePhase1EvaluationRoot(
                                        state,
                                        ref),
                                });
                            computeLocks(
                                inputFlake.inputs,
                                childNode,
                                inputAttrPath,
                                readLockFile(state.fetchSettings, inputFlake.lockFilePath()).root.get_ptr(),
                                inputAttrPath,
                                inputFlake.logicalRoot,
                                inputFlake.carrierRoot,
                                inputFlake.parseRoot,
                                false);
                        }

                        else {
                            std::optional<Phase1FlakeRoots> roots;
                            std::optional<FlakeRef> lockedRef;

                            // Handle non-flake 'path:./...' inputs.
                            if (auto resolvedRoots = resolveRelativeInputRoots(*input.ref)) {
                                roots.emplace(Phase1FlakeRoots{
                                    .logicalRoot = resolvedRoots->logicalRoot,
                                    .carrierRoot = overriddenCarrierRoot,
                                    .evaluationRoot = makeEvaluationFlakeRoot(resolvedRoots->logicalRoot.value),
                                    .parseRoot = resolvedRoots->parseRoot,
                                    .preserveLiveEvaluationRoot = true,
                                });
                                lockedRef.emplace(*input.ref);
                            } else {
                                EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
                                auto detached = environment.openDetachedEffectScope();
                                auto resolved = environment.resolveFetchIdentity(
                                    detached,
                                    FetchIdentityRequest::make(input.ref->input, useRegistriesInputs));
                                auto fetched = environment.materializeFetch(detached, std::move(resolved.identity));
                                auto mounted = environment.mountFetchedInput(detached, std::move(fetched));
                                lockedRef.emplace(fetchers::Input(*mounted.lockedInput.value), input.ref->subdir);
                                roots.emplace(Phase1FlakeRoots{
                                    .logicalRoot = makeLogicalFlakeRoot(resolved.phase1Root, lockedRef->subdir),
                                    .carrierRoot = makeCarrierRoot(resolved.phase1Root),
                                    .evaluationRoot = makeEvaluationFlakeRoot(resolved.phase1Root, lockedRef->subdir),
                                    .parseRoot = makePhase1ParseRoot(
                                        resolved.phase1Root,
                                        lockedRef->subdir,
                                        shouldUseLivePhase1EvaluationRoot(
                                            state,
                                            *input.ref)),
                                    .preserveLiveEvaluationRoot = shouldUseLivePhase1EvaluationRoot(
                                        state,
                                        *input.ref),
                                });
                            }

                            auto childNode = make_ref<LockedNode>(
                                EvaluationLockedFlakeRef{*lockedRef},
                                OriginalFlakeRef{ref},
                                false,
                                overriddenParentPath);

                            nodePaths.emplace(childNode, std::move(*roots));

                            node->inputs.insert_or_assign(id, childNode);
                        }
                    }

                } catch (Error & e) {
                    e.addTrace({}, "while updating the flake input '%s'", inputAttrPathS);
                    throw;
                }
            }
        };

        nodePaths.emplace(
            newLockFile.root,
                Phase1FlakeRoots{
                    .logicalRoot = flake.logicalRoot,
                    .carrierRoot = flake.carrierRoot,
                    .evaluationRoot = flake.evaluationRoot,
                    .parseRoot = flake.parseRoot,
                    .preserveLiveEvaluationRoot = shouldUseLivePhase1EvaluationRoot(
                        state,
                        flake.originalRef.value),
                });

        computeLocks(
            flake.inputs,
            newLockFile.root,
            {},
            lockFlags.recreateLockFile ? nullptr : oldLockFile.root.get_ptr(),
            {},
            flake.logicalRoot,
            flake.carrierRoot,
            flake.parseRoot,
            false);

        for (auto & i : lockFlags.inputOverrides)
            if (!overridesUsed.count(i.first))
                warn(
                    "the flag '--override-input %s %s' does not match any input",
                    printInputAttrPath(i.first),
                    i.second);

        for (auto & i : lockFlags.inputUpdates)
            if (!updatesUsed.count(i))
                warn("'%s' does not match any input of this flake", printInputAttrPath(i));

        /* Check 'follows' inputs. */
        newLockFile.check();

        debug("new lock file: %s", newLockFile);

        auto writableRef =
            topRef.input.getSourcePath()
                ? flake.lockFileRef.value.input.getSourcePath()
                    ? std::optional<FlakeRef>(flake.lockFileRef.value)
                    : std::optional<FlakeRef>(topRef)
                : std::nullopt;
        auto sourcePath =
            writableRef
                ? writableRef->input.getSourcePath()
                : std::nullopt;

        /* Check whether we need to / can write the new lock file. */
        if (newLockFile != oldLockFile || lockFlags.outputLockFilePath) {

            auto diff = LockFile::diff(oldLockFile, newLockFile);

            if (lockFlags.writeLockFile) {
                if (sourcePath || lockFlags.outputLockFilePath) {
                    if (auto unlockedInput = newLockFile.isUnlocked(state.fetchSettings)) {
                        if (lockFlags.failOnUnlocked)
                            throw Error(
                                "Not writing lock file of flake '%s' because it has an unlocked input ('%s'). "
                                "Use '--allow-dirty-locks' to allow this anyway.",
                                topRef,
                                unlockedInput->value);
                        if (state.fetchSettings.warnDirty)
                            warn(
                                "not writing lock file of flake '%s' because it has an unlocked input ('%s')",
                                topRef,
                                unlockedInput->value);
                    } else {
                        if (!lockFlags.updateLockFile)
                            throw Error(
                                "flake '%s' requires lock file changes but they're not allowed due to '--no-update-lock-file'",
                                topRef);

                        auto newLockFileS = fmt("%s\n", newLockFile);

                        if (lockFlags.outputLockFilePath) {
                            if (lockFlags.commitLockFile)
                                throw Error("'--commit-lock-file' and '--output-lock-file' are incompatible");
                            writeFile(*lockFlags.outputLockFilePath, newLockFileS);
                        } else {
                            auto relPath =
                                ((writableRef ? writableRef->subdir : topRef.subdir) == ""
                                    ? ""
                                    : (writableRef ? writableRef->subdir : topRef.subdir) + "/")
                                + "flake.lock";
                            auto outputLockFilePath = *sourcePath / relPath;

                            bool lockFileExists = pathExists(outputLockFilePath);

                            auto s = chomp(diff);
                            if (lockFileExists) {
                                if (s.empty())
                                    warn("updating lock file %s", PathFmt(outputLockFilePath));
                                else
                                    warn("updating lock file %s:\n%s", PathFmt(outputLockFilePath), s);
                            } else
                                warn("creating lock file %s: \n%s", PathFmt(outputLockFilePath), s);

                            std::optional<std::string> commitMessage = std::nullopt;

                            if (lockFlags.commitLockFile) {
                                std::string cm;

                                cm = settings.commitLockFileSummary.get();

                                if (cm == "") {
                                    cm = fmt("%s: %s", relPath, lockFileExists ? "Update" : "Add");
                                }

                                cm += "\n\nFlake lock file updates:\n\n";
                                cm += filterANSIEscapes(diff, true);
                                commitMessage = cm;
                            }

                            (writableRef ? writableRef->input : topRef.input).putFile(
                                CanonPath(relPath),
                                newLockFileS,
                                commitMessage);
                        }

                        /* Rewriting the lockfile changed the top-level
                           repo, so we should re-read it. FIXME: we could
                           also just clear the 'rev' field... */

                        /* No explicit `flake.lockFilePath().invalidateCache()`
                           here: master's AD-1 sweep removed the per-path
                           `invalidateCache(CanonPath)` API. CachingSourceAccessor
                           memoizes `maybeLstat`/`readLink` only (readFile passes
                           through); `getFlake(state, topRef, ...)` below re-fetches
                           through the fetcher cache and picks up a fresh accessor
                           for the rewritten input. Subsequent in-process
                           `lockFlake` calls from the same EvalState either go
                           through `resetFileCache` (repl `:reload`) or build a
                           fresh `InstallableFlake` memoizing `_lockedFlake`, so
                           no stale `pathExists` observation leaks. */

                        auto prevLockedRef = flake.lockedRef;

                        // Capture old root carrier path before re-reading.
                        auto oldRootCarrierPath = flake.carrierRoot.value.path;

                        flake = getFlake(state, topRef, useRegistriesTop);

                        // Writing the lock file changes the repository's NAR
                        // hash (a new file was added), so the new flake fetch
                        // lands in a different store path than the one used
                        // when computeLocks built nodePaths.  Rebase every
                        // nodePaths entry that was derived from the old root
                        // carrier onto the new one so that evaluationRoot and
                        // carrierRoot remain consistent in buildResolvedFlakeGraph.
                        if (flake.carrierRoot.value.path != oldRootCarrierPath) {
                            // NOTE: the root entry in nodePaths is NOT updated here.
                            // buildResolvedFlakeGraph constructs its own root Phase1FlakeRoots
                            // from the (already re-read) flake object directly, overriding
                            // whatever nodePaths contains for the root.

                            // Rebase direct child inputs of the root node.
                            // Only direct children are rebased — transitive inputs
                            // (e.g., root/sub0 accessed through a "../" upward ref)
                            // are resolved through their own parent node and must
                            // not be rebased independently.
                            for (auto & [inputName, inputSpec] : newLockFile.root->inputs) {
                                if (auto child = std::get_if<ref<LockedNode>>(&inputSpec)) {
                                    auto it = nodePaths.find(*child);
                                    if (it == nodePaths.end())
                                        continue;
                                    auto & roots = it->second;
                                    if (roots.carrierRoot.value.path != oldRootCarrierPath)
                                        continue;
                                    if (roots.evaluationRoot.value.path == oldRootCarrierPath)
                                        continue;
                                    auto relPath = oldRootCarrierPath.makeRelative(
                                        roots.evaluationRoot.value.path);
                                    auto newEvalPath = CanonPath(relPath, flake.evaluationRoot.value.path);
                                    roots.evaluationRoot = EvaluationFlakeRootPath{
                                        SourcePath{flake.evaluationRoot.value.accessor, newEvalPath}};
                                    roots.carrierRoot = flake.carrierRoot;
                                    roots.logicalRoot = LogicalFlakeRootPath{
                                        SourcePath{flake.logicalRoot.value.accessor, newEvalPath}};
                                }
                            }
                        }

                        if (lockFlags.commitLockFile && flake.lockedRef.value.input.getRev()
                            && prevLockedRef.value.input.getRev() != flake.lockedRef.value.input.getRev())
                            warn("committed new revision '%s'", flake.lockedRef.value.input.getRev()->gitRev());
                    }
                } else
                    throw Error(
                        "cannot write modified lock file of flake '%s' (use '--no-write-lock-file' to ignore)", topRef);
            } else {
                warn("not writing modified lock file of flake '%s':\n%s", topRef, chomp(diff));
                flake.forceDirty = true;
            }
        }

        auto resolvedGraph = buildResolvedFlakeGraph(state, flake, newLockFile, nodePaths, auth);

        return LockedFlake{
            .flake = std::move(flake),
            .lockFile = std::move(newLockFile),
            .resolvedGraph = std::move(resolvedGraph),
        };

    } catch (Error & e) {
        e.addTrace({}, "while updating the lock file of flake '%s'", flake.lockedRef.value.to_string());
        throw;
    }
    }); // AuthorityGate::withAuthority
}

LockedFlake
lockFlake(const Settings & settings, EvalState & state, const FlakeRef & topRef, const LockFlags & lockFlags)
{
    auto useRegistries = lockFlags.useRegistries.value_or(settings.useRegistries);
    auto useRegistriesTop = useRegistries ? fetchers::UseRegistries::All : fetchers::UseRegistries::No;
    return lockFlake(settings, state, topRef, lockFlags, getFlake(state, topRef, topRef, useRegistriesTop, {}));
}

LockedFlake
lockFlake(const Settings & settings, EvalState & state, const SourcePath & flakeDir, const LockFlags & lockFlags)
{
    /* We need a fake flakeref to put in the `Flake` struct, but it's not used for anything. */
    auto fakeRef = parseFlakeRef(state.fetchSettings, "flake:get-flake");
    return lockFlake(
        settings,
        state,
        fakeRef,
        lockFlags,
        readFlake(
            state,
            OriginalFlakeRef{fakeRef},
            ResolvedFlakeRef{fakeRef},
            EvaluationLockedFlakeRef{fakeRef},
            makeLogicalFlakeRoot(flakeDir),
            makeCarrierRoot(flakeDir),
            makeEvaluationFlakeRoot(flakeDir),
            makeParseFlakeRoot(flakeDir),
            {}));
}

static ref<SourceAccessor> makeInternalFS()
{
    auto internalFS = make_ref<MemorySourceAccessor>(MemorySourceAccessor{});
    internalFS->setPathDisplay("«flakes-internal»", "");
    internalFS->addFile(
        CanonPath("call-flake.nix"),
#include "call-flake.nix.gen.hh" // IWYU pragma: keep
    );
    return internalFS;
}

static auto internalFS = makeInternalFS();

static Value * requireInternalFile(EvalState & state, CanonPath path)
{
    SourcePath p{internalFS, path};
    auto v = state.allocValue();
    state.evalFile(p, *v); // has caching
    return v;
}

const ResolvedFlakeNode * ResolvedFlakeGraph::findNode(const ResolvedFlakeGraphNodeKey & key) const
{
    auto it = nodes.find(key);
    return it == nodes.end() ? nullptr : &it->second;
}

const ResolvedFlakeNode * ResolvedFlakeGraph::findNode(const ResolvedFlakeGraphRootKey & key) const
{
    return findNode(makeResolvedFlakeGraphNodeKey(asFlakeGraphNodeKey(key)));
}

const ResolvedFlakeNode & ResolvedFlakeGraph::requireNode(const ResolvedFlakeGraphNodeKey & key) const
{
    auto * node = findNode(key);
    if (!node)
        throw Error("internal error: resolved flake graph is missing node '%s'", asFlakeGraphNodeKey(key).value);
    return *node;
}

const ResolvedFlakeNode & ResolvedFlakeGraph::requireNode(const ResolvedFlakeGraphRootKey & key) const
{
    return requireNode(makeResolvedFlakeGraphNodeKey(asFlakeGraphNodeKey(key)));
}

const ResolvedFlakeNode & ResolvedFlakeGraph::rootNode() const
{
    return requireNode(rootKey);
}

static void emitResolvedSourceInfo(
    EvalState & state,
    const LockedFlake & lockedFlake,
    const ResolvedFlakeGraphNodeKey & key,
    const ResolvedFlakeNode & node,
    Value & vSourceInfo)
{
    auto sourceNode = lockedFlake.resolvedGraph.findNode(node.sourceInfoKey);
    if (!sourceNode)
        throw Error(
            "internal error: flake node '%s' references missing source-info node '%s'",
            asFlakeGraphNodeKey(key).value,
            asFlakeGraphNodeKey(node.sourceInfoKey).value);

    auto carrierStorePath = sourceNode->carrierStorePath;
    emitTreeAttrs(
        state,
        carrierStorePath,
        sourceNode->lockedInput,
        vSourceInfo,
        depSourceForFlakeGraphNode(node.sourceInfoKey),
        false,
        asFlakeGraphNodeKey(node.sourceInfoKey) == asFlakeGraphNodeKey(lockedFlake.resolvedGraph.rootKey)
            && lockedFlake.flake.forceDirty);
}

static void mkResolvedInputSpecValue(EvalState & state, const ResolvedFlakeInputSpec & spec, Value & value)
{
    if (auto targetKey = spec.targetNodeKey()) {
        value.mkString(asFlakeGraphNodeKey(*targetKey).value, state.mem);
        return;
    }

    auto & follows = *spec.followsPath();
    auto list = state.buildList(follows.size());
    for (size_t i = 0; i < follows.size(); ++i)
        (list[i] = state.allocValue())->mkString(follows[i], state.mem);
    value.mkList(list);
}

void validateResolvedFlakeGraph(const ResolvedFlakeGraph & graph)
{
    graph.rootNode();

    for (auto & [key, node] : graph.nodes) {
        if (node.parentKey && !graph.findNode(*node.parentKey))
            throw Error(
                "internal error: resolved flake node '%s' references missing parent '%s'",
                asFlakeGraphNodeKey(key).value,
                asFlakeGraphNodeKey(*node.parentKey).value);

        if (!graph.findNode(node.sourceInfoKey))
            throw Error(
                "internal error: resolved flake node '%s' references missing source-info node '%s'",
                asFlakeGraphNodeKey(key).value,
                asFlakeGraphNodeKey(node.sourceInfoKey).value);

        try {
            if (!node.relativePath.value.empty())
                (void) CanonPath(node.relativePath.value);
        } catch (Error &) {
            throw Error(
                "internal error: resolved flake node '%s' has invalid relativePath '%s'",
                asFlakeGraphNodeKey(key).value,
                node.relativePath.value);
        }

        if (node.inputSpecs.size() != node.resolvedInputs.size())
            throw Error(
                "internal error: resolved flake node '%s' has mismatched inputSpecs/resolvedInputs sizes",
                asFlakeGraphNodeKey(key).value);

        for (auto & [inputName, _inputSpec] : node.inputSpecs)
            if (node.resolvedInputs.find(inputName) == node.resolvedInputs.end())
                throw Error(
                    "internal error: resolved flake node '%s' is missing resolved input for '%s'",
                    asFlakeGraphNodeKey(key).value,
                    inputName);

        for (auto & [inputName, targetKey] : node.resolvedInputs) {
            if (node.inputSpecs.find(inputName) == node.inputSpecs.end())
                throw Error(
                    "internal error: resolved flake node '%s' is missing input spec for '%s'",
                    asFlakeGraphNodeKey(key).value,
                    inputName);

            if (!graph.findNode(targetKey))
                throw Error(
                    "internal error: resolved flake node '%s' resolves input '%s' to missing node '%s'",
                    asFlakeGraphNodeKey(key).value,
                    inputName,
                    asFlakeGraphNodeKey(targetKey).value);
        }
    }
}

static void buildResolvedFlakeGraphValue(EvalState & state, const LockedFlake & lockedFlake, Value & value)
{
    auto graph = state.buildBindings(2);
    graph.alloc("root").mkString(asFlakeGraphNodeKey(lockedFlake.resolvedGraph.rootKey).value, state.mem);

    auto nodes = state.buildBindings(lockedFlake.resolvedGraph.nodes.size());
    for (auto & [key, node] : lockedFlake.resolvedGraph.nodes) {
        auto nodeValue = state.buildBindings(12);

        if (node.parentKey)
            nodeValue.alloc("parent").mkString(asFlakeGraphNodeKey(*node.parentKey).value, state.mem);
        else
            nodeValue.alloc("parent").mkNull();

        nodeValue.alloc("sourceInfoKey").mkString(asFlakeGraphNodeKey(node.sourceInfoKey).value, state.mem);

        auto inputSpecs = state.buildBindings(node.inputSpecs.size());
        for (auto & [inputName, inputSpec] : node.inputSpecs)
            mkResolvedInputSpecValue(state, inputSpec, inputSpecs.alloc(state.symbols.create(inputName)));
        nodeValue.alloc("inputSpecs").mkAttrs(inputSpecs);

        auto resolvedInputs = state.buildBindings(node.resolvedInputs.size());
        for (auto & [inputName, targetKey] : node.resolvedInputs)
            resolvedInputs.alloc(state.symbols.create(inputName)).mkString(asFlakeGraphNodeKey(targetKey).value, state.mem);
        nodeValue.alloc("resolvedInputs").mkAttrs(resolvedInputs);

        nodeValue.alloc("isFlake").mkBool(node.isFlake);
        emitResolvedSourceInfo(state, lockedFlake, key, node, nodeValue.alloc("sourceInfo"));
        state.retainPathAccessor(node.carrierPath().accessor);
        nodeValue.alloc("carrierPath").mkPath(node.carrierPath(), state.mem);

        auto sourceNode = lockedFlake.resolvedGraph.findNode(node.sourceInfoKey);
        if (!sourceNode)
            throw Error(
                "internal error: resolved flake node '%s' references missing source-info node '%s'",
                asFlakeGraphNodeKey(key).value,
                asFlakeGraphNodeKey(node.sourceInfoKey).value);
        auto carrierStorePath = sourceNode->carrierStorePath;
        auto logicalRootPath = CanonPath(
            !node.relativePath.value.empty()
                ? state.store->printStorePath(carrierStorePath) + "/" + node.relativePath.value
                : state.store->printStorePath(carrierStorePath));

        // flakePath is the authoritative phase-2 import root. Keep it as the
        // mounted/store-backed evaluation root, but do not attach preserve-mode
        // path provenance here: callers still expect raw path coercion on `./.`
        // to copy the root and produce the historical `<hash>-<hash>-source`
        // shape. `outPath` is the value that carries the registered-source
        // identity for traced output semantics.
        {
            auto & vFlakePath = nodeValue.alloc("flakePath");
            state.retainPathAccessor(node.flakePath().accessor);
            vFlakePath.mkPath(node.flakePath(), state.mem);
        }

        {
            auto & vDisplayFlakePath = nodeValue.alloc("displayFlakePath");
            state.retainPathAccessor(node.displayPath().accessor);
            vDisplayFlakePath.mkPath(node.displayPath(), state.mem);
        }

        nodeValue.alloc("relativePath").mkString(node.relativePath.value, state.mem);
        nodeValue.alloc("subdir").mkString(node.subdir.value, state.mem);

        // Authoritative outPath: computed from carrierStorePath + relativePath.
        // call-flake.nix consumes this directly instead of reconstructing it
        // from sourceInfo.outPath + relativePath.
        // Carries PathObject with node-key identity so that deps recorded
        // from values derived from self.outPath (e.g., self + "/file.txt")
        // use the correct registered source identity.
        {
            auto outPathStr = state.store->printStorePath(carrierStorePath);
            if (!node.relativePath.value.empty())
                outPathStr += "/" + node.relativePath.value;
            auto & vOutPath = nodeValue.alloc("outPath");
            vOutPath.mkString(
                outPathStr,
                NixStringContext{NixStringContextElem::Opaque{.path = carrierStorePath}},
                state.mem);
            state.publishPathProvenance(vOutPath, PathObject{
                .source = depSourceForFlakeGraphNode(key),
                .rootPath = logicalRootPath,
            });
        }

        nodes.alloc(state.symbols.create(asFlakeGraphNodeKey(key).value)).mkAttrs(nodeValue);
    }

    graph.alloc("nodes").mkAttrs(nodes);
    value.mkAttrs(graph);
}

/// Evaluate a locked flake's outputs.
///
/// Flake evaluation is two-phase by necessity:
///   1. lockFlake: discovers inputs (partial eval of flake.nix), resolves them
///      against the lock file and registries, fetches missing inputs, writes
///      flake.lock. This is the "graph discovery" phase — the dependency graph
///      must be fully known before evaluation begins.
///   2. callFlake (this function): constructs the resolved `inputs` attrset and
///      calls `outputs`. This is the "evaluation" phase.
///
/// The eval-trace system intentionally instruments only callFlake. lockFlake
/// runs before any DepCaptureScope exists, so its file reads are structurally
/// excluded from trace recording. callFlake therefore consumes a fully resolved
/// semantic graph and performs no fetches or lock-file parsing itself.
void callFlake(EvalState & state, const LockedFlake & lockedFlake, Value & vRes)
{
    experimentalFeatureSettings.require(Xp::Flakes);

    EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
    auto detached = environment.openDetachedEffectScope();

    for (auto & [key, node] : lockedFlake.resolvedGraph.nodes) {
        auto sourceNode = lockedFlake.resolvedGraph.findNode(node.sourceInfoKey);
        if (!sourceNode)
            throw Error(
                "internal error: resolved flake node '%s' references missing source-info node '%s'",
                asFlakeGraphNodeKey(key).value,
                asFlakeGraphNodeKey(node.sourceInfoKey).value);
        environment.authorizeStorePath(detached, sourceNode->carrierStorePath);
    }

    Value * vCallFlake = requireInternalFile(state, CanonPath("call-flake.nix"));
    auto vGraph = state.allocValue();
    buildResolvedFlakeGraphValue(state, lockedFlake, *vGraph);

    Value * args[] = {vGraph};
    state.callFunction(*vCallFlake, args, vRes, noPos);
}

std::optional<Fingerprint> LockedFlake::getFingerprint(Store & store, const fetchers::Settings & fetchSettings) const
{
    auto inputFingerprint = flake.lockedRef.value.input.getFingerprint(store);
    if (!inputFingerprint)
        return std::nullopt;

    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::LockedFlakeFingerprint>();
    builder.field("input-fingerprint", *inputFingerprint);
    builder.field("locked-subdir", flake.lockedRef.value.subdir);
    auto lockFileUnlocked = static_cast<bool>(lockFile.isUnlocked(fetchSettings));
    builder.field("lock-file-unlocked", lockFileUnlocked);
    if (!lockFileUnlocked)
        builder.field("lock-file", fmt("%s", lockFile));

    builder.field("resolved-graph-root-key", asFlakeGraphNodeKey(resolvedGraph.rootKey).value);
    for (const auto & [key, node] : resolvedGraph.nodes) {
        builder.field("resolved-graph-node-key", asFlakeGraphNodeKey(key).value);
        builder.field("resolved-graph-node-source-info-key", asFlakeGraphNodeKey(node.sourceInfoKey).value);
        builder.field("resolved-graph-node-locked-version-identity", node.lockedVersionIdentity);
        builder.field("resolved-graph-node-carrier-store-path", store.printStorePath(node.carrierStorePath));
        builder.field("resolved-graph-node-relative-path", node.relativePath.value);
        builder.field("resolved-graph-node-subdir", node.subdir.value);
        builder.field("resolved-graph-node-is-flake", node.isFlake);
        if (node.parentKey)
            builder.field("resolved-graph-node-parent-key", asFlakeGraphNodeKey(*node.parentKey).value);

        for (const auto & [inputName, inputSpec] : node.inputSpecs) {
            builder.field("resolved-graph-input-spec-name", inputName);
            if (const auto * targetKey = inputSpec.targetNodeKey()) {
                builder.field("resolved-graph-input-spec-kind", "target-key");
                builder.field("resolved-graph-input-spec-target-key", asFlakeGraphNodeKey(*targetKey).value);
            } else {
                const auto & followsPath = *inputSpec.followsPath();
                builder.field("resolved-graph-input-spec-kind", "follows-path");
                builder.field("resolved-graph-input-spec-follows-count", static_cast<uint64_t>(followsPath.size()));
                for (const auto & component : followsPath)
                    builder.field("resolved-graph-input-spec-follows-component", component);
            }
        }

        for (const auto & [inputName, targetKey] : node.resolvedInputs) {
            builder.field("resolved-graph-resolved-input-name", inputName);
            builder.field("resolved-graph-resolved-input-target-key", asFlakeGraphNodeKey(targetKey).value);
        }
    }

    /* Include revCount and lastModified because they're not
       necessarily implied by the content fingerprint (e.g. for
       tarball flakes) but can influence the evaluation result.

       A lazy revCount is computed by the fetcher, so its value is
       functionally determined by `rev`. We only need to record its
       presence, not force its value. A lazy and a concrete revCount
       that would resolve to the same value produce different
       fingerprints, sacrificing some cache hits to avoid the cost
       of forcing. */
    if (auto revCount = get(flake.lockedRef.value.input.attrs, "revCount")) {
        if (std::get_if<fetchers::LazyAttr>(revCount)) {
            builder.field("has-rev-count", true);
        } else if (auto n = flake.lockedRef.value.input.getRevCount()) {
            builder.field("rev-count", *n);
        }
    }
    if (auto lastModified = flake.lockedRef.value.input.getLastModified())
        builder.field("last-modified", *lastModified);

    // FIXME: as an optimization, if the flake contains a lock file
    // and we haven't changed it, then it's sufficient to use
    // flake.sourceInfo.storePath for the fingerprint.
    auto digest = builder.finish();
    Fingerprint fingerprint(eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm()));
    std::copy(digest.bytes.begin(), digest.bytes.end(), fingerprint.hash);
    return fingerprint;
}

Flake::~Flake() {}

/// Compute an eval-trace digest of the resolved flake graph.
///
/// Captures the graph topology (node keys, parent chains, inputSpecs,
/// resolvedInputs, follows) and per-node locked input identity.
///
/// Uses the locked input's serialized representation (lockedRef.input)
/// instead of carrier/flake store paths.  Store paths are mutable — they
/// change when lockFlake() writes flake.lock to the working tree — but
/// call-flake.nix must not read flake.lock, so the evaluation result is
/// determined by the locked input identities, not the store path.  The
/// dep verification system catches content changes in source files.
///
/// Fields are length-prefixed (uint64_t, native byte order) to prevent
namespace {

class FlakeRootLoaderHolder final : public RootLoaderHolder
{
    EvalState & state_;
    ref<const LockedFlake> lockedFlake_;

public:
    FlakeRootLoaderHolder(EvalState & state, ref<const LockedFlake> lockedFlake)
        : state_(state)
        , lockedFlake_(std::move(lockedFlake))
    {
    }

    Value * loadRoot() override
    {
        if (getEnv("NIX_ALLOW_EVAL").value_or("1") == "0")
            throw Error("not everything is cached, but evaluation is not allowed");

        auto vFlake = state_.allocValue();
        callFlake(state_, *lockedFlake_, *vFlake);
        state_.forceAttrs(*vFlake, noPos, "while parsing cached flake data");

        auto outputs = vFlake->attrs()->get(state_.symbols.create("outputs"));
        assert(outputs);
        return outputs->value;
    }
};

}

ref<eval_trace::TraceSession> openTraceCache(EvalState & state, ref<const LockedFlake> lockedFlake)
{
    EvalEnvironment environment(makeSessionEvalEnvironmentAuthority(state));
    auto detached = environment.openDetachedEffectScope();
    auto sessionConfigRequest = buildTraceSessionConfigRequest(
        *lockedFlake,
        lockedFlake->getFingerprint(*state.store, state.fetchSettings));
    auto authorityNodes = buildFlakeAuthorityNodeSpecs(*lockedFlake);
    auto rootLoader = RootLoaderCapability::create(
        std::make_unique<FlakeRootLoaderHolder>(state, lockedFlake));
    auto authorityRequest = FlakeGraphTraceSessionAuthorityRequest::create(
        std::move(rootLoader),
        std::move(authorityNodes),
        {});

    auto sessionInputs = environment.captureSessionOpenInputs(detached, state.getLookupPath());
    auto sessionOpen = assembleFlakeTraceSessionOpen(
        std::move(sessionInputs),
        std::move(authorityRequest),
        sessionConfigRequest);
    auto session = environment.openEvalSession(std::move(sessionOpen));

    return environment.traceSession(session);
}

} // namespace flake

} // namespace nix
