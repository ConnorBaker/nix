{
  lib,
  stdenv,
  mkMesonDerivation,

  meson,
  ninja,
  pkg-config,

  jq,
  git,
  mercurial,
  unixtools,
  util-linux,

  nix-store,
  nix-expr,
  nix-cli,

  busybox-sandbox-shell ? null,

  # Configuration Options

  pname ? "nix-functional-tests",
  version,

  # For running the functional tests against a different pre-built Nix.
  test-daemon ? null,
}:

let
  inherit (lib) fileset;
in

mkMesonDerivation (
  finalAttrs:
  {
    inherit pname version;

    workDir = ./.;
    fileset = fileset.unions [
      ../../nix-meson-build-support
      ../../scripts/nix-profile.sh.in
      ../../.version
      ../../tests/functional
      ./.
    ];

    nativeBuildInputs = [
      meson
      ninja
      pkg-config

      jq
      git
      mercurial
      unixtools.script

      # Explicitly splice the hostHost variant to fix LLVM tests. The nix-cli
      # has to be in PATH, but must come from the host context where it's built
      # with libc++.
      (nix-cli.__spliced.hostHost or nix-cli)
    ]
    ++ lib.optionals stdenv.hostPlatform.isLinux [
      # For various sandboxing tests that needs a statically-linked shell,
      # etc.
      busybox-sandbox-shell
      # For Overlay FS tests need `mount`, `umount`, and `unshare`.
      # For `script` command (ensuring a TTY)
      # TODO use `unixtools` to be precise over which executables instead?
      util-linux
    ];

    buildInputs = [
      nix-store
      nix-expr
    ];

    preConfigure =
      # TEMP hack for Meson before make is gone, where
      # `src/nix-functional-tests` is during the transition a symlink and
      # not the actual directory directory.
      ''
        cd $(readlink -e $PWD)
        echo $PWD | grep tests/functional
      '';

    mesonCheckFlags = [
      "--print-errorlogs"
    ];

    mesonFlags = [
      (lib.mesonBool "plugin-c-api" nix-cli.exportsPluginCApi)
    ];

    doCheck = true;

    installPhase = ''
      mkdir $out
    '';

    meta = {
      platforms = lib.platforms.unix;
    };

    passthru.tests = lib.optionalAttrs stdenv.hostPlatform.isLinux {
      # Run the tests whose expectations depend on waiting for a builder's
      # exit with pidfd_open() disabled via seccomp, as on Linux before
      # 5.3, to exercise the fallback of killing the builder at log EOF.
      run-without-pidfd = finalAttrs.finalPackage.overrideAttrs (prevAttrs: {
        pname = "${prevAttrs.pname}-run-without-pidfd";
        checkPhase = ''
          runHook preCheck
          # (Its own HOME, so that resolving the store leaves nothing
          # behind in the one the tests will use.)
          if HOME=$(mktemp -d) enosys --syscall pidfd_open -- nix -v --version | grep '^Features: .*wait-for-builder-exit'; then
            echo "pidfd_open() is still available under enosys" >&2
            exit 1
          fi
          enosys --syscall pidfd_open -- meson test --no-rebuild --print-errorlogs builder-exit external-builders timeout
          runHook postCheck
        '';
      });
    };

  }
  // lib.optionalAttrs (test-daemon != null) {
    # TODO rename to _NIX_TEST_DAEMON_PACKAGE
    NIX_DAEMON_PACKAGE = test-daemon;
    _NIX_TEST_CLIENT_VERSION = nix-cli.version;
  }
)
