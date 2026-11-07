cxx = meson.get_compiler('cpp')

deps_private_no_pkgconfig = []
subdir('nix-meson-build-support/common/asan-options')

libstoreconsumer_tester = executable(
  'test-libstoreconsumer',
  'main.cc',
  # A static program has no build child helper, so it runs itself.
  cpp_args : get_option('default_library') == 'static' ? [
    '-DBUILD_CHILD_MAIN',
  ] : [],
  dependencies : deps_private_no_pkgconfig + [
    dependency('nix-store'),
    dependency('nix-store-c'),
  ],
  build_by_default : false,
)
