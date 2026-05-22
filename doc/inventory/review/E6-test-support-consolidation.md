# E6 — test-support consolidation validation

Validation pass for cluster F (N28 / N39 / N41) in `doc/inventory/candidates/24-test-infrastructure-mirrors.md`. Walks every fixture under `src/*-tests/` and `src/*-test-support/`, every `meson.build` `test_env` block, and confirms or refines each proposal.

## Scope walked

- `src/libutil-tests/`
- `src/libutil-test-support/`
- `src/libstore-tests/`
- `src/libstore-test-support/`
- `src/libfetchers-tests/`
- `src/libexpr-tests/`
- `src/libexpr-test-support/`
- `src/libflake-tests/`

The relevant base classes live in `src/libutil-test-support/include/nix/util/tests/characterization.hh` (`CharacterizationTest`, free `testAccept()`), `src/libutil-test-support/include/nix/util/tests/json-characterization.hh` (`JsonCharacterizationTest<T>`, free `readJsonTest`/`writeJsonTest`/`checkpointJson`), `src/libutil-test-support/include/nix/util/tests/test-data.hh` (free `getUnitTestData()`), `src/libstore-test-support/include/nix/store/tests/protocol.hh` (`ProtoTest<Proto, dir>` and `VersionedProtoTest<Proto, dir>` with the `_NO_JSON` variant macros), and `src/libstore-test-support/include/nix/store/tests/libstore.hh` (`LibStoreTest`).

`getUnitTestData()` is a free function reading the env var; `unitTestData` in fixtures is a redundant local cache. `goldenMaster()` is a pure virtual in `CharacterizationTest`; every concrete fixture must implement it. `ProtoTest`/`VersionedProtoTest` already accept a `protocolDir` template parameter and synthesize `goldenMaster` themselves — they do not have the per-fixture `unitTestData` boilerplate.

## Test-fixture inventory

Categories used:

- **PureCT** = pure `CharacterizationTest` user (subclass of `CharacterizationTest` only, plus optional `LibStoreTest` mixin).
- **PureJCT** = pure `JsonCharacterizationTest<T>` user.
- **PureVPT** = pure `VersionedProtoTest`/`ProtoTest` user (already takes `protocolDir` template arg; out of scope for N41 but in scope for N39).
- **MixedJCT** = combines `JsonCharacterizationTest` with custom helper methods, `SetUp()`, or non-trivial constructor body.
- **DerivedReuse** = subclass of an existing characterization fixture (chains, no own `unitTestData` member).
- **Custom** = no use of the characterization machinery at all.

| File | Class(es) | Category | unitTestData subdir | Notes |
| - | - | - | - | - |
| `src/libfetchers-tests/public-key.cc` | `PublicKeyTest` | PureJCT | `public-key` | trivial; no `SetUp`. |
| `src/libstore-tests/build-result.cc` | `BuildResultTest` (CT mixin) + `BuildResultJsonTest` (JCT) | DerivedReuse | `build-result` | only `BuildResultTest` carries `unitTestData`; `BuildResultJsonTest` inherits. |
| `src/libstore-tests/common-protocol.cc` | `CommonProtoTest : ProtoTest<CommonProto, commonProtoDir>` | PureVPT | (template arg `"common-protocol"`) | already declarative. |
| `src/libstore-tests/content-address.cc` | `ContentAddressTest` + `ContentAddressJsonTest` | DerivedReuse + extra member (`mockXpSettings`) | `content-address` | base carries `mockXpSettings`. |
| `src/libstore-tests/derivation-advanced-attrs.cc` | `DerivationAdvancedAttrsTest`, `CaDerivationAdvancedAttrsTest` | MixedJCT (HAZARD) | `derivation/ia` (base), `derivation/ca` (override in `SetUp`) | `unitTestData` is a `protected` mutable member; the CA subclass *reassigns* it in `SetUp()`. Multi-segment subdir. |
| `src/libstore-tests/derivation/test-support.hh` | `DerivationTest`, `CaDerivationTest`, `DynDerivationTest`, `ImpureDerivationTest` | MixedJCT | `derivation` (base only) | derived classes only override `mockXpSettings` in `SetUp()`, not the directory; reuse parent's `unitTestData`. |
| `src/libstore-tests/derivation/external-formats.cc` | `DerivationOutputJsonTest`, `DynDerivationOutputJsonTest`, `CaDerivationOutputJsonTest`, `ImpureDerivationOutputJsonTest`, `DerivationJsonAtermTest`, `DynDerivationJsonAtermTest` | DerivedReuse | inherited from `DerivationTest` family | mix-in `JsonCharacterizationTest<DerivationOutput>` / `<Derivation>` over the base — no own `unitTestData`. |
| `src/libstore-tests/derivation/invariants.cc` | `FillInOutputPathsTest` | MixedJCT (HAZARD) | `derivation/invariants` | non-trivial constructor: passes a custom `DummyStore` config to `LibStoreTest(ref<Store>)` constructor. |
| `src/libstore-tests/derivations.cc` | `TryResolveTest` | MixedJCT | `derivation/try-resolve` | inline `EnableExperimentalFeature caFeature{"ca-derivations"}` data member (RAII guard runs at construction) plus helper methods. |
| `src/libstore-tests/derived-path.cc` | `DerivedPathTest`, `SingleDerivedPathJsonTest`, `DerivedPathJsonTest` | DerivedReuse | `derived-path` | base carries `unitTestData`. |
| `src/libstore-tests/dummy-store.cc` | `DummyStoreTest`, `DummyStoreJsonTest` | MixedJCT | `dummy-store` | overrides `static SetUpTestSuite` to call `initLibStore(false)`. Also carries inline `TEST(...)` (non-fixture) cases. |
| `src/libstore-tests/nar-info.cc` | `NarInfoTestV1`, `NarInfoTestV2`, `NarInfoTestV3` | PureCT (HAZARD: 3 fixtures, 1 file) | `nar-info/json-1`, `nar-info/json-2`, `nar-info/json-3` | three sibling fixtures with versioned subdirs; each adds `.json` to the stem in `goldenMaster`. |
| `src/libstore-tests/outputs-spec.cc` | `OutputsSpecTest`, `ExtendedOutputsSpecTest` | PureCT (HAZARD: 2 fixtures, 1 file) | `outputs-spec`, `outputs-spec/extended` | two sibling fixtures, the second extends the first's subdir. |
| `src/libstore-tests/path-info.cc` | `PathInfoTestV1`, `PathInfoTestV2`, `PathInfoTestV3` | PureCT (HAZARD: 3 fixtures, 1 file) | `path-info/json-1`, `path-info/json-2`, `path-info/json-3` | mirror of nar-info; appends `.json` in `goldenMaster`. |
| `src/libstore-tests/path.cc` | `StorePathTest` | PureCT | `store-path` | trivial. |
| `src/libstore-tests/realisation.cc` | `RealisationTest`, `UnkeyedRealisationTest`, `RealisationJsonTest`, `UnkeyedRealisationJsonTest`, `RealisationSigningTest` | PureJCT (HAZARD: 2 sibling fixtures share subdir) | `realisation` (twice) | both `RealisationTest` and `UnkeyedRealisationTest` declare `unitTestData = getUnitTestData() / "realisation"` — duplicate. |
| `src/libstore-tests/serve-protocol.cc` | `ServeProtoTest : VersionedProtoTest<ServeProto, serveProtoDir>` | PureVPT | (template arg `"serve-protocol"`) | already declarative. |
| `src/libstore-tests/store-reference.cc` | `StoreReferenceTest` | PureCT | `store-reference` | appends `.txt` to stem in `goldenMaster`. |
| `src/libstore-tests/worker-protocol.cc` | `WorkerProtoTest : VersionedProtoTest<WorkerProto, workerProtoDir>` | PureVPT | (template arg `"worker-protocol"`) | also has free-function tests for ordering of `WorkerProto::Version`. |
| `src/libstore-tests/worker-substitution.cc` | `WorkerSubstitutionTest` | MixedJCT (HAZARD) | `worker-substitution` | non-trivial constructor: instantiates two `DummyStore` refs and passes one to `LibStoreTest(ref<Store>)`. Also `SetUpTestSuite` calls `initLibStore(false)`. |
| `src/libstore-tests/nix_api_store.cc` | (no fixture using `getUnitTestData`; raw inline use in 6 test bodies) | Custom (uses `_NIX_TEST_UNIT_DATA` env but no `CharacterizationTest`) | n/a (reads `derivation/ca/self-contained.json` directly) | HAZARD for N41: not a fixture, but it still depends on N28's env var. |
| `src/libutil-tests/archive.cc` | `NarTest`, `InvalidNarTest` | PureCT | `nars` | appends `.nar` suffix in `goldenMaster`. |
| `src/libutil-tests/git.cc` | `GitTest` | MixedJCT | `git` | overrides `SetUp()` to enable `git-hashing` xp feature. |
| `src/libutil-tests/hash.cc` | `HashTest`, `BLAKE3HashTest` | DerivedReuse | `hash` | base; `BLAKE3HashTest` adds `mockXpSettings` via `SetUp()`. |
| `src/libutil-tests/memory-source-accessor.cc` | `MemorySourceAccessorTestErrors` (Custom), `MemorySourceAccessorTest` (PureCT), `MemorySourceAccessorJsonTest` (DerivedReuse) | mixed file: 1 Custom + 1 PureCT + 1 DerivedReuse | `memory-source-accessor` | the Custom fixture coexists in same TU. |
| `src/libutil-tests/nar-listing.cc` | `NarListingTest`, `NarListingJsonTest` | DerivedReuse | `nar-listing` | trivial. |

Also walked but contain only non-`CharacterizationTest` fixtures or no fixtures: `src/libexpr-tests/` (zero `CharacterizationTest` use — uses `LibExprTest` from `libexpr-test-support`), `src/libfetchers-tests/{access-tokens,attrs,git,git-utils,input,nix_api_fetchers}.cc` (none use `CharacterizationTest`), `src/libflake-tests/{flakeref,nix_api_flake,url-name}.cc` (none use `CharacterizationTest`).

### Counts

- **PureVPT/PureCT-only fixtures, fits N41 cleanly**: 1 in fetchers (`PublicKeyTest`), 6 in libstore (`StoreReferenceTest`, `StorePathTest`, plus the V1/V2/V3 trios for nar-info and path-info — but those are sibling fixtures sharing a file), 4 in libutil (`NarTest`, `MemorySourceAccessorTest`, `NarListingTest`, the `HashTest` base). Roughly the 25+ count cited in N41 holds, but a non-trivial subset pulls in extra members (`mockXpSettings`, `caFeature`, helper methods) or non-trivial constructor bodies.
- **PureVPT (already declarative via template arg)**: 3 — `CommonProtoTest`, `ServeProtoTest`, `WorkerProtoTest`.
- **MixedJCT/DerivedReuse fixtures with own `unitTestData`**: ~15 (see hazards section).
- **Hazardous fixtures requiring rewrite if N41 is applied as-catalogued**: 6 (see hazards section).

## meson.build test_env inventory

| File | Sites | Shape | Extras beyond `_NIX_TEST_UNIT_DATA` |
| - | - | - | - |
| `src/libutil-tests/meson.build` | 2 (main `test()`, optional `enosys` `test()` on Linux) | inline literal, no `test_env` variable | **none** — neither site sets `HOME`. |
| `src/libstore-tests/meson.build` | 2 (main `test()` shares variable; benchmarks `benchmark()` block uses inline literal) | named `test_env` variable, conditionally extended | `HOME`, conditional `NIX_REMOTE` (non-Windows). Benchmark site only sets `_NIX_TEST_UNIT_DATA` (no `HOME`). |
| `src/libfetchers-tests/meson.build` | 1 | inline literal | `HOME`, `NIX_STORE=''`. |
| `src/libexpr-tests/meson.build` | 1 (main `test()`); benchmark block has no env | inline literal | `HOME`, `NIX_STORE=''`. The benchmark `benchmark()` does NOT set `_NIX_TEST_UNIT_DATA` at all. |
| `src/libflake-tests/meson.build` | 1 | inline literal | `HOME`, `NIX_STORE=''`, `NIX_CONFIG='extra-experimental-features = flakes'`. |

### Categorisation

- **Identical** (just `_NIX_TEST_UNIT_DATA`, optional `HOME`):
  - `libutil-tests` main `test()` — `_NIX_TEST_UNIT_DATA` only.
  - `libutil-tests` `enosys` `test()` — `_NIX_TEST_UNIT_DATA` only.
  - `libstore-tests` benchmark `benchmark()` — `_NIX_TEST_UNIT_DATA` only.
- **Identical-plus-extras**:
  - `libstore-tests` main `test()` — `_NIX_TEST_UNIT_DATA` + `HOME` + conditional `NIX_REMOTE`.
  - `libfetchers-tests` — `_NIX_TEST_UNIT_DATA` + `HOME` + `NIX_STORE`.
  - `libexpr-tests` main `test()` — `_NIX_TEST_UNIT_DATA` + `HOME` + `NIX_STORE`.
  - `libflake-tests` — `_NIX_TEST_UNIT_DATA` + `HOME` + `NIX_STORE` + `NIX_CONFIG`.
- **Divergent**: none structurally — all are flat env dicts.

### Refinements vs. N28's stated canonical block

The catalog claim "every test-suite carries identical `{ _NIX_TEST_UNIT_DATA, HOME }`" is only partly correct:

- `libutil-tests` does **not** set `HOME` in either site.
- `libstore-tests` benchmark block does **not** set `HOME`.
- `libfetchers-tests`/`libexpr-tests`/`libflake-tests` add `NIX_STORE=''`, which N28 did not mention.
- `libflake-tests` additionally adds `NIX_CONFIG`.
- Only `libstore-tests` carries the conditional `NIX_REMOTE` Windows guard (which N28 mentions).

So the "shared block" the catalog proposes should canonicalise is really three layered things:

1. `_NIX_TEST_UNIT_DATA` (universal — present in all 7 sites).
2. `HOME = build_dir / 'test-home'` (5 of 7 sites; absent in libutil and the libstore benchmark).
3. Per-suite extras (`NIX_STORE`, `NIX_CONFIG`, `NIX_REMOTE`) — divergent.

## Per-proposal validation

### N28 (meson env consolidation)

**Refined.** The catalog claim that all 7 sites share a single `{ _NIX_TEST_UNIT_DATA, HOME }` block is overstated by inspection:

- 3 of the 7 sites set `_NIX_TEST_UNIT_DATA` only (no `HOME`): `libutil-tests` main `test()`, `libutil-tests` enosys `test()`, `libstore-tests` benchmark.
- 4 of 7 sites set `_NIX_TEST_UNIT_DATA + HOME`, with three of those adding `NIX_STORE=''` and one (`libflake-tests`) further adding `NIX_CONFIG`.
- Only `libstore-tests` carries the `NIX_REMOTE` Windows-vs-Unix conditional.

The consolidation is still valid, but the shape is not a single `nix_test_env` dict — it is two nested layers:

- A `nix_test_env_data` dict containing only `_NIX_TEST_UNIT_DATA = meson.current_source_dir() / 'data'`.
- A `nix_test_env_full` dict that extends `nix_test_env_data` with `HOME = meson.current_build_dir() / 'test-home'` (and the `NIX_REMOTE` Windows conditional, since `HOME = test-home` and `NIX_REMOTE = test-home/store` co-vary).

The 5 suites that opt into `HOME` use `nix_test_env_full`; the 2 sites that need `_NIX_TEST_UNIT_DATA` only use `nix_test_env_data`. Per-suite extras (`NIX_STORE=''`, `NIX_CONFIG`) are spread on top with the `+` merge meson already supports.

This still removes the bulk of the duplication (the path expressions for `_NIX_TEST_UNIT_DATA`, `HOME`, `NIX_REMOTE`, and the `host_machine.system() != 'windows'` guard) but does not collapse to a single dict. **Effort: still small, but the include must export two variables, not one.**

### N39 (declarative test grid)

**Confirmed in shape, deferred in dependency.** The proposal — a declarative `(types, versions, formats)` grid plus a `--update-goldens` mode — fits the three `VersionedProtoTest` users (`CommonProtoTest`, `ServeProtoTest`, `WorkerProtoTest`) and a subset of `JsonCharacterizationTest` users (the param-pair-driven ones: `PublicKeyTest`, `BuildResultJsonTest`, `ContentAddressJsonTest`, `RealisationJsonTest`, `UnkeyedRealisationJsonTest`, `DummyStoreJsonTest`, `NarListingJsonTest`).

But:

- Many fixtures have *non-uniform* shape and would not flatten into a grid:
  - `nar-info.cc`/`path-info.cc` use 3 fixtures × suffix-appending `goldenMaster` (`.json`) — the grid would need to know about goldenMaster suffixing per fixture.
  - `derivation-advanced-attrs.cc` uses two TYPED_TEST_SUITE fixtures + custom helper methods (`testRequiredSystemFeatures`, `testDerivationOptions`) that are not just `from_json`/`to_json`.
  - `derivation/external-formats.cc` defines `from_aterm`/`to_aterm` plus JSON, which is a third "format" beyond the cited (`json`, `bin`).
  - `derivations.cc::TryResolveTest` uses `checkpointJson` mid-test for before/after state — not a single round-trip per type.
  - `worker-substitution.cc::WorkerSubstitutionTest` constructs two stores in its constructor and the tests are scenario-driven, not type-driven.
- A pure auto-generation `--update-goldens` mode is already present in spirit: `testAccept()` reads `_NIX_TEST_ACCEPT=1`, and `writeTest`/`writeJsonTest` regenerate goldens. The missing piece is per-stem auto-discovery (today the `(stem, value)` table is hand-written) — that *is* the production-side reflection in N16/U10, exactly as the catalog says.

The grid format itself would primarily benefit the ~10 param-driven fixtures and the 3 `VersionedProtoTest` users. The cataloger flags that this candidate compounds with N16/U10 (boost.describe), and confirms the cited wire-vs-JSON asymmetry is real (`VERSIONED_CHARACTERIZATION_TEST_NO_JSON` exists; some types only have `.bin` goldens, others only `.json`).

**Defer until #9/U10 (production-side reflection) lands.** Without auto-derived codecs the "grid" buys little over the existing macro stack. With them, it becomes a one-line entry per type. **Effort: medium, gated on U10.**

### N41 (CharacterizationTest constructor)

**Refined.** The mechanical part — `class FooTest : public CharacterizationTest { unitTestData = getUnitTestData() / "foo"; goldenMaster(stem) { return unitTestData / stem; } };` — is genuinely 25+ sites of mechanical boilerplate, and the proposed constructor parameter

```
class CharacterizationTest {
  std::filesystem::path unitTestData;
  CharacterizationTest(std::string subdir) : unitTestData(getUnitTestData() / subdir) {}
  std::filesystem::path goldenMaster(std::string_view stem) const override { return unitTestData / stem; }
};
```

would clean those up in ~16 fixtures.

But several sites do not fit the "subdir as a constructor argument" shape:

1. **Suffix-appending `goldenMaster` overrides.** Four fixtures append a literal suffix to the test stem: `NarInfoTestV{1,2,3}` and `PathInfoTestV{1,2,3}` append `.json`; `NarTest` appends `.nar`; `StoreReferenceTest` appends `.txt`. The constructor-only API (`CharacterizationTest("foo")`) cannot express this — it would still need a virtual `goldenMaster` override. Recommendation: pass the suffix as an optional second constructor argument (`CharacterizationTest("nar-info/json-1", ".json")`), or keep `goldenMaster` overridable.

2. **Multi-segment subdirs.** Many fixtures use composite paths: `derivation/ia`, `derivation/ca`, `derivation/try-resolve`, `derivation/invariants`, `nar-info/json-1`, `path-info/json-1`, `outputs-spec/extended`. A `std::string` subdir argument folded with `getUnitTestData() / subdir` handles this fine because `operator/` accepts a path with separators.

3. **Runtime-mutable `unitTestData` (HAZARD).** `CaDerivationAdvancedAttrsTest` reassigns `unitTestData = getUnitTestData() / "derivation" / "ca"` inside `SetUp()`. If `unitTestData` becomes a `private const` constructor-fixed member, this pattern breaks. Two viable fixes: (a) keep `unitTestData` `protected` and assignable, or (b) require this fixture to use a different constructor argument (which it could, since the CA derivation tests are a subclass that could call `CharacterizationTest("derivation/ca")` directly).

4. **Multiple sibling fixtures sharing a subdir or sharing a file (HAZARD).** `RealisationTest` and `UnkeyedRealisationTest` both declare `unitTestData = getUnitTestData() / "realisation"`. Under the constructor-arg shape they both spell `: CharacterizationTest("realisation")` — fine but worth noting the duplication isn't compacted further. The V1/V2/V3 fixtures in `nar-info.cc`/`path-info.cc` similarly become three siblings each calling `CharacterizationTest("nar-info/json-N", ".json")`.

5. **Non-trivial constructor bodies (HAZARD).** Three fixtures already have non-trivial constructors:
   - `FillInOutputPathsTest()` calls `LibStoreTest(ref<Store>)` with a custom `DummyStore` config.
   - `TryResolveTest` has data-member `EnableExperimentalFeature caFeature{"ca-derivations"}` that runs at construction.
   - `WorkerSubstitutionTest()` constructs two `DummyStore` refs and passes one to `LibStoreTest(ref<Store>)`.
   These already work fine under multi-base-init order; they just need to add `: CharacterizationTest("foo"), LibStoreTest(...)` to the existing init list. **No structural conflict.**

6. **`SetUp()`-only customizations.** Several fixtures override `SetUp()` (not the constructor) to set `mockXpSettings`, call `initLibStore(false)`, etc. These are unaffected — `SetUp()` runs after construction.

7. **Out-of-scope sites.** `nix_api_store.cc` calls `nix::getUnitTestData()` inline 6 times inside `TEST_F` bodies — these are not `CharacterizationTest` fixtures and N41 cannot touch them. They still depend on N28's `_NIX_TEST_UNIT_DATA` env var.

8. **Already-declarative bases.** `ProtoTest`/`VersionedProtoTest` already accept `protocolDir` as a template parameter (this is the same pattern N41 proposes for `CharacterizationTest`). The 3 protocol fixtures are out of scope for N41 — they got the same simplification a release ago. This is direct evidence the constructor-arg shape works.

**Net.** N41 as catalogued is valid for ~16 of the ~22 fixtures. The remaining 6 either need a second `suffix` constructor argument (4 fixtures) or special handling for `SetUp()`-time subdir reassignment (1) or aren't in scope (1). **Effort: still small** — but the `CharacterizationTest` API needs (`subdir`, optional `suffix`) parameters, not just one.

## Hazards / fixtures that don't fit

1. **`CaDerivationAdvancedAttrsTest` reassigns `unitTestData` in `SetUp()`** — `derivation-advanced-attrs.cc`. Forces the member to remain non-const and protected, *or* requires factoring out a separate fixture class with the alternate subdir baked in at construction.

2. **Suffix-appending `goldenMaster`** — 6 fixtures (`NarInfoTestV{1,2,3}`, `PathInfoTestV{1,2,3}`, `NarTest`, `StoreReferenceTest`). A constructor-arg-only API loses expressive power. Either add an optional suffix arg or keep `goldenMaster` overridable as today.

3. **Multiple fixtures per file pointing at versioned subdirs** — `nar-info.cc` (V1/V2/V3), `path-info.cc` (V1/V2/V3), `outputs-spec.cc` (base + extended), `realisation.cc` (Realisation + UnkeyedRealisation share `realisation` subdir). Compatible with the constructor-arg shape but worth a callout: N41 will still leave duplicated `CharacterizationTest("realisation")` literals in two sibling classes.

4. **Non-trivial constructor bodies** — `FillInOutputPathsTest`, `TryResolveTest`, `WorkerSubstitutionTest`. These already chain constructors and will continue to; the only new requirement is adding `CharacterizationTest("subdir")` to the mem-initializer-list.

5. **Raw inline `getUnitTestData()` calls** — `nix_api_store.cc` has 6 inline reads of `getUnitTestData() / "derivation/ca/self-contained.json"` inside test bodies. Not a `CharacterizationTest`-derived fixture; N41 cannot help. Still depends on N28's env var.

6. **`libutil-tests` does not set `HOME`** — the catalog's claim about a canonical `{ _NIX_TEST_UNIT_DATA, HOME }` block is wrong for libutil. Not a code bug; just a miscount in the catalog.

7. **The libstore-tests benchmark block does not set `HOME` and is the second of two sites in libstore-tests** — the catalog claim "libstore-tests (twice)" is correct in count but the two sites are *not* identical. The benchmark site is a strict subset.

8. **`libexpr-tests` benchmarks block has no env at all** — including no `_NIX_TEST_UNIT_DATA`. If the expr benchmarks ever start using golden data they will hit a runtime error from `getUnitTestData()`. Out of scope for N28 but worth a note.

9. **`libstore-tests` is the only site with a Windows conditional (`NIX_REMOTE`)** — not divergent, but the conditional logic must live somewhere in the shared include.

## Recommended sequencing

N28 (build-side meson include) and N41 (fixture-side `CharacterizationTest` constructor) are independent and can land in either order. They share no symbols and touch disjoint files (`*.meson.build` vs `*.cc`).

N39 (declarative test grid + `--update-goldens`) shares a target audience with both, but its main payoff requires the production-side reflection from #9/U10 (boost.describe-driven codecs) — without it, the grid is mostly a re-spelling of the existing macro stack. Defer N39 until U10 (or its replacement) is in flight.

Recommended order:

1. **N41 first.** It is the smallest semantic change (modify one base class + ~16 fixture declarations, optionally add a suffix parameter). It does not depend on any meson change. Doing it first reduces the diff of any subsequent fixture-touching work.

2. **N28 next.** Pure build-side; no runtime semantics. Refine the proposal to two layered dicts (`nix_test_env_data` and `nix_test_env_full`) given the inventory above. Land after N41 only because doing so leaves the per-fixture diffs in pure C++ and the per-suite diffs in pure meson — easier to review and bisect.

3. **N39 deferred** until production-side reflection (U10) lands. When it does, the grid format becomes a single source of truth driving both production codecs and golden-file generation.

## Open questions

1. **Should the `CharacterizationTest` constructor accept a path-suffix in addition to the subdir?** Six fixtures append a literal suffix (`.json`, `.nar`, `.txt`) to the test stem. Either (a) add `(subdir, suffix)` constructor pair, (b) push the suffix into the call sites (`readJsonTest("foo.json", ...)`) — which forces every fixture caller to know its file extension and is a regression in ergonomics, or (c) keep `goldenMaster` virtual and let those 6 fixtures override. Recommend (a).

2. **What to do about `CaDerivationAdvancedAttrsTest` reassigning `unitTestData` in `SetUp()`?** The cleanest answer is to factor `derivation/ca` as an independent fixture (calling `CharacterizationTest("derivation/ca")`), inheriting only the helper methods. That removes the runtime mutation. But it would touch test setup ordering — a minor refactor in its own right.

3. **Should the shared meson include also carry `NIX_STORE=''` and `HOME`?** Three of five `*-tests/meson.build` users carry `NIX_STORE=''`; one of them also carries `NIX_CONFIG`. The smallest scope for N28 is just `_NIX_TEST_UNIT_DATA + HOME` (the genuinely-shared parts). Adding `NIX_STORE` to the shared dict would force the libutil suites to opt out, which is more annoying than opting in. Recommend keeping the shared include minimal.

4. **What happens to the 6 inline `getUnitTestData()` reads in `nix_api_store.cc`?** N41 cannot help. They could be wrapped behind a thin `CharacterizationTest`-style fixture, but that's a separate refactor. They will continue to depend on N28's env var being set.

5. **Should `libexpr-tests` benchmarks set `_NIX_TEST_UNIT_DATA`?** Currently they do not. If any benchmark starts using golden data, it will fail. Adding the env at the benchmark `benchmark()` site is one line — worth doing as part of N28 for symmetry.

6. **N39's grid: per-fixture or global?** A global grid (one per protocol or one per type-family) would let consumers add a single line per (type, version) pair. A per-fixture grid loses most of the leverage. Settle this only after U10 lands and the production-side codec table is in hand.
