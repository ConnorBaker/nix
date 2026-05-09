#pragma once
///@file
/// SemanticRegistry: dep path resolution for verification and recording.
///
/// Replaces InputResolver/FrozenInputResolver and
/// SessionMountTable/DynamicMountTable/MountResolutionGate.
///
/// Forward index (entries_): DepSource → SourcePath for verification.
///   resolveCurrentDepHash calls resolve(source, key) to read the current
///   file content for hash comparison.
///
/// Reverse index (mountPoints_): CanonPath → [(DepSource, subdir)]
///   for recording-side provenance resolution. resolveDepPathKey calls
///   reverseResolve(path) to map a store path back to its DepSource.
///
/// CRITICAL INVARIANT: Both indexes must use the same identity namespace.
/// The forward entries and mount points are both populated from
/// ResolvedFlakeGraph node keys (via DepSource::fromNodeKey). This ensures
/// that deps recorded via reverseResolve (which reads mountPoints_) produce
/// DepSource values that resolve() (which reads entries_) can find.
///
/// Runtime roots (non-graph fetchTree inputs) use a separate identity
/// namespace (DepSource::fromRuntimeRoot with canonical runtime-fetch
/// identities). These are added to entries_ via addEntry() at session open
/// from SessionRuntimeRoots, and their mount points are added dynamically
/// during cold eval via TraceSession::registerRuntimeRootMount() from the
/// environment-owned fetch completion path.
///
/// The two namespaces are disjoint by construction: graph node keys are
/// short names like "root", "nixpkgs", "sub0"; runtime root identities are
/// canonical hashed source keys derived from locked fetch attrs.
/// DepSource::fromNodeKey and DepSource::fromRuntimeRoot enforce this at the
/// call site level.
///
/// ACCESS CONTROL: addEntry() and addMountPoint() are private, accessible
/// only to TraceSession (session-open population and dynamic runtime-root
/// registration during cold eval).
/// SemanticRegistryTestAccess provides test-only bypass and lives in
/// src/libexpr-tests/eval-trace/semantic-registry-test-access.hh — it
/// must not be included from production sources.

#include "nix/expr/eval-trace/semantic-objects.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nix { struct TraceRuntime; }

namespace nix::eval_trace {

class SemanticRegistry
{
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> entries_;
    boost::unordered_flat_map<CanonPath, std::vector<std::pair<DepSource, RegistryMountSubdir>>, std::hash<CanonPath>> mountPoints_;

    /// Mutation is restricted to session-open (TraceSession) and
    /// runtime-root registration (TraceRuntime).  DepCaptureScope takes
    /// const SemanticRegistry & — the registry is effectively immutable
    /// from the recording/verification perspective.
    friend class TraceSession;
    friend struct ::nix::TraceRuntime;
    friend struct SemanticRegistryTestAccess;

    void addEntry(const DepSource & source, SourcePath path)
    {
        entries_.insert_or_assign(source, std::move(path));
    }

    void addMountPoint(CanonPath mountPoint, DepSource source, RegistryMountSubdir subdir)
    {
        // piecewise_construct avoids libstdc++'s
        // __is_implicitly_default_constructible_v<pair<..., Tagged<_,
        // CanonPath>>> probe fired by the two-arg emplace_back path.
        // The probe instantiates the pair's default ctor, which
        // hard-errors because Tagged's default member initializer
        // `T{}` tries to default-construct CanonPath.
        mountPoints_[std::move(mountPoint)].emplace_back(
            std::piecewise_construct,
            std::forward_as_tuple(std::move(source)),
            std::forward_as_tuple(std::move(subdir)));
    }

public:
    SemanticRegistry() = default;

    explicit SemanticRegistry(
        boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> entries)
        : entries_(std::move(entries))
    {}

    /// Forward: resolve (source, key) → SourcePath.
    std::optional<SourcePath> resolve(
        const DepSource & source, const std::string & key) const;

    /// Reverse: resolve absPath → (DepSource, relKey) by walking mount points.
    std::optional<std::pair<DepSource, CanonPath>> reverseResolve(
        const CanonPath & absPath) const;

    /// Reverse: resolve path → PathObject with typed source identity.
    std::optional<PathObject> resolvePathObject(const SourcePath & path) const;

    bool contains(const DepSource & source) const { return entries_.contains(source); }
    size_t size() const { return entries_.size(); }
    size_t mountPointCount() const { return mountPoints_.size(); }
    const auto & entries() const { return entries_; }
};

} // namespace nix::eval_trace
