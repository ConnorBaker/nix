# One-off entry point for the nix-store-bench rig. Builds a single
# NixOS VM test with arbitrary parameters; no Hydra-jobset wiring.
#
# Every bench parameter lives in `./bench-options.nix` — its option
# schema declares the types, defaults, and cross-option assertions.
# This file just captures the user's overlay with `args@{ ... }` (so
# only explicitly-supplied arguments flow through), threads it into
# `evalTest`, and surfaces assertions + warnings on the way back out.
#
# The schema is strict about types: use `--argstr` for string-valued
# options (`benchName`, `fs`, `throttle`, `layout`, `replica`,
# `dispatchOnly`, `name`) and `--arg` (with a Nix expression) for
# everything else.
#
# CLI usage:
#   nix build --no-link -L -f tests/nixos/nix-store-bench/adhoc.nix \
#     --argstr benchName gc_barabasi \
#     --argstr fs btrfs --argstr throttle nvme \
#     --argstr layout sharded --argstr replica multi \
#     --argstr dispatchOnly syscall \
#     --arg nPaths 10000 --arg threads 4
#
# Programmatic usage:
#   import tests/nixos/nix-store-bench/adhoc.nix {
#     benchName = "gc_clusters"; fs = "zfs"; throttle = "nvme";
#     nPaths = 10000;
#   }
#
# `(nPaths, threads[, threads2])` must match a registered
# `BENCHMARK_CAPTURE` row in `src/libstore-tests/optimise-bench.cc`;
# the schema's `nPaths` assertion does not check the thread axes.
# GC benches without `dispatchOnly` run a regression-gated
# `bench.py ab` step (defaults: VFS parity ≤ 5%, syscalls
# ≤ 5% increase, wall ≤ 5% slower). Pass `--wall-improvement 0.10`
# etc. to `bench.py ab` outside the VM to demand an actual
# win; pass `--argstr dispatchOnly syscall` (or `iouring`) on the
# adhoc invocation to switch the in-VM post-step to
# `bench.py summary` (no thresholds).
args@{
  # Path to the project root containing this checkout's flake. Only
  # this file's own arg; everything else is forwarded to the schema.
  flakeRoot ? toString ../../..,
  ...
}:

let
  flake = builtins.getFlake flakeRoot;
  system = builtins.currentSystem;
  nixpkgs = flake.inputs.nixpkgs;
  # `overlays.internal` adds `nixComponents2`, the same package set
  # other tests get via `nixpkgsFor.<system>.native.nixComponents2`.
  pkgs = import nixpkgs {
    inherit system;
    overlays = [ flake.overlays.internal ];
  };
  nixComponents = pkgs.nixComponents2;
  inherit (pkgs) lib;

  nixos-lib = import (nixpkgs + "/nixos/lib") { };

  # `args` (an `args@{...}` view binding) holds only the keys the
  # caller actually supplied; defaults are not present here, so the
  # schema's defaults in `bench-options.nix` win for anything we
  # don't override. The schema's types are strict — callers must
  # pass typed values (use `--arg` not `--argstr` for non-string
  # options).
  benchArgs = builtins.removeAttrs args [ "flakeRoot" ];

  # Single evalModules: type-check + apply defaults + cross-validate
  # in one pass. `mk-test.nix` provides nodes/testScript; we read
  # `eval.config.test` (synthesised by `testModules`) for the result.
  eval = nixos-lib.evalTest {
    imports = [
      ./bench-options.nix
      ./mk-test.nix
      { config.bench = benchArgs; }
    ];
    hostPkgs = pkgs;
    defaults = {
      nixpkgs.pkgs = pkgs;
      nix.checkAllErrors = false;
      nix.package = nixComponents.nix-cli;
      documentation.enable = false;
      system.tools.nixos-option.enable = false;
    };
    # `pkgs` is set by `nixos/lib/testing/pkgs.nix` from `hostPkgs`;
    # only `nixComponents` needs an explicit `_module.args` entry.
    _module.args = { inherit nixComponents; };
  };

  # Surface assertions/warnings manually — runTest's nixosTest class
  # doesn't process them (see `nixos/lib/testing/legacy.nix:11`).
  failedAssertions =
    map (a: a.message) (lib.filter (a: !a.assertion) eval.config.assertions);
in

if failedAssertions != [ ] then
  throw "nix-store-bench: bench-options assertion failures:\n${
    lib.concatMapStringsSep "\n" (m: "  - ${m}") failedAssertions
  }"
else

# `builtins.deepSeq cfg.bench` forces eager evaluation of every option
# so `types.enum` failures (e.g. `fs = "ext42"`) surface at eval time,
# matching the prior hand-written `lib.elem` checks.
lib.showWarnings eval.config.warnings (
  builtins.deepSeq eval.config.bench eval.config.test
)
