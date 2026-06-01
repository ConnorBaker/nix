#include "nix/store/store-api.hh"
#include "nix/store/local-store.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/input-materialisation.hh"
#include "nix/expr/materialisation-scheduler.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/source-view.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"

#include <nlohmann/json.hpp>

#include <limits>

namespace nix {

CanonPath storeMountKey(const Store & store, const StorePath & path)
{
    /* `printStorePath(p) = storeDir + "/" + p.to_string()` and a store
       path is a single component `<hash>-<name>`, so the store-relative
       mount key (after the eval-root re-root strips `<storeDir>`) is the
       leading-slash-rooted single component `/<hash>-<name>`. */
    (void) store;
    return CanonPath(std::string(path.to_string()));
}

SourcePath EvalState::rootPath(CanonPath path)
{
    return {rootFS, std::move(path)};
}

SourcePath EvalState::rootPath(std::string_view path)
{
    /* FIXME: Move this out of EvalState, since it's using native
       std::filesystem::path and current working directory. */
    return {rootFS, CanonPath(absPath(path).string())};
}

SourcePath EvalState::storePath(const StorePath & path)
{
    return {rootFS, CanonPath{store->printStorePath(path)}};
}

StorePath EvalState::devirtualizeStorePath(const StorePath & path)
{
    /* Already devirtualized this fake path before? Return the cached
       real path without re-forcing. */
    {
        auto rewrites = virtualPathRewrites_.readLock();
        if (auto it = rewrites->find(path); it != rewrites->end())
            return it->second;
    }

    /* Is this a deferred-mount stand-in (Item 2, §6.1.1)? If not, the
       path is already real — return it unchanged. */
    std::optional<ref<InputMaterialisation>> mat;
    {
        auto mats = virtualMounts_.readLock();
        if (auto it = mats->find(path); it != mats->end())
            mat = it->second;
    }
    if (!mat)
        return path;

    /* Force the materialisation. This is where the deferred dryRun
       walk finally happens, and where the lock-narHash mismatch check
       (carried by `mat`'s `expectedNarHash`) fires — soundness
       obligation (a). The fake path is content-keyed off the input's
       pre-narHash identity, so it does NOT equal the real CA path
       (which is keyed off the narHash); the rewrite below is therefore
       always a genuine fake → real substitution. */
    auto realPath = (*mat)->force().first;

    debug(
        "virtual-mount: devirtualising deferred fake path '%s' → real CA path '%s' (the deferred walk runs HERE)",
        store->printStorePath(path),
        store->printStorePath(realPath));

    {
        auto rewrites = virtualPathRewrites_.lock();
        rewrites->insert_or_assign(path, realPath);
    }
    return realPath;
}

void EvalState::ensureLazyPathCopied(const StorePath & path)
{
    if (settings.isReadOnly())
        return;

    /* Deferred-mount devirtualisation (Item 2, §6.1.1, Design C). If
       `path` is a fake stand-in, force the mat to learn the real CA
       path, then mount the accessor under the REAL key and allowPath
       it so the panic-on-mismatch copy below compares real-vs-real and
       subsequent reads of the real path resolve. We keep the fake
       mount in place too: values already minted with `Opaque{fakePath}`
       keep reading through it. */
    auto realPath = devirtualizeStorePath(path);
    if (realPath != path) {
        if (auto mount = storeFS->getMount(storeMountKey(*store, path))) {
            storeFS->mount(storeMountKey(*store, realPath), ref(mount));
            allowPath(realPath);
        }
    }

    auto mount = storeFS->getMount(storeMountKey(*store, realPath));
    if (!mount)
        return;

    /* This Copy reads the whole tree. On a partial clone, prime every
       reachable blob in ONE coalesced provider request first, so the
       copy doesn't fault them in one-by-one (one HTTPS round-trip per
       file). No-op for a fully-cloned repo or any non-Git accessor —
       the base `prefetchSubtree` does nothing. Mirrors the prefetch in
       `InputMaterialisation::force()`; both are the seam that lets
       lazy-fetch and tree materialisation compose efficiently. */
    mount->prefetchSubtree(CanonPath::root, std::numeric_limits<unsigned>::max());

    /* Item (b): base-plus-overlay assembly. When the mounted accessor is a
       dirty git working tree (a `SourceViewAccessor` with a `recipe::Overlay`
       over a committed-tree base), most files are unchanged from the base.
       Rather than re-copy the WHOLE tree, materialise the committed base
       ONCE (cached across edits by the tree-OID bridge) and ASSEMBLE the
       dirty store path from it: reflink/hardlink the unchanged majority,
       write only the changed files. The assembler hashes the result once
       (the unavoidable read — no double walk) and verifies it against the
       evaluator-minted name (`realPath`). On any non-applicability (no
       LocalStore, base not materialisable, base not a usable prefix) we
       fall through to the plain full copy below. */
    std::optional<StorePath> assembled;
    if (auto * localStore = dynamic_cast<LocalStore *>(&*store)) {
        if (auto * view = dynamic_cast<SourceViewAccessor *>(&*mount)) {
            if (auto * ov = std::get_if<recipe::Overlay>(&view->recipe)) {
                try {
                    /* Materialise the committed base tree to a store path.
                       This is a full copy of the base, content-addressed by
                       its tree OID. The base accessor carries no fingerprint
                       (`GitRepoImpl::getRawAccessor`), so `fetchToStore`'s
                       own cache can't dedup it and would re-walk the whole
                       committed tree on every edit. The committed tree OID
                       is stable across edits and O(1) via `getRootTreeHash`,
                       so memoise the base store path on it (per EvalState):
                       materialise once per (process, tree OID), reuse
                       thereafter. The memo value is re-validated (a GC may
                       have reaped it) before trusting it. */
                    auto baseTreeOid = view->base->getRootTreeHash();
                    std::optional<StorePath> basePathOpt;
                    if (baseTreeOid) {
                        auto bases = materialisedBases_.readLock();
                        if (auto it = bases->find(*baseTreeOid);
                            it != bases->end() && store->isValidPath(it->second))
                            basePathOpt = it->second;
                    }
                    auto basePath = basePathOpt ? *basePathOpt
                                                : fetchToStore(
                                                      fetchSettings,
                                                      *store,
                                                      SourcePath{view->base},
                                                      FetchMode::Copy,
                                                      realPath.name() + "-base");
                    if (baseTreeOid && !basePathOpt)
                        materialisedBases_.lock()->insert_or_assign(*baseTreeOid, basePath);
                    /* No `expectedPath`: the assembler computes the true
                       narHash of the assembled tree and returns whatever
                       path that content names. We then CHECK it equals the
                       `realPath` the evaluator already minted. A dirty
                       workdir overlay is never `rev`-locked, so `realPath`
                       here was produced by the slow path's own force over
                       this same accessor — i.e. a divergence would be an
                       ASSEMBLER bug, not a stale lock. So on any divergence
                       we fall back to the full copy (always correct) rather
                       than surfacing a misleading mismatch error. */
                    auto candidate = localStore->assembleCAPathFromBase(
                        basePath, ref(mount), ov->entries, ov->whiteouts, realPath.name(), std::nullopt);
                    if (candidate && *candidate == realPath) {
                        assembled = candidate;
                        debug(
                            "virtual-mount: assembled '%s' from base '%s' (+%d changed, %d whiteouts) — unchanged "
                            "files reflinked/hardlinked, not re-copied",
                            store->printStorePath(*assembled),
                            store->printStorePath(basePath),
                            ov->entries.size(),
                            ov->whiteouts.size());
                    } else if (candidate) {
                        debug(
                            "virtual-mount: assembled path '%s' != expected '%s' (assembler/overlay disagreement) — "
                            "full copy",
                            store->printStorePath(*candidate),
                            store->printStorePath(realPath));
                    }
                } catch (Error & e) {
                    /* "Not applicable" returns nullopt (handled above).
                       Any thrown error: fall back to the full copy, which
                       is unconditionally correct. */
                    debug(
                        "virtual-mount: base-overlay assembly failed for '%s' (%s) — full copy",
                        realPath.name(),
                        e.what());
                }
            }
        }
    }

    /* TODO: We could memoise this in-memory if necessary. */
    auto storePath = assembled ? *assembled
                               : fetchToStore(
                                     fetchSettings,
                                     *store,
                                     SourcePath{ref(mount)},
                                     /* Force a copy. mountInput does a dryRun to just calculate the storePath and narHash. */
                                     FetchMode::Copy,
                                     realPath.name());

    /* The copied path must equal the name we minted. A mismatch means the
       bytes we just hashed do not match the narHash that named `realPath`.

       Two distinct causes, distinguished by whether `realPath` was a
       deferred stand-in:

       - For a `virtualMounts_` stand-in (slow-path Item-2 defer),
         `realPath` came from `mat->force()`, which already verified the
         content against the input's `expectedNarHash`; a mismatch here
         can then only be an internal inconsistency (e.g. unsound caching
         across accessor types that fetch the same git rev differently —
         tarball vs local/remote git). That is a bug: panic.

       - For the known-narHash DEFER (item (c)), `realPath` is the real CA
         path minted directly from the lock's narHash, and no force ran
         before this copy. A mismatch then means the source CONTENT
         changed since it was locked (a stale lock over a `path:`/dirty
         input). That is a user-facing condition, not a bug — report it as
         the same NAR-hash-mismatch error the eager path raises (errno
         102), naming the expected (locked) vs actual store path. */
    if (storePath != realPath) {
        bool wasStandIn;
        {
            auto rewrites = virtualPathRewrites_.readLock();
            wasStandIn = rewrites->find(path) != rewrites->end() && path != realPath;
        }
        if (wasStandIn)
            panic(fmt(
                "hashed store path computed by the evaluator ('%1%') does not match what was computed when copying to the store ('%2%'), this is a bug",
                store->printStorePath(realPath),
                store->printStorePath(storePath)));
        throw Error(
            (unsigned int) 102,
            "NAR hash mismatch for '%s': the source content does not match the hash it was locked with "
            "(expected store path '%s' but the content hashes to '%s'). The input may have changed since "
            "its hash was recorded; re-lock it (e.g. `nix flake update`) or remove the stale lock.",
            realPath.name(),
            store->printStorePath(realPath),
            store->printStorePath(storePath));
    }
}

void EvalState::ensureLazyPathsCopied(const NixStringContext & context)
{
    for (const auto & c : context) {
        if (auto * o = std::get_if<NixStringContextElem::Opaque>(&c.raw))
            /* TODO: This could be done in parallel. */
            ensureLazyPathCopied(o->path);
        else if (auto * sv = std::get_if<NixStringContextElem::SourceVirtual>(&c.raw))
            /* Demand the placeholder's storePath. The scheduler
               handles in-process coalescing + persistent cache
               lookup; the side effect we want is mounting the
               accessor in storeFS so subsequent reads find it. */
            (void) materialisationScheduler->outPathOf(sv->placeholder);
    }
}

StringMap EvalState::resolveSourceVirtualContext(const NixStringContext & context)
{
    /* Collect every SourceVirtual placeholder reachable from `context`,
       then resolve them all in one batched call. `outPathsOf` groups
       by `SourceContentId` (so 200 cargo-workspace packages sharing a
       contentId trigger one walk total) and dispatches independent
       contentIds in parallel via ThreadPool. Per-element `outPathOf`
       would serialise the walks and lose the cargo-workspace win. */
    std::vector<SourcePlaceholder> svBatch;
    for (auto & c : context)
        if (auto * sv = std::get_if<NixStringContextElem::SourceVirtual>(&c.raw))
            svBatch.push_back(sv->placeholder);

    StringMap rewrites;

    /* Item 2 (§6.1.1): a deferred-mount `Opaque{fakePath}` stand-in
       must be rewritten to its real CA path before its text is
       serialised. This is the SINGLE point that covers every
       boundary, because the §6.4.3 cover-fix sweep made
       `resolveSourceVirtualContext` the universal pre-serialise step:
       every CLI output mode, eval-cache write, and diagnostic embedder
       calls it (paired with `ensureLazyPathsCopied`) and applies the
       returned rewrites to the body. So a flake-self / input `outPath`
       displayed by `nix eval` prints the real path (forcing the
       deferred walk at that boundary), while metadata-only reads
       (`.rev` / `.lastModified`, or a context-discarding
       `stringLength`) carry no Opaque element here and stay
       walk-free. For a non-deferred real path `devirtualizeStorePath`
       is the identity, so this loop is a no-op on the common case. */
    for (auto & c : context)
        if (auto * o = std::get_if<NixStringContextElem::Opaque>(&c.raw)) {
            auto realPath = devirtualizeStorePath(o->path);
            if (realPath != o->path) {
                allowPath(realPath);
                rewrites.insert_or_assign(store->printStorePath(o->path), store->printStorePath(realPath));
            }
        }

    if (svBatch.empty())
        return rewrites;

    auto resolved = materialisationScheduler->outPathsOf(svBatch);
    for (auto & [placeholder, storePath] : resolved) {
        allowPath(storePath);
        rewrites.insert_or_assign(placeholder.render(), store->printStorePath(storePath));
    }
    return rewrites;
}

std::string EvalState::resolveAndRewrite(std::string text, const NixStringContext & context)
{
    /* The canonical "emit a context-bearing string" choke-point: resolve
       every SourceVirtual placeholder / deferred-mount stand-in in
       `context`, materialise them (so the real paths exist), and rewrite
       `text` to the real store paths in one call. Any NEW serialisation
       boundary (a string written to a `.drv`, a file, an env var, or
       user-facing output) should route its body through here rather than
       re-spelling the resolve→ensure→rewrite triple by hand — the latter
       is exactly how the structuredAttrs/derivation-field leak slipped in
       (a boundary that did part of the dance). `rewriteStrings` is a
       literal substring substitution, so callers that need to rewrite a
       *structured* value (e.g. derivation env + structuredAttrs JSON)
       still apply the returned map field-by-field; for those, call
       `resolveSourceVirtualContext` + `ensureLazyPathsCopied` directly. */
    auto rewrites = resolveSourceVirtualContext(context);
    ensureLazyPathsCopied(context);
    return rewrites.empty() ? text : rewriteStrings(text, rewrites);
}

StorePath
EvalState::mountInput(fetchers::Input & input, const fetchers::Input & originalInput, ref<SourceAccessor> accessor)
{
    /* Known-narHash fast path: if the lock specified the narHash and
       the corresponding store path is already valid (or substitutable
       at low cost), skip the full-tree NAR walk entirely. This is what
       makes lazy clone meaningful on first eval for monorepos:
       `mountInput`'s dryRun walk is what would otherwise force every
       blob to be fetched. The mismatch check moves from "every mount"
       to "the first force boundary that reads narHash" — we still
       fail loudly, just later and only if the consumer cares.

       The path follows master's existing substitution shape: derive
       the CA store path from the recorded narHash, ensure it's
       valid/substitutable, and use it. If `ensurePath` fails (no
       substituter, no local copy), fall through to the dryRun walk.

       Why `getNarHash` (not `peekNarHashAttr`) here: `originalInput`
       comes from the lockfile reader / final-input reconciliation,
       both of which only ever store concrete strings in `narHash`.
       The `LazyAttr` form is introduced exclusively by the slow path
       below; it cannot appear in `originalInput` at this point. So
       `getNarHash` will not actually trigger a force here, and using
       it preserves identical behaviour with master's check. */
    if (auto knownHash = originalInput.getNarHash()) {
        /* Whether the input pins IMMUTABLE content — the precondition for
           deferring the narHash-vs-content check past mount (see the defer
           branch below). A git/treeOID `rev` denotes a fixed object, so a
           rev present is the signal — EXCEPT for a `path:` input, which
           accepts a user-supplied "fake" `rev` attr (path.cc) while
           pointing at a MUTABLE directory; its narHash is only a snapshot.
           So a `path:` input is never treated as immutable here regardless
           of a pasted `rev`, and falls through to the slow path's eager
           verification. */
        bool immutableRev = originalInput.getRev() && originalInput.getType() != "path";
        try {
            auto candidate = store->makeFixedOutputPathFromCA(
                input.getName(),
                ContentAddressWithReferences::fromParts(ContentAddressMethod::Raw::NixArchive, *knownHash, {}));
            store->addTempRoot(candidate);
            if (store->isValidPath(candidate)) {
                debug(
                    "virtual-mount: input '%s' known-narHash fast path, store path '%s' already valid — no walk",
                    input.to_string(),
                    store->printStorePath(candidate));
                allowPath(candidate);
                storeFS->mount(storeMountKey(*store, candidate), accessor);
                input.attrs.insert_or_assign("narHash", knownHash->to_string(HashFormat::SRI, true));
                return candidate;
            }
            /* Try substitution before deferring. `ensurePath` THROWS when
               there is no substituter that can provide the path (the cold
               local-only case) — catch that here so we proceed to the
               defer branch below rather than escaping to the slow-path
               dryRun. Any other store error also falls through to defer:
               we hold the narHash, so the name is sound regardless. Skip
               the substitution attempt entirely for an input that won't
               defer (no `rev`, see the gate below) — no point paying a
               substituter round-trip; the slow path handles it. */
            if (immutableRev) {
                try {
                    store->ensurePath(candidate);
                } catch (Error & e) {
                    debug(
                        "mountInput known-narHash: substitution unavailable for '%s' (%s) — deferring copy",
                        store->printStorePath(candidate),
                        e.what());
                }
            }
            if (store->isValidPath(candidate)) {
                debug(
                    "virtual-mount: input '%s' known-narHash fast path, store path '%s' substituted — no walk",
                    input.to_string(),
                    store->printStorePath(candidate));
                allowPath(candidate);
                storeFS->mount(storeMountKey(*store, candidate), accessor);
                input.attrs.insert_or_assign("narHash", knownHash->to_string(HashFormat::SRI, true));
                return candidate;
            }

            /* Known-narHash DEFER (item (c)): the CA path is neither
               valid locally nor substitutable, but we already hold its
               narHash, so we already know its store-path NAME. Rather
               than fall through to the slow path's eager dryRun walk —
               which would re-derive the very narHash we are holding —
               mint the real `candidate` now (no walk), mount the live
               accessor under it, and DEFER the byte-copy to the first
               hard demand (`ensureLazyPathCopied` at the derivation
               boundary). A consumer that reads only metadata
               (`outPath`/`rev`/`lastModified`) then walks ZERO times; a
               build copies exactly ONCE (the unavoidable ingestion),
               versus the two walks (dryRun-to-name + copy) the slow path
               would pay.

               GATE — `immutableRev` (see its definition above): the input
               carries a git/treeOID `rev` AND is not a `path:` input. This
               is STRICTER than `isLocked`, and than a bare `getRev()`,
               deliberately so. Deferring moves the narHash-vs-content check
               from mount time to the copy boundary, which a metadata-only
               consumer never reaches — so the defer is only sound when the
               content cannot have drifted from the narHash that names it.

               A `rev`-pinned git/github/etc. input satisfies that: the rev
               addresses a fixed object, immutable under us. But the bare
               `getRev()` is NOT sufficient, because a `path:` input accepts
               a user-supplied "fake" `rev` attr (path.cc's allowed-attrs)
               while pointing at a MUTABLE directory whose narHash was only
               a snapshot. If we deferred such a `path:` input and its
               directory changed after locking (without a re-lock), a
               metadata-only read (`.rev`/`.lastModified`) would silently
               accept the stale-locked value instead of throwing the
               NAR-hash mismatch the eager slow path raises. So `path:` is
               excluded by `getType() != "path"` in `immutableRev`, and
               falls through to the slow path, whose `InputMaterialisation`
               carries `expectedNarHash` and verifies eagerly when bytes are
               demanded, exactly as master. (A rev-less tarball/`path:`/etc.
               also falls through — a missed optimisation, not a soundness
               gap; conservative on purpose.)

               An UNLOCKED input that merely CARRIES a `narHash` attr
               (e.g. `fetchTree { url=…; narHash=…; }` with no `rev`) is the
               same: the hash is an UNVERIFIED claim over non-pinned
               content, so it must keep eager verification.

               Soundness of the rev-pinned defer beyond the gate: unlike
               the slow-path Item-2 fake stand-in, `candidate` is the REAL
               canonical CA path keyed on the known narHash, so no fake
               path can persist into a `.drv`, lockfile, or eval cache, and
               it is sound in pure eval too (no §6.4.7(c) hazard). The
               (pathological) stale-lock mismatch — a rev whose recorded
               narHash disagrees with the rev's actual content, i.e. a
               corrupted lock — still fires at the copy boundary
               (`ensureLazyPathCopied`) as a graceful `Error(102)`.
               `devirtualizeStorePath(candidate)` is the identity here
               (candidate is not a stand-in), so the existing copy boundary
               already does the right thing with no extra registration.

               If the input is not `immutableRev` we deliberately do nothing
               here and let control fall out of the fast-path block to the
               slow path below. */
            if (immutableRev) {
                debug(
                    "virtual-mount: input '%s' known-narHash, deferring copy of '%s' (no walk to name)",
                    input.to_string(),
                    store->printStorePath(candidate));
                allowPath(candidate);
                storeFS->mount(storeMountKey(*store, candidate), accessor);
                input.attrs.insert_or_assign("narHash", knownHash->to_string(HashFormat::SRI, true));
                return candidate;
            }
            debug(
                "mountInput known-narHash: input '%s' is UNLOCKED (narHash is an unverified claim) — not deferring; "
                "slow path will verify against expectedNarHash",
                input.to_string());
        } catch (Error & e) {
            debug("mountInput fast-path failed for '%s': %s — falling back to dryRun", input.to_string(), e.what());
        }
    }

    /* Slow path: defer the dryRun walk via a `InputMaterialisation`.
       The mat is the backing object for the `narHash` lazy thunk —
       forcing the thunk drives `mat->force()`, which performs at most
       one walk regardless of how many lazy thunks share the mat.

       For an UNLOCKED input (no `expectedNarHash`) we now also defer
       the *store-path* walk past mount (Item 2 defer-past-mount,
       PROPOSAL.md §6.1.1, Design C): instead of forcing the mat to
       learn a concrete CA path, we mint a content-deterministic fake
       store path, mount the live accessor under it, and return the
       fake. A consumer that reads only metadata (`outPath`/`rev`/
       `lastModified`, never `narHash`, never bytes, never a
       derivation) then walks ZERO times. This closes the one axis
       where DetSys `lazy-trees=true` is more lazy than us (§8.6 gap
       1) — but soundly: the fake path carries a registry discriminator
       (`virtualMounts_`) and is rewritten to the real CA path by
       `ensureLazyPathCopied`/`devirtualizeStorePath` at every hard
       demand, so it never reaches a derivation or the lockfile.

       LOCKED inputs that reach the slow path (the fast path fell
       through because the CA path was neither valid nor substitutable)
       keep the eager force: they have an `expectedNarHash`, they will
       be written to a lockfile (which forces the narHash anyway).

       We restrict the defer with three conjuncts, each closing a
       distinct hazard while keeping the proposal's stated beneficiary
       set (§6.1.1: "--impure / relative / dirty", read metadata-only):

       (1) `!originalInput.isLocked(fetchSettings)` — only TRULY
       unlocked inputs (a git input with no `rev`, a `path:` with no
       narHash, a dirty workdir). A `rev`-pinned `fetchTree` is locked
       and keeps the eager force, so the existing rev-pinned fetchTree
       walk-dedup tests (`whole-input-tree-dedup`, `subtree-dedup`, …)
       are unaffected. This is also the exact DetSys gap-1 axis.

       (2) `!originalInput.hasNarHashAttr()` — an unlocked input that
       nonetheless carries an explicit `narHash` (the "unlocked but
       checked by NAR hash" warning case) keeps the eager force so its
       `mat`'s `expectedNarHash` mismatch check fires at eval time, as
       before.

       (3) `!settings.pureEval` — in pure eval the only input that can
       reach this slow path unlocked is the flake-self of a relative
       `path:` flake; such a flake CAN have a fully-locked `flake.lock`
       and thus an eval cache, into which a fake `Opaque` store path
       would persist unsoundly (PROPOSAL.md §6.4.7(c) reason 2). Impure
       eval has no eval cache (`useEvalCache && pureEval` gate, see
       `openEvalCache`), so the fake path can never persist there.
       Extending the defer to the pure-eval flake-self is a sound
       follow-up gated on re-adding the eval-cache-write
       devirtualisation guard. */
    auto deferStorePath =
        !originalInput.isLocked(fetchSettings) && !originalInput.hasNarHashAttr() && !settings.pureEval;

    auto mat = make_ref<InputMaterialisation>(
        fetchSettings, store, accessor, input.getName(), originalInput.peekNarHashAttr());

    /* Pin the mat in `inputMaterialisations_` keyed on the input's pre-narHash
       attrs JSON. The key is computed via `attrsToJSONForKey` so two
       mounts of the same input share one mat — the lazy thunk we
       install below points at that single mat, so `mat->force()` is
       walked at most once across all consumers. We use `originalInput`
       (not `input`) because `input.attrs` may already contain a
       narHash from an earlier reconciliation pass; the lazy form
       always overwrites whatever's there. */
    auto matKey = fetchers::attrsToJSONForKey(originalInput.attrs).dump();
    {
        auto mats = inputMaterialisations_.lock();
        auto [it, inserted] = mats->try_emplace(matKey, mat);
        if (!inserted)
            mat = it->second;
    }

    /* Compute the store path. For a locked-but-fell-through input we
       force the mat now (its mismatch-against-`expectedNarHash` check
       fires here, exactly as before). For an unlocked input we DEFER:
       mint a content-deterministic fake store path from the same
       pre-narHash `inputMaterialisations_` key (`matKey`), so no walk runs.

       The fake hash is `SHA256(matKey)`, fed through
       `makeFixedOutputPathFromCA` with the same method/refs the real
       CA path uses — so the fake path is shaped exactly like a real
       NAR-CA store path (single `<hash>-<name>` component, routable
       through `storeMountKey`/`rootFS`) but is keyed off the input's
       *identity* rather than its *content*. It therefore never
       collides with the real CA path (which is keyed off the narHash),
       guaranteeing `devirtualizeStorePath`'s rewrite is a genuine
       substitution. The `virtualMounts_` registry below is the
       discriminator; see its doc-comment for why this is not the
       §6.4.7(c) failure mode. */
    auto storePath = [&] {
        if (!deferStorePath) {
            debug(
                "virtual-mount: input '%s' slow path, forcing materialisation at mount (NOT deferred: locked or "
                "narHash-checked or pure-eval) — walks only on a cache miss (see the following cache-hit / 'hashing' "
                "lines)",
                input.to_string());
            return mat->force().first;
        }
        auto fakeHash = hashString(HashAlgorithm::SHA256, matKey);
        auto fakePath = store->makeFixedOutputPathFromCA(
            input.getName(),
            ContentAddressWithReferences::fromParts(ContentAddressMethod::Raw::NixArchive, fakeHash, {}));
        debug(
            "virtual-mount: input '%s' DEFERRED past mount (unlocked) — fake path '%s', no walk until first hard demand",
            input.to_string(),
            store->printStorePath(fakePath));
        return fakePath;
    }();

    allowPath(storePath); // FIXME: should just whitelist the entire virtual store
    storeFS->mount(storeMountKey(*store, storePath), accessor);

    /* Record the fake → mat association so `ensureLazyPathCopied` /
       `devirtualizeStorePath` can force the walk and rewrite to the
       real CA path at the first hard demand. (No-op for the eager
       branch: `storePath` is already the real path there, so it is
       never a deferred stand-in.) */
    if (deferStorePath) {
        auto mounts = virtualMounts_.lock();
        mounts->insert_or_assign(storePath, mat);
    }

    /* Install the lazy thunk into `input.attrs["narHash"]`. Future
       `emitTreeAttrs` calls observe the LazyAttr branch and emit a
       Nix-language thunk; consumers that don't read `.narHash` skip
       the lazy thunk's body entirely. */
    input.attrs.insert_or_assign("narHash", makeVirtualNarHashAttr(mat));

    return storePath;
}

} // namespace nix
