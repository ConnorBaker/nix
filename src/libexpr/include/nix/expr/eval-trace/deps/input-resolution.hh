#pragma once
///@file
/// Input resolution and file provenance tracking for eval-trace dep recording.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/canon-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nix {

struct InterningPools;
struct Value;
class EvalState;

/**
 * Resolve an absolute path to an (inputName, relativePath) pair using
 * a mount-point-to-input mapping. Walks up the path trying each prefix.
 */
std::optional<std::pair<std::string, CanonPath>> resolveToInput(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

/**
 * Record a file dependency, resolving to an input-relative path if possible.
 * In non-flake mode (mountToInput empty), records absolute paths with
 * inputName="". In flake mode, paths that can't be resolved to any input
 * are recorded with inputName="<absolute>" so they are validated directly
 * against the real filesystem.
 */
void recordDep(
    InterningPools & pools,
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput,
    std::string_view storeName = {});

/**
 * Provenance information from a readFile call, used to connect
 * fromJSON/fromTOML to the file that was read. Stored in
 * RootTrackerScope::readFileProvenanceMap (Lifetime 2), set by
 * prim_readFile and consumed by prim_fromJSON/prim_fromTOML.
 */
struct ReadFileProvenance {
    CanonPath path;
    Blake3Hash contentHash;
};


/**
 * Record a RawContent dep if the string value came from readFile.
 * Checks the readFileStringPtrs map by pointer identity (O(1)).
 * Called by string builtins that observe raw bytes (stringLength,
 * hashString, substring, match, split, replaceStrings) and eqValues.
 * No-op if dep tracking is inactive or the string didn't come from readFile.
 */
[[gnu::cold]] void maybeRecordRawContentDep(EvalState & state, const Value & v);

/**
 * Resolve an absolute path to a (source, key) pair for dep recording,
 * using the same resolution logic as recordDep. Helper for provenance
 * consumers that need to construct dep keys.
 */
std::pair<std::string, std::string> resolveProvenance(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

} // namespace nix
