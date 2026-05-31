/* pathSet:: closure predicates — the oracle the operator suites trust.
 *
 * `ancestorOfMember`, `memberOrDescendantOfMember`, and `inClosure`
 * (declared in filtering-source-accessor.hh, defined in
 * filtering-source-accessor.cc) are used as the EXPECTED-VALUE oracle in
 * the Restrict / Mask / SoundnessGuard / DirectorySynthesizer PROPs — but
 * were never themselves directly validated. A bug in the optimised
 * `ancestorOfMember` (it uses a `lower_bound` trick rather than a scan)
 * would silently corrupt every test that trusts it.
 *
 * Strategy: validate each optimised predicate against a brute-force
 * O(n) reference (the gold standard for an optimised predicate), then
 * pin the closure-operator axioms (extensive, monotone, idempotent —
 * https://en.wikipedia.org/wiki/Closure_operator) that the docs claim
 * for the admit/deny closures `cl_admit(S)=S∪anc(S)` and
 * `cl_deny(W)=W∪desc(W)`. */

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/tests/source-accessor.hh"

namespace nix {

namespace {

/* ---- brute-force O(n) reference implementations (the spec) ---- */

/* p is a STRICT ancestor of some member s ∈ S: s is p-or-below and s ≠ p,
   i.e. s.isWithin(p) && s != p. */
bool refAncestorOfMember(const std::set<CanonPath> & s, const CanonPath & p)
{
    for (auto & m : s)
        if (m != p && m.isWithin(p))
            return true;
    return false;
}

/* p ∈ S or p is a descendant of some member: ∃ w ∈ S . p.isWithin(w).
   (p.isWithin(w) covers both p == w and p strictly below w.) */
bool refMemberOrDescendantOfMember(const std::set<CanonPath> & s, const CanonPath & p)
{
    for (auto & w : s)
        if (p.isWithin(w))
            return true;
    return false;
}

bool refInClosure(const std::set<CanonPath> & s, const CanonPath & p)
{
    return refMemberOrDescendantOfMember(s, p) || refAncestorOfMember(s, p);
}

/* All strict ancestors of `p` up to and including root. */
std::set<CanonPath> ancestorsOf(const CanonPath & p)
{
    std::set<CanonPath> out;
    auto cur = p;
    while (!cur.isRoot()) {
        auto parent = cur.parent();
        if (!parent)
            break;
        cur = *parent;
        out.insert(cur);
    }
    return out;
}

/* The admit-closure as a concrete set: cl_admit(S) = S ∪ anc(S).
   Finite (ancestors walk up to root), so idempotence is testable as a
   set equality without an infinite universe. */
std::set<CanonPath> admitClosureSet(const std::set<CanonPath> & s)
{
    std::set<CanonPath> a;
    for (auto & m : s) {
        a.insert(m);
        for (auto & anc : ancestorsOf(m))
            a.insert(anc);
    }
    return a;
}

} // namespace

#ifndef COVERAGE

/* The headline: each optimised predicate equals its brute-force spec,
   for arbitrary (S, p). This is what makes the predicates trustworthy as
   the operator-suite oracle, and directly exercises the `lower_bound`
   optimisation in `ancestorOfMember`. */
RC_GTEST_PROP(PathSet, AncestorOfMemberMatchesReference, (const std::set<CanonPath> & s, const CanonPath & p))
{
    RC_ASSERT(pathSet::ancestorOfMember(s, p) == refAncestorOfMember(s, p));
}

RC_GTEST_PROP(PathSet, MemberOrDescendantMatchesReference, (const std::set<CanonPath> & s, const CanonPath & p))
{
    RC_ASSERT(pathSet::memberOrDescendantOfMember(s, p) == refMemberOrDescendantOfMember(s, p));
}

RC_GTEST_PROP(PathSet, InClosureMatchesReference, (const std::set<CanonPath> & s, const CanonPath & p))
{
    RC_ASSERT(pathSet::inClosure(s, p) == refInClosure(s, p));
}

/* inClosure is exactly the disjunction of the two halves (the definition;
   pins that the impl wires them together, not just that each half is
   right). */
RC_GTEST_PROP(PathSet, InClosureIsDisjunctionOfHalves, (const std::set<CanonPath> & s, const CanonPath & p))
{
    RC_ASSERT(
        pathSet::inClosure(s, p) == (pathSet::memberOrDescendantOfMember(s, p) || pathSet::ancestorOfMember(s, p)));
}

/* Closure axiom — EXTENSIVE: every member of S is in closure(S). */
RC_GTEST_PROP(PathSet, ClosureExtensive, (const std::set<CanonPath> & s))
{
    for (auto & m : s) {
        RC_ASSERT(pathSet::inClosure(s, m));
        /* A member is in BOTH the admit closure (as a member) and the
           deny closure (as a member). */
        RC_ASSERT(pathSet::memberOrDescendantOfMember(s, m));
    }
}

/* Closure axiom — MONOTONE: S ⊆ T ⇒ closure(S) ⊆ closure(T), pointwise.
   We build T = S ∪ extra so the subset relation holds by construction. */
RC_GTEST_PROP(
    PathSet, ClosureMonotone, (const std::set<CanonPath> & s, const std::set<CanonPath> & extra, const CanonPath & p))
{
    std::set<CanonPath> t = s;
    t.insert(extra.begin(), extra.end());

    if (pathSet::inClosure(s, p))
        RC_ASSERT(pathSet::inClosure(t, p));
    if (pathSet::memberOrDescendantOfMember(s, p))
        RC_ASSERT(pathSet::memberOrDescendantOfMember(t, p));
    if (pathSet::ancestorOfMember(s, p))
        RC_ASSERT(pathSet::ancestorOfMember(t, p));
}

/* Closure axiom — IDEMPOTENT (admit closure): cl_admit(cl_admit(S)) =
   cl_admit(S). Tested as set equality on the finite admit closure
   (ancestors only, so it materialises). */
RC_GTEST_PROP(PathSet, AdmitClosureIdempotent, (const std::set<CanonPath> & s))
{
    auto once = admitClosureSet(s);
    auto twice = admitClosureSet(once);
    RC_ASSERT(once == twice);
}

/* Idempotence of the DENY predicate, bounded-universe form: deny closure
   adds descendants (infinite), so we probe over U = W ∪ {p} ∪ anc(p).
   Materialise D = {u ∈ U : memberOrDescendantOfMember(W,u)} and assert
   re-applying the predicate with D as the base agrees with W at p. */
RC_GTEST_PROP(PathSet, DenyClosureIdempotentBounded, (const std::set<CanonPath> & w, const CanonPath & p))
{
    std::set<CanonPath> universe = w;
    universe.insert(p);
    for (auto & a : ancestorsOf(p))
        universe.insert(a);

    std::set<CanonPath> d;
    for (auto & u : universe)
        if (pathSet::memberOrDescendantOfMember(w, u))
            d.insert(u);

    RC_ASSERT(pathSet::memberOrDescendantOfMember(d, p) == pathSet::memberOrDescendantOfMember(w, p));
}

/* Empty-set boundary: the empty path-set's closure is empty — no path is
   in the closure of ∅. (The operator factories treat ∅ as the identity
   sentinel; this pins the predicate-level behaviour the factories rely
   on.) */
RC_GTEST_PROP(PathSet, EmptySetClosureIsEmpty, (const CanonPath & p))
{
    std::set<CanonPath> empty;
    RC_ASSERT(!pathSet::inClosure(empty, p));
    RC_ASSERT(!pathSet::ancestorOfMember(empty, p));
    RC_ASSERT(!pathSet::memberOrDescendantOfMember(empty, p));
}

#endif

} // namespace nix
