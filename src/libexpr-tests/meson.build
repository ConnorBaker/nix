project(
  'nix-expr-tests',
  'cpp',
  version : files('.version'),
  default_options : [
    'cpp_std=c++23',
    # TODO(Qyriad): increase the warning level
    'warning_level=1',
    'errorlogs=true', # Please print logs for tests that fail
  ],
  meson_version : '>= 1.1',
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
  'eval-trace/helpers.cc',
  # eval-trace/dep/
  'eval-trace/dep/hash.cc',
  'eval-trace/dep/format.cc',
  'eval-trace/dep/tracker.cc',
  'eval-trace/dep/child-range-exclusion.cc',
  'eval-trace/dep/dep-stability.cc',
  'eval-trace/dep/oracle-basic.cc',
  'eval-trace/dep/oracle-nested.cc',
  'eval-trace/dep/copied-path.cc',
  'eval-trace/dep/stat-hash-store.cc',
  'eval-trace/dep/replay-pollution.cc',
  'eval-trace/dep/replay-integration.cc',
  # eval-trace/store/
  'eval-trace/store/cache.cc',
  'eval-trace/store/record-verify.cc',
  'eval-trace/store/recovery.cc',
  'eval-trace/store/serialization.cc',
  'eval-trace/store/parent-context.cc',
  'eval-trace/store/nixpkgs-patterns.cc',
  'eval-trace/store/nixpkgs-optimization.cc',
  'eval-trace/store/hash.cc',
  'eval-trace/store/git-identity.cc',
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
  # eval-trace/verify/
  'eval-trace/verify/cold-compare.cc',
  'eval-trace/verify/cold-verify.cc',
  'eval-trace/verify/per-sibling-core.cc',
  'eval-trace/verify/per-sibling-edge.cc',
  'eval-trace/verify/integration.cc',
  # eval-trace/nix-binding/
  'eval-trace/nix-binding/basic.cc',
  'eval-trace/nix-binding/precision.cc',
  'eval-trace/nix-binding/override.cc',
  'eval-trace/nix-binding/edge-cases.cc',
  'json.cc',
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
)

test(
  meson.project_name(),
  this_exe,
  env : {
    '_NIX_TEST_UNIT_DATA' : meson.current_source_dir() / 'data',
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
