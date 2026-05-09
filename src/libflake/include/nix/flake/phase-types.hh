#pragma once
///@file

#include "nix/expr/eval-environment/domains.hh"
#include "nix/util/tagged.hh"
#include "nix/util/hash.hh"
#include "nix/flake/flakeref.hh"

#include <string_view>

namespace nix::flake {

using OriginalFlakeRef = Tagged<struct OriginalFlakeRefTag_, FlakeRef>;
using ResolvedFlakeRef = Tagged<struct ResolvedFlakeRefTag_, FlakeRef>;
using EvaluationLockedFlakeRef = Tagged<struct EvaluationLockedFlakeRefTag_, FlakeRef>;
using PersistedLockFileFlakeRef = Tagged<struct PersistedLockFileFlakeRefTag_, FlakeRef>;

using LogicalFlakeRootPath = Tagged<struct LogicalFlakeRootPathTag_, SourcePath>;
using CarrierRootPath = Tagged<struct CarrierRootPathTag_, SourcePath>;
using ParseFlakeRootPath = Tagged<struct ParseFlakeRootPathTag_, SourcePath>;
using DisplayFlakeRootPath = Tagged<struct DisplayFlakeRootPathTag_, SourcePath>;
using EvaluationFlakeRootPath = Tagged<struct EvaluationFlakeRootPathTag_, SourcePath>;
using LogicalFlakeRelativePath = Tagged<struct LogicalFlakeRelativePathTag_, std::string>;
using LockedVersionIdentity = Tagged<struct LockedVersionIdentityTag_, EvalTraceHash>;
// Resolved graph node keys stay typed through the graph/adapter boundary.
// Only lockfile serialization and external CLI consumers unwrap to the raw
// `FlakeGraphNodeKey` namespace.
using ResolvedFlakeGraphNodeKey = Tagged<struct ResolvedFlakeGraphNodeKeyTag_, FlakeGraphNodeKey>;
using ResolvedFlakeGraphRootKey = Tagged<struct ResolvedFlakeGraphRootKeyTag_, FlakeGraphNodeKey>;

inline ResolvedFlakeGraphNodeKey makeResolvedFlakeGraphNodeKey(const FlakeGraphNodeKey & key)
{
    return ResolvedFlakeGraphNodeKey{key};
}

inline ResolvedFlakeGraphRootKey makeResolvedFlakeGraphRootKey(const FlakeGraphNodeKey & key)
{
    return ResolvedFlakeGraphRootKey{key};
}

inline const FlakeGraphNodeKey & asFlakeGraphNodeKey(const ResolvedFlakeGraphNodeKey & key)
{
    return key.value;
}

inline const FlakeGraphNodeKey & asFlakeGraphNodeKey(const ResolvedFlakeGraphRootKey & key)
{
    return key.value;
}

inline LogicalFlakeRootPath makeLogicalFlakeRoot(const SourcePath & root, std::string_view subdir = {})
{
    return subdir.empty()
        ? LogicalFlakeRootPath{root}
        : LogicalFlakeRootPath{root / CanonPath(subdir)};
}

inline CarrierRootPath makeCarrierRoot(const SourcePath & root)
{
    return CarrierRootPath{root};
}

inline ParseFlakeRootPath makeParseFlakeRoot(const SourcePath & root, std::string_view subdir = {})
{
    return subdir.empty()
        ? ParseFlakeRootPath{root}
        : ParseFlakeRootPath{root / CanonPath(subdir)};
}

inline DisplayFlakeRootPath makeDisplayFlakeRoot(const SourcePath & root, std::string_view subdir = {})
{
    return subdir.empty()
        ? DisplayFlakeRootPath{root}
        : DisplayFlakeRootPath{root / CanonPath(subdir)};
}

inline SourcePath parseRootToFlakeNixPath(const ParseFlakeRootPath & root)
{
    return root.value / "flake.nix";
}

inline EvaluationFlakeRootPath makeEvaluationFlakeRoot(const SourcePath & root, std::string_view subdir = {})
{
    return subdir.empty()
        ? EvaluationFlakeRootPath{root}
        : EvaluationFlakeRootPath{root / CanonPath(subdir)};
}

inline SourcePath logicalRootToLockFilePath(const LogicalFlakeRootPath & root)
{
    return root.value / "flake.lock";
}

inline EvaluationFlakeRootPath appendEvaluationFlakeRelativePath(
    const EvaluationFlakeRootPath & root,
    std::string_view relativePath)
{
    return EvaluationFlakeRootPath{
        SourcePath{
            root.value.accessor,
            relativePath.empty()
                ? root.value.path
                : CanonPath(relativePath, root.value.path),
        }};
}

inline LogicalFlakeRelativePath makeLogicalFlakeRelativePath(
    const CarrierRootPath & carrierRoot,
    const EvaluationFlakeRootPath & evaluationRoot)
{
    return evaluationRoot.value.path == carrierRoot.value.path
        ? LogicalFlakeRelativePath{std::string{}}
        : LogicalFlakeRelativePath{carrierRoot.value.path.makeRelative(evaluationRoot.value.path)};
}

} // namespace nix::flake
