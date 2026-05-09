#pragma once
///@file
/// Dependency recording — provenance helpers and re-exports.
///
/// The primary recording interface is DepRecordingContext (dep-recording-context.hh).
/// This header provides provenance allocation helpers and structured dep recording.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/position.hh"

#include <vector>

namespace nix {

/**
 * Allocate a provenance record and return a Pos::ProvenanceRef for use
 * in PosTable origins.
 */
Pos::ProvenanceRef allocateProvenanceRef(
    InterningPools & pools, DepSourceId srcId, FilePathId fpId, DataPathId dpId, StructuredFormat format);

/**
 * Resolve a Pos::ProvenanceRef to its full ProvenanceRecord.
 */
const ProvenanceRecord & resolveProvenanceRef(InterningPools & pools, const Pos::ProvenanceRef & ref);

} // namespace nix
