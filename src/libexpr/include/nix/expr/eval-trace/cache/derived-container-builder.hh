#pragma once
///@file

#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/value.hh"

#include <optional>

namespace nix::eval_trace {

/// Output container kind for DerivedContainerBuilder.  The kind is fixed
/// at the call site and gates which `finish*` method is reachable: the
/// shape-kind mismatch (list output from attrset source, or vice versa)
/// that the historical ShapePreservingReview sentinel could only catch at
/// runtime via assert is now a `requires`-clause failure at compile time.
enum class ContainerKind : std::uint8_t { Attrs, List };

/**
 * Shared traced-container propagation helper for **shape-preserving**
 * container operations.  It tracks a single reproducible structured-container
 * provenance across the source operands and re-registers it on the derived
 * output when that origin remains representable as a StructuredObject.
 *
 * IMPORTANT — CORRECT USE REQUIRES UNDERSTANDING THREE THINGS:
 *
 * 1. SHAPE-PRESERVING REQUIREMENT
 *    Only use this for operations where:
 *      - the output's container type matches the source's container type
 *        (no list-from-attrset or attrset-from-list conversions), AND
 *      - for list outputs: output.len ≤ source.len
 *      - for attrset outputs: output.keys ⊆ source.keys
 *
 *    Correct examples: removeAttrs, intersectAttrs, mapAttrs, filter, sort,
 *    partition sublists, groupBy sublists.
 *    Do NOT use for: concatLists, attrValues, catAttrs, ExprOpUpdate (//) —
 *    see existing comments at those sites.  ExprOpUpdate is a set UNION:
 *    two disjoint subsets of the same traced container can reunite into an
 *    output larger than either source, violating the subset requirement.
 *
 * 2. SUBSUMPTION DEPENDENCY
 *    Safety on the CurrentTraceDep path relies on Content-dep subsumption.
 *    When the source file is unchanged, resolveDepHash<CurrentTraceDep>
 *    calls session.isFileVerified() and returns the stored dep hash
 *    directly without recomputing.  The stored hash is hash(output.len)
 *    or hash(output.keys) — NOT the source's current shape.  The stored
 *    hash compares against itself and always passes.  It is also written
 *    to L1 as VerifiedHash under the VerifiedSubsumption capability.
 *
 *    Only when the Content dep changes does recomputation occur, and at that
 *    point full re-evaluation is triggered regardless.
 *
 *    WITHOUT subsumption, a shape dep on a filtered output (output.len !=
 *    source.len) would fail verification even when the source file did not
 *    change.  Correctness depends on subsumption being in effect on the
 *    CurrentTraceDep path.
 *
 *    See also: maybeRecordListLenDep and maybeRecordAttrKeysDep in
 *    shape-recording.hh, which record the shape deps this builder enables.
 *
 * 3. STRUCTURAL VARIANT RECOVERY
 *    Structural variant recovery (tryStructuralVariantRecovery in
 *    verifier.cc) normally loads each candidate group's dep keys
 *    via loadKeySet and iterates them as CandidateDep.  CandidateDep
 *    intentionally does NOT take the subsumption shortcut inside
 *    resolveDepHash — for every dep (Structural or otherwise), the
 *    compute path runs and produces `hash(op(current F))`.  This keeps
 *    `computePresortedTraceHash(repDeps)` honest: a candidate matches a
 *    history entry iff its evaluation against the CURRENT filesystem
 *    would reproduce that entry's recorded trace hash.
 *
 *    Historical-hash L1 poisoning is unrepresentable.  Compute-path
 *    writes (`cacheComputedHash`) are unconditional and sound — they
 *    store `hash(op(current F))` for any origin.  Subsumption-path
 *    writes (`cacheVerifiedHash`) require the `VerifiedSubsumption`
 *    witness, whose factory `grantVerifiedSubsumption<O>` is SFINAE-
 *    restricted to `CurrentTrace`; a `CandidateDep` cannot mint the
 *    witness, and (post-F1 fix) never even reaches the subsumption
 *    branch.
 *
 *    When the source file is unchanged (structural op applied to
 *    current F yields the candidate's recorded output shape):
 *      - resolveDepHash<CandidateDep> computes hash(op(current F))
 *      - This equals the candidate's stored dep.hash
 *      - Candidate trace hash matches history -> recovery succeeds
 *
 *    When the source file changed (op applied to current F yields a
 *    different output shape):
 *      - resolveDepHash<CandidateDep> computes hash(op(current F))
 *      - This does NOT equal the candidate's stored dep.hash
 *      - Candidate trace hash does not match history -> recovery correctly fails
 *
 *    Affected: filter, removeAttrs, intersectAttrs, partition sublists,
 *    groupBy sublists (when output shape != source shape).
 *    NOT affected: sort, mapAttrs (output shape == source shape).
 *
 * When the sources disagree on container provenance, the builder leaves the
 * output container unregistered rather than synthesizing a lossy merged
 * origin. Attrset child-level origins still flow through Attr::pos in that
 * case.
 *
 * Parameterized by `ContainerKind`: only `finishList` is reachable on
 * `DerivedContainerBuilder<ContainerKind::List>`, and only `finishAttrs`
 * on `DerivedContainerBuilder<ContainerKind::Attrs>`.  Calling the wrong
 * `finish*` method is a compile error, not a debug-only runtime assert.
 */
template<ContainerKind K>
class DerivedContainerBuilder
{
    std::optional<TraceAccess> access;
    std::optional<TracedContainerProvenance> uniqueProvenance;
    bool conflictingProvenance = false;
    const Value * uniqueSourcePtr = nullptr;

    void adopt(const TracedContainerProvenance & provenance, const Value * srcPtr)
    {
        if (conflictingProvenance)
            return;

        if (!uniqueProvenance) {
            uniqueProvenance = provenance;
            uniqueSourcePtr = srcPtr;
            return;
        }

        if (uniqueProvenance->sourceId == provenance.sourceId
            && uniqueProvenance->filePathId == provenance.filePathId
            && uniqueProvenance->dataPathId == provenance.dataPathId
            && uniqueProvenance->format == provenance.format)
            return;

        uniqueProvenance.reset();
        uniqueSourcePtr = nullptr;
        conflictingProvenance = true;
    }

public:
    DerivedContainerBuilder()
        : access(TraceAccess::current())
    {
    }

    void addShapePreservingSource(const Value & source)
    {
        if (!access)
            return;

        if (auto * provenance = access->lookupTracedContainer(&source))
            adopt(*provenance, &source);
    }

    bool willRegisterContainer() const noexcept
    {
        return access && !conflictingProvenance && uniqueProvenance.has_value();
    }

    void registerContainer(Value & output) const
    {
        if (!access || conflictingProvenance || !uniqueProvenance)
            return;

        auto * provenance = access->allocateProvenance(
            uniqueProvenance->sourceId,
            uniqueProvenance->filePathId,
            uniqueProvenance->dataPathId,
            uniqueProvenance->format);
        access->registerTracedContainer(&output, provenance);
    }

    void finishList(EvalMemory & mem, Value & output, const ListBuilder & list) const
        requires (K == ContainerKind::List)
    {
        if (willRegisterContainer() && !list.hasHeapStorage()) {
            auto stableList = ListBuilder(mem, list.listSize(), true);
            for (size_t i = 0; i < list.listSize(); ++i)
                stableList[i] = list[i];
            output.mkList(stableList);
        } else {
            output.mkList(list);
        }
        // Runtime shape check (debug only — zero cost under NDEBUG).
        // Catches oversized-output: output.len > source.len
        // (concatLists pattern).  The shape-kind mismatch case (list
        // output from attrset source) is now a compile error via the
        // requires clause, so the kind branch here is `nList` only.
        //
        // Does NOT fire on alias paths (prim_filter's same==true path, prim_sort's
        // len==0 path) because finishList is not called on those paths — the
        // builder is abandoned and the bitwise copy v = *args[1] preserves
        // storage-identity provenance lookup automatically.
        //
        // For prim_partition and prim_groupBy, finishList is called multiple
        // times on the same builder (once per sublist output).  Each call is
        // independent: a separate ListBuilder and Value & are passed.  The
        // assertion is correct for each individual call because each sublist
        // must independently satisfy output.len <= source.len.
        //
        // Checked after mkList because ListBuilder::size is private; we use
        // output.listSize() instead.
        if (uniqueSourcePtr && !uniqueSourcePtr->isThunk()
            && uniqueSourcePtr->type() == nList)
        {
            assert(output.listSize() <= uniqueSourcePtr->listSize() &&
                   "DerivedContainerBuilder<List>: output list longer than source — "
                   "shape not preserved (concatLists pattern).");
        }
        registerContainer(output);
    }

    void finishAttrs(Value & output, Bindings * attrs) const noexcept
        requires (K == ContainerKind::Attrs)
    {
        // Runtime shape check (debug only).  noexcept is preserved — assert
        // calls abort(), which does not throw.
        if (uniqueSourcePtr && !uniqueSourcePtr->isThunk()
            && uniqueSourcePtr->type() == nAttrs)
        {
            assert(attrs->size() <= uniqueSourcePtr->attrs()->size() &&
                   "DerivedContainerBuilder<Attrs>: output attrset larger than source — "
                   "possible set-union misuse.");
        }
        output.mkAttrs(attrs);
        registerContainer(output);
    }
};

using DerivedAttrsBuilder = DerivedContainerBuilder<ContainerKind::Attrs>;
using DerivedListBuilder = DerivedContainerBuilder<ContainerKind::List>;

} // namespace nix::eval_trace
