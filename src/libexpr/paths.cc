#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/input-materialisation.hh"
#include "nix/expr/materialisation-scheduler.hh"
#include "nix/util/mounted-source-accessor.hh"
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

    /* TODO: We could memoise this in-memory if necessary. */
    auto storePath = fetchToStore(
        fetchSettings,
        *store,
        SourcePath{ref(mount)},
        /* Force a copy. mountInput does a dryRun to just calculate the storePath and narHash. */
        FetchMode::Copy,
        realPath.name());

    /* Catch hash mismatches more loudly. This is more likely caused by unsound
       caching of different accessor types that fetch the same repo with
       the same git revision, but with different kinds of accessors (think
       tarball-based fetchers vs local/remote git accessors). */
    if (storePath != realPath) {
        panic(fmt(
            "hashed store path computed by the evaluator ('%1%') does not match what was computed when copying to the store ('%2%'), this is a bug",
            store->printStorePath(realPath),
            store->printStorePath(storePath)));
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
            /* Try substitution before falling through. */
            store->ensurePath(candidate);
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
