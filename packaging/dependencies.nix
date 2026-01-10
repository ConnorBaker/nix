# These overrides are applied to the dependencies of the Nix components.

{
  # Flake inputs; used for sources
  inputs,

  # The raw Nixpkgs, not affected by this scope
  pkgs,

  stdenv,
}:

let
  inherit (pkgs) lib;
in
scope: {
  inherit stdenv;

  boehmgc =
    (pkgs.boehmgc.override {
      enableLargeConfig = true;
    }).overrideAttrs
      (attrs: {
        # Increase the initial mark stack size to avoid stack
        # overflows, since these inhibit parallel marking (see
        # GC_mark_some()). To check whether the mark stack is too
        # small, run Nix with GC_PRINT_STATS=1 and look for messages
        # such as `Mark stack overflow`, `No room to copy back mark
        # stack`, and `Grew mark stack to ... frames`.
        NIX_CFLAGS_COMPILE = "-DINITIAL_MARK_STACK_SIZE=1048576";
      });

  # Immer persistent data structures for attribute sets
  # Upgraded to 0.9.0 and patched for Boehm GC compatibility
  immer = pkgs.immer.overrideAttrs (old: rec {
    version = "0.9.0";
    src = pkgs.fetchFromGitHub {
      owner = "arximboldi";
      repo = "immer";
      rev = "v${version}";
      hash = "sha256-jP0DXOu21jMkPunXv4CIBb4wVusT21GAWaGcVDnxzzE=";
    };
    patches = (old.patches or [ ]) ++ [
      # Fix static initialization order issues with Boehm GC by using
      # lazy initialization for the `noone` sentinel in transience policies.
      ./immer-lazy-noone.patch
    ];
    # Disable tests, examples, benchmarks, and fuzzers
    cmakeFlags = (old.cmakeFlags or [ ]) ++ [
      "-Dimmer_BUILD_TESTS=OFF"
      "-Dimmer_BUILD_EXAMPLES=OFF"
      "-Dimmer_BUILD_EXTRAS=OFF"
      "-Dimmer_BUILD_DOCS=OFF"
    ];
    # Keep header-only installation behavior
    dontBuild = true;
    dontUseCmakeBuildDir = true;
  });

  lowdown = pkgs.lowdown.overrideAttrs (prevAttrs: rec {
    version = "2.0.2";
    src = pkgs.fetchurl {
      url = "https://kristaps.bsd.lv/lowdown/snapshots/lowdown-${version}.tar.gz";
      hash = "sha512-cfzhuF4EnGmLJf5EGSIbWqJItY3npbRSALm+GarZ7SMU7Hr1xw0gtBFMpOdi5PBar4TgtvbnG4oRPh+COINGlA==";
    };
    nativeBuildInputs = prevAttrs.nativeBuildInputs ++ [ pkgs.buildPackages.bmake ];
    postInstall =
      lib.replaceStrings [ "lowdown.so.1" "lowdown.1.dylib" ] [ "lowdown.so.2" "lowdown.2.dylib" ]
        (prevAttrs.postInstall or "");
  });

  # TODO: Remove this when https://github.com/NixOS/nixpkgs/pull/442682 is included in a stable release
  toml11 =
    if lib.versionAtLeast pkgs.toml11.version "4.4.0" then
      pkgs.toml11
    else
      pkgs.toml11.overrideAttrs rec {
        version = "4.4.0";
        src = pkgs.fetchFromGitHub {
          owner = "ToruNiina";
          repo = "toml11";
          tag = "v${version}";
          hash = "sha256-sgWKYxNT22nw376ttGsTdg0AMzOwp8QH3E8mx0BZJTQ=";
        };
      };

  # TODO Hack until https://github.com/NixOS/nixpkgs/issues/45462 is fixed.
  boost =
    (pkgs.boost.override {
      extraB2Args = [
        "--with-container"
        "--with-context"
        "--with-coroutine"
        "--with-iostreams"
        "--with-url"
      ];
      enableIcu = false;
    }).overrideAttrs
      (old: {
        # Need to remove `--with-*` to use `--with-libraries=...`
        buildPhase = lib.replaceStrings [ "--without-python" ] [ "" ] old.buildPhase;
        installPhase = lib.replaceStrings [ "--without-python" ] [ "" ] old.installPhase;
      });
}
