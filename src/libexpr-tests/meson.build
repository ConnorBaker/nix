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
  'hvm4/hvm4-arithmetic-test.cc',
  'hvm4/hvm4-attrs-test.cc',
  'hvm4/hvm4-backend-test.cc',
  'hvm4/hvm4-bigint-test.cc',
  'hvm4/hvm4-boolean-test.cc',
  'hvm4/hvm4-capability-test.cc',
  'hvm4/hvm4-closure-test.cc',
  'hvm4/hvm4-comparison-test.cc',
  'hvm4/hvm4-conditional-test.cc',
  'hvm4/hvm4-derivation-test.cc',
  'hvm4/hvm4-float-test.cc',
  'hvm4/hvm4-import-test.cc',
  'hvm4/hvm4-integration-test.cc',
  'hvm4/hvm4-known-limitations-test.cc',
  'hvm4/hvm4-lambda-test.cc',
  'hvm4/hvm4-let-test.cc',
  'hvm4/hvm4-list-test.cc',
  'hvm4/hvm4-null-test.cc',
  'hvm4/hvm4-path-test.cc',
  'hvm4/hvm4-pattern-test.cc',
  'hvm4/hvm4-primop-test.cc',
  'hvm4/hvm4-rec-test.cc',
  'hvm4/hvm4-runtime-test.cc',
  'hvm4/hvm4-stress-test.cc',
  'hvm4/hvm4-string-test.cc',
  'hvm4/hvm4-with-test.cc',
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

include_dirs = [ include_directories('.'), include_directories('hvm4') ]


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
