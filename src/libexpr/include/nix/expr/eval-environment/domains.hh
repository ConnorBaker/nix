#pragma once

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/tagged.hh"

#include <memory>
#include <ostream>
#include <string>

namespace nix {

using FlakeSourceIdentity = Tagged<struct FlakeSourceIdentityTag_, EvalTraceHash>;
using StableRecoveryKey = Tagged<struct StableRecoveryKeyTag_, EvalTraceHash>;
using ExternalRootIdentity = Tagged<struct ExternalRootIdentityTag_, CanonPath>;
using FileEvalAbsoluteFilePath = Tagged<struct FileEvalAbsoluteFilePathTag_, CanonPath>;
using FileEvalExpressionHash = Tagged<struct FileEvalExpressionHashTag_, EvalTraceHash>;
using FileEvalLogicalSourceIdentity = Tagged<struct FileEvalLogicalSourceIdentityTag_, EvalTraceHash>;
using FileEvalAutoArgsHash = Tagged<struct FileEvalAutoArgsHashTag_, EvalTraceHash>;
using StoreDirIdentity = Tagged<struct StoreDirIdentityTag_, CanonPath>;
using SessionCurrentSystem = Tagged<struct SessionCurrentSystemTag_, std::string>;

using FlakeGraphNodeKey = Tagged<struct FlakeGraphNodeKeyTag_, std::string>;
using FlakeInputSubdir = Tagged<struct FlakeInputSubdirTag_, std::string>;

inline std::ostream & operator<<(std::ostream & out, const FlakeGraphNodeKey & key)
{
    return out << key.value;
}

using LookupPathPrefix = Tagged<struct LookupPathPrefixTag_, std::string>;
using LookupPathRawValue = Tagged<struct LookupPathRawValueTag_, std::string>;

using FlakeSessionReuseKey = Tagged<struct FlakeSessionReuseKeyTag_, EvalTraceHash>;
using FileEvalSessionReuseKeyValue = Tagged<struct FileEvalSessionReuseKeyValueTag_, EvalTraceHash>;

namespace fetchers { struct Input; }

/// Original input before resolution — as specified in the flake or fetchTree call.
/// Used for computing runtime fetch identity keys on unlocked inputs.
using OriginalFetchInput = Tagged<struct OriginalFetchInputTag_,
    std::shared_ptr<const fetchers::Input>>;

/// Locked input before finalization — no narHash/__final attrs.
/// Produced by resolveFetchIdentity, consumed by mount.
using ResolvedLockedInput = Tagged<struct ResolvedLockedInputTag_,
    std::shared_ptr<const fetchers::Input>>;

/// Locked input after finalization — has narHash + __final attrs.
/// Produced by mountInput, carried through completion.
using FinalizedLockedInput = Tagged<struct FinalizedLockedInputTag_,
    std::shared_ptr<const fetchers::Input>>;

} // namespace nix
