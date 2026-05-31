#pragma once
///@file

#include <set>

#include <rapidcheck/gen/Arbitrary.h>

#include "nix/util/canon-path.hh"
#include "nix/util/memory-source-accessor.hh"

namespace rc {

using namespace nix;

/**
 * Generates random `CanonPath` values up to a small bounded depth and
 * width — the goal is shrinkable inputs for property tests of operators
 * over path sets, not exhaustive coverage of weird path strings.
 *
 * Components are drawn from a small alphabet to maximise the chance of
 * collisions (so we exercise duplicate-path code paths) and keep
 * shrinking output readable.
 */
template<>
struct Arbitrary<CanonPath>
{
    static Gen<CanonPath> arbitrary();
};

/**
 * Generates random `std::set<CanonPath>` values: a bounded number of
 * arbitrary CanonPaths inserted into a set. Empty set is a frequent
 * outcome (exercises the identity-element short-circuits documented in
 * `doc/tecnix-survey/PROPOSAL.md` §3).
 */
template<>
struct Arbitrary<std::set<CanonPath>>
{
    static Gen<std::set<CanonPath>> arbitrary();
};

/**
 * Tunable knobs for the symlink-aware `Arbitrary<MemorySourceAccessor>`
 * generator. See `doc/tecnix-survey/PROPOSAL.md` §3 (L8 symlink-containment)
 * for the rationale behind the defaults (symlink containment and why symlinks
 * must appear in the generator to exercise the hard case of L8).
 *
 * - `symlinkProbability` is the per-entry probability that a random
 *   tree slot becomes a Symlink rather than a Regular.
 * - `brokenSymlinkProbability` is the *conditional* probability (given
 *   the entry is a Symlink) that the link target is not in the
 *   final-planted set, exercising the "broken symlink" code path.
 *
 * The defaults (`0.30` and `0.20`) are deliberately tuned so that L8
 * "symlink containment" preconditions fire on a non-trivial fraction
 * of samples; PROP tests that need stronger coverage can install a
 * `MemoryFsConfigGuard` in scope.
 */
struct MemoryFsConfig
{
    double symlinkProbability = 0.30;
    double brokenSymlinkProbability = 0.20; ///< conditional on Symlink
};

/**
 * Read the thread-local `MemoryFsConfig` consulted by
 * `Arbitrary<MemorySourceAccessor>::arbitrary()`. The default value
 * matches `MemoryFsConfig{}` until a `MemoryFsConfigGuard` is in
 * scope.
 */
const MemoryFsConfig & currentMemoryFsConfig();

/**
 * RAII-installed thread-local override consulted by
 * `Arbitrary<MemorySourceAccessor>`. Tests that cannot tolerate
 * symlinks install a config with `symlinkProbability = 0`.
 *
 * Restores the *previous* config on destruction (not the default).
 * This matters under nested guards or PROPs that throw mid-body —
 * the next test on the same thread starts from a clean baseline.
 */
struct MemoryFsConfigGuard
{
    explicit MemoryFsConfigGuard(MemoryFsConfig cfg);
    ~MemoryFsConfigGuard();

    MemoryFsConfigGuard(const MemoryFsConfigGuard &) = delete;
    MemoryFsConfigGuard & operator=(const MemoryFsConfigGuard &) = delete;
    MemoryFsConfigGuard(MemoryFsConfigGuard &&) = delete;
    MemoryFsConfigGuard & operator=(MemoryFsConfigGuard &&) = delete;

private:
    MemoryFsConfig prevConfig;
};

/**
 * Generates random in-memory trees as fixtures for operator property
 * tests. Files have arbitrary content; directory structure is induced
 * from the random `addFile` paths. The resulting accessor has no
 * fingerprint set (LocalCapability semantics) — set it explicitly in
 * tests that need it.
 *
 * Two-pass generation (see SKETCH §7 Step 1):
 *
 * 1. **Pass 1** plants Regular files. Path collisions and
 *    `parent-is-Regular` conflicts are silently dropped.
 * 2. **Pass 2** plants Symlinks. Resolvable targets are picked from the
 *    set of Regulars *actually planted in pass 1*; broken targets are
 *    drawn from the same `Arbitrary<CanonPath>` alphabet (some may
 *    accidentally land on planted paths — that is acceptable because
 *    "broken" only encodes intent, not a hard guarantee).
 *
 * This guarantees chain depth ≤ 1 by construction (resolvable targets
 * can never be other generated symlinks), so cycle / 1024-link cap
 * pathology cannot occur in generated fixtures.
 */
template<>
struct Arbitrary<MemorySourceAccessor>
{
    static Gen<MemorySourceAccessor> arbitrary();
};

/**
 * Correlated tree + path-set fixture: a `MemorySourceAccessor`
 * paired with a `std::set<CanonPath>` whose members are guaranteed
 * to be **paths actually planted on the tree**. This eliminates the
 * `RC_PRE(s ⊆ planted_paths(base))` discard pattern that arises when
 * `Arbitrary<MemorySourceAccessor>` and `Arbitrary<std::set<CanonPath>>`
 * are drawn independently.
 *
 * Use this for PROPs whose precondition involves "members of S that
 * are present on base" — the symlink-containment family in
 * `restrict-source-accessor.cc` is the canonical example. Without
 * the correlation, those PROPs spend their sample budget on tuples
 * where `s` and the tree don't intersect, and `RC_PRE` gives up.
 *
 * The set is non-empty only when the tree is non-empty; an empty
 * tree pairs with an empty set (the PROP's `RC_PRE(!s.empty())`
 * still discards but only on truly degenerate samples).
 */
struct TreeAndSet
{
    nix::MemorySourceAccessor tree;
    std::set<nix::CanonPath> s;
};

template<>
struct Arbitrary<TreeAndSet>
{
    static Gen<TreeAndSet> arbitrary();
};

} // namespace rc
