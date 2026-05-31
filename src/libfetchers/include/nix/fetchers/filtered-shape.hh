#pragma once
///@file
///
/// Internal helper exposed for testing.
///
/// `collectFilteredShape` is the walker that powers the filtered-
/// source cache (§5 of doc/tecnix-survey/PROPOSAL.md). It mirrors
/// `SourceAccessor::dumpPath`'s structural traversal but accumulates
/// a content-free shape digest plus the accepted-path set instead of
/// emitting NAR bytes. The user filter is consulted on each child;
/// the root is always included.
///
/// Exposed here so unit tests can exercise the walker directly —
/// notably the symlink-stops-at-target invariant — without going
/// through the full `fetchToStore2` pipeline. The production caller
/// is `fetchToStore2` in `fetch-to-store.cc`; everyone else should
/// stay away from this.

#include "nix/util/canon-path.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"

#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

struct FilteredShape
{
    Hash shapeHash;
    boost::unordered_flat_set<CanonPath> accepted;
};

/** Walk `accessor` from `root`, calling `filter` on each child entry.
 *  Returns the digest of the accepted shape plus the absolute
 *  accepted-path set. Root is always included; symlinks are recorded
 *  by target string and NOT recursed through. */
FilteredShape collectFilteredShape(SourceAccessor & accessor, const CanonPath & root, PathFilter & filter);

} // namespace nix
