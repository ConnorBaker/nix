# Test-infrastructure mirrors

Candidates N28, N39, N41 (Cluster F). Tests are out of catalog scope by
design — but the *test-side patterns* are evidence of *production-side*
patterns: a test harness shape that catalogs a Cartesian product
(versions × types × formats) is *direct evidence* that the
production-side handling of that product is mechanical and hand-coded.
This section catalogs three such mirrors.

The cross-references on each candidate point at the production-side
candidate (or N-candidate) the test-side pattern reflects.

| # | Verdict | Effort |
| - | ------- | ------ |
| N28 | VALID | small |
| N39 | VALID | medium |
| N41 | VALID | small |

---

N28. **The `_NIX_TEST_UNIT_DATA` pattern is duplicated across 7 test suites.** Every test-suite `meson.build` carries an identical `_NIX_TEST_UNIT_DATA` plumbing: `test_env = { '_NIX_TEST_UNIT_DATA' : meson.current_source_dir() / 'data', 'HOME' : meson.current_build_dir() / 'test-home', }`. Pass 1 named this in passing under N18 (`_NIX_TEST_BUILD_DIR`/`_NIX_TEST_*` env vars). Independent grep of `src/*-tests/meson.build` shows 7 sites that each repeat the block: `libutil-tests` (twice — once for tests, once for benchmarks), `libstore-tests` (twice), `libfetchers-tests`, `libexpr-tests`, `libflake-tests`. The C++ side reads `_NIX_TEST_UNIT_DATA` in `getUnitTestData()` (once, in `libutil-test-support`). The build side spells out the env var by hand in every test-suite. No `nix-meson-test-suite()` macro exists.
- **Validation:** VALID. A `nix-meson-build-support/test-suite/` shared meson include that provides a `nix_test_env` dict variable (or a function) callers spread into their `test_env`. The same include can carry the `'NIX_REMOTE'` Windows-vs-Unix conditional. Pure cleanup; no semantic change. **Compounds with N18 (env-var iceberg — specifically the build-side half). See also:** Cluster F, E6 (test-support consolidation follow-up). Effort: small.

N39. **The "test-data folder" pattern mirrors the protocol versioning surface.** N16 names the `JsonCharacterizationTest<T>` / `VersionedProtoTest<Proto>` pattern. Independent inspection of `src/libstore-tests/data/` shows directories like `worker-protocol/`, `serve-protocol/`, `path-info/`, etc. Each contains `<test-stem>.bin` and `<test-stem>.json` golden files, parameterised by the cited protocol version. For each (T, Version) pair, a `<stem>.bin` golden + a `<stem>.json` golden; for each (T) JSON-only test, a `<stem>.json` golden. Two observations: (a) The test data is in source-tree; on-disk filenames encode the type and version. There is no tool that *generates* the test data from a typelist + version-list — each new test requires a developer to write the `VERSIONED_CHARACTERIZATION_TEST(...)` macro invocation, *and* commit the matching `.bin` + `.json`. N16's reflection proposal (boost.describe) auto-derives the read/write codecs; an adjacent automation could auto-generate the golden files at test time (a `--update` mode), making the test harness self-bootstrapping. (b) Several types have JSON-only golden files; conversely, several wire-serialised types have no JSON variant. The asymmetry is undocumented; the test fixture decides whether to test JSON, wire, or both, per macro choice (`VERSIONED_CHARACTERIZATION_TEST` vs `_NO_JSON`). The cataloger of section 18 should note: a production-side type that has wire serialiser but no JSON serialiser is a partial-coverage smell.
- **Validation:** VALID. A declarative test-grid format (one entry per type listing `(versions, formats)`); the harness fans out into the `TEST_F`s. Plus a `--update-goldens` mode (already convention in many projects) that regenerates the on-disk goldens. **Compounds with N16; uses U10 (boost.describe). See also:** Cluster F, #176 (the partial-coverage smell on the wire-vs-JSON asymmetry is direct evidence for #176's reflection direction). Effort: medium.

N41. **Per-test-fixture `unitTestData` member is the same line in 25+ fixtures.** Every characterisation test fixture starts with the same line: `class FooTest : public CharacterizationTest { std::filesystem::path unitTestData = getUnitTestData() / "foo"; };`. Independent grep finds 25+ `unitTestData = getUnitTestData() / ...` lines across `src/libstore-tests/`, `src/libutil-tests/`, `src/libfetchers-tests/`, etc. Each fixture spells the same member declaration — the only difference is the subdirectory name. This is bracket-bracket boilerplate the catalog targets — not a *latent bug* but pure repetition that would benefit from a `MAKE_CHARACTERIZATION_FIXTURE(name, subdir)` macro that synthesises the class declaration. A simpler shape: a `CharacterizationTest` constructor accepting the subdir name, eliminating the per-fixture `unitTestData` member entirely.
- **Validation:** VALID. In `CharacterizationTest`, replace the need for `unitTestData` member with a virtual `subdir()` method or a constructor parameter. Each fixture becomes `class FooTest : public CharacterizationTest { public: FooTest() : CharacterizationTest("foo") {} };`. Or simpler, a helper macro `CHARACTERIZATION_FIXTURE(FooTest, "foo")`. **Compounds with N16, N28, N39. See also:** Cluster F. Effort: small.
