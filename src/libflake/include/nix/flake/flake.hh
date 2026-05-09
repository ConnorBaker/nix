#pragma once
///@file

#include "nix/expr/eval-environment/domains.hh"
#include "nix/util/types.hh"
#include "nix/flake/flakeref.hh"
#include "nix/flake/phase-types.hh"
#include "nix/flake/lockfile.hh"
#include "nix/expr/value.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"

namespace nix {

class EvalState;

namespace flake {

struct Settings;

struct FlakeInput;

typedef std::map<FlakeId, FlakeInput> FlakeInputs;

/**
 * FlakeInput is the 'Flake'-level parsed form of the "input" entries
 * in the flake file.
 *
 * A FlakeInput is normally constructed by the 'parseFlakeInput'
 * function which parses the input specification in the '.flake' file
 * to create a 'FlakeRef' (a fetcher, the fetcher-specific
 * representation of the input specification, and possibly the fetched
 * local store path result) and then creating this FlakeInput to hold
 * that FlakeRef, along with anything that might override that
 * FlakeRef (like command-line overrides or "follows" specifications).
 *
 * A FlakeInput is also sometimes constructed directly from a FlakeRef
 * instead of starting at the flake-file input specification
 * (e.g. overrides, follows, and implicit inputs).
 *
 * A FlakeInput will usually have one of either "ref" or "follows"
 * set.  If not otherwise specified, a "ref" will be generated to a
 * 'type="indirect"' flake, which is treated as simply the name of a
 * flake to be resolved in the registry.
 */

struct FlakeInput
{
    std::optional<FlakeRef> ref;
    /**
     * true = process flake to get outputs
     *
     * false = (fetched) static source path
     */
    bool isFlake = true;
    std::optional<InputAttrPath> follows;
    FlakeInputs overrides;
};

struct ConfigFile
{
    using ConfigValue = std::variant<std::string, int64_t, Explicit<bool>, std::vector<std::string>>;

    std::map<std::string, ConfigValue> settings;

    void apply(const Settings & settings);
};

/**
 * A flake in context
 */
struct Flake
{
    /**
     * The original flake specification (by the user)
     */
    OriginalFlakeRef originalRef;

    /**
     * registry references and caching resolved to the specific underlying flake
     */
    ResolvedFlakeRef resolvedRef;

    /**
     * the specific local store result of invoking the fetcher
     */
    EvaluationLockedFlakeRef lockedRef;

    /**
     * The ref that should be persisted into lock files.
     *
     * This may differ from `lockedRef` when evaluating a top-level local
     * working tree with `inputs.self` attributes: evaluation should keep the
     * live working-tree view, while lock-file entries must stay tied to the
     * last stable locked source.
     */
    PersistedLockFileFlakeRef lockFileRef;

    /**
     * The live logical flake root used for relative-path resolution and
     * lock-file location.
     */
    LogicalFlakeRootPath logicalRoot;

    /**
     * Carrier root that bounds relative-path resolution for `logicalRoot`.
     *
     * This may be a live source root even when phase-2 evaluation happens from
     * a store-backed copy.
     */
    CarrierRootPath carrierRoot;

    /**
     * Exact live phase-1 import root used to parse/evaluate `flake.nix`.
     *
     * Phase-2 mounted import roots are carried separately on
     * `ResolvedFlakeNode::evaluationRoot` after graph construction.
     */
    ParseFlakeRootPath parseRoot;

    /**
     * Exact phase-1 physical import root used to evaluate `flake.nix`.
     *
     * This may differ from `parseRoot` when diagnostics should stay anchored
     * to a caller-visible local path but evaluation must use a different
     * accessor-backed root.
     */
    EvaluationFlakeRootPath evaluationRoot;

    /**
     * Pretend that `lockedRef` is dirty.
     */
    bool forceDirty = false;

    std::optional<std::string> description;

    FlakeInputs inputs;

    /**
     * Attributes to be retroactively applied to the `self` input
     * (such as `submodules = true`).
     */
    fetchers::Attrs selfAttrs;

    /**
     * 'nixConfig' attribute
     */
    ConfigFile config;

    ~Flake();

    SourcePath flakePath() const
    {
        return parseRoot.value;
    }

    SourcePath lockFilePath() const
    {
        return logicalRootToLockFilePath(logicalRoot);
    }
};

Flake getFlake(EvalState & state, const FlakeRef & flakeRef, fetchers::UseRegistries useRegistries);

/**
 * Fingerprint of a locked flake; used as a cache key.
 */
typedef Hash Fingerprint;

struct ResolvedFlakeInputSpec
{
    /**
     * Original input declaration from the resolved lock graph.
     *
     * This is exposed to Nix for graph inspection and invariant checking.
     * Phase-2 flake evaluation should consume ResolvedFlakeNode::resolvedInputs
     * for the actual `inputs` attrset after all `follows` edges have been
     * collapsed.
     */
    std::variant<ResolvedFlakeGraphNodeKey, InputAttrPath> target;

    const ResolvedFlakeGraphNodeKey * targetNodeKey() const
    {
        return std::get_if<ResolvedFlakeGraphNodeKey>(&target);
    }

    const InputAttrPath * followsPath() const
    {
        return std::get_if<InputAttrPath>(&target);
    }
};

struct ResolvedFlakeNode
{
    /**
     * Structural parent in the resolved lock graph, when present.
     *
     * Informational only: phase-2 path resolution must use evaluationRoot /
     * carrierRoot / relativePath directly rather than reconstructing paths
     * from the parent chain.
     */
    std::optional<ResolvedFlakeGraphNodeKey> parentKey;
    /**
     * Node key whose sourceInfo metadata is authoritative for this node.
     *
     * Relative path inputs inherit sourceInfo from the nearest non-relative
     * ancestor so sourceInfo.outPath continues to name the carrier store root.
     */
    ResolvedFlakeGraphNodeKey sourceInfoKey;
    /**
     * Original input declarations before `follows` collapse.
     */
    std::map<FlakeId, ResolvedFlakeInputSpec> inputSpecs;
    /**
     * Final phase-2 input mapping after `follows` collapse.
     *
     * This is the authoritative contract for constructing the `inputs`
     * attrset in call-flake.nix.
     */
    std::map<FlakeId, ResolvedFlakeGraphNodeKey> resolvedInputs;
    bool isFlake = true;
    /**
     * Carrier root for this node.
     *
     * This is the source root that backs the logical flake root. It is used
     * for sourceInfo emission and as the mount-table root for stripping
     * `subdir` when authoring flake-input dep keys.
     */
    CarrierRootPath carrierRoot;
    /**
     * Exact phase-2 import root used to evaluate flake.nix.
     */
    EvaluationFlakeRootPath evaluationRoot;
    /**
     * Caller-visible phase-2 display root used for diagnostics.
     *
     * This may differ from `evaluationRoot` for unlocked local roots. In that
     * case, phase 2 imports from `evaluationRoot` but error positions should
     * still point at the live local tree.
     */
    DisplayFlakeRootPath displayRoot;
    /**
     * Authoritative store path for the mounted carrier root.
     *
     * Phase 2 must use this for `sourceInfo.outPath`, `self.outPath`, and
     * store-path authorization. Recomputing from `lockedInput` is not reliable
     * for all fetchers.
     */
    StorePath carrierStorePath;
    /**
     * Exact logical flake root relative to carrierRoot.
     *
     * Retained for fingerprinting/debugging and for reasoning about carrier
     * root vs evaluation root. Phase 2 imports from evaluationRoot, while the
     * public flake value continues to publish sourceInfo.outPath plus this
     * relative path.
     */
    LogicalFlakeRelativePath relativePath;
    /**
     * Locked `dir` metadata preserved for debugging/fingerprinting.
     *
     * Phase-2 evaluation should not reconstruct paths from this field.
     */
    FlakeInputSubdir subdir;
    /**
     * Locked fetcher input for sourceInfo emission and CLI re-fetch.
     *
     * This is the per-node sourceInfo data replacing the former `lockNode`
     * field.  Unlike `lockNode` (which carried the entire lock graph node
     * including nested inputs/follows), this is just the fetcher identity
     * and attributes needed for `emitTreeAttrs`.
     */
    fetchers::Input lockedInput;
    /**
     * Pre-computed canonical version identity hash for session graph digest.
     *
     * This remains stable across equivalent locked-input representations and
     * avoids ad hoc string fallbacks. Content-sensitive fingerprints win when
     * available; otherwise the identity is derived canonically from rev /
     * dirtyRev / stableIdentity / fetcher attrs.
     */
    LockedVersionIdentity lockedVersionIdentity;

    const SourcePath & carrierPath() const
    {
        return carrierRoot.value;
    }

    const SourcePath & flakePath() const
    {
        return evaluationRoot.value;
    }

    const SourcePath & displayPath() const
    {
        return displayRoot.value;
    }
};

struct ResolvedFlakeGraph
{
    /**
     * Total resolved graph exported from phase 1 into call-flake.nix.
     *
     * Every phase-2-reachable node must have authoritative carrierRoot,
     * evaluationRoot, sourceInfoKey, and resolvedInputs entries before evaluation
     * begins. parentKey/sourceInfoKey/resolvedInputs targets must all resolve
     * within this map, and evaluationRoot must remain within carrierRoot.
     */
    ResolvedFlakeGraphRootKey rootKey;
    std::map<ResolvedFlakeGraphNodeKey, ResolvedFlakeNode> nodes;

    const ResolvedFlakeNode * findNode(const ResolvedFlakeGraphNodeKey & key) const;
    const ResolvedFlakeNode * findNode(const ResolvedFlakeGraphRootKey & key) const;
    const ResolvedFlakeNode & requireNode(const ResolvedFlakeGraphNodeKey & key) const;
    const ResolvedFlakeNode & requireNode(const ResolvedFlakeGraphRootKey & key) const;

    const ResolvedFlakeNode & rootNode() const;
};

struct LockedFlake
{
    Flake flake;
    LockFile lockFile;
    ResolvedFlakeGraph resolvedGraph;

    std::optional<Fingerprint> getFingerprint(Store & store, const fetchers::Settings & fetchSettings) const;
};

struct LockFlags
{
    /**
     * Whether to ignore the existing lock file, creating a new one
     * from scratch.
     */
    bool recreateLockFile = false;

    /**
     * Whether to update the lock file at all. If set to false, if any
     * change to the lock file is needed (e.g. when an input has been
     * added to flake.nix), you get a fatal error.
     */
    bool updateLockFile = true;

    /**
     * Whether to write the lock file to disk. If set to true, if the
     * any changes to the lock file are needed and the flake is not
     * writable (i.e. is not a local Git working tree or similar), you
     * get a fatal error. If set to false, Nix will use the modified
     * lock file in memory only, without writing it to disk.
     */
    bool writeLockFile = true;

    /**
     * Throw an exception when the flake has an unlocked input.
     */
    bool failOnUnlocked = false;

    /**
     * Whether to use the registries to lookup indirect flake
     * references like 'nixpkgs'.
     */
    std::optional<bool> useRegistries = std::nullopt;

    /**
     * Whether to apply flake's nixConfig attribute to the configuration
     */

    bool applyNixConfig = false;

    /**
     * Whether unlocked flake references (i.e. those without a Git
     * revision or similar) without a corresponding lock are
     * allowed. Unlocked flake references with a lock are always
     * allowed.
     */
    bool allowUnlocked = true;

    /**
     * Whether to commit changes to flake.lock.
     */
    bool commitLockFile = false;

    /**
     * The path to a lock file to read instead of the `flake.lock` file in the top-level flake
     */
    std::optional<SourcePath> referenceLockFilePath;

    /**
     * The path to a lock file to write to instead of the `flake.lock` file in the top-level flake
     */
    std::optional<std::filesystem::path> outputLockFilePath;

    /**
     * Flake inputs to be overridden.
     */
    std::map<NonEmptyInputAttrPath, FlakeRef> inputOverrides;

    /**
     * Flake inputs to be updated. This means that any existing lock
     * for those inputs will be ignored.
     */
    std::set<NonEmptyInputAttrPath> inputUpdates;
};

/*
 * Compute an in-memory lock file for the specified top-level flake, and optionally write it to file, if the flake is
 * writable.
 */
LockedFlake
lockFlake(const Settings & settings, EvalState & state, const FlakeRef & flakeRef, const LockFlags & lockFlags);

LockedFlake
lockFlake(const Settings & settings, EvalState & state, const SourcePath & flakeDir, const LockFlags & lockFlags);

void callFlake(EvalState & state, const LockedFlake & lockedFlake, Value & v);

/**
 * Validate structural integrity of a resolved flake graph.
 *
 * Checks: root key exists, all parentKey/sourceInfoKey/resolvedInputs
 * references resolve, evaluationRoot within carrierRoot, relativePath
 * consistent, inputSpecs↔resolvedInputs bijection.  Throws on failure.
 *
 * Exposed for unit testing — production code calls this internally
 * from buildResolvedFlakeGraph.
 */
void validateResolvedFlakeGraph(const ResolvedFlakeGraph & graph);

/**
 * Open an eval trace (BSàlC: verifying trace) for a flake.
 */
ref<eval_trace::TraceSession> openTraceCache(EvalState & state, ref<const LockedFlake> lockedFlake);

} // namespace flake

/**
 * An internal builtin similar to `fetchTree`, except that it
 * always treats the input as final (i.e. no attributes can be
 * added/removed/changed).
 */
void prim_fetchFinalTree(EvalState & state, const PosIdx pos, Value ** args, Value & v);

} // namespace nix
