#pragma once
///@file
/// Input resolution and file provenance tracking for eval-trace dep recording.

#include "nix/expr/eval-trace/semantic-objects.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tagged.hh"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nix {

struct InterningPools;
struct Value;
class EvalState;

namespace eval_trace { class SemanticRegistry; }
namespace eval_trace { struct TraceAccess; }
namespace eval_trace { class CanonicalHashBuilder; }

using SimpleDepKeyAtom = Tagged<struct SimpleDepKeyAtomTag_, std::string>;

/**
 * Record a file dependency. Resolves provenance via the SemanticRegistry
 * on the current DepCaptureScope.
 * No-op if no DepCaptureScope is active (e.g., during lockFlake).
 */
void recordDep(
    const eval_trace::TraceAccess & access,
    const SourcePath & path,
    const DepHashValue & hash,
    CanonicalQueryKind depType,
    const std::optional<PathObject> & origin = std::nullopt,
    SimpleDepKeyAtom storeName = {});

/**
 * Record a RawContent dep if the string value came from readFile.
 */
[[gnu::cold]] void maybeRecordRawContentDep(const eval_trace::TraceAccess & access, const Value & v);

/**
 * Record a FileBytes dep without reading the file a second time.
 *
 * Resolves provenance via the SemanticRegistry and records a FileBytes
 * dep whose hash comes from EvalEnvironmentSharedState::fileContentHashCache.
 * On cache miss, reads and hashes the file once and populates the cache.
 *
 * Applies the scope-dedup fast path: if the current scope's seenDeps
 * already contains this dep key, returns early without touching the
 * cache or the filesystem.
 *
 * Preserves recordDep's `maybeLstat` guard for absolute-source deps:
 * a FileBytes dep keyed on a missing absolute path is dropped rather
 * than recorded with a bogus hash.
 *
 * No-op if no DepCaptureScope is active (mirrors recordDep).
 */
void recordFileBytesDepViaCache(
    const eval_trace::TraceAccess & access,
    EvalState & state,
    const SourcePath & path,
    const std::optional<PathObject> & origin);

/**
 * Resolved dep path: source + relative key within that source.
 */
struct ResolvedDepPath {
    DepSource source;
    std::string key;
};

struct DerivedStorePathDepKey
{
    CanonPath pathKey;
    /// Typed store-name atom for the derived path (not a raw string).
    SimpleDepKeyAtom storeName;
};

struct StorePathAvailabilityDepKey
{
    StorePath storePath;
};

struct RuntimeFetchIdentityDepKey
{
    fetchers::Attrs inputAttrs;
};

RuntimeRootSourceKey makeRuntimeRootSourceKey(const RuntimeFetchIdentityDepKey & key);
std::string renderRuntimeFetchIdentityDisplay(const RuntimeFetchIdentityDepKey & key);

/**
 * Resolve an absolute path to a (source, key) pair for dep recording.
 */
ResolvedDepPath resolveDepPathKey(
    const SourcePath & path,
    const eval_trace::SemanticRegistry & registry,
    const std::optional<PathObject> & origin = std::nullopt);

/**
 * Resolve provenance via the SemanticRegistry reached through the given
 * TraceAccess. Returns nullopt if no registry is active.
 */
std::optional<ResolvedDepPath> resolveProvenanceViaRegistry(
    const eval_trace::TraceAccess & access,
    const SourcePath & path,
    const std::optional<PathObject> & origin = std::nullopt);

/**
 * Resolve a path to a logical runtime/flake root for value-origin propagation.
 * Returns nullopt for absolute paths outside the semantic registry.
 *
 * Two overloads:
 *  - Access-taking: the primary entry point, used from every call site that
 *    already holds a `TraceAccess`.
 *  - No-access: kept for `EvalState::callPathFilter` (see `primops.cc`), which
 *    has no `TraceAccess` in scope and is reached through recursive primop
 *    dispatch where threading an access parameter would require signature
 *    changes across the filter API.  The body uses the current fiber's
 *    standalone dep-context fallback to obtain a registry.
 */
std::optional<PathObject> resolvePathObjectViaRegistry(const SourcePath & path);

std::optional<PathObject> resolvePathObjectViaRegistry(
    const eval_trace::TraceAccess & access,
    const SourcePath & path);

DerivedStorePathDepKey decodeDerivedStorePathDepKey(const InterningPools & pools, DerivedStorePathDepKeyId keyId);
StorePathAvailabilityDepKey decodeStorePathAvailabilityDepKey(const InterningPools & pools, StorePathAvailabilityDepKeyId keyId);
RuntimeFetchIdentityDepKey decodeRuntimeFetchIdentityDepKey(const InterningPools & pools, RuntimeFetchIdentityDepKeyId keyId);
std::optional<fetchers::Input> makeRuntimeFetchIdentityInput(
    const fetchers::Settings & settings,
    const RuntimeFetchIdentityDepKey & key);

/// Feed the canonical non-trace-context dep-key material into a framed hash builder.
/// Keeps typed dep-key structure alive until the actual hashing boundary
/// instead of reaching for the raw encoded blob substrate.
void feedCanonicalDepKeyMaterial(
    eval_trace::CanonicalHashBuilder & builder,
    const InterningPools & pools,
    const Dep::Key & key);

std::string renderSimpleDepKeyDisplay(
    const InterningPools & pools,
    const Dep::Key & key);

} // namespace nix
