#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <rapidcheck.h>

#include "nix/util/canon-path.hh"
#include "nix/util/memory-source-accessor.hh"

#include "nix/util/tests/source-accessor.hh"

namespace rc {

using namespace nix;

namespace {

/* Component alphabet — small + stable for shrinking. Keeping the
   alphabet tiny maximises path-set collisions, which is what makes
   property tests over operator semantics interesting (most random
   paths produced are reachable from each other). */
Gen<std::string> arbitraryPathComponent()
{
    return gen::element<std::string>("a", "b", "c", "d", "x", "y", "sub");
}

/* Thread-local config consulted by `Arbitrary<MemorySourceAccessor>`.
   `MemoryFsConfigGuard` installs a value here in RAII fashion and
   restores the previous (not the default) value on destruction so
   PROPs that throw mid-body don't leak custom configs into the next
   test on the same thread. */
thread_local MemoryFsConfig tlMemoryFsConfig{};

} // anonymous namespace

const MemoryFsConfig & currentMemoryFsConfig()
{
    return tlMemoryFsConfig;
}

MemoryFsConfigGuard::MemoryFsConfigGuard(MemoryFsConfig cfg)
    : prevConfig(tlMemoryFsConfig)
{
    tlMemoryFsConfig = cfg;
}

MemoryFsConfigGuard::~MemoryFsConfigGuard()
{
    tlMemoryFsConfig = prevConfig;
}

Gen<CanonPath> Arbitrary<CanonPath>::arbitrary()
{
    /* Bounded depth via `gen::scale(0.25, …)` to keep paths shallow
       (rapidcheck's default container size grows linearly with the
       test counter; we want depths in the 0–4 range typically). */
    return gen::apply(
        [](const std::vector<std::string> & comps) {
            CanonPath p = CanonPath::root;
            for (auto & c : comps)
                p = p / c;
            return p;
        },
        gen::scale(0.25, gen::container<std::vector<std::string>>(arbitraryPathComponent())));
}

Gen<std::set<CanonPath>> Arbitrary<std::set<CanonPath>>::arbitrary()
{
    /* Bounded size — empty set is frequent (exercises identity-element
       short-circuits documented in OP-COMBINATOR §3 "Empty-S case"). */
    return gen::apply(
        [](const std::vector<CanonPath> & v) { return std::set<CanonPath>(v.begin(), v.end()); },
        gen::scale(0.25, gen::container<std::vector<CanonPath>>(gen::arbitrary<CanonPath>())));
}

namespace {

/* Per-entry generator state. Carries enough random material for the
   two-pass planter to:

     - decide whether the entry is a Symlink (vs. a Regular) based on
       `roleRoll < cfg.symlinkProbability * scale`;
     - decide, if Symlink, whether the target is broken (vs. resolvable)
       based on `brokenRoll < cfg.brokenSymlinkProbability * scale`;
     - resolve the target by either picking a planted path
       (`targetIndex % planted.size()`) or by treating `targetSeed` as
       a free path drawn from the same alphabet as the symlink site.

   The two random integers are pre-generated so the lambda is purely
   functional and the planter is deterministic given the rapidcheck
   sample. */
struct SymlinkGenEntry
{
    CanonPath path;
    std::string content;
    int roleRoll;         ///< 0..9999 — symlink-vs-regular roll
    int brokenRoll;       ///< 0..9999 — broken-vs-resolvable roll (only consulted for symlinks)
    int targetIndex;      ///< Used to pick a planted path for resolvable targets.
    CanonPath targetSeed; ///< Used as the broken-target string verbatim.
};

Gen<SymlinkGenEntry> arbitrarySymlinkGenEntry()
{
    return gen::apply(
        [](const CanonPath & p,
           const std::string & content,
           int roleRoll,
           int brokenRoll,
           int targetIndex,
           const CanonPath & targetSeed) {
            return SymlinkGenEntry{p, content, roleRoll, brokenRoll, targetIndex, targetSeed};
        },
        gen::arbitrary<CanonPath>(),
        gen::container<std::string>(gen::inRange<char>('a', 'z' + 1)),
        gen::inRange<int>(0, 10000),
        gen::inRange<int>(0, 10000),
        gen::inRange<int>(0, 1 << 20),
        gen::arbitrary<CanonPath>());
}

/* Plant a Regular file at `p` with `content`. Returns true iff the
   planting succeeded; failures from `parent-is-Regular` /
   `symlink-in-path` collisions are swallowed (consistent with the
   pre-symlink generator's "yield a partial-but-valid tree" contract). */
bool plantRegular(MemorySourceAccessor & acc, const CanonPath & p, const std::string & content)
{
    try {
        if (!acc.root)
            acc.open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
        auto * f = acc.open(p, MemorySourceAccessor::File{MemorySourceAccessor::File::Regular{}});
        if (!f)
            return false;
        if (auto * r = std::get_if<MemorySourceAccessor::File::Regular>(&f->raw)) {
            r->contents = content;
            return true;
        }
        return false;
    } catch (Error &) {
        return false;
    }
}

/* Plant a Symlink at `p` pointing at `target` (a raw string the
   accessor stores verbatim and which `resolveSymlinks` will
   tokenize). Same swallow-conflicts contract as `plantRegular`. */
bool plantSymlink(MemorySourceAccessor & acc, const CanonPath & p, const std::string & target)
{
    try {
        if (!acc.root)
            acc.open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
        auto * f = acc.open(p, MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{target}});
        if (!f)
            return false;
        if (auto * s = std::get_if<MemorySourceAccessor::File::Symlink>(&f->raw)) {
            s->target = target;
            return true;
        }
        return false;
    } catch (Error &) {
        return false;
    }
}

} // anonymous namespace

namespace {

/* Shared planter: builds a tree from a vector of symlink-aware
   entries and returns both the accessor and the list of paths that
   were *successfully* planted (Regulars from pass 1 plus Symlinks
   from pass 2). This is the substrate for both
   `Arbitrary<MemorySourceAccessor>` and `Arbitrary<TreeAndSet>`. */
struct PlantedTree
{
    MemorySourceAccessor acc;
    std::vector<CanonPath> planted; ///< all paths actually present on `acc`
};

PlantedTree
plantTree(const std::vector<SymlinkGenEntry> & entries, const MemoryFsConfig & cfg = currentMemoryFsConfig())
{
    const int symlinkThreshold = static_cast<int>(cfg.symlinkProbability * 10000);
    const int brokenThreshold = static_cast<int>(cfg.brokenSymlinkProbability * 10000);

    PlantedTree result;

    /* Classify entries first so we can do two passes without
       re-rolling the random material. */
    std::vector<const SymlinkGenEntry *> regularSlots;
    std::vector<const SymlinkGenEntry *> symlinkSlots;
    for (auto & e : entries) {
        if (e.path.isRoot())
            continue;
        if (e.roleRoll < symlinkThreshold)
            symlinkSlots.push_back(&e);
        else
            regularSlots.push_back(&e);
    }

    /* Pass 1: plant Regulars. Track regulars-only here because
       symlink-target resolution in Pass 2 needs to pick from
       Regulars-and-Directories (not from other symlinks, to keep
       chain depth bounded by 1). */
    std::vector<CanonPath> regulars;
    for (auto * e : regularSlots) {
        if (plantRegular(result.acc, e->path, e->content)) {
            regulars.push_back(e->path);
            result.planted.push_back(e->path);
        }
    }

    /* Pass 2: plant Symlinks. The resolvable target set is now
       final; broken targets are drawn from the same alphabet as
       the path itself. Targets are emitted as absolute strings
       (`abs()`) for determinism — see SKETCH §8 "Path-target
       encoding". */
    for (auto * e : symlinkSlots) {
        std::string target;
        bool wantBroken = e->brokenRoll < brokenThreshold;
        if (!wantBroken && !regulars.empty()) {
            auto idx = static_cast<size_t>(e->targetIndex) % regulars.size();
            target = regulars[idx].abs();
        } else {
            /* Broken (or pass-1 produced no planted regulars,
               which would make every Symlink technically broken
               anyway). The seed is drawn from the same alphabet
               as the symlink site. May accidentally land on a
               planted path; that's acceptable per SKETCH §3 —
               "broken" encodes intent, not a hard guarantee. */
            target = e->targetSeed.abs();
        }
        if (plantSymlink(result.acc, e->path, target))
            result.planted.push_back(e->path);
    }

    return result;
}

} // anonymous namespace

Gen<MemorySourceAccessor> Arbitrary<MemorySourceAccessor>::arbitrary()
{
    /* Two-pass build (SKETCH §7 Step 1):
         Pass 1 — plant Regulars (symlink slots are deferred);
         Pass 2 — plant Symlinks now that the resolvable set
                  (everything successfully planted in pass 1) is final.

       Path collisions and `parent-is-Regular` conflicts are silently
       dropped: the `MemorySourceAccessor::open(create=…)` call throws
       `NotADirectory` when an intermediate component is already a
       Regular file (e.g. planted `/x` followed by `/x/y`), and we want
       the generator to produce *any* valid tree rather than reject
       samples wholesale. We deliberately avoid `addFile` because it
       calls `shared_from_this()`, which throws on a stack-allocated
       accessor. */
    return gen::apply(
        [](const std::vector<SymlinkGenEntry> & entries) { return plantTree(entries).acc; },
        gen::scale(0.25, gen::container<std::vector<SymlinkGenEntry>>(arbitrarySymlinkGenEntry())));
}

Gen<TreeAndSet> Arbitrary<TreeAndSet>::arbitrary()
{
    /* Generate (entries, includeRolls) jointly:
       - `entries` drives the same planter as
         `Arbitrary<MemorySourceAccessor>`, but with a *boosted*
         per-entry symlink probability since this generator is for
         tests whose precondition involves "S contains a symlink";
         leaving the default 0.30 makes the symlink-finding
         preconditions rare across the cross product (s, planted,
         "exists symlink in s").
       - `includeRolls` is a parallel `int` per entry; we include
         the corresponding planted path in `s` when
         `roll % 4 != 0` — yielding ~75% inclusion per planted path,
         heavily biasing toward non-empty `s` while still leaving
         room for the empty case (when *no* planted paths get
         selected, which is exponentially rare for non-trivial
         trees).

       The result: when the tree has N planted paths,
         - probability `s` empty ≈ 0.25^N (vanishing for N>3);
         - expected |s| ≈ 0.75 * N.

       The boosted symlink probability is passed to `plantTree`
       directly rather than via `MemoryFsConfigGuard` — the guard's
       thread-local would be out of scope by the time the lambda
       runs. */
    return gen::apply(
        [](const std::vector<SymlinkGenEntry> & entries, const std::vector<int> & rolls) {
            MemoryFsConfig boosted{.symlinkProbability = 0.5, .brokenSymlinkProbability = 0.2};
            auto pt = plantTree(entries, boosted);
            std::set<CanonPath> s;
            for (size_t i = 0; i < pt.planted.size(); ++i) {
                int r = rolls.empty() ? 1 : rolls[i % rolls.size()];
                if ((r & 3) != 0) // ~75% inclusion
                    s.insert(pt.planted[i]);
            }
            return TreeAndSet{std::move(pt.acc), std::move(s)};
        },
        /* Use `gen::nonEmpty` so the entry vector has at least one
           element. With ~75% inclusion per planted path, |s| > 0
           with high probability when the tree has any planted paths
           (i.e. when at least one entry didn't collide on a path
           conflict). */
        gen::scale(
            0.5,
            gen::nonEmpty<std::vector<SymlinkGenEntry>>(
                gen::container<std::vector<SymlinkGenEntry>>(arbitrarySymlinkGenEntry()))),
        gen::container<std::vector<int>>(gen::inRange<int>(1, 100)));
}

} // namespace rc
