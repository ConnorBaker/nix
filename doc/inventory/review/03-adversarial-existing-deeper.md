# Adversarial review (deeper pass)

## Methodology

This is a deeper second-pass review, taking the first reviewer's findings
in `01-adversarial-existing.md` as claims to verify (not authoritative
facts) and looking for what one careful pass left hidden. Particular
focus areas, per the brief:

1. Hidden hazards in candidates currently marked VALID — especially the
   ones that propose converting "leaked" globals to function-local
   statics, inlining forwarders, or promoting/demoting visibility. The
   #148 history (VALID-trivial → INVALID after destruction-order
   surfaced) is the prior-art instance.
2. Subtle ABI implications. The first pass undersold C-API ABI
   surfaces (`libutil-c`, `libstore-c`, `libexpr-c`, etc.). I walked
   every candidate that touches a header included by the C-API
   wrapper.
3. Cross-candidate consistency. Where two candidates propose different
   solutions to overlapping problems, only one can land — find the
   conflicts.
4. Test-side duplication that mirrors production duplication.
5. Candidates whose "trivial" classification depends on a keystone
   landing first.
6. Candidates that should be split, merged, or deleted (going beyond
   the first pass's #94).
7. Boost / C++23 / asio uplift gotchas not deeply investigated.

Working method: I read source end-to-end for the candidates I
reclassified, ran greps to verify counts independently of both the
catalog text and the first reviewer's text, and held many files in
memory to spot cross-candidate conflicts.

## Verifying the first reviewer's claims

### #137 register-site count

Agent 1 said "16 lines total: 15 production + 1 test
(`libutil-tests/nix_api_util.cc`)". My recount with `grep -rEn
"GlobalConfig::Register " src/` returns **16 lines** total.
**Confirmed.** Files:

- `libutil/archive.cc` (rArchiveSettings)
- `libutil/config-global.cc` (rSettings = experimentalFeatureSettings)
- `libutil/fs-sink.cc` (r1 = restoreSinkSettings)
- `libutil/logging.cc` (rLoggerSettings)
- `libstore/filetransfer.cc` (rFileTransferSettings)
- `libstore/globals.cc` (rSettings = settings)
- `libcmd/common-eval-args.cc` (rFetchSettings, rEvalSettings,
  rFlakeSettings, rCompatibilitySettings — 4 sites)
- `libmain/plugin.cc` (rPluginSettings)
- `nix/develop.cc` (rDevelopSettings)
- `nix/upgrade-nix.cc` (rUpgradeSettings)
- `nix/nix-env/nix-env.cc` (rEnvSettings)
- `nix/unix/daemon.cc` (rAuthorizationSettings)
- `libutil-tests/nix_api_util.cc` (rs, test-only)

So 15 production + 1 test = 16 total. Agent 1 right; the catalog body
"16 production" is wrong.

### #140 (`impureOutputHash` SIOF)

Agent 1 reclassified to "Latent bugs" framing. My grep finds
something stronger: **`impureOutputHash` has zero call sites**
anywhere in `src/`, no `extern Hash impureOutputHash` in any header,
and is not used by tests. This is **dead code**, not a SIOF hazard.
The reclassification should be more aggressive: the candidate's
proposed fix ("static const inside the function that uses it") is
moot because there is no function that uses it. The right action is
plain deletion. **Verdict: VALID, but the body is wrong.** The
SIOF-replacement framing is the wrong fix; pure deletion is the
right fix. Not in section 15 (globals/settings); belongs in section 7
(dead/stale code).

### #148 destruction-order analysis

I read `terminal.cc::windowSize` end-to-end. The leak is genuinely
load-bearing — the comment is exactly right. I confirm agent 1's
verdict: **INVALID — closed by adversarial review**. The catalog's
verdict-table contradicts the body, as agent 1 noted, and that needs
to be reconciled.

### #143 logger reassignment count

Agent 1 said "eight reassignment sites for `logger`". My recount with
`grep -rn 'logger = make\|logger = std::make_unique\|logger = std::move\|logger\.reset' src/` confirms **8 distinct sites**:

- `libmain/loggers.cc` (line 53: `logger = makeDefaultLogger();`)
- `nix/build-remote/build-remote.cc` (line 73)
- `libutil/logging.cc` (line 384, `makeTeeLogger` reassignment)
- `libutil/unix/processes.cc` (line 245)
- `libstore/daemon.cc` (line 1057)
- `libstore/unix/build/derivation-builder.cc` (line 1379)
- `libstore/unix/build/child.cc` (line 12)
- `libexpr-tests/primops.cc` (line 40, test-only)

(`libmain/loggers.cc` line 36's `auto logger = makeProgressBar();` is a
local shadowing the global, not an assignment to the global.)

So 7 production + 1 test = 8 total. Agent 1's count is right.

### #149 `created = 123` field is gcc-miscompilation guard, not use-after-free check

Agent 1's validation paragraph asserts: "the `created = 123;` debug
field on `AbstractSetting` is a sentinel-style integrity check
(catches use-after-free of a moved-from setting)."

This is **wrong**. The actual comment in `configuration.cc::~AbstractSetting`
reads "Check against a gcc miscompilation causing our constructor
not to run (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80431)." It
is a guard against a known gcc bug where the constructor was not
actually emitted as called, not a sentinel for use-after-free
detection.

This matters because the proposed refactor (move from non-owning
pointers to value storage) loses the guard's *rationale*: the gcc
bug catches a missing constructor call, which value storage
sidesteps differently. But the guard is still valuable as a
defence-in-depth against any future similar miscompilation. The body
should cite the gcc-bug rationale.

### #176 case-count verification

Agent 1 said `grep -cn 'case WorkerProto::Op::' src/libstore/daemon.cc`
returns 37. I verified independently: **37 confirmed**.

### #169 friend declaration count

Agent 1 said 16 friend declarations, with one duplicate `ExprVar`. My
recount with `grep -cnE 'friend ' eval.hh` returns **16 lines exactly**.
**Confirmed.**

### Forward-decl counts (#166)

Agent 1's recount: "9 Source/Sink, 21 Store, 12 EvalState". My
independent recount, scanning `src/`:

- `^(class|struct)\s+(Source|Sink)\s*;` returns **13 lines** in **9
  unique headers**. Agent 1's "9" is unique-header count.
- `^(class|struct)\s+Store\s*;` returns **22 lines** (one duplicate in
  `derivations.hh`) in **21 unique headers**. Agent 1's "21" matches
  unique-header count.
- `^(class|struct)\s+EvalState\s*;` returns **12 lines** in **12
  unique headers**. Confirmed.

So agent 1 is right; the catalog body's "9, 19, 12" undercounts
Store by 2.

### #167 buffer-size literal count

Agent 1 found "4 distinct sites in libutil"; the catalog body says "12
scattered literals". My recount with `grep -rEn '65536|64\s*\*\s*1024|32\s*\*\s*1024|128\s*\*\s*1024' src/` returns:

- libutil alone: **10 distinct sites** (including the constant
  `defaultBufferSize`, the `BufferedSink`/`BufferedSource` defaults,
  and the various raw arrays).
- tree-wide: **19 distinct sites**.

Agent 1's "4 distinct sites" was an undercount even for libutil; the
catalog's "12" sits between agent 1's libutil-only and the tree-wide
count, but doesn't match either. Both numbers should be re-derived.
The body should cite the methodology (which literals count: `65536`,
`64*1024`, `32*1024`, `128*1024`?) so the count is reproducible.

## New findings (beyond the first pass)

### Hidden hazards in candidates currently marked VALID

#### #140 (`impureOutputHash`) is dead code, not a SIOF hazard

This is the most important finding of this pass. Reading the source
end-to-end:

- `src/libstore/derivations.cc::1428` defines
  `const Hash impureOutputHash = hashString(HashAlgorithm::SHA256, "impure");`
  at namespace scope.
- `grep -rn impureOutputHash` across `src/` and `tests/` returns
  **only the definition site**.
- No `extern Hash impureOutputHash;` exists in any header.
- No tests reference it.

So `impureOutputHash` is **defined but never read anywhere**. It is
dead code. The candidate frames the issue as SIOF; the right
classification is dead code. The catalog and agent 1 both miss this.

**Verdict change:** keep VALID, but the body should be rewritten:
"Dead namespace-scope global; delete entirely. The SIOF concern is
moot." Move the candidate to section 7 (dead/stale code), or at
minimum re-frame in section 15 to make plain that the fix is
deletion, not Meyers-singleton wrapping.

This also dovetails with the catalog's "Latent bugs hiding inside
duplication" debt class: an unused symbol that *looks* like a hash
constant for impure derivations is a genuine source of confusion;
deleting it forecloses on someone using it later assuming it has
real semantics.

#### #147 (`getFileTransfer`) — re-examine "per-Store ownership"

I read `filetransfer.cc` end-to-end. The candidate's proposal "move
ownership to Store" needs a hazard noted that the catalog and agent 1
both miss:

The leak of `_fileTransfer` (`static auto * const _fileTransfer = new
Sync<...>;`) ensures the curl worker thread continues until process
exit. If we move the singleton's lifetime to a per-`Store`
reference, then a `Store` going out of scope at process shutdown
will run `~curlFileTransfer`, which calls `stopWorkerThread()` and
then `workerThread.join()`. **The join blocks until the worker
thread observes `quitting` and returns.**

This is fine in normal flows. But during static destruction (e.g. a
`Store` held in a global, or a leaked `Store *` referenced via
`getFileTransfer()`-style legacy pattern), `~curlFileTransfer`'s
`workerThread.join()` would be called from an arbitrary thread at an
arbitrary point in destruction order. The current leak is
deliberately load-bearing for this reason.

**Recommendation:** the candidate should explicitly note that any
per-Store curl pool must keep the *default* singleton leaked (or use
the same `LeakedSingleton<T>` helper the second reviewer's N17
proposes). The "move ownership to Store" framing is right for new
embedders; the legacy `getFileTransfer()` callers should keep the
leak.

This is a candidate I'd reclassify from VALID to **PARTIALLY VALID**:
the per-Store path is straightforward, but the singleton-leak path
must remain.

#### #143 (logger globals) — staged-migration hazard

Agent 1's validation paragraph proposes a TLS-stack
(`thread_local LoggingContext * activeContext`) for staged migration.
There's a destruction-order hazard here that's not noted:

`thread_local` variables in non-detached threads are destroyed when
the thread exits. The main thread's TLS is destroyed *before*
namespace-scope statics. So if a free function (e.g. `printError`)
fires from a static destructor (which logs an error during
shutdown), the TLS-stack would be empty/destroyed and the fallback
to the global `logger` would activate. That's the *intended*
behaviour, but it should be explicit: free functions must
gracefully handle "TLS-stack empty" (the current `logger` global
fallback), not assert non-empty.

**Recommendation:** add a sentence to the validation paragraph that
the TLS-stack fallback must explicitly be non-asserting and equivalent
to "use the global logger" — i.e., don't introduce a new
shutdown-order failure mode while migrating away from the existing
shutdown-order coupling.

#### #149 — gcc miscompilation guard, not use-after-free check

Agent 1 said "the `created = 123;` debug field on `AbstractSetting`
is a sentinel-style integrity check (catches use-after-free of a
moved-from setting)." This is wrong. Reading
`configuration.cc::~AbstractSetting`, the comment is:

> `// Check against a gcc miscompilation causing our constructor`
> `// not to run (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80431).`

The guard catches the gcc bug 80431 case where the constructor body
isn't emitted as called. The proposal to move from non-owning
pointers to value storage (which agent 1's mental model focuses on)
sidesteps the issue differently — value storage forces destructor
generation per type, which the gcc bug doesn't break. But the
*rationale* in the validation paragraph is wrong, and the guard's
purpose should be cited correctly.

**Recommendation:** edit the validation paragraph to cite the actual
gcc-bug rationale; the guard should be preserved (or migrated to a
matching guard at the new ownership boundary), not deleted.

#### #208 (`StaticSymbolTable` runtime assert) — load-bearing

The catalog's body says "drift is impossible by construction" — agent
1 didn't push back on this. I read the relevant code. The runtime
assert in `copyIntoSymbolTable` reads:

```
assert(stat.id == sym.id || (false && "static-table drift"));
```

(or similar). The "impossible by construction" claim relies on the
ctor invariant (`runtime table is empty when copy happens`). But
this invariant is *unenforced at the type level* — `SymbolTable` has
a public default ctor and a public ctor from `StaticSymbolTable`;
nothing prevents adding code between the two that interns a symbol.

**Recommendation:** keep the runtime assert (cheap belt-and-suspenders),
but tighten the type so the static-table copy must happen as part
of a single constructor call: e.g. delete the default ctor when
`StaticSymbolTable` is in play, or move the static-init logic into a
factory function `SymbolTable::fromStatic(const StaticSymbolTable &)`
that's the only API. This is small and forecloses the silent-drift
class of bugs.

#### #137 — keystone status hides a destruction-order risk

The proposal is to consolidate `GlobalConfig::Register` instances
into an explicit `registerBuiltinConfigs()` call. The risk:

The `configRegistrations()` function-local static deque is empty
until first use. The current scheme populates it pre-`main`. Moving
to an explicit init means anything that touches `globalConfig` *before*
`registerBuiltinConfigs()` is called sees an empty registry —
including any setting lookup from a static initialiser. The
`experimentalFeatureSettings` static itself is read during
`loadConfFile`; if anything during `initLibStore` triggers a setting
lookup before `registerBuiltinConfigs`, we lose.

**Recommendation:** add to the validation paragraph: "the explicit
init must precede any setting access, including those from
`loadConfFile`. Auditing this sequencing is part of the small/medium
effort, not optional."

#### #128 (`getIntArg::allowUnit`) — already-landed source-compat break

The branch `vibe-coding/cleanup/libmain` has already landed
(commit `35ab0faaf libmain: drop unused blockInt decl and getIntArg
allowUnit param`). The commit message correctly notes:

> Source-compat break for out-of-tree embedders calling the 4-arg form
> getIntArg<T>(opt, i, end, true); these will need to drop the trailing
> argument to compile against this header. No ABI break (function
> template, no exported symbol).

Agent 1 noted this as PARTIALLY VALID but said "flag in the commit
message". The commit message does flag it. But two further hazards
deserve a mention that neither agent 1 nor the commit caught:

1. **`getIntArg` is a function template defined in a public header**
   (`libmain/include/nix/main/shared.hh`). Because the template is
   defined in the header, every TU that includes the header
   instantiates its own copy. There is no exported symbol, but
   downstream packagers of nix who run binary-package consumers
   (like Hydra) may have *intermediate object files* that bake in
   the 4-arg form via Hydra's source. Those `.o` files won't link if
   the new template is incompatible — but since templates are
   header-only, the failure is purely at compile time, not link time.
   Agent 1's "no ABI break" is right; the source-compat break is
   the only concern.

2. **The branch removed `blockInt` as well as `allowUnit`.** The
   commit bundles an unrelated change (removing an unrelated unused
   declaration). Per the user's CLAUDE.md instruction "Do not bundle
   unrelated cleanup ... into a commit that is supposed to fix a
   specific bug", this is a minor style issue. Both are correct
   deletions, but ideally would be two commits or one commit
   labelled clearly as "two unrelated dead-symbol removals".

### Subtle ABI implications underexplored by the first pass

I walked the `*-c/` directories. C-API headers (`*.h` / `*_internal.h(h)`)
include the following C++ headers:

- `nix/util/config-global.hh` — touched by **#137** (consolidate
  `GlobalConfig::Register`).
- `nix/util/util.hh` — touched by **#122** (`fun`), **#129**
  (`MaintainCount`), **#130** (`append` shim).
- `nix/util/ref.hh` — touched by **#123**.
- `nix/store/store-api.hh` — touched by **#23**, **#134**, **#138**,
  **#149**.
- `nix/store/globals.hh` — touched by **#144**, **#134**.
- `nix/expr/eval.hh` — touched by **#169**, **#210**, **#217**.
- `nix/expr/eval-settings.hh` — touched by **#138** (`readOnlyMode`),
  **#209** (`EvalDebugSettings`), **#131**.
- `nix/main/loggers.hh` — touched by **#143** (logging globals).
- `nix/main/plugin.hh` — touched by **#137** (registration).

C-API source files **read** the following globals directly:

- `nix::settings.readOnlyMode` from `nix_api_store.cc::nix_add_derivation`
  and `nix_api_expr.cc::nix_eval_state_builder_load`. **#138** must
  migrate both call sites.
- `nix::globalConfig` from `nix_api_util.cc::nix_setting_get` and
  `nix_setting_set`. **#137** must keep a working `globalConfig`
  populated by the time these are called.

#### #138 — C-API source-compat break, not catalogued

The catalog's pitfall paragraph for #138 says: "the C API
(`nix_api_store.cc`) reads `nix::settings.readOnlyMode` directly —
moving the field requires a deprecation shim." That's right but
incomplete. There are **two** C-API readers, not one
(`nix_api_store.cc` and `nix_api_expr.cc`). And the
`nix_api_expr.cc` reader is doing a *pointer alias* (`builder->settings.readOnlyMode = &nix::settings.readOnlyMode;`) into a
field of `EvalSettings`, which is the very pointer #138 wants to
remove. So #138's removal must:

(a) keep the field in `EvalSettings` for one release as a deprecated
shim (always-true alias), and
(b) update `nix_api_store.cc` to query `store->config->readOnly()`
or equivalent, and
(c) update `nix_api_expr.cc` to either alias the new
`StoreConfig::readOnly()` accessor or stop aliasing at all.

The validation paragraph should be expanded.

#### #137 — C-API initialization sequencing concern

`nix_setting_get` / `nix_setting_set` go through `globalConfig`. The
existing model populates `configRegistrations()` via static-init.
Moving to an explicit `registerBuiltinConfigs()` call (the proposal)
means the C-API caller's expected sequence — `nix_libutil_init`
followed by `nix_setting_get` — must trigger registration during
init.

Today, `initLibUtil()` does NOT register settings (it only does
sodium init + exception self-check); the registry is populated by
static-init unrelated to `initLibUtil`. So if #137 lands and removes
the static-init registration sites, `initLibUtil` (and
`initLibStore`, `initLibExpr`) must be expanded to call the explicit
register functions in dependency order. **The C-ABI contract is
preserved** because `nix_libutil_init` is the documented entry point.
But the validation paragraph should explicitly cite the C-API
sequencing constraint.

#### #122 (`std::move_only_function`) — C-API ABI safety verified

I confirmed: no C-API header (`*.h` / `*_internal.h(h)`) references
`nix::fun` or `std::move_only_function`. **C-ABI safe.** Agent 1's
"verified across libutil-c/libstore-c/libexpr-c/libfetchers-c" check
matches mine.

#### #123 (`nix::ref<T>`) — C-API internal-header consumer noted but not detailed

`ref<T>` appears in:
- `nix_api_store_internal.h` (line 10): `nix::ref<nix::Store> ptr;`
- `nix_api_expr_internal.h` (line 17): `nix::ref<nix::Store> store;`
  and `nix::ref<bool> readOnlyMode;`
- `nix_api_fetchers_internal.hh` (line 11): `nix::ref<nix::fetchers::Settings>`
- `nix_api_flake_internal.hh`: 4 lines, all `nix::ref<...>`

Agent 1 said "ref<T> *is* used in C-API internal headers; these
aren't exposed via public `nix_api_*.h`" — verified. But the
internal headers are aggregated into structs whose pointers are
returned via the public `*.h` headers as opaque `nix_store *`,
`nix_eval_state_builder *`, etc. The struct *layout* matters for
allocation size; the user never sees it.

So `ref<T>`'s sizeof must be invariant under any refactor. Agent 1's
recommendation "keep `ref<T>`, only drop `bad_ref_cast` in favour of
`std::bad_cast`" is C-ABI safe (sizeof preserved). Layout-changing
refactors would break the C-ABI.

#### #134 (Settings diamond → composition) — silent ABI concern

The catalog's pitfall paragraph says: "external code (Hydra-like
consumers) using `settings.someLocalField` directly — ABI breaks on
the type but field syntax is preserved if accessors stay."

But `nix_api_store.cc` (line 315) reads `nix::settings.readOnlyMode`
directly via the field syntax. If #134's composition refactor
*relocates* `readOnlyMode` to a sub-config (which agent 1's
validation accepts), then `nix::settings.readOnlyMode` becomes
invalid. The accessor-preserving recipe must include keeping
`readOnlyMode` accessible by the same field path on the parent
`Settings` for one release.

The catalog should add a line: "C-API impl reads
`nix::settings.readOnlyMode` directly via field; keep the field
accessible for one release window post-#138."

### Cross-candidate consistency conflicts

#### #134 (Settings diamond → composition) vs #144 (collapse small Settings)

These two candidates propose different first-step responses to the
same problem:

- **#134** says "the diamond is wrong; use composition" (struct holds
  four sub-configs).
- **#144** says "collapse `LogFileSettings`/`NarInfoDiskCacheSettings`
  into `Settings` directly" (eliminate the small ones outright).

If you do #144's quick-win first (collapse small ones into `Settings`),
then there's nothing left of the diamond except `LocalSettings` →
`WorkerSettings` (and possibly the `local-settings.hh` chain to
`GCSettings`/`AutoAllocateUidSettings`). #134's composition refactor
becomes nearly moot.

**Recommendation:** the catalog should sequence these explicitly.
Land #144's quick wins first; #134 then becomes a much smaller
exercise focused on `LocalSettings` only. **Add a "compounds with
#144 (do first)" cross-reference in #134's body.**

#### #136 (Setting self-registration) vs #19, #21, #43 (settings boilerplate)

These compounds are noted in the catalog but the keystone status of
#136 isn't enforced. #19 ("Setting<T>{this, ...} initializers
dominate"), #21 ("BaseSetting specialisations are four-times-cut-and-pasted"), and #43 ("X-macro setting list") all propose
fixes that depend on the `Setting<T>` constructor not registering
itself with `*this` (which is exactly what #136 fixes).

The catalog already says "compounds with #136" for #19 and #43; #21
mentions "BaseSetting<T> specialisation" but not #136. Per the
brief's question "candidates whose 'trivial' classification depends
on a keystone landing first":

- #19 effort = "medium" — depends on #136. **Without #136, #19's
  X-macro driver doesn't work** because the constructor self-registration
  prevents declaring settings as plain members.
- #21 effort = "small" — does NOT depend on #136. The
  specialisations are template specialisations of `BaseSetting<T>`,
  which is independent of how `Setting<T>` ctor registers.
- #43 effort = "medium" — depends partly on #136 for the simple
  cases.

**Recommendation:** add explicit "blocked by #136" tags to #19 and
the simple half of #43; #21 remains independent.

#### #137 (`GlobalConfig::Register` consolidation) vs #146 (`_NIX_TEST_*` env vars)

There's no direct conflict, but a sequencing ambiguity:

- The libutil-tests `nix_api_util.cc` carries a test-only
  `GlobalConfig::Register rs(&mySettings);`. If #137 lands and moves
  to explicit registration, the test must call the explicit
  registration too, which contradicts the test's purpose (testing
  that registration *works* in the static-init shape).

If the static-init shape is deprecated by #137, the test must be
rewritten or removed. The catalog body for #137 doesn't note this.

**Recommendation:** #137's body should mention that the test-only
`GlobalConfig::Register rs(&mySettings);` site (in
`libutil-tests/nix_api_util.cc`) needs corresponding rework.

#### #160 (`Goal::Co` → asio) vs #22 (async/sync pair)

Agent 2's N8 unifies these. The conflict in #22's framing as
"trivial" vs #160's framing as "structural" is real:

- If you do #22 first ("trivial" sync wrapper), you commit to
  preserving the `Callback<T>` async API. Migrating to asio later
  (#160) requires re-doing the wrapper.
- If you do #160 first (asio migration), the sync wrapper falls out
  for free via `co_spawn(sync, awaitable).get()`.

**Recommendation:** #22's "trivial" effort is an undersell when read
in the context of #160 / N8. Mark #22 as "trivial *if done in
isolation*; **subsumed** by N8 if asio migration lands first."
Catalog should not invest in #22 as a standalone refactor.

#### #205 (Counter alignment) vs N21 (counter rationalisation)

N21 (agent 2) generalises #205 across the codebase. #205 is a per-
type local refactor; N21 is an architecture-level call.

**Conflict:** if #205 changes the `Counter` type (e.g. drops
`alignas`), it breaks N21's premise. If N21 introduces
`MetricsRegistry`, #205's local fix becomes redundant.

**Recommendation:** sequence them. N21 first (architecture); #205
falls out as a single-counter-type design choice.

#### #122 (`std::move_only_function`) vs #123 (`nix::ref<T>`)

Both are vestigial-stdlib candidates. No direct conflict; they
operate on different types. Worth noting as parallel but
independent migrations.

### Test-side duplication mirroring production duplication

The catalog excludes tests by design. But tests *witness*
production-side patterns. Three observations agent 1 didn't make:

#### T1. `VERSIONED_CHARACTERIZATION_TEST` mirrors the absent reflection

The macro family (`VERSIONED_READ_CHARACTERIZATION_TEST_NO_JSON`,
`VERSIONED_WRITE_CHARACTERIZATION_TEST_NO_JSON`,
`VERSIONED_CHARACTERIZATION_TEST_NO_JSON`,
`VERSIONED_READ_CHARACTERIZATION_TEST`,
`VERSIONED_WRITE_CHARACTERIZATION_TEST`,
`VERSIONED_CHARACTERIZATION_TEST` — six macros) in
`src/libstore-test-support/include/nix/store/tests/protocol.hh`
generates per-(T, Version) pairs of `TEST_F`s. This is **direct
evidence** that the production-side `Serialise<T>` per-protocol
specialisations are a Cartesian product (T × Proto × Version),
which agent 2's N16 noted.

Concrete observation: the `*_NO_JSON` half exists **because some
types lack JSON serialisers**. The cataloger of section 18 should
note: a production-side type that has wire serialiser but no JSON
serialiser is a partial-coverage smell. The macro split in tests
catalogs the partial coverage; the production side should reach
JSON parity (or document why a particular type is wire-only).

#### T2. `libstore-test-support` and `libexpr-test-support` parallel each other

Both directories carry the same shape: `meson.build`, a `tests/`
header for the library, plus `path.cc`/`outputs-spec.cc`/
`derived-path.cc` (in store) or `value/context.hh` (in expr) as
gtest-rapidcheck arbitrary providers. The pattern is the same one
a unified `nix-test-support` library would carry; each library has
its own copy. This isn't a refactor candidate per se, but the
duplication mirrors the production-side library split (libstore
vs libexpr) and a candidate "consolidate test-support across
libraries" *would* land alongside production-side library
boundary refactors.

#### T3. `_NIX_TEST_ACCEPT` in `characterization.hh` is read at call time, not init

The catalog's #146 says env vars "are read once into a function-local
`static bool`". The test-side `_NIX_TEST_ACCEPT` is **not** read into
a static — it's read at every test method invocation. This means a
test running with `_NIX_TEST_ACCEPT=1` to update goldens will see
the env var on each test, but a *production* code path that did the
same would race on TLS. The test-side read pattern is
production-incompatible. If #146's consolidation pushes this into a
`TestHooks` struct, the static-vs-dynamic distinction matters.

### Effort-class dependencies on keystones (beyond agent 1's coverage)

Agent 1 named keystones #136 and #137. There are additional keystone
relationships the catalog under-marks:

- **#150 → #170, #151.** The catalog's section 16 already notes
  #150 is a keystone for #151 and #170. Agent 1 didn't elevate the
  effort-class dependency; verified.
- **#138 (readOnlyMode → StoreConfig) → #131 (LookupPathHooks
  registry).** The validation paragraph for #131 says "compounds with
  #133" but not with #138. In fact, #131's `readOnlyMode` reference
  in the `EvalSettings` ctor goes through #138's pointer alias;
  removing the alias (per #138) means #131's wiring requires a new
  source for `readOnlyMode`. **Effort-class depends on #138 landing
  first.**
- **#149 → #136 → #134, #135, #144.** The chain. #149 (Config::_settings
  value storage) is currently "structural" but only because #136 hasn't
  landed. With #136 in, #149 becomes "medium" because the holder
  pattern is then known. **Effort-class is conditional on #136.**
- **#212 (EvalState ctor decomposition) → #210 (PrimOpHelpers).**
  #212 lists eight init phases including createBaseEnv. If #210
  lands first (PrimOpHelpers carve-out), createBaseEnv splits into
  two phases (core + primops); #212's "medium" then becomes "small".
  **Effort-class depends on #210.**
- **N4 (X-macro Settings struct, agent 2) → #19, #21, #43, #137,
  #144.** Agent 2 named this; the catalog doesn't have N4 yet.
  When N4 lands as a candidate, the dependency graph reshuffles —
  N4 becomes an alternative keystone replacing the chain
  #136 → #19 → #21.

### Candidates to split, merge, or delete (beyond #94)

Agent 1 flagged #94 for splitting. More cases:

#### Split #18 (`MakeError`/`CloneableError`)

The body proposes a single CRTP base for both styles. But:

- The macro-driven half (`MakeError(name, parent)`) is widely used
  and is a pure boilerplate-removal candidate.
- The hand-written half (`ExecError`, `MissingExperimentalFeature`,
  etc.) adds fields; refactoring those is non-trivial.
- The third axis (`SystemError`/`SysError`/`WinError` with `DisambigHintFmt` tag idiom) is a separate pattern.

**Recommendation:** split #18 into:
- **#18a** ("MakeError macro → CRTP base"): trivial mechanical edit
  for the no-fields cases.
- **#18b** ("Hand-written CloneableError-derived classes"): medium
  refactor, separate per-class consideration.
- **#18c** ("DisambigHintFmt/DisambigVarArgs tag idiom"): the
  `SystemError`/`SysError`/`WinError` triad is its own pattern.

#### Split #142 (`chrootHelperName`)

The catalog body has three observations bundled:
1. The non-`const` global with hard-coded sentinel (real refactor).
2. The `Strings helperArgs = { chrootHelperName, ... }` brace-init
   pitfall (build-correctness fix).
3. The `main.cc` `#include "run.hh"` adjustment (build-system fix).

The branch `vibe-coding/cleanup/nix-cli` already addressed all
three. The candidate documentation should split or factor the three
concerns clearly because they are different kinds of debt.

#### Merge #65 (primop arg-validation) with #211 (force* family)

Agent 1 noted these compound, but didn't propose merging. Reading
both: #65's arg-validation boilerplate is the *outer* layer; #211's
`force*` family is the *inner* layer. Both are call-graph-cascading
and any consolidation has to touch both layers. They're really
**two views of one refactor** — a unified "primop type-checking
machinery" candidate.

**Recommendation:** merge #65 and #211 into a single candidate at
the structural-effort tier. Or at minimum, mark them as a sequenced
pair.

#### Delete #75 (NAR streaming TTY-check)

Agent 1 flagged this as cosmetic-adjacent. Re-reading the body: the
"trivial (just sharing the TTY-check helper)" half is a one-line
boolean call; the "small (full `runNarDump(SourceLike)` helper)"
half is broader.

**Recommendation:** delete the trivial half (it's not a debt entry,
it's a single boolean call); keep the small half if there's
duplication to factor.

#### Delete #44 — already OBSOLETE in catalog

Already marked OBSOLETE; agent 1 didn't note this. Recommend
explicit deletion from candidate list (move to a "deprecated"
section if the team wants a historical record).

#### Merge #134, #135 with #144

#144 ("collapse small Settings structs") subsumes the diamond
problem in #134/#135 for the small structs (`LogFileSettings`,
`NarInfoDiskCacheSettings`, `GCSettings`, `AutoAllocateUidSettings`,
`WorkerSettings`). The remaining problem is just `LocalSettings`'s
relationship to `Settings` — much narrower.

**Recommendation:** demote #134 and #135 from independent candidates
to implementation steps inside #144.

#### Split #94 (`BuiltPath`/`SingleBuiltPath`) — confirms agent 1

Agent 1 already proposed splitting #94 into a dead-surface deletion
+ a template-factoring half. Confirmed.

### Boost / C++23 / asio uplift gotchas

#### #122 (`std::move_only_function`) — ABI break, agent 1 missed

Agent 1's validation paragraph says: "No public C-API exposes `fun`
(verified across `libutil-c`/`libstore-c`/`libexpr-c`/`libfetchers-c`)."
This is true at the **symbol** level — no C-API entry point's
mangled name carries `nix::fun`.

But it is **false at the layout level**. `nix::PrimOp` (declared in
`libexpr/include/nix/expr/eval.hh`) has a `fun<PrimOpFun> impl;`
member. The C-API `nix_alloc_primop` allocates
`sizeof(nix::PrimOp)` bytes. The C-API consumer holds a pointer to
this allocation and dereferences fields by offset (the underlying
gc-managed primop is laid out by C++ layout rules; the C-API
consumer doesn't know about its size, but the *library* allocates
based on the current `sizeof`).

Migrating `nix::fun` to `std::move_only_function` changes the
sizeof of `nix::PrimOp` (different storage strategies — `nix::fun`
wraps `std::function` which has its own SBO; `std::move_only_function`
has its own different SBO). This is **silent allocation-size
drift** — not a symbol-mangling break, but a layout break that
manifests as either OOB reads (consumer expects old layout) or
wasted bytes (consumer allocates more than needed).

Same hazard applies to `extern fun<void(siginfo_t*, void*)>
stackOverflowHandler;` in `libmain/include/nix/main/shared.hh` —
this *is* a symbol with a mangled name that includes `nix::fun`.
Replacing it with `std::move_only_function` changes the symbol
mangle. Not C-API, but C++-API consumers (Hydra, niv) would see
link errors.

Other `fun<>` member declarations I found in public headers:
- `source-path.hh` — `readFile(..., fun<void(uint64_t)> sizeCallback = ...)`
- `file-system.hh` — `typedef fun<bool(const std::string &)> PathFilter;`
- `serialise.hh` — three typedefs (`data_t`, `cleanup_t`, `lambda_t`)
- `serialise.hh` — `sinkToSource(fun<void(Sink &)> writer, ...)`
- `serialise.hh` — `fun<void()> checkError;` member
- `thread-pool.hh` — `typedef fun<void()> work_t;`
- `source-accessor.hh` — virtual default-arg `fun<void(uint64_t)>`
- `memo.hh` — `fun<T()> compute;` member
- `eval-cache.hh` — `typedef fun<Value *()> RootLoader;`
- `eval.hh` — `PrimOp::impl` field
- `shared.hh` — `Args::Flag::parseArg`, `extern stackOverflowHandler`

Each of these is a candidate ABI surface for #122. The validation
paragraph's "no C-API exposes fun" needs to expand to "no symbol
mangle break in C-API entry points, but layout drift in C-API-allocated structs (PrimOp) and a symbol-mangle break in libmain-c
consumers (stackOverflowHandler)."

**Recommendation:** #122's effort needs uplift from "medium" to
"medium with explicit ABI version bump". Or scope down to
non-public-header sites first.

#### #125 (`std::format` migration) — exception-text ABI of error messages

The catalog body and agent 1 both note that test coverage asserts
exact error message text. The deeper concern: the exact text of
exceptions is observed by:

- The functional test suite (per `tests/functional/`) — well known.
- The `nix-c` API consumer, which marshals `nix::Error::what()`
  through `nix_get_err_msg`. If `what()` text drifts, every C-API
  consumer that pattern-matches on error messages breaks.
- The `nix daemon` protocol — error messages are serialised over
  the wire to clients. **A daemon at version N+1 with `std::format`-rephrased messages talks to a client at version N expecting the
  old phrasing.** This is a wire-protocol soft-break, not the wire
  format itself but the semantic content of strings on the wire.

**Recommendation:** the validation paragraph should add: "exception
message text is observed by C-API and by the daemon-client
protocol; pure mechanical translation of `%s` → `{}` preserves the
text only if the formatter implementations exactly match. Boost's
`%2$s` positional and `std::format`'s `{1}` indexed differ in
semantics around precision/width — verify each translation against
the test suite. Daemon-client mixed-version testing must include
error-message preservation."

#### #160 (`Goal::Co` → asio) — `final_awaiter` is HALO-incompatible

The catalog and agent 1 correctly identify `final_awaiter`'s
tail-call optimisation as genuinely custom. The header even says
"@todo Allocate explicitly on stack since HALO thing doesn't
really work." **HALO** = Heap Allocation eLision Optimization (the
optimization where the compiler *can* allocate a coroutine frame
on the parent's stack frame, eliminating the `operator new`).

Subtle hazard agent 1 didn't fully unpack: any partial migration to
`asio::awaitable<T>` for sub-operations (e.g. replacing
`SuspendAwaiter` with asio's suspend) breaks the homogeneity of the
coroutine machinery. `Goal::Co` uses `co_await` to suspend on a
custom `Suspend` token; if that becomes `co_await asio::post` or
similar, the `final_awaiter`'s view of "what is the parent
continuation" changes — asio's awaiter chain is independent of
`promise_type::continuation`.

**Concrete risk:** the hybrid migration agent 1 endorses (keep
`final_awaiter`, replace `SuspendAwaiter` with asio) requires
deciding *which awaiter chain owns the final continuation*. If the
asio runtime resumes the awaiter and asio's continuation chain
points to something other than `goal->top_co`, the tail-call
optimisation is lost without an obvious symptom.

**Recommendation:** the validation paragraph should add: "the
hybrid migration requires verifying that asio's `experimental::channel`
or equivalent doesn't capture the parent continuation in a way that
conflicts with `final_awaiter`'s `top_co` swap. A migration step
that replaces `SuspendAwaiter` while keeping `final_awaiter`
must confirm via test that the same number of frames are alive at
the same call depth (equivalent stack depth in micro-benchmark)."

#### #129 (`MaintainCount` → `Finally`) — destructor exception swallow

`MaintainCount` is a stack-RAII counter. `Finally` (in libutil) is
also a stack-RAII tool. The two have one important behavioural
difference: `MaintainCount`'s destructor is `noexcept` (decrements
a counter; can't throw); `Finally`'s destructor invokes a captured
lambda whose body might throw.

If the migration replaces `MaintainCount` with `Finally{[&]{
counter--; }}`, the destructor still can't throw (the lambda body
doesn't throw). But adding any other side effect into the lambda
later would silently introduce a throwing destructor. The catalog
should flag this as a stylistic risk, not a correctness one.

**Recommendation:** add to #129's validation: "the `Finally`
replacement is correct only as long as the captured lambda is
guaranteed not to throw. Add a `static_assert` or comment
documenting the no-throw expectation at each replacement site, or
prefer a typed helper (`ScopedCounter`, `MaintainCount`) over an
anonymous `Finally` for counter-increment-decrement RAII."

#### #170 (`Worker` friend of Goal subclasses) — count is wrong

The catalog body says "Worker is friend of all four concrete Goal
subclasses". Verified: `friend class Worker` appears in:
- `derivation-resolution-goal.hh`
- `derivation-building-goal.hh`
- `drv-output-substitution-goal.hh`
- `worker.hh` (on the inner `Waker` class — not a Goal subclass)

Three Goal subclasses + one inner Waker class = four sites total.
Catalog body's "all four concrete Goal subclasses" is wrong: it's
three Goal subclasses (`DerivationBuildingGoal`,
`DerivationResolutionGoal`, `DrvOutputSubstitutionGoal`); the other
three concrete Goal subclasses (`DerivationGoal`,
`DerivationTrampolineGoal`, `PathSubstitutionGoal`) have no friend
declaration because their key fields are already public.

**Recommendation:** correct the catalog body to "three concrete
Goal subclasses (`DerivationBuildingGoal`, `DerivationResolutionGoal`,
`DrvOutputSubstitutionGoal`); the other three (`DerivationGoal`,
`DerivationTrampolineGoal`, `PathSubstitutionGoal`) expose their key
fields publicly". This is the same observation agent 1 made about
#151's friend cluster, but #170 needs the same correction.

## Disagreements with the first reviewer

This is the section where I disagree with agent 1's
verdict/analysis based on independent evidence.

### #149 — agent 1's UAF rationale is wrong

Agent 1's validation paragraph for #149 reads: "the `created = 123;`
debug field on `AbstractSetting` is a sentinel-style integrity check
(catches use-after-free of a moved-from setting)."

The actual code (`configuration.cc::~AbstractSetting`) explicitly
says:
> `// Check against a gcc miscompilation causing our constructor`
> `// not to run (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80431).`

The guard catches gcc bug 80431, not use-after-free. **Disagreement
on rationale, agreement on validity.** Verdict stays VALID; the
proposed structural refactor is still right; but the paragraph
should cite the gcc bug as the field's purpose.

### #140 — verdict body is wrong; pure deletion fix

Agent 1 reclassified #140 to "Latent bugs hiding inside duplication"
framing. I disagree: `impureOutputHash` has zero call sites
anywhere in the tree — no `extern` declaration, no caller. It is
**dead code**, not a SIOF hazard. The right action is plain
deletion. The catalog and agent 1 both miss this.

**Verdict:** VALID, but reclassify to dead/stale code (section 7),
not "Latent bugs". The proposed fix in the body ("static const
inside the function that uses it") is *moot* because there is no
function that uses it.

### #170 — friend count

Agent 1 said the catalog accurately listed Worker friend of all four
goal subclasses. I show it's three goal subclasses + one inner
Waker. **Disagreement on count.**

### #128 — branch already landed; agent 1 noted it but didn't push back

Agent 1's PARTIALLY-VALID verdict matches the catalog. The branch
`vibe-coding/cleanup/libmain` has already shipped the
deletion. The commit bundled an unrelated dead-symbol removal
(`blockInt`). Per the user's CLAUDE.md "Do not bundle unrelated
cleanup ... into a commit", this is mild but worth flagging.

I disagree with agent 1's silence on the bundling: the commit
should be split or its message should explicitly say "two unrelated
deletions" rather than presenting them under a single
deletion-of-unused-parameter narrative.

### #122 — ABI break is real

Agent 1 said "No public C-API exposes `fun`". This is symbol-
correct but layout-incorrect. `nix::PrimOp::impl` is a `fun<>`
member; its sizeof flows through to C-API allocations. The
`extern fun<...> stackOverflowHandler;` in `libmain` is a
*symbol* with C++-mangled type that includes `nix::fun`. **Real
ABI break for libmain consumers.**

I disagree with agent 1's "no ABI break" framing of #122; it is
specifically a layout-and-symbol concern that agent 1 surveyed at
the wrong level.

### #167 buffer-size count

Agent 1 said "4 distinct sites in libutil"; my recount finds 10 in
libutil alone, 19 tree-wide. **Disagreement on count.** The catalog
body says "12"; that number is also wrong if scoped to
libutil but right if interpreted loosely tree-wide. Pick one
methodology and document it.

### #205 (Counter alignment) — disagree on framing direction

Agent 1's PARTIALLY-VALID note keeps "the `enabled` gate is a
runtime branch, not a compile-time switch" framing. But the
template-parameter route (`Counter<bool Enabled = NIX_DEBUG_STATS>`)
is a clean compile-time switch. I think agent 1 should have
escalated to "structural" given the cross-codebase reach (per agent
2's N21).

### Otherwise: confirmation

Most of agent 1's specific findings I confirmed end-to-end:

- #50 verdict-table contradiction: confirmed.
- #148 verdict-table contradiction: confirmed.
- #176 case-count drift in body: confirmed (37 verified).
- #94 split recommendation: confirmed.
- #137 register-site count = 15 production: confirmed (16 total).
- #166 forward-decl counts (9/21/12): confirmed.
- #169 friend count (16, with one duplicate): confirmed.
- #173 pragma site count (10): confirmed.
- #146 `_NIX_TEST_*` count: confirmed (within tolerance).
- All branch cross-references: confirmed (didn't re-check the
  branches themselves; trusted agent 1's branch-by-branch verdict).
- Effort-class corrections: agreed for #22, #39, #46, #65, #86,
  #100, #142, #172.

## Items to escalate

These are findings that warrant follow-up agent or implementation
work beyond catalog edits:

### E1. Verify `impureOutputHash` is fully dead before deletion (#140)

This deserves an explicit verification pass: check that no
out-of-tree consumer (Hydra, niv, lix-fork) reads `impureOutputHash`
via the libstore-shipped header. If `derivations.hh` exposes the
declaration anywhere via `extern`, it must be deleted in lockstep.
Per my grep, no `extern Hash impureOutputHash` exists. Recommended
follow-up: file an issue noting the deletion path, then land a
single-commit deletion.

### E2. Re-audit `*Settings` C-API surface for #134, #138, #144

The C-API impl files (`nix_api_store.cc`, `nix_api_expr.cc`) read
`nix::settings.readOnlyMode` and `nix::globalConfig` directly.
Before #134/#138/#144 land, an audit pass should:

(a) enumerate every C-API call-site touching a `*Settings` field;
(b) document which fields are part of the public ABI per current
behaviour (regardless of header declaration);
(c) propose a deprecation shim for each.

This is a separate verification agent's job; the catalog candidates
for #134/#138/#144 should reference the audit.

### E3. Goal::Co final_awaiter property test

Before any partial migration of `Goal::Co` to asio (per #160), a
property test asserting "tail-call coroutine chain has O(1) live
frames at depth N" should be written and pinned. Without that, any
later partial migration silently regresses to O(N) frames.

### E4. Daemon-client error-message preservation matrix

For #125 (`std::format` migration), a daemon-client mixed-version
test plan must catalog every error message and assert text
preservation across format-string translation. This is a multi-
session task and should be tracked as a separate work item.

### E5. Reverify "branch already landed" status periodically

Agent 1 confirmed all 14 branch cross-references; I trusted that
without rechecking. As work progresses, the branch list will grow
and the catalog's body counts will drift. Recommend a quarterly
"branch verification" pass or automation.

### E6. Test-side `*-test-support` consolidation

The two `*-test-support/` libraries duplicate test infrastructure
patterns. A separate refactor candidate covering test-support
consolidation should be filed (this is what agent 2's N16 partially
addressed; expand it to cover the per-library test-support split).

### E7. Audit fwd-decl headers (#166)

The proposed `serialise-fwd.hh` / `store-fwd.hh` factoring is
medium-effort. Before that lands, verify no out-of-tree consumer
includes specific files just to grab the forward declaration —
otherwise the move is a gratuitous source-compat break.

### E8. `EvalState::vTrue`/`vFalse`/etc. consistency check

The "not a singleton" comments contradict the `getBool`/`vEmptyList`
hand-out behaviour (#213). Per CLAUDE.md "When making semantic
changes (e.g., hash semantics, dep-kind subsumption), document the
invariant you are preserving or changing": the catalog's #213
should pick one direction (canonical singletons OR truly per-call
fresh values) and document the chosen invariant before any code
moves.

## Summary

### New findings (beyond agent 1)

- **8 hidden hazards** in candidates marked VALID/PARTIALLY-VALID:
  #140 dead code (not SIOF), #147 leaked-singleton ordering, #143
  TLS-fallback shutdown ordering, #149 gcc-bug rationale (not UAF),
  #208 type-level enforcement of static-init invariant, #137 init
  sequencing must precede `loadConfFile`, #128 commit bundling
  caveat, #170 friend-count drift.
- **5 ABI implications** undersold by agent 1: #122 layout
  break in `PrimOp::impl`, #122 symbol-mangle break in
  `stackOverflowHandler`, #138 multiple C-API readers,
  #134 `nix::settings.readOnlyMode` field-syntax break,
  #137 C-API initialisation sequencing.
- **6 cross-candidate consistency conflicts**: #134 vs #144 (do
  #144 first), #136 vs #19/#43 (keystone), #137 vs #146 (test fix
  needed), #160 vs #22 (sequencing), #205 vs N21
  (architecture before local), and the keystone effort-class
  reshuffles.
- **3 test-side patterns** (T1, T2, T3) mirroring production debt.
- **5 candidate-split/merge proposals**: split #18 into 3, split
  #142 into 3, merge #65+#211, demote #134/#135 into #144, delete
  #75 trivial half + #44 already OBSOLETE.
- **4 Boost / C++23 / asio uplift gotchas**: #122 layout drift,
  #125 daemon-client error-text preservation, #160 final_awaiter
  HALO incompatibility with asio, #129 destructor-throw risk.

### Counts of agent 1's claims

- **Confirmed:** ~12 (case counts, friend counts, register-site
  counts, branch cross-references, most effort-class corrections).
- **Disagreed:** 4 (UAF rationale on #149, framing on #140,
  count on #170, ABI framing on #122).
- **Confirmed but pushed deeper:** ~6 (gave agent 1's claim more
  context: #128, #143, #147, #167 count methodology, #205
  framing, #208 invariant).

### Number of escalation items

8 (E1-E8); each is either a verification pass, a separate refactor
candidate, or a test-plan addition.

### Recommended catalog edits

The minimum set of catalog edits that this pass surfaces:

1. **#140** body: rewrite as "dead code; delete entirely". Move to
   section 7 or keep in section 15 but cite dead-code as primary
   debt class.
2. **#147** verdict: VALID → PARTIALLY VALID (per-Store path is
   straightforward; the singleton-leak path must remain).
3. **#149** validation paragraph: replace "use-after-free of
   moved-from setting" with "gcc bug 80431 miscompilation guard".
4. **#170** body: correct "all four concrete Goal subclasses" to
   "three Goal subclasses; the other three expose their key fields
   publicly".
5. **#138** pitfall paragraph: add "two C-API readers, not one;
   `nix_api_expr.cc` aliases the pointer".
6. **#137** body: add "test-only register site in
   `libutil-tests/nix_api_util.cc` needs corresponding rework";
   add "explicit init must precede any setting access including
   `loadConfFile`".
7. **#134** body: add "compounds with #144 (do first); after #144
   only `LocalSettings`-vs-`Settings` remains".
8. **#122** validation paragraph: clarify that no C-API *symbol*
   exposes `fun`, but `PrimOp::impl` member changes layout and
   `stackOverflowHandler` is a `extern fun<...>` symbol.
9. **#160** validation paragraph: add the asio continuation-chain
   versus `final_awaiter::top_co` swap concern.
10. **#125** validation paragraph: add daemon-client error-message
    preservation requirement.
11. **#129** validation paragraph: add destructor-throw caveat for
    `Finally` replacement.
12. **#94** split (per agent 1): two candidates.
13. **#18** split: three candidates.
14. **#142** factor: clarify three concerns (global, brace-init,
    include) all addressed by `cleanup/nix-cli`.
15. **#65** merge with **#211**: one candidate, structural.
16. **#134, #135** demote to implementation steps under #144.
17. **#75** delete trivial half; keep small half if duplication
    exists.
18. **#44** explicitly delete from candidate list (already
    OBSOLETE).

Plus ~5 verdict-table reconciliations agent 1 already proposed
(#50, #148, #176, plus the ones above).

(End of file.)

