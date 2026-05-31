{
  lib,
  buildPackages,
  stdenv,
  mkMesonExecutable,
  writableTmpDirAsHomeHook,

  nix-fetchers,
  nix-fetchers-c,
  nix-store-test-support,

  libgit2,
  rapidcheck,
  gtest,
  gbenchmark,
  runCommand,

  # Configuration Options

  version,
  resolvePath,
  withBenchmarks ? false,
}:

let
  inherit (lib) fileset;
in

mkMesonExecutable (finalAttrs: {
  pname = "nix-fetchers-tests";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions [
    ../../nix-meson-build-support
    ./nix-meson-build-support
    ../../.version
    ./.version
    ./meson.build
    ./meson.options
    (fileset.fileFilter (file: file.hasExt "cc") ./.)
    (fileset.fileFilter (file: file.hasExt "hh") ./.)
  ];

  buildInputs = [
    nix-fetchers
    nix-fetchers-c
    nix-store-test-support
    rapidcheck
    gtest
    libgit2
  ]
  ++ lib.optionals withBenchmarks [
    gbenchmark
  ];

  mesonFlags = [
    (lib.mesonBool "benchmarks" withBenchmarks)
  ];

  passthru = {
    tests = {
      run =
        runCommand "${finalAttrs.pname}-run"
          {
            meta.broken = !stdenv.hostPlatform.emulatorAvailable buildPackages;
            buildInputs = [ writableTmpDirAsHomeHook ];
          }
          (
            ''
              export _NIX_TEST_UNIT_DATA=${resolvePath ./data}
              ${stdenv.hostPlatform.emulator buildPackages} ${lib.getExe finalAttrs.finalPackage}
            ''
            + lib.optionalString withBenchmarks ''
              ${stdenv.hostPlatform.emulator buildPackages} ${lib.getExe' finalAttrs.finalPackage "nix-fetchers-benchmarks"}
            ''
            + ''
              touch $out
            ''
          );
    };
  };

  meta = {
    platforms = lib.platforms.unix ++ lib.platforms.windows;
    mainProgram = finalAttrs.pname + stdenv.hostPlatform.extensions.executable;
  };

})
