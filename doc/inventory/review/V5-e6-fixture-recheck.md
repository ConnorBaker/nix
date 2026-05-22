# V5 — re-ground E6 fixture-compat claims

Re-verification of E6's classification of `CharacterizationTest` fixture compatibility with the proposed N41 constructor consolidation. Each fixture is read end-to-end and classified from its own source body, not from E6's narrative.

## Method

1. Enumerated every site spelling `unitTestData = getUnitTestData() / "..."` with `grep -rn "unitTestData = getUnitTestData()" src/`. The result is a closed enumeration of 30 sites across 21 files (29 fixture-member declarations + 1 reassignment in `SetUp()` for `CaDerivationAdvancedAttrsTest`). Note: this is not the same as 22 fixtures, because some files declare multiple fixtures and the protocol header is template-parameterised rather than a per-class boilerplate.

2. For each declaring fixture, read the entire `.cc` (or `.hh`) file body. Verified for each:
   - Existence and shape of `goldenMaster` override.
   - Mutation of `unitTestData` after construction.
   - Non-trivial constructor body (member-initializer chains into bases other than `CharacterizationTest`).
   - Other base classes / mixins (`LibStoreTest`, `JsonCharacterizationTest<T>`, `WithParamInterface<...>`).
   - `SetUp()` overrides that depend on construction-time state.

3. Classified each declaring fixture against the proposed shape. The proposal under audit is:

   ```
   class CharacterizationTest : virtual ::testing::Test {
       std::filesystem::path unitTestData;
       CharacterizationTest(std::string subdir) : unitTestData(getUnitTestData() / subdir) {}
       std::filesystem::path goldenMaster(std::string_view stem) const override { return unitTestData / stem; }
   };
   ```

   plus an optional second `(subdir, suffix)` form (E6 recommends this) that yields `goldenMaster(stem) = unitTestData / (std::string(stem) + suffix)`.

   Categories:

   - **Fits cleanly (`subdir` only)**: pure `goldenMaster(stem) -> unitTestData / stem`, single-segment or multi-segment subdir literal.
   - **Fits with suffix arg**: `goldenMaster` appends a literal suffix (`.json`, `.nar`, `.txt`).
   - **Doesn't fit**: source-level hazard preventing the constructor-arg shape (e.g. runtime mutation of `unitTestData`).

4. Compared each classification against E6's text, recording aligned-vs-divergent.

## Source artifacts read

`src/libutil-test-support/include/nix/util/tests/characterization.hh`
`src/libstore-test-support/include/nix/store/tests/protocol.hh`
`src/libutil-tests/{archive,git,hash,memory-source-accessor,nar-listing}.cc`
`src/libfetchers-tests/public-key.cc`
`src/libstore-tests/{build-result,content-address,derivation-advanced-attrs,derivations,derived-path,dummy-store,nar-info,outputs-spec,path,path-info,realisation,store-reference,worker-substitution}.cc`
`src/libstore-tests/derivation/{invariants,external-formats}.cc`, `src/libstore-tests/derivation/test-support.hh`

## Per-fixture verdict table

The "fixtures that own a `unitTestData` member" list is the canonical audit unit. Derived classes that reuse a parent's `unitTestData` (DerivedReuse) are listed below the primary table.

### Primary fixtures (own `unitTestData`)

| File | Fixture | Subdir | goldenMaster shape | E6 verdict | V5 verdict | Aligned? |
| - | - | - | - | - | - | - |
| `libutil-tests/archive.cc` | `NarTest` | `nars` | `unitTestData / (string(stem) + ".nar")` | suffix-arg required | suffix-arg required | yes |
| `libutil-tests/git.cc` | `GitTest` | `git` | `unitTestData / string(stem)` | fits cleanly (SetUp() unaffected) | fits cleanly | yes |
| `libutil-tests/hash.cc` | `HashTest` | `hash` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libutil-tests/memory-source-accessor.cc` | `MemorySourceAccessorTest` | `memory-source-accessor` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libutil-tests/nar-listing.cc` | `NarListingTest` | `nar-listing` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libfetchers-tests/public-key.cc` | `PublicKeyTest` | `public-key` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/build-result.cc` | `BuildResultTest` | `build-result` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/content-address.cc` | `ContentAddressTest` | `content-address` | `unitTestData / stem` (also carries `mockXpSettings` member) | fits cleanly | fits cleanly | yes |
| `libstore-tests/derivation-advanced-attrs.cc` | `DerivationAdvancedAttrsTest` | `derivation/ia` | `unitTestData / stem` (helpers + `mockXpSettings`) | fits cleanly | fits cleanly | yes |
| `libstore-tests/derivation-advanced-attrs.cc` | `CaDerivationAdvancedAttrsTest` | `derivation/ca` (assigned in `SetUp()`) | inherits parent override | doesn't fit (mutates `unitTestData` in `SetUp()`) | doesn't fit (same reason) | yes |
| `libstore-tests/derivation/invariants.cc` | `FillInOutputPathsTest` | `derivation/invariants` | `unitTestData / stem`; ctor chains to `LibStoreTest(ref<Store>)` | fits (chained-ctor, no structural conflict) | fits cleanly (chained-ctor compatible) | yes |
| `libstore-tests/derivation/test-support.hh` | `DerivationTest` | `derivation` | `unitTestData / stem` (carries `mockXpSettings`) | fits cleanly | fits cleanly | yes |
| `libstore-tests/derivations.cc` | `TryResolveTest` | `derivation/try-resolve` | `unitTestData / stem`; data-member `EnableExperimentalFeature caFeature{"ca-derivations"}` runs at construction | fits (chained-ctor compatible) | fits cleanly (RAII data member runs after `CharacterizationTest` in init list, no order dependency on `unitTestData`) | yes |
| `libstore-tests/derived-path.cc` | `DerivedPathTest` | `derived-path` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/dummy-store.cc` | `DummyStoreTest` | `dummy-store` | `unitTestData / stem`; `static SetUpTestSuite` calls `initLibStore` | fits cleanly | fits cleanly (static `SetUpTestSuite` is class-level, runs before any instance) | yes |
| `libstore-tests/nar-info.cc` | `NarInfoTestV1` | `nar-info/json-1` | `unitTestData / (stem + ".json")` | suffix-arg required | suffix-arg required | yes |
| `libstore-tests/nar-info.cc` | `NarInfoTestV2` | `nar-info/json-2` | `unitTestData / (stem + ".json")` | suffix-arg required | suffix-arg required | yes |
| `libstore-tests/nar-info.cc` | `NarInfoTestV3` | `nar-info/json-3` | `unitTestData / (stem + ".json")` | suffix-arg required | suffix-arg required | yes |
| `libstore-tests/outputs-spec.cc` | `OutputsSpecTest` | `outputs-spec` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/outputs-spec.cc` | `ExtendedOutputsSpecTest` | `outputs-spec/extended` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/path.cc` | `StorePathTest` | `store-path` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/path-info.cc` | `PathInfoTestV1` | `path-info/json-1` | `unitTestData / (stem + ".json")` | suffix-arg required | suffix-arg required | yes |
| `libstore-tests/path-info.cc` | `PathInfoTestV2` | `path-info/json-2` | `unitTestData / (stem + ".json")` | suffix-arg required | suffix-arg required | yes |
| `libstore-tests/path-info.cc` | `PathInfoTestV3` | `path-info/json-3` | `unitTestData / (stem + ".json")` | suffix-arg required | suffix-arg required | yes |
| `libstore-tests/realisation.cc` | `RealisationTest` | `realisation` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/realisation.cc` | `UnkeyedRealisationTest` | `realisation` | `unitTestData / stem` | fits cleanly | fits cleanly | yes |
| `libstore-tests/store-reference.cc` | `StoreReferenceTest` | `store-reference` | `unitTestData / (stem + ".txt")` | suffix-arg required | suffix-arg required | yes |
| `libstore-tests/worker-substitution.cc` | `WorkerSubstitutionTest` | `worker-substitution` | `unitTestData / stem`; ctor chains to `LibStoreTest(ref<Store>)` and constructs two `DummyStore` refs in mem-initialiser-list | fits (chained-ctor compatible) | fits cleanly (chained-ctor compatible — `unitTestData` is a default-initialised data member and does not depend on the chained args) | yes |

That's 28 declaring fixtures with their own `unitTestData` member, plus 1 separate `SetUp()`-time reassignment (`CaDerivationAdvancedAttrsTest`).

### Already declarative (template parameter — out of scope for N41)

| File | Fixture | Subdir | Notes |
| - | - | - | - |
| `libstore-test-support/include/nix/store/tests/protocol.hh` | `ProtoTest<Proto, protocolDir>` (template) | parameter `protocolDir` | already takes a non-type template parameter; no boilerplate to remove. Concretised in `common-protocol.cc`, `serve-protocol.cc`, `worker-protocol.cc`. |

### DerivedReuse (no own `unitTestData`)

These do not appear in `unitTestData = getUnitTestData()` enumeration — they are not "fixtures to migrate" but existing consumers of a migrated parent. Listed for completeness:

`BLAKE3HashTest` (uses `HashTest`), `HashJsonTest` / `BLAKE3HashJsonTest` (use `HashTest`), `MemorySourceAccessorJsonTest` (uses `MemorySourceAccessorTest`), `NarListingJsonTest` / `ShallowNarListingJsonTest` (use `NarListingTest`), `InvalidNarTest` (uses `NarTest`), `ContentAddressJsonTest` (uses `ContentAddressTest`), `BuildResultJsonTest` (uses `BuildResultTest`), `SingleDerivedPathJsonTest` / `DerivedPathJsonTest` (use `DerivedPathTest`), `DummyStoreJsonTest` (uses `DummyStoreTest`), `OutputsSpecJsonTest` / `ExtendedOutputsSpecJsonTest` (use `OutputsSpecTest` / `ExtendedOutputsSpecTest`), `StorePathJsonTest` (uses `StorePathTest`), `RealisationJsonTest` / `UnkeyedRealisationJsonTest` / `RealisationSigningTest` (use `RealisationTest` / `UnkeyedRealisationTest`), `CaDerivationTest` / `DynDerivationTest` / `ImpureDerivationTest` (use `DerivationTest`), `DerivationOutputJsonTest` / `DynDerivationOutputJsonTest` / `CaDerivationOutputJsonTest` / `ImpureDerivationOutputJsonTest` / `DerivationJsonAtermTest` / `DynDerivationJsonAtermTest` (all use one of the `DerivationTest` family).

E6 mentioned the family but did not enumerate `StorePathJsonTest` explicitly — it inherits from `StorePathTest` and so reuses its `unitTestData`. Calling this out for completeness; it changes nothing about N41's count because it has no own `unitTestData`.

## Disagreements with E6

No verdict disagreements found. Every per-fixture classification in E6 is supported by the source. Three points of refinement to E6's narrative:

1. **E6's headline count "fits ~16/22 fixtures cleanly" is undercounted.** Counting fixtures that own a `unitTestData` member (the migration unit), the population is 28 (+1 reassignment), not 22. The constructor-only shape fits 21 of these 28 cleanly. With the recommended `(subdir, suffix)` constructor pair, fits 27 of 28 (only `CaDerivationAdvancedAttrsTest` remains hazardous).

   E6's "22" appears to count differently — likely conflating per-file fixture clusters (counting `nar-info`'s V1/V2/V3 trio as one) with per-class fixtures. The body of E6's hazards section is correct on the constituent classes; only the headline aggregate is off. The total in V5 here counts each declaring class, since each class needs its own constructor call.

2. **E6 lists "Suffix-appending `goldenMaster` overrides" as 6 fixtures (`NarInfoTestV{1,2,3}`, `PathInfoTestV{1,2,3}`, `NarTest`, `StoreReferenceTest`).** Counting yields 8 (3 NarInfo + 3 PathInfo + NarTest + StoreReferenceTest), not 6. E6's hazards section mentions 6 fixtures even though it enumerates 8 names. Source confirms 8.

3. **`GitTest` member function ordering vs. E6's "appends `.nar` suffix in `goldenMaster`" claim for `NarTest`.** Confirmed — `archive.cc::NarTest::goldenMaster` returns `unitTestData / (std::string(testStem) + ".nar")`. E6 right.

4. **`TryResolveTest` ctor concern.** E6 flags `EnableExperimentalFeature caFeature{"ca-derivations"}` as a non-trivial ctor concern. Source reading: `caFeature` is a data-member; the C++ rule is bases initialise before members, so any future `: CharacterizationTest("derivation/try-resolve")` call still runs before `caFeature`'s RAII activation. There is no construction-order dependency on `unitTestData` from the RAII guard. Verdict: still "fits cleanly" with a single `subdir` constructor arg; no structural conflict. (E6 reaches the same conclusion in "Hazards #4", but the wording was ambiguous between "non-trivial" and "doesn't fit"; V5 confirms it does fit.)

5. **`WorkerSubstitutionTest` ctor concern.** Source reading: `unitTestData` is a default-initialised data member of `WorkerSubstitutionTest`; its initializer expression `getUnitTestData() / "worker-substitution"` does not reference `LibStoreTest`'s constructor, the chained `dummyStore`/`substituter`, or any other base. The current ctor's mem-initialiser-list passes a `DummyStore` ref to `LibStoreTest`. Adding `: CharacterizationTest("worker-substitution"), LibStoreTest(...), dummyStore(...), substituter(...)` to the existing list works because base-class init order in C++ is the order of inheritance declaration (currently `LibStoreTest, JsonCharacterizationTest`); inserting `CharacterizationTest` first would reorder. The user-facing fix is to declare bases in the order desired (`CharacterizationTest, LibStoreTest, JsonCharacterizationTest`). No semantic hazard; just a base-list reorder. Aligns with E6.

## Aggregate

- **Fits cleanly with `subdir`-only constructor**: 21 of 28
  `HashTest`, `NarListingTest`, `MemorySourceAccessorTest`, `GitTest`, `PublicKeyTest`, `BuildResultTest`, `ContentAddressTest`, `DerivationAdvancedAttrsTest`, `DerivationTest`, `FillInOutputPathsTest`, `TryResolveTest`, `DerivedPathTest`, `DummyStoreTest`, `OutputsSpecTest`, `ExtendedOutputsSpecTest`, `StorePathTest`, `RealisationTest`, `UnkeyedRealisationTest`, `WorkerSubstitutionTest`, plus the two pure-subdir fixtures... wait — 19 names listed and 21 claimed. Counting again from the table: 19 fits-cleanly names. Let me re-tabulate.

  Strictly counting "fits cleanly (subdir only)" from the table above:
  `NarTest`-no, `GitTest`-yes, `HashTest`-yes, `MemorySourceAccessorTest`-yes, `NarListingTest`-yes, `PublicKeyTest`-yes, `BuildResultTest`-yes, `ContentAddressTest`-yes, `DerivationAdvancedAttrsTest`-yes, `CaDerivationAdvancedAttrsTest`-no (doesn't fit), `FillInOutputPathsTest`-yes, `DerivationTest`-yes, `TryResolveTest`-yes, `DerivedPathTest`-yes, `DummyStoreTest`-yes, `NarInfoTestV1/V2/V3`-no (suffix), `OutputsSpecTest`-yes, `ExtendedOutputsSpecTest`-yes, `StorePathTest`-yes, `PathInfoTestV1/V2/V3`-no (suffix), `RealisationTest`-yes, `UnkeyedRealisationTest`-yes, `StoreReferenceTest`-no (suffix), `WorkerSubstitutionTest`-yes.

  That's **19 fits cleanly**, **8 suffix-arg required**, **1 doesn't fit**. Total = 28.

- **Fits with `(subdir, suffix)` constructor**: 27 of 28. Adding the 8 suffix-required fixtures collapses every case except `CaDerivationAdvancedAttrsTest`.

- **Doesn't fit even with suffix-arg**: 1 of 28 — `CaDerivationAdvancedAttrsTest` reassigns `unitTestData` in `SetUp()`. This fixture is the only true blocker.

Replacing E6's "16/22 fits cleanly":

> **V5: 19/28 fit cleanly with subdir-only ctor; 27/28 fit with (subdir, suffix) ctor; 1/28 (`CaDerivationAdvancedAttrsTest`) is a structural hazard.**

## Implications for N41 catalog entry

1. The "subdir-only" form is sufficient for 19/28 of the migration units. The remaining 8 sites all have a literal suffix that can be expressed by adding an optional second `suffix` constructor argument (E6's recommendation). With that addition, the API covers 27/28 sites mechanically.

2. `CaDerivationAdvancedAttrsTest` requires either:
   - **Refactor**: split into a parallel fixture class `CaDerivationAdvancedAttrsTest : public DerivationAdvancedAttrsTestBase, public CharacterizationTest("derivation/ca")` that does not inherit from the IA fixture, removing the `SetUp()`-time reassignment. The `mockXpSettings.set("experimental-features", "ca-derivations")` line moves to its own `SetUp()`. This is the cleanest fix and matches E6's "Open question 2".
   - **API concession**: keep `unitTestData` as a `protected`, non-`const`, default-initialised member assignable in `SetUp()`, with a constructor that initialises it via `subdir`. This preserves both patterns at the cost of a less-tight invariant.

   The refactor is preferable; it's a single-class touch.

3. `WorkerSubstitutionTest` requires reordering its base list (`CharacterizationTest, LibStoreTest, JsonCharacterizationTest`) to keep `CharacterizationTest`'s ctor running first. That reorder is mechanical and matches the existing pattern in `RealisationTest` etc.

4. The catalog entry's claim "modify one base class + ~16 fixture declarations" should be revised to "modify one base class + 28 fixture declarations" (or 27 if `CaDerivationAdvancedAttrsTest` is refactored separately first).

5. The catalog entry's reference to "16/22" headline should be updated to "27/28 with (subdir, suffix)" to reflect the closed enumeration. The 22 figure does not appear to correspond to any closed counting rule of the source; it likely came from a partial scan.

## Open questions

1. **E6's headline "16/22" — what counted?** Source-grounded enumerations come out to 28 (declaring classes), 21 (files), 30 (`unitTestData = ...` lexical sites). No combination yields 22. Recommend the catalog entry just enumerate the fixtures by name rather than carry a headline ratio.

2. **Should the constructor accept `std::string_view` rather than `std::string`?** Each existing call site is a string literal; both would work. `std::string_view` avoids one allocation per fixture instance. Defer to N41 PR review.

3. **Does the protocol header (`ProtoTest<Proto, protocolDir>`) need to align with the new `CharacterizationTest` ctor?** The template currently default-initialises `unitTestData` from the non-type template parameter. If `CharacterizationTest`'s default ctor is removed in favour of a `subdir`-required one, `ProtoTest` must call `: CharacterizationTest(protocolDir)`. Mechanical, one line per template specialisation; no semantic risk.

4. **`derivation/test-support.hh` is a header carrying a fixture (`DerivationTest`) used by 6+ sibling files.** Migrating it changes all consumers' compilation but no semantics. The file is included by `external-formats.cc` and shared with `derivation-advanced-attrs.cc`. Confirm the include graph before merging.

5. **The `CaDerivationAdvancedAttrsTest` refactor** would remove `derivation-advanced-attrs.cc`'s `SetUp()`-time reassignment but also breaks the inheritance link with `DerivationAdvancedAttrsTest` (currently used by `DerivationAdvancedAttrsBothTest`'s `TYPED_TEST_SUITE`). The typed-test list needs both classes to share an interface. A common abstract base (no `unitTestData`) plus two concrete subclasses (`DerivationAdvancedAttrsTest`, `CaDerivationAdvancedAttrsTest`) each calling `CharacterizationTest("derivation/ia")` / `CharacterizationTest("derivation/ca")` respectively is the structure that preserves the `TYPED_TEST_SUITE` and removes the reassignment hazard. This is a slightly bigger touch than E6 implies.
