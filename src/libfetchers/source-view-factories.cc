#include "nix/util/source-view.hh"
#include "nix/util/source-accessor.hh"

#include "nix/fetchers/directory-synthesizer-source-accessor.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/fetchers/mask-source-accessor.hh"
#include "nix/fetchers/restrict-source-accessor.hh"
#include "nix/fetchers/soundness-guard-source-accessor.hh"
#include "nix/fetchers/translate-source-accessor.hh"

namespace nix {

namespace {

/* View filtering means "the path doesn't exist in this view," not
   "access denied for security reasons." Build a `MakeNotFoundError`
   (the typedef for FileNotFound-flavoured callbacks declared in
   filtering-source-accessor.hh) and adapt it into the
   `MakeNotAllowedError` shape that `FilteringSourceAccessor::checkAccess`
   expects. The adapter throws `FileNotFound` directly; the dummy
   `RestrictedPathError` return is never reached. Convention:
   `MakeNotFoundError` for view factories,
   `MakeNotAllowedError` reserved for security-flavoured uses
   (`AllowListSourceAccessor` / restrictEval). */
MakeNotFoundError filteredOutFileNotFound()
{
    return [](const CanonPath & p) -> FileNotFound {
        return FileNotFound("path '%s' does not exist in this view", p.abs());
    };
}

MakeNotAllowedError makeFilteredOutError()
{
    return adaptToNotAllowed(filteredOutFileNotFound());
}

} // anonymous namespace

ref<SourceViewAccessor> sourceViewRoot(ref<SourceAccessor> base)
{
    SourceViewIdentity id;
    if (base->fingerprint) {
        id.fingerprint = base->fingerprint;
        id.purity = ViewPurity::ContentAddressed;
    } else {
        id.purity = ViewPurity::LocalCapability;
    }
    return make_ref<SourceViewAccessor>(base, /*readImpl=*/base, recipe::Root{}, std::move(id), SourceProvenance{});
}

ref<SourceViewAccessor> sourceViewSubtree(ref<SourceAccessor> base, CanonPath subpath)
{
    /* `makeTranslate` handles L1b/L1c identity short-circuit (subpath = /
       returns base) and L1d morphism fusion (Translate ∘ Translate).
       Translate always admits, so no error builder is needed (defaults to
       `neverDenied()`). */
    ref<SourceAccessor> readImpl = makeTranslate(base, subpath);

    SourceViewIdentity id;
    id.purity = base->fingerprint ? ViewPurity::ContentAddressed : ViewPurity::LocalCapability;

    return make_ref<SourceViewAccessor>(
        base, readImpl, recipe::Subtree{std::move(subpath)}, std::move(id), SourceProvenance{});
}

ref<SourceViewAccessor>
sourceViewSubset(ref<SourceAccessor> filteredBase, Hash shapeHash, std::set<CanonPath> acceptedPaths, CanonPath subpath)
{
    /* Inner-to-outer construction. Composition is
       `D(S₁) ∘ Restrict(S) ∘ Translate(p)`:
         - Translate is the innermost wrap (sees raw base paths).
         - Restrict is the pure Prism filter against the wrapper's
           `/`-rooted namespace.
         - DirectorySynthesizer adds ancestor walkability: when
           callers walk toward leaves in `layer1Region(recipe)`, the
           intermediate dirs are surfaced as synthesised tDirectory
           entries even when Restrict legitimately denied them and
           base lacked them.
       Phase 1 of the algebraic-accessors plan extracted synthesis
       from Restrict so the strict commutativity law holds at
       `maybeLstat` for stacked Restricts (see
       doc/tecnix-survey/PROPOSAL.md §2.Q Phase-1 self-audit + §5
       "Restrict-with-synthesis was the wrong factoring").
       Each `make…` factory handles its own identity short-circuits
       (subpath = /, paths = ∅) and morphism fusion. */
    auto recipe = recipe::Subset{shapeHash, acceptedPaths, subpath};
    auto sptr = std::make_shared<const std::set<CanonPath>>(std::move(acceptedPaths));
    auto l1ptr = std::make_shared<const std::set<CanonPath>>(layer1Region(recipe));
    /* Translate + DirectorySynthesizer always admit (defaulted error
       builder); only `makeRestrict` genuinely throws on a filtered-out
       path, so it keeps the `makeFilteredOutError()` callback. */
    auto readImpl = makeDirectorySynthesizer(
        makeRestrict(makeTranslate(filteredBase, subpath), sptr, makeFilteredOutError()), l1ptr);

    SourceViewIdentity id;
    id.purity = filteredBase->fingerprint ? ViewPurity::ContentAddressed : ViewPurity::LocalCapability;

    auto view =
        make_ref<SourceViewAccessor>(filteredBase, readImpl, std::move(recipe), std::move(id), SourceProvenance{});

    /* Subpath-aware fingerprint seed: when re-anchored at a non-root
       subpath, prefer the base's per-subpath identity over its
       Input-level field. Two revs sharing the same subtree-SHA share
       cache rows for the filtered subpath source. */
    if (!subpath.isRoot()) {
        if (auto [_, fp] = filteredBase->getFingerprint(subpath); fp)
            view->fingerprint = *fp;
    }
    return view;
}

ref<SourceViewAccessor> sourceViewOverlay(
    ref<SourceAccessor> base, ref<SourceAccessor> overlay, std::set<CanonPath> entries, std::set<CanonPath> whiteouts)
{
    auto recipe = recipe::Overlay{whiteouts, entries};

    ref<SourceAccessor> readImpl = base;

    /* Empty-Overlay sentinel: factory short-circuits to `readImpl = base`
       directly. The recipe still carries the (empty) entries/whiteouts
       so consumers (e.g. `getProvenance`) see a recognisable shape. */
    if (!entries.empty() || !whiteouts.empty()) {
        /* Overlay needs both regions; compute them in one visit. */
        auto reg = regions(recipe);
        auto wptr = std::make_shared<const std::set<CanonPath>>(whiteouts);
        ref<SourceAccessor> masked = makeMask(base, wptr, makeFilteredOutError());

        ref<SourceAccessor> layered = masked;
        if (!entries.empty()) {
            /* `Restrict(∅)(overlay) = identity(overlay)` would let
               overlay paths leak through Union ahead of the base —
               but the recipe convention is "entries == ∅ means no
               overlay contribution," not "expose all overlay files
               unfiltered." So we only wire the overlay branch when
               entries is non-empty. */
            auto eptr = std::make_shared<const std::set<CanonPath>>(entries);
            ref<SourceAccessor> restricted = makeRestrict(overlay, eptr, makeFilteredOutError());
            layered = makeLayer({restricted, masked});

            auto l1ptr = std::make_shared<const std::set<CanonPath>>(std::move(reg.layer1));
            /* DirectorySynthesizer always admits — defaulted error builder. */
            layered = makeDirectorySynthesizer(layered, l1ptr);
        }

        auto l2ptr = std::make_shared<const std::set<CanonPath>>(std::move(reg.layer2));
        /* SoundnessGuard always admits (Layer-2-only) — defaulted error builder. */
        readImpl = makeSoundnessGuard(layered, l2ptr);
    }

    SourceViewIdentity id;
    /* Overlay over a content-addressed base + dirty entries → local. */
    id.purity = ViewPurity::LocalCapability;

    SourceProvenance prov;
    prov.dirtyFiles = entries;
    prov.deletedFiles = whiteouts;

    return make_ref<SourceViewAccessor>(
        base,
        readImpl,
        std::move(recipe),
        std::move(id),
        std::move(prov),
        /*overlay=*/overlay);
}

ref<SourceViewAccessor> sourceViewTreeHandle(ref<SourceAccessor> treeRooted, Hash treeOid)
{
    SourceViewIdentity id;
    id.gitTree = treeOid;
    id.fingerprint = treeRooted->fingerprint;
    id.purity = ViewPurity::ContentAddressed;
    return make_ref<SourceViewAccessor>(
        treeRooted, /*readImpl=*/treeRooted, recipe::TreeHandle{treeOid}, std::move(id), SourceProvenance{});
}

ref<SourceViewAccessor>
sourceViewLocalCheckout(ref<SourceAccessor> base, std::filesystem::path checkoutPath, SourceProvenance provenance)
{
    SourceViewIdentity id;
    id.purity = ViewPurity::LocalCapability;
    if (!provenance.checkoutRoot)
        provenance.checkoutRoot = checkoutPath;
    return make_ref<SourceViewAccessor>(
        base,
        /*readImpl=*/base,
        recipe::LocalCheckout{checkoutPath},
        std::move(id),
        std::move(provenance),
        /*overlay=*/nullptr,
        /*checkoutPath=*/checkoutPath);
}

} // namespace nix
