project(
  'nix-fetchers-tests',
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
  dependency('nix-store-test-support'),
  dependency('nix-fetchers'),
  dependency('nix-fetchers-c'),
]
deps_public_maybe_subproject = []
subdir('nix-meson-build-support/subprojects')

subdir('nix-meson-build-support/export-all-symbols')
subdir('nix-meson-build-support/windows-version')

rapidcheck = dependency('rapidcheck')
deps_private += rapidcheck

gtest = dependency('gtest', main : true)
deps_private += gtest

libgit2 = dependency('libgit2')
deps_private += libgit2

subdir('nix-meson-build-support/common')

sources = files(
  'access-tokens.cc',
  'algebra-cross-cutting.cc',
  'attrs.cc',
  'directory-synthesizer-source-accessor.cc',
  'factory-fusion.cc',
  'fetch-to-store-bridge.cc',
  'filtered-shape.cc',
  'git-fingerprint.cc',
  'git-lazy-fetch-setting.cc',
  'git-promisor-wiring.cc',
  'git-utils.cc',
  'git.cc',
  'input.cc',
  'layer-source-accessor.cc',
  'mask-source-accessor.cc',
  'nix_api_fetchers.cc',
  'path-set.cc',
  'pkt-line.cc',
  'projection.cc',
  'public-key.cc',
  'restrict-source-accessor.cc',
  'soundness-guard-source-accessor.cc',
  'source-view.cc',
  'strip-prefix-source-accessor.cc',
  'translate-source-accessor.cc',
  'workdir-overlay.cc',
)

include_dirs = [ include_directories('.') ]


this_exe = executable(
  meson.project_name(),
  sources,
  dependencies : deps_private_subproject + deps_private + deps_other,
  include_directories : include_dirs,
  # TODO: -lrapidcheck, see ../libutil-support/build.meson
  link_args : linker_export_flags + [ '-lrapidcheck' ],
  # get main from gtest
  install : true,
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

# Build benchmarks if enabled. These microbenchmark OUR source-materialisation
# mechanisms (fingerprint composition, projection key encoding, the
# filtered-shape walk) as a single-tree regression guard. The cross-tree
# comparison lives in benchmarks/ (gbench links one tree's libs and so cannot
# compare across forks).
if get_option('benchmarks')
  gbenchmark = dependency('benchmark', required : true)

  benchmark_sources = files(
    'bench-main.cc',
    'fingerprint-bench.cc',
    'filtered-shape-bench.cc',
    'projection-bench.cc',
  )

  benchmark_exe = executable(
    'nix-fetchers-benchmarks',
    benchmark_sources,
    dependencies : deps_private_subproject + deps_private + deps_other + [
      gbenchmark,
    ],
    include_directories : include_dirs,
    link_args : linker_export_flags,
    install : true,
  )

  benchmark(
    'nix-fetchers-benchmarks',
    benchmark_exe,
    env : {
      '_NIX_TEST_UNIT_DATA' : meson.current_source_dir() / 'data',
      'HOME' : meson.current_build_dir() / 'bench-home',
      'NIX_STORE' : '',
    },
  )
endif
