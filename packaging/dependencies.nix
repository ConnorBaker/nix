# These overrides are applied to the dependencies of the Nix components.

{
  # The raw Nixpkgs, not affected by this scope
  pkgs,

  stdenv,
}:

let
  inherit (pkgs) lib;
in
scope: {
  inherit stdenv;

  # mimalloc must not advise its arenas to use transparent huge pages. It
  # calls madvise(MADV_HUGEPAGE) on each arena it reserves, 1 GiB at first.
  # Where transparent_hugepage/defrag is `madvise` (the kernel's default), a
  # page fault in such an arena that finds no free 2 MiB block compacts and
  # reclaims memory synchronously, and where free memory is fragmented the
  # compaction mostly fails and the fault falls back to small pages after the
  # stall. Each of the daemon's workers is forked from it and touches a few
  # tens of MiB of the arena it inherits.
  #
  # The C define MI_NO_THP removes only that madvise call. allow_thp keeps its
  # default, so where transparent huge pages are `always` the kernel can still
  # back the arenas with them, and mimalloc still purges in 2 MiB units so as
  # not to split them. The CMake option of the same name sets no such define;
  # it selects allow_thp=0 instead, which calls prctl(PR_SET_THP_DISABLE), and
  # every process the daemon starts, builds included, would inherit that
  # across execve. The NixOS test mimalloc-thp-advice fails if mimalloc
  # advises again or disables them.
  #
  # Nix's BumpMemoryResource still asks for huge pages for the evaluator's
  # memory, and its page faults face the same stalls where they cannot be had.
  mimalloc = pkgs.mimalloc.overrideAttrs (attrs: {
    env = attrs.env or { } // {
      NIX_CFLAGS_COMPILE = toString [
        (attrs.env.NIX_CFLAGS_COMPILE or "")
        "-DMI_NO_THP"
      ];
    };
  });

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
        env.NIX_CFLAGS_COMPILE = "-DINITIAL_MARK_STACK_SIZE=1048576";
      });

  curl = pkgs.curl.override {
    http3Support = !pkgs.stdenv.hostPlatform.isWindows;
    # Make sure we enable all the dependencies for Content-Encoding/Transfer-Encoding decompression.
    zstdSupport = true;
    brotliSupport = true;
    zlibSupport = true;
  };

  libblake3 = pkgs.libblake3.override {
    useTBB =
      !(
        stdenv.hostPlatform.isWindows
        || stdenv.hostPlatform.isStatic
        # Some tbb tests fail with libc++.
        || (stdenv.cc.libcxx != null && stdenv.cc.libcxx.isLLVM)
      );
  };

  # TODO Hack until https://github.com/NixOS/nixpkgs/issues/45462 is fixed.
  # TODO Boost 1.91 has a regression in boost.url :(
  boost =
    (pkgs.boost189.override {
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
