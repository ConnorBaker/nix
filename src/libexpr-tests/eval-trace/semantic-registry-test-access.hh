#pragma once
///@file
/// Test-only access to SemanticRegistry mutation methods.
///
/// Production code must populate a SemanticRegistry through TraceSession
/// (session-open) or TraceRuntime (dynamic runtime-root registration).
/// This header exposes SemanticRegistryTestAccess — a friend struct that
/// bypasses the private addEntry/addMountPoint access control — and must
/// never be included from production sources.

#include "nix/expr/eval-trace/store/semantic-registry.hh"

namespace nix::eval_trace {

/// Test-only access to SemanticRegistry mutation methods.
/// Production code must use TraceSession or TraceRuntime.
struct SemanticRegistryTestAccess {
    static void addEntry(SemanticRegistry & r, const DepSource & source, SourcePath path)
    {
        r.addEntry(source, std::move(path));
    }
    static void addMountPoint(SemanticRegistry & r, CanonPath mp, DepSource source, std::string subdir)
    {
        r.addMountPoint(
            std::move(mp),
            std::move(source),
            RegistryMountSubdir{subdir.empty() ? CanonPath::root : CanonPath("/" + subdir)});
    }
};

} // namespace nix::eval_trace
