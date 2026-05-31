#include "nix/expr/materialisation-scheduler.hh"
#include "nix/expr/eval.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/projection.hh"
#include "nix/fetchers/source-view-git.hh"
#include "nix/store/store-api.hh"
#include "nix/store/local-store.hh"
#include "nix/util/source-view.hh"
#include "nix/util/thread-pool.hh"

#include <future>
#include <limits>

namespace nix {

SourcePlaceholder MaterialisationScheduler::registerView(Registration reg)
{
    auto placeholder = SourcePlaceholder::make(reg.contentId, reg.name);
    auto regPtr = std::make_shared<Registration>(std::move(reg));

    /* Insert; if already present, the existing one wins (same
       contentId+name ⇒ same placeholder ⇒ same registration
       semantically; we can keep either). */
    bool inserted;
    {
        auto regs = registrations_.lock();
        inserted = regs->try_emplace(placeholder, regPtr).second;
    }
    /* Maintain the contentId → registration secondary index (PF-5), but
       only on a genuinely new placeholder so re-registration doesn't
       accumulate duplicate index entries. */
    if (inserted)
        regsByContent_.lock()->emplace(regPtr->contentId, std::move(regPtr));
    return placeholder;
}

std::shared_ptr<const MaterialisationScheduler::Registration>
MaterialisationScheduler::lookup(const SourcePlaceholder & p) const
{
    auto regs = registrations_.readLock();
    auto it = regs->find(p);
    if (it == regs->end())
        return nullptr;
    return it->second;
}

std::optional<Hash> MaterialisationScheduler::peekNarHash(const SourceContentId & contentId)
{
    /* Fast path: in-process map. */
    {
        auto m = narHashByContent_.readLock();
        auto it = m->find(contentId);
        if (it != m->end())
            return it->second;
    }

    /* Try the persistent `SourceContentToNarHash` projection — a
       dedicated content-keyed realisation registry (distinct domain
       from the filtered-shape rows). */
    if (auto narHash = fetchers::SourceContentToNarHash::peek(state.fetchSettings, contentId.to_string())) {
        narHashByContent_.lock()->emplace(contentId, *narHash);
        return narHash;
    }

    return std::nullopt;
}

void MaterialisationScheduler::cacheNarHash(const SourceContentId & contentId, const Hash & narHash)
{
    narHashByContent_.lock()->emplace(contentId, narHash);
    state.fetchSettings.getCache()->upsert(
        fetchers::SourceContentToNarHash::key(contentId.to_string()),
        fetchers::SourceContentToNarHash::toValue(narHash));
}

Hash MaterialisationScheduler::narHashOf(const SourceContentId & contentId)
{
    /* Cache hit (in-process or persistent) ⇒ no walk. */
    if (auto cached = peekNarHash(contentId))
        return *cached;

    /* Coalesce concurrent walks for the same contentId. */
    std::promise<Hash> ours;
    auto ourFuture = ours.get_future().share();
    std::shared_future<Hash> winnerFuture;
    bool weAreTheWinner = false;

    inFlight_.try_emplace_and_cvisit(
        contentId,
        ourFuture,
        [&](auto & ent) {
            weAreTheWinner = true;
            winnerFuture = ent.second;
        },
        [&](const auto & ent) { winnerFuture = ent.second; });

    if (!weAreTheWinner) {
        /* Coalesced: another observer of the SAME contentId is already
           walking — we wait on its result and do NOT walk. This is the
           cargo-workspace dedup made visible (N packages sharing a
           contentId ⇒ this fires N-1 times, one walk total). */
        debug("materialise: contentId %s coalesced onto an in-flight walk — no walk", contentId.to_string());
        return winnerFuture.get();
    }

    debug("materialise: contentId %s is the walk winner — computing narHash now", contentId.to_string());

    /* Winner: find *a* registration for this contentId via the
       secondary index (PF-5 — O(1) average vs the old linear scan of
       all registrations). Any registration sharing the contentId is an
       equivalent walk source. */
    std::shared_ptr<Registration> reg;
    {
        auto idx = regsByContent_.readLock();
        if (auto it = idx->find(contentId); it != idx->end())
            reg = it->second;
    }

    if (!reg) {
        ours.set_exception(
            std::make_exception_ptr(
                Error("MaterialisationScheduler: no registration for contentId %s", contentId.to_string())));
        inFlight_.erase(contentId);
        return winnerFuture.get(); // throws
    }

    try {
        auto narHash = computeNarHash(*reg);
        cacheNarHash(contentId, narHash);
        ours.set_value(narHash);
        inFlight_.erase(contentId);
        return narHash;
    } catch (...) {
        ours.set_exception(std::current_exception());
        inFlight_.erase(contentId);
        throw;
    }
}

Hash MaterialisationScheduler::computeNarHash(const Registration & reg)
{
    /* Track Z.gap5 — filtered-subset tree-synthesis NORMALISATION (NOT
       walk-avoidance). For a Git-rooted `Subset` view we can synthesise
       a Git tree OID for the accepted subset purely from existing
       tree/blob OIDs (blob-free), then consult the persistent
       `treeHashToNarHash` projection on it.

       The win is cross-base/cross-pipeline identity: two DIFFERENT base
       trees (or two filter pipelines) that accept an IDENTICAL subset
       synthesise the SAME OID and share one `treeHashToNarHash` row.
       The `;shape`-suffixed `filteredSourcePathToHash` row does NOT give
       them that — it keys on the *source fingerprint*, so a different
       base never shares it. This consult is therefore the only place
       such sources dedup their walk.

       On a HIT we skip the walk. On a MISS this is pure normalisation:
       we still do the full DryRun NAR walk below (the cold compute reads
       every accepted blob — same cost as the plain walk), then write the
       result back keyed on the synthetic OID so the SECOND eval of any
       source resolving to that same synthetic tree hits here.

       PF-4: synthesise the OID ONCE (it's a base-subtree metadata walk +
       treebuilder, deterministic from base-OID + acceptedPaths) and
       reuse it for both the peek and the miss writeback, instead of
       recomputing it in each. */
    auto synthOid = synthesiseViewTreeOidFor(reg);
    if (synthOid)
        if (auto narHash = fetchers::TreeHashToNarHash::peek(state.fetchSettings, *synthOid)) {
            debug(
                "materialise: synthesis bridge HIT for '%s' — narHash from synthetic tree OID, no walk", reg.name);
            return *narHash;
        }

    /* Walk via fetchToStore in DryRun mode to get the narHash
       without copying. The view is the source the walk uses; the
       view's read methods enforce its recipe (Subset filters reads
       against `acceptedPaths`, Overlay routes entries to overlay,
       …) so fetchToStore needs no separate filter. */
    prefetchForWalk(reg);
    auto sourcePath = SourcePath{reg.view};
    auto [_, narHash] = fetchToStore2(
        state.fetchSettings,
        *state.store,
        sourcePath,
        FetchMode::DryRun,
        reg.name,
        reg.method,
        nullptr,
        NoRepair,
        reg.refs.others);

    writeSynthesisedNarHash(reg, synthOid, narHash);

    return narHash;
}

void MaterialisationScheduler::prefetchForWalk(const Registration & reg)
{
    /* Whole-tree walk imminent — coalesce all reachable blob backfills
       into one request (no-op unless the base is a partial-clone git
       accessor). Unbounded depth because the walk is exhaustive; the
       view forwards the root prefetch through its operator stack,
       re-anchoring onto the base subtree path (Subtree via Translate)
       or over-fetching harmlessly (filtered Subset). Mirrors
       `InputMaterialisation::force` / `ensureLazyPathCopied`. */
    reg.view->prefetchSubtree(CanonPath::root, std::numeric_limits<unsigned>::max());
}

std::optional<Hash> MaterialisationScheduler::synthesiseViewTreeOidFor(const Registration & reg)
{
    /* The synthetic-subset-tree OID for the Track Z.gap5 bridge. Sound
       to share a `treeHashToNarHash` row on it ONLY for the NixArchive
       method (whose narHash equals the tree's canonical NAR dump — the
       projection's invariant); `synthesiseViewTreeOid` itself further
       returns nullopt for non-git / non-Subset / empty-accept views. */
    if (reg.method != ContentAddressMethod::Raw::NixArchive)
        return std::nullopt;
    return synthesiseViewTreeOid(state.fetchSettings, *reg.view);
}

void MaterialisationScheduler::writeSynthesisedNarHash(
    const Registration & reg, const std::optional<Hash> & synthOid, const Hash & narHash)
{
    /* Key the freshly-walked narHash on the (precomputed) synthetic
       subset tree OID. Equal synthetic OID ⇒ equal accepted subset ⇒
       equal filtered NAR (the RapidCheck
       `SynthesiseTreeNarHashEquivalsFilteredWalk` property in
       git-fingerprint.cc), so this row is sound to share. Shared by the
       DryRun path (`computeNarHash`) and the fused Copy path
       (`materialiseFused`) so the cargo cold path keeps the cross-base
       normalisation regardless of which walk produced the hash. The OID
       is computed once by the caller (`synthesiseViewTreeOidFor`) and
       threaded in to avoid re-synthesising it (PF-4). */
    if (!synthOid)
        return;
    debug(
        "materialise: synthesis bridge MISS for '%s' — walked, wrote back narHash on synthetic tree OID %s",
        reg.name,
        synthOid->gitRev());
    state.fetchSettings.getCache()->upsert(
        fetchers::TreeHashToNarHash::key(*synthOid), fetchers::TreeHashToNarHash::toValue(narHash));
}

StorePath MaterialisationScheduler::materialiseFused(const Registration & reg)
{
    /* One walk: Copy yields BOTH the storePath and the narHash. For
       NixArchive the copy's hash IS the narHash, and addToStoreFromDump
       derives the same storePath `narHashOf`+`makeFixedOutputPathFromCA`
       would (walk → hash → derive → move, with its own isValidPath skip).
       So this is value-equivalent to the DryRun-then-Copy pair, minus the
       redundant DryRun walk. Caller guarantees this is the sole observer
       for the contentId, so there's no shared walk to coalesce via
       `inFlight_`. */
    debug(
        "materialise: cold fused walk for '%s' (sole observer of its contentId) — one Copy yields hash + bytes",
        reg.name);
    prefetchForWalk(reg);
    auto sourcePath = SourcePath{reg.view};
    auto [storePath, narHash] = fetchToStore2(
        state.fetchSettings,
        *state.store,
        sourcePath,
        FetchMode::Copy,
        reg.name,
        reg.method,
        nullptr,
        NoRepair,
        reg.refs.others);
    cacheNarHash(reg.contentId, narHash);
    /* Keep the gap5 cross-base writeback even though we fused away the
       DryRun: a filtered-subset source materialised here (incl. the
       cargo cold path after PF-1) must still populate the synthetic-OID
       bridge that the prelude `computeNarHash` used to. */
    writeSynthesisedNarHash(reg, synthesiseViewTreeOidFor(reg), narHash);
    return storePath;
}

StorePath MaterialisationScheduler::outPathOf(const SourcePlaceholder & placeholder)
{
    auto reg = lookup(placeholder);
    if (!reg)
        throw Error("placeholder %s has no registration", placeholder.render());

    /* When the narHash is already known (in-process memo or persistent
       cache), compute the outPath and copy only if the store path is
       missing — the warm/shared case, zero or one walk. */
    if (auto cached = peekNarHash(reg->contentId)) {
        auto outPath = state.store->makeFixedOutputPathFromCA(
            reg->name, ContentAddressWithReferences::fromParts(reg->method, *cached, reg->refs));
        if (!state.store->isValidPath(outPath)) {
            prefetchForWalk(*reg);
            auto sourcePath = SourcePath{reg->view};
            fetchToStore2(
                state.fetchSettings,
                *state.store,
                sourcePath,
                FetchMode::Copy,
                reg->name,
                reg->method,
                nullptr,
                NoRepair,
                reg->refs.others);
        }
        return outPath;
    }

    /* narHash NOT cached: the old code walked TWICE — `narHashOf`'s
       DryRun to compute the hash, then a Copy to ingest. For NixArchive
       the Copy already yields the narHash, so fuse them into ONE walk
       (`materialiseFused`); the DryRun is pure overhead here. This is
       value-equivalent and ALSO preserves the cargo coalescing: the
       fused Copy populates the narHash caches, so sibling placeholders
       (same contentId, different name) then hit `peekNarHash` above and
       only pay their own unavoidable per-name ingestion — exactly as the
       DryRun-coalesced path did, minus one redundant walk. (Non-NixArchive
       falls back to the original narHashOf-then-Copy shape.) */
    if (reg->method == ContentAddressMethod::Raw::NixArchive)
        return materialiseFused(*reg);

    auto narHash = narHashOf(reg->contentId);
    auto outPath = state.store->makeFixedOutputPathFromCA(
        reg->name, ContentAddressWithReferences::fromParts(reg->method, narHash, reg->refs));
    if (!state.store->isValidPath(outPath)) {
        prefetchForWalk(*reg);
        auto sourcePath = SourcePath{reg->view};
        fetchToStore2(
            state.fetchSettings,
            *state.store,
            sourcePath,
            FetchMode::Copy,
            reg->name,
            reg->method,
            nullptr,
            NoRepair,
            reg->refs.others);
    }
    return outPath;
}

void MaterialisationScheduler::materialiseGroup(
    const std::vector<SourcePlaceholder> & group,
    const std::unordered_map<SourcePlaceholder, std::shared_ptr<const Registration>> & regs,
    std::unordered_map<SourcePlaceholder, StorePath> & out)
{
    /* narHash is cached (the prelude in outPathsOf ran narHashOf for this
       contentId), so the first sibling's outPathOf takes the peek-hit
       branch and Copies once if its path is missing. */
    auto & firstPh = group.front();
    auto firstPath = outPathOf(firstPh);
    out.emplace(firstPh, firstPath);

    if (group.size() == 1)
        return;

    /* The link fast path needs a LocalStore (hardlinks + registerValidPath)
       that isn't read-only. Otherwise every sibling falls back to its own
       outPathOf Copy (the pre-#1 behaviour). */
    auto * localStore = dynamic_cast<LocalStore *>(&*state.store);
    auto cached = peekNarHash(regs.at(firstPh)->contentId);

    /* narSize is name-independent (the NAR doesn't encode the store path), so
       every linked sibling's narSize is the first sibling's. `firstPath` is
       loop-invariant, so query it ONCE here rather than once per sibling — the
       cargo-workspace shape can have hundreds of siblings, each `queryPathInfo`
       being a path-info DB / cache lookup. Only needed when we'll actually link
       (a valid `firstPath` + LocalStore + cached narHash). */
    std::optional<uint64_t> firstNarSize;
    if (localStore && cached)
        firstNarSize = state.store->queryPathInfo(firstPath)->narSize;

    if (group.size() >= 2)
        debug(
            "materialise: contentId group of %d siblings — copied '%s' once, %s the rest",
            group.size(),
            state.store->printStorePath(firstPath),
            localStore ? "hardlinking" : "copying (non-LocalStore, no hardlink)");

    for (size_t i = 1; i < group.size(); ++i) {
        auto & ph = group[i];
        auto reg = regs.at(ph);

        /* A self-reference would make this sibling's NAR differ from the
           first's (the embedded self-path hash-part differs), so the
           bytes are NOT identical and linking is unsound — fall back to
           an independent copy. Same fallback for a non-LocalStore or
           when the narHash somehow isn't cached. */
        bool linkable = localStore && cached && !reg->refs.self;

        if (!linkable) {
            out.emplace(ph, outPathOf(ph));
            continue;
        }

        auto desc = ContentAddressWithReferences::fromParts(reg->method, *cached, reg->refs);
        auto pathB = state.store->makeFixedOutputPathFromCA(reg->name, desc);

        if (state.store->isValidPath(pathB)) {
            out.emplace(ph, std::move(pathB));
            continue;
        }

        try {
            auto toInfo = ValidPathInfo::makeFromCA(*state.store, reg->name, std::move(desc), *cached);
            /* Hoisted above the loop (see `firstNarSize`); `linkable` implies
               it's set. */
            toInfo.narSize = *firstNarSize;
            localStore->registerLinkedCAPath(firstPath, toInfo);
            debug(
                "materialise: hardlinked sibling '%s' from '%s' (no re-walk, no re-copy)",
                state.store->printStorePath(toInfo.path),
                state.store->printStorePath(firstPath));
            out.emplace(ph, toInfo.path);
        } catch (Error & e) {
            /* Any link-time failure (e.g. the CA re-derivation assert, an
               unexpected store state): fall back to an independent copy
               so the demand still resolves correctly. */
            debug("copy-once-link-N fell back to a copy for '%s': %s", reg->name, e.what());
            out.emplace(ph, outPathOf(ph));
        }
    }
}

std::unordered_map<SourcePlaceholder, StorePath>
MaterialisationScheduler::outPathsOf(std::span<const SourcePlaceholder> placeholders)
{
    /* Group by contentId. Demands sharing a contentId share one
       walk; demands with distinct contentIds dispatch in parallel. */
    std::unordered_map<SourceContentId, std::vector<SourcePlaceholder>> byContent;
    std::unordered_map<SourcePlaceholder, std::shared_ptr<const Registration>> regs;
    for (auto & p : placeholders) {
        auto reg = lookup(p);
        if (!reg)
            throw Error("placeholder %s has no registration", p.render());
        byContent[reg->contentId].push_back(p);
        regs.emplace(p, reg);
    }

    std::unordered_map<SourcePlaceholder, StorePath> out;

    /* Single placeholder: skip the narHash prelude entirely and let
       `outPathOf` do its single-walk fusion (peek → on miss, one Copy
       that yields the hash). Running the prelude `narHashOf` here would
       populate the cache and thereby DEFEAT that fusion (outPathOf would
       then peek-hit and Copy → two walks again). This is the dominant
       single-source eval shape. */
    if (placeholders.size() == 1) {
        auto & p = placeholders.front();
        out.emplace(p, outPathOf(p));
        return out;
    }

    /* Multiple DISTINCT contentIds: pre-compute their narHashes in
       parallel (ThreadPool). Each distinct contentId's walk is
       independent, so fanning out overlaps their I/O; the
       per-placeholder `outPathOf` loop below then peek-hits and only
       pays each name's unavoidable ingestion Copy.

       NOTE (PF-1): we deliberately do NOT run a prelude for the
       SINGLE-contentId case (one source observed by N placeholders —
       the cargo-workspace shape). Running `narHashOf` there would
       populate the cache and force the first sibling's `outPathOf` onto
       the peek-hit Copy branch, costing a DryRun walk PLUS a Copy walk
       = two cold walks. Skipping it lets `materialiseGroup`'s first
       `outPathOf` take the `materialiseFused` path (one Copy that
       yields the hash), which `cacheNarHash`es so the remaining
       siblings still peek-hit and hardlink — one cold walk total, and
       the gap5 synthesis writeback is preserved because `materialiseFused`
       calls `writeSynthesisedNarHash` directly. The prelude is
       only worth it across DISTINCT contentIds, where the walks don't
       share a result to fuse. */
    if (byContent.size() > 1) {
        ThreadPool pool;
        for (auto & [cid, _] : byContent) {
            pool.enqueue([this, cid] { (void) narHashOf(cid); });
        }
        pool.process();
    }

    /* Now materialise storePaths, one contentId GROUP at a time. The
       narHash is cached (the prelude above guarantees it). Within a
       group, `materialiseGroup` does copy-once-link-N: Copy the first
       sibling's bytes once, hardlink the rest into their name-stamped CA
       paths (Perf #1) — distinct CA paths ⇒ no write-write contention.
       Groups are independent, so run them on a ThreadPool. Each group
       writes into its own `out` shard (a side vector indexed by group),
       merged single-threaded after the join (`out` is not thread-safe).
       `process()` rethrows the first task exception (batch-abort). */
    std::vector<std::vector<SourcePlaceholder>> groups;
    groups.reserve(byContent.size());
    for (auto & [_, g] : byContent)
        groups.push_back(g);

    std::vector<std::unordered_map<SourcePlaceholder, StorePath>> shards(groups.size());
    ThreadPool pool;
    for (size_t i = 0; i < groups.size(); ++i)
        pool.enqueue([&, i] { materialiseGroup(groups[i], regs, shards[i]); });
    pool.process();

    for (auto & shard : shards)
        for (auto & [ph, sp] : shard)
            out.emplace(ph, std::move(sp));
    return out;
}

} // namespace nix
