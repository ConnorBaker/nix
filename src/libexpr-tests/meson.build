project(
  'nix-expr-tests',
  'cpp',
  version : files('.version'),
  default_options : [
    'cpp_std=c++23',
    # TODO(Qyriad): increase the warning level
    'warning_level=1',
    'errorlogs=true', # Please print logs for tests that fail
    # Unity builds merge test TUs and collide on same-named static helpers
    # (e.g. DepTrackingTest, keys()) that are intentionally per-file local.
    'unity=off',
  ],
  meson_version : '>= 1.8',
  license : 'LGPL-2.1-or-later',
)

cxx = meson.get_compiler('cpp')

subdir('nix-meson-build-support/deps-lists')

deps_private_maybe_subproject = [
  dependency('nix-expr'),
  dependency('nix-expr-c'),
  dependency('nix-expr-test-support'),
]
deps_public_maybe_subproject = []
subdir('nix-meson-build-support/subprojects')

subdir('nix-meson-build-support/export-all-symbols')
subdir('nix-meson-build-support/windows-version')

rapidcheck = dependency('rapidcheck')
deps_private += rapidcheck

gtest = dependency('gtest')
deps_private += gtest

gmock = dependency('gmock')
deps_private += gmock

configdata = configuration_data()
configdata.set_quoted('PACKAGE_VERSION', meson.project_version())

config_priv_h = configure_file(
  configuration : configdata,
  output : 'expr-tests-config.hh',
)

subdir('nix-meson-build-support/common')

sources = files(
  'derived-path.cc',
  'error_traces.cc',
  'eval.cc',
  'store-path-coercion.cc',
  'path-accessor-preservation.cc',
  'eval-trace/helpers.cc',
  # eval-trace/ (top-level)
  'eval-trace/fiber-owner-thread.cc',
  # eval-trace/dep/
  'eval-trace/dep/hash.cc',
  'eval-trace/dep/format.cc',
  'eval-trace/dep/tracker.cc',
  'eval-trace/dep/child-range-exclusion.cc',
  'eval-trace/dep/epoch-record.cc',
  'eval-trace/dep/oracle-basic.cc',
  'eval-trace/dep/oracle-nested.cc',
  'eval-trace/dep/copied-path.cc',
  'eval-trace/dep/source-tree-soundness.cc',
  'eval-trace/dep/replay-pollution.cc',
  'eval-trace/dep/replay-integration.cc',
  'eval-trace/dep/dep-stability.cc',
  'eval-trace/dep/epoch-stability.cc',
  'eval-trace/dep/epoch-bugs.cc',
  'eval-trace/dep/nix-binding-determinism.cc',
  'eval-trace/dep/show-for-hash.cc',
  # eval-trace/store/
  'eval-trace/store/cache.cc',
  'eval-trace/store/record-verify.cc',
  'eval-trace/store/recovery.cc',
  'eval-trace/store/sv-telemetry.cc',
  'eval-trace/store/serialization.cc',
  'eval-trace/store/nixpkgs-patterns.cc',
  'eval-trace/store/nixpkgs-optimization.cc',
  'eval-trace/store/hash.cc',
  'eval-trace/store/git-identity.cc',
  'eval-trace/store/git-identity-recovery.cc',
  'eval-trace/store/trace-value-context.cc',
  'eval-trace/store/trace-parent-slot.cc',
  'eval-trace/store/copied-path.cc',
  'eval-trace/store/nar-identity.cc',
  'eval-trace/store/derived-store-path.cc',
  'eval-trace/store/store-path-availability.cc',
  'eval-trace/store/runtime-fetch-identity.cc',
  'eval-trace/store/call-arg-nixbinding.cc',
  'eval-trace/store/dep-overapprox.cc',
  'eval-trace/store/result-encoding.cc',
  'eval-trace/store/set-once.cc',
  'eval-trace/store/cqk-lattice.cc',
  'eval-trace/store/dep-source.cc',
  'eval-trace/store/semantic-registry.cc',
  'eval-trace/store/policy-digest.cc',
  'eval-trace/store/session-config.cc',
  'eval-trace/store/concurrency.cc',
  'eval-trace/store/error-handling.cc',
  'eval-trace/store/multi-session.cc',
  'eval-trace/store/large-trace.cc',
  'eval-trace/store/cross-session-subsumption.cc',
  'eval-trace/store/eval-info.cc',
  'eval-trace/store/in-memory-rows-backend.cc',
  # eval-trace/traced-data/
  'eval-trace/traced-data/json-scalar.cc',
  'eval-trace/traced-data/json-containers.cc',
  'eval-trace/traced-data/readdir-override.cc',
  'eval-trace/traced-data/readdir-shape.cc',
  'eval-trace/traced-data/advanced.cc',
  'eval-trace/traced-data/gaps-soundness.cc',
  'eval-trace/traced-data/gaps-rawcontent.cc',
  'eval-trace/traced-data/gaps-propagation.cc',
  # eval-trace/traced-data/dep-precision/
  'eval-trace/traced-data/dep-precision/creation.cc',
  'eval-trace/traced-data/dep-precision/typeof.cc',
  'eval-trace/traced-data/dep-precision/formals.cc',
  'eval-trace/traced-data/dep-precision/formals-comprehensive.cc',
  'eval-trace/traced-data/dep-precision/formals-cache.cc',
  'eval-trace/traced-data/dep-precision/attrkeys.cc',
  'eval-trace/traced-data/dep-precision/attrkeys-multi.cc',
  'eval-trace/traced-data/dep-precision/hasattr.cc',
  'eval-trace/traced-data/dep-precision/hasattr-multi.cc',
  'eval-trace/traced-data/dep-precision/hasattr-nix-added.cc',
  'eval-trace/traced-data/dep-precision/listlen.cc',
  'eval-trace/traced-data/dep-precision/listlen-builtins.cc',
  'eval-trace/traced-data/dep-precision/preserve.cc',
  'eval-trace/traced-data/dep-precision/edge-cases.cc',
  'eval-trace/traced-data/dep-precision/pipelines.cc',
  'eval-trace/traced-data/dep-precision/builtins-extra.cc',
  'eval-trace/traced-data/dep-precision/nixpkgs-patterns.cc',
  'eval-trace/traced-data/dep-precision/function-application.cc',
  'eval-trace/traced-data/dep-precision/language-features.cc',
  'eval-trace/traced-data/dep-precision/dirset-aggregation.cc',
  'eval-trace/traced-data/dep-precision/haskey-conflict.cc',
  # eval-trace/traced-data/
  'eval-trace/traced-data/dirset-verification.cc',
  # eval-trace/traced-data/materialization/
  'eval-trace/traced-data/materialization/single-scope.cc',
  'eval-trace/traced-data/materialization/cross-scope-attrkeys.cc',
  'eval-trace/traced-data/materialization/cross-scope-haskey.cc',
  'eval-trace/traced-data/materialization/cross-scope-typeof.cc',
  'eval-trace/traced-data/materialization/cross-scope-cache.cc',
  'eval-trace/traced-data/materialization/variant-roundtrip.cc',
  'eval-trace/traced-data/materialization/nested-diagnostic.cc',
  'eval-trace/traced-data/materialization/nested-invalidation.cc',
  'eval-trace/traced-data/materialization/multi-source.cc',
  'eval-trace/traced-data/materialization/standalone-verify.cc',
  'eval-trace/traced-data/materialization/error-recovery.cc',
  'eval-trace/traced-data/materialization/pointer-equality.cc',
  # eval-trace/verify/
  'eval-trace/verify/integration.cc',
  'eval-trace/verify/fetch-tree.cc',
  'eval-trace/verify/derivation.cc',
  'eval-trace/verify/path-coercion.cc',
  # eval-trace/nix-binding/
  'eval-trace/nix-binding/basic.cc',
  'eval-trace/nix-binding/precision.cc',
  'eval-trace/nix-binding/override.cc',
  'eval-trace/nix-binding/edge-cases.cc',
  # eval-trace/property/ — generator infrastructure
  'eval-trace/property/expr-gen.cc',
  'eval-trace/property/gen/attr-access.cc',
  'eval-trace/property/gen/attr-names-length.cc',
  'eval-trace/property/gen/attrset-pipeline.cc',
  'eval-trace/property/gen/call-package.cc',
  'eval-trace/property/gen/compound.cc',
  'eval-trace/property/gen/concat-strings.cc',
  'eval-trace/property/gen/conditional-dep.cc',
  'eval-trace/property/gen/conditional-dep-else.cc',
  'eval-trace/property/gen/deep-attrset-access.cc',
  'eval-trace/property/gen/deep-pipeline.cc',
  'eval-trace/property/gen/elem-at.cc',
  'eval-trace/property/gen/error.cc',
  'eval-trace/property/gen/filter-length.cc',
  'eval-trace/property/gen/filter-sort-multi.cc',
  'eval-trace/property/gen/fold-merge-pipeline.cc',
  'eval-trace/property/gen/foldl-sum.cc',
  'eval-trace/property/gen/from-json.cc',
  'eval-trace/property/gen/function-chain.cc',
  'eval-trace/property/gen/from-toml-access.cc',
  'eval-trace/property/gen/get-env.cc',
  'eval-trace/property/gen/has-attr.cc',
  'eval-trace/property/gen/if-then-else.cc',
  'eval-trace/property/gen/import-json-chain.cc',
  'eval-trace/property/gen/import-tree.cc',
  'eval-trace/property/gen/intersect-access.cc',
  'eval-trace/property/gen/json-conditional.cc',
  'eval-trace/property/gen/let.cc',
  'eval-trace/property/gen/list-from-json.cc',
  'eval-trace/property/gen/list-pipeline.cc',
  'eval-trace/property/gen/map-attrs-access.cc',
  'eval-trace/property/gen/map-length.cc',
  'eval-trace/property/gen/mixed-dep-string.cc',
  'eval-trace/property/gen/multi-binding-let.cc',
  'eval-trace/property/gen/nested-import-pipeline.cc',
  'eval-trace/property/gen/multi-json-string.cc',
  'eval-trace/property/gen/multi-source-attr.cc',
  'eval-trace/property/gen/nested-attrset-access.cc',
  'eval-trace/property/gen/overlay.cc',
  'eval-trace/property/gen/path-exists-if.cc',
  'eval-trace/property/gen/path-exists.cc',
  'eval-trace/property/gen/read-dir-map-attrs.cc',
  'eval-trace/property/gen/readdir-count.cc',
  'eval-trace/property/gen/readdir-json-merge.cc',
  'eval-trace/property/gen/read-file.cc',
  'eval-trace/property/gen/rec-attrset.cc',
  'eval-trace/property/gen/rec-cross-binding.cc',
  'eval-trace/property/gen/remove-attrs-access.cc',
  'eval-trace/property/gen/replace-strings.cc',
  'eval-trace/property/gen/scalar.cc',
  'eval-trace/property/gen/selective-attrset.cc',
  'eval-trace/property/gen/sibling-trace.cc',
  'eval-trace/property/gen/three-source-pipeline.cc',
  'eval-trace/property/gen/sort-length.cc',
  'eval-trace/property/gen/string-interpolation-json.cc',
  'eval-trace/property/gen/string-interpolation.cc',
  'eval-trace/property/gen/triple-source-merge.cc',
  'eval-trace/property/gen/try-eval.cc',
  'eval-trace/property/gen/with-expr.cc',
  'eval-trace/property/generator-test.cc',
  # eval-trace/property/invariant/ — core cache invariants
  'eval-trace/property/invariant/commutativity.cc',
  'eval-trace/property/invariant/cross-session-soundness.cc',
  'eval-trace/property/invariant/dep-correctness.cc',
  'eval-trace/property/invariant/determinism.cc',
  'eval-trace/property/invariant/idempotence.cc',
  'eval-trace/property/invariant/invalidation.cc',
  'eval-trace/property/invariant/monotonicity.cc',
  'eval-trace/property/invariant/precision.cc',
  'eval-trace/property/invariant/recovery.cc',
  'eval-trace/property/invariant/soundness.cc',
  'eval-trace/property/invariant/structural-override.cc',
  'eval-trace/property/invariant/trace-hash-determinism.cc',
  'eval-trace/property/invariant/trace-transparency.cc',
  # eval-trace/property/builtin/ — per-builtin dep tracking
  'eval-trace/property/builtin/attrset/attr-names.cc',
  'eval-trace/property/builtin/attrset/cat-attrs.cc',
  'eval-trace/property/builtin/attrset/has-attr.cc',
  'eval-trace/property/builtin/attrset/intersect-attrs.cc',
  'eval-trace/property/builtin/attrset/map-attrs.cc',
  'eval-trace/property/builtin/attrset/remove-attrs.cc',
  'eval-trace/property/builtin/attrset/update-operator.cc',
  'eval-trace/property/builtin/list/concat-lists.cc',
  'eval-trace/property/builtin/list/elem-at.cc',
  'eval-trace/property/builtin/list/filter.cc',
  'eval-trace/property/builtin/list/foldl.cc',
  'eval-trace/property/builtin/list/length.cc',
  'eval-trace/property/builtin/list/map.cc',
  'eval-trace/property/builtin/list/sort.cc',
  'eval-trace/property/builtin/string/concat-strings-sep.cc',
  'eval-trace/property/builtin/string/replace-strings.cc',
  'eval-trace/property/builtin/string/string-length.cc',
  'eval-trace/property/builtin/string/substring.cc',
  'eval-trace/property/builtin/string/tostring-provenance.cc',
  'eval-trace/property/builtin/fs/dir-of.cc',
  'eval-trace/property/builtin/fs/import-file.cc',
  'eval-trace/property/builtin/fs/path-exists.cc',
  'eval-trace/property/builtin/fs/pathexists-if.cc',
  'eval-trace/property/builtin/fs/read-dir.cc',
  'eval-trace/property/builtin/comparison/equality.cc',
  'eval-trace/property/builtin/comparison/less-than.cc',
  'eval-trace/property/builtin/formals/ellipsis.cc',
  'eval-trace/property/builtin/formals/no-ellipsis.cc',
  'eval-trace/property/builtin/trace/parent-slot.cc',
  'eval-trace/property/builtin/trace/value-context.cc',
  # eval-trace/property/composition/ — nixpkgs-fidelity patterns
  'eval-trace/property/composition/attrset-pipeline.cc',
  'eval-trace/property/composition/call-package.cc',
  'eval-trace/property/composition/conditional-dep.cc',
  'eval-trace/property/composition/import-tree.cc',
  'eval-trace/property/composition/list-pipeline.cc',
  'eval-trace/property/composition/multi-binding-let.cc',
  'eval-trace/property/composition/multi-source-merge.cc',
  'eval-trace/property/composition/overlay.cc',
  'eval-trace/property/composition/rec-attrset.cc',
  'eval-trace/property/composition/sibling-trace.cc',
  'eval-trace/property/composition/try-eval-error.cc',
  'eval-trace/property/composition/try-eval.cc',
  # eval-trace/property/coverage/ — builtins batch coverage
  'eval-trace/property/coverage/arithmetic-ops.cc',
  'eval-trace/property/coverage/attrvalues-ops.cc',
  'eval-trace/property/coverage/boolean-ops.cc',
  'eval-trace/property/coverage/compositions.cc',
  'eval-trace/property/coverage/container-transforms.cc',
  'eval-trace/property/coverage/eval-control.cc',
  'eval-trace/property/coverage/fromtoml.cc',
  'eval-trace/property/coverage/list-concat-op.cc',
  'eval-trace/property/coverage/list-transforms.cc',
  'eval-trace/property/coverage/readdir-attr-access.cc',
  'eval-trace/property/coverage/string-builtins.cc',
  'eval-trace/property/coverage/string-concat-op.cc',
  'eval-trace/property/coverage/toxml.cc',
  'eval-trace/property/coverage/type-checks.cc',
  # eval-trace/property/special/ — error handling, concurrency
  'eval-trace/property/special/concurrent-eval.cc',
  'eval-trace/property/special/error-path.cc',
  'json.cc',
  'lazy-fetcher-attr.cc',
  'main.cc',
  'nix_api_expr.cc',
  'nix_api_external.cc',
  'nix_api_value.cc',
  'nix_api_value_internal.cc',
  'primops.cc',
  'search-path.cc',
  'trivial.cc',
  'value/context.cc',
  'value/print.cc',
  'value/value.cc',
)

include_dirs = [
  include_directories('.'),
]


this_exe = executable(
  meson.project_name(),
  sources,
  config_priv_h,
  dependencies : deps_private_subproject + deps_private + deps_other,
  include_directories : include_dirs,
  # TODO: -lrapidcheck, see ../libutil-support/build.meson
  link_args : linker_export_flags + [ '-lrapidcheck' ],
  install : true,
  cpp_pch : do_pch ? [ 'pch/precompiled-headers.hh' ] : [],
  # Unity builds merge test TUs and collide on same-named static helpers.
  override_options : ['unity=off'],
)

test(
  meson.project_name(),
  this_exe,
  env : {
    '_NIX_TEST_UNIT_DATA' : meson.current_source_dir() / 'data',
    'HOME' : meson.current_build_dir() / 'test-home',
    'NIX_STORE' : '',
  },
  protocol : 'gtest',
)

# Build benchmarks if enabled
if get_option('benchmarks')
  gbenchmark = dependency('benchmark', required : true)

  benchmark_sources = files(
    'bench-main.cc',
    'dynamic-attrs-bench.cc',
    'get-drvs-bench.cc',
    'regex-cache-bench.cc',
  )

  benchmark_exe = executable(
    'nix-expr-benchmarks',
    benchmark_sources,
    config_priv_h,
    dependencies : deps_private_subproject + deps_private + deps_other + [
      gbenchmark,
    ],
    include_directories : include_dirs,
    link_args : linker_export_flags,
    install : true,
    cpp_pch : do_pch ? [ 'pch/precompiled-headers.hh' ] : [],
  )

  benchmark(
    'nix-expr-benchmarks',
    benchmark_exe,
  )
endif
