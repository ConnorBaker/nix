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
  'boundary-audit-attrcursor-getstring.cc',
  'boundary-audit-attrdb-setstring.cc',
  'boundary-audit-body-slicing.cc',
  'boundary-audit-builtins-getflake.cc',
  'boundary-audit-builtins-hashstring.cc',
  'boundary-audit-builtins-tofile.cc',
  'boundary-audit-derivation-strict.cc',
  'boundary-audit-eval-json.cc',
  'boundary-audit-eval-raw.cc',
  'boundary-audit-flake-input-attrs.cc',
  'boundary-audit-flake-ref-to-string.cc',
  'boundary-audit-instantiate-eval.cc',
  'boundary-audit-nix-env-query-json.cc',
  'boundary-audit-nix-env-query-xml.cc',
  'boundary-audit-nix-prefetch.cc',
  'boundary-audit-nix-run-program.cc',
  'boundary-audit-discard-context.cc',
  'boundary-audit-prim-import.cc',
  'boundary-audit-property.cc',
  'boundary-audit-repl-print.cc',
  'boundary-audit-replace-strings.cc',
  'boundary-audit-trace-warn-throw.cc',
  'boundary-audit-user-env-manifest.cc',
  'boundary-audit-value-to-xml-derivation.cc',
  'derived-path.cc',
  'error_traces.cc',
  'eval.cc',
  'json.cc',
  'lazy-derivation-defer.cc',
  'lazy-fetcher-attr.cc',
  'input-materialisation.cc',
  'main.cc',
  'materialisation-scheduler.cc',
  'nix_api_expr.cc',
  'nix_api_external.cc',
  'nix_api_value.cc',
  'nix_api_value_internal.cc',
  'parse-cache.cc',
  'primops.cc',
  'search-path.cc',
  'trivial.cc',
  'value/context.cc',
  'value/print.cc',
  'value/value.cc',
)

include_dirs = [ include_directories('.') ]


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
    'materialisation-scheduler-bench.cc',
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

  benchmark('nix-expr-benchmarks', benchmark_exe)
endif
