# E1 — `impureOutputHash` deletion verification

**Charter:** Confirm that `nix::impureOutputHash`, defined in
`src/libstore/derivations.cc` as
`const Hash impureOutputHash = hashString(HashAlgorithm::SHA256, "impure");`,
is genuinely dead and safe to delete as a standalone PR.

**Verdict (TL;DR):** Safe to delete with no deprecation cycle and no
downstream coordination required. The symbol has internal linkage,
is absent from every public header, was orphaned in March 2022 by
commit `50912d02e` ("Get rid of `impureOutputHash`") which removed
the header declaration and every caller but accidentally left the
namespace-scope definition behind, and the lix fork has already
dropped it independently.

---

## In-tree references

`grep -rn impureOutputHash` (entire worktree) returns three
classes of hits:

- **Definition (1 site).** `src/libstore/derivations.cc`, namespace
  scope:
  `const Hash impureOutputHash = hashString(HashAlgorithm::SHA256, "impure");`
  This is the sole use of the identifier as a C++ symbol.
- **Documentation (catalog/review notes).** Multiple hits in
  `doc/inventory/candidates/15-globals-settings.md`,
  `doc/inventory/verified/06-libstore-derivations.md`, and
  `doc/inventory/review/03-adversarial-existing-deeper.md` and
  `05-consolidation-log.md`. These are the catalog entries
  themselves; they do not consume the symbol.
- **Declarations / readers / writers / extern decls.** Zero. The
  identifier appears nowhere in `src/libstore/include/`, nowhere
  in any other `.cc`/`.hh`, and in no `tests/` file.

The unrelated `"impure"` string literal appears several times in
`derivations.cc` for JSON/serialisation key names (e.g. `if (hashS == "impure"sv)`,
`res["impure"] = true`); these are unrelated to the
`impureOutputHash` Hash value and do not depend on it.

## Public-header exposure

**Not exported via headers.** `grep -rn impureOutputHash src/libstore/include/`
returns no matches. There is no `extern const Hash impureOutputHash;`
declaration anywhere in the installed-header tree, and there never
has been on `master` since commit `50912d02e` (March 2022; see
"Historical context" below). Downstream code attempting
`extern const Hash impureOutputHash;` in a translation unit of its
own would compile but fail to link, because the symbol has internal
linkage in `derivations.o` (see next section).

## Symbol export from `libnixstore`

**Symbol has internal linkage and is not exported.** Inspection
of the locally-built `build-tsan/src/libstore/libnixstore.dylib`
with `nm`:

- `nm -m`  reports the symbol as `non-external` in `__DATA,__bss`,
  with mangled name `__ZN3nixL16impureOutputHashE` — the embedded
  `L` after the namespace `3nix` is the Itanium ABI marker for
  *internal linkage* (`static` or anonymous-namespace), confirming
  the toolchain treated this definition as internal.
- `nm -g` (exported globals only) returns no match for the symbol.

The reason is a C++ language rule, not a build-system flag:
`const` namespace-scope variables have *internal* linkage by default
unless explicitly declared `extern` (`[basic.link]/3.2`). The
definition site uses neither `extern` nor a prior `extern`
declaration, so the symbol is per-translation-unit. No build-system
visibility flags are set in `nix-meson-build-support/export-all-symbols/meson.build`
(its `linker_export_flags` only fires on Cygwin/Windows DLLs), and
the libstore meson.build does not pass `-fvisibility=hidden`, but
neither matters here — the linkage is internal at the language
level.

**Consequence:** the symbol cannot be resolved by any
out-of-tree consumer linked against `libnixstore`, regardless of
how that consumer declares it. Deletion cannot be an ABI break,
because the symbol was never part of the ABI surface.

## Downstream consumers

Surveyed two known forks/embedders by shallow clone and grep:

- **lix** (`https://git.lix.systems/lix-project/lix`, fork point
  late 2023 / early 2024). `grep -rn impureOutputHash` over the
  full clone returns **zero matches**. lix already removed the
  orphan definition independently — `lix/libstore/derivations.cc`
  exists and does not contain the line.
- **NixOS/hydra** (`https://github.com/NixOS/hydra`).
  `grep -rn impureOutputHash` over the full clone returns
  **zero matches**. Hydra does not reference the symbol.

I cannot exhaustively survey private/proprietary embedders, but
the language-level internal-linkage finding above makes it
impossible for any such embedder to be linked against this symbol
today. An embedder that *re-defines* `nix::impureOutputHash` in
its own translation unit would also be unaffected by deletion in
upstream Nix, since its definition is local to its own object
file.

## Historical context

`git log --all -S impureOutputHash -- src/libstore/derivations.cc`
on `master` shows two relevant commits:

- **`5cd72598f` ("Add support for impure derivations", 2022).**
  Introduced the symbol along with a header declaration
  `extern const Hash impureOutputHash;` in `src/libstore/derivations.hh`
  and a use site in `hashDerivationModulo` that, for impure
  derivations, populated every output's hash with this constant
  to produce a `DrvHash::Kind::Deferred` modulo-hash.
- **`50912d02e` ("Get rid of `impureOutputHash`", John Ericson,
  2022-03-31).** Removed the `extern` declaration from
  `derivations.hh` and the `hashDerivationModulo` use site that
  populated `outputHashes` with `impureOutputHash`, replacing the
  Impure branch with `return DrvHash::Kind::Deferred;`. The
  commit message argues this hash-modulo construction was
  "very suspicious because two almost-equal derivations that only
  differ in depending on different impure derivations could have
  the same drv hash modulo". The commit's *intent* was full
  removal — the title is "Get rid of `impureOutputHash`" — but
  the namespace-scope definition in `derivations.cc` was missed
  and left orphaned. No subsequent commit on `master` re-added
  any reader, and `git log -S "const Hash impureOutputHash" -- src/libstore/derivations.cc`
  shows the line has been continuously present (just orphaned)
  since `5cd72598f`. The 2023 commit `5334c9c792` ("HashType:
  Rename to HashAlgorithm") rewrote `htSHA256` to
  `HashAlgorithm::SHA256` on this line as part of a
  tree-wide enum rename, but did not change its semantics or
  reachability.

So the symbol has been dead since 2022-03-31, ~4 years, surviving
multiple revert/reapply cycles around `hashDerivationModulo`
(`4f91e9599` revert, `100e7cc33` reapply, `faca7db63` revert)
because none of those touched the orphan definition either.

## Recommendation

**Plain deletion as a standalone PR.** No deprecation cycle, no
downstream coordination, no `[[deprecated]]` annotation needed:

1. The symbol has internal linkage (C++ language rule for `const`
   at namespace scope), so it is not part of `libnixstore`'s ABI
   surface and cannot be linked against by any external consumer.
2. The symbol is not declared in any installed header, so no
   compilation against the public headers can reference it.
3. Both surveyed downstream consumers (lix, hydra) contain zero
   references to it.
4. The 2022 commit that intended removal is on master and merely
   left the definition behind by oversight — finishing that
   removal is faithful to the original author's intent.

The PR should: (a) remove the single line from `src/libstore/derivations.cc`,
(b) credit `50912d02e` in the commit message as completing
that removal, and (c) reference this verification as evidence
that the orphan has been safely dead for ~4 years.

**Risk:** negligible. The only failure mode would be a
*proprietary* embedder that has copied the definition into its
own header and relied on linkage via Nix's translation unit —
but that is foreclosed by the internal-linkage analysis above
(the embedder's `extern` declaration would never have linked
against Nix's definition; if their build worked, they have their
own definition).
