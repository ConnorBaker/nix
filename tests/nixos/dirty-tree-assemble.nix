# NixOS VM test: dirty-tree base-plus-overlay assembly (item (b)) at scale,
# in a real store on a real (disk-backed) filesystem — the setting the
# microbenchmark cannot reproduce (it runs on tmpfs).
#
# Scenario: a large git working tree (hundreds of files / tens of MB) is
# edited one file at a time, and each edit's source is materialised into
# the store. With (b), `ensureLazyPathCopied` materialises the committed
# base ONCE and assembles each dirty revision from it — reflinking/
# hardlinking the unchanged majority instead of re-copying the whole tree.
#
# The test asserts the load-bearing, store-independent property: the
# assembled dirty source path SHARES the unchanged files' inodes with the
# materialised base (hardlinked, not duplicated), so editing one file does
# NOT write a second full copy of the tree's bytes. It also asserts the
# assembler actually fired (the `assembled` debug marker) and SOUNDNESS
# (the assembled path is a valid store path with correct contents, i.e. it
# passed the in-store re-hash guard) and that `nix store verify` accepts
# it.
#
# Run: nix build .#hydraJobs.tests.dirty-tree-assemble

{ lib, ... }:

let
  # Big enough that "copy all" vs "link all-but-one" is a clear difference
  # and the byte duplication would be obviously wasteful, small enough to
  # stay fast inside a VM.
  nFiles = 300;
  fileKiB = 32; # ~9.4 MiB of distinct blob bytes
in
{
  name = "dirty-tree-assemble";

  nodes = {
    machine =
      { pkgs, ... }:
      {
        environment.systemPackages = [
          pkgs.git
          pkgs.coreutils
        ];
        nix.settings.substituters = lib.mkForce [ ];
        nix.extraOptions = "experimental-features = nix-command flakes";
        # A real disk-backed store (not tmpfs) so the byte-copy cost — and
        # thus the saving — is real.
        virtualisation.diskSize = 4096;
      };
  };

  testScript =
    { nodes, ... }:
    ''
      start_all()
      machine.wait_for_unit("multi-user.target")

      machine.succeed("git config --global user.email test@localhost")
      machine.succeed("git config --global user.name test")
      machine.succeed("git config --global init.defaultBranch main")
      machine.succeed("git config --global commit.gpgsign false")

      system = machine.succeed("nix eval --impure --raw --expr builtins.currentSystem").strip()

      # --- build a large git working tree + a flake that uses it as src ---
      machine.succeed(
          "rm -rf /tmp/repo && mkdir -p /tmp/repo/pkgs && cd /tmp/repo && git init -q"
      )
      # Distinct, incompressible content per file. Write raw /dev/urandom
      # directly (NOT `… | base64 | head -c`, whose closing `head` sends
      # SIGPIPE to `base64` and fails under `set -o pipefail`).
      machine.succeed(
          "cd /tmp/repo && for i in $(seq 1 ${toString nFiles}); do "
          "head -c ${toString (fileKiB * 1024)} /dev/urandom > pkgs/f$i.dat; "
          "done"
      )
      machine.succeed(
          "cd /tmp/repo && cat > flake.nix <<EOF\n"
          "{ outputs = { self }: { drv = derivation { name = \"uses-src\"; system = \"" + system + "\"; "
          "builder = \"/bin/sh\"; args = [ \"-c\" \":\" ]; src = self; }; }; }\n"
          "EOF"
      )
      machine.succeed("cd /tmp/repo && git add -A && git commit -q -m init")

      # --- EDIT one file, dirty the tree, instantiate (forces the source to
      # materialise at the derivation boundary, where the assembler runs) ---
      machine.succeed("echo EDITED-ONCE > /tmp/repo/pkgs/f1.dat")
      machine.succeed(
          "cd /tmp/repo && nix eval 'git+file:///tmp/repo#drv.drvPath' "
          "--impure --raw -vvvv 2> /tmp/eval1.log > /dev/null"
      )

      # The assembler must have fired (not the full-copy fallback).
      machine.succeed("grep -q \"assembled '\" /tmp/eval1.log")

      # --- locate the assembled source path + the materialised base -------
      store = machine.succeed("nix eval --impure --raw --expr builtins.storeDir").strip()
      # The drv's input source is the assembled dirty path; isolate it from
      # the '-base' path by name.
      src = machine.succeed(
          "cd /tmp/repo && nix derivation show 'git+file:///tmp/repo#drv' --impure "
          "| grep -oE '" + store + "/[a-z0-9]+-source' | grep -v -- '-base' | head -1"
      ).strip()
      base = machine.succeed("ls -d " + store + "/*-source-base | head -1").strip()
      print("assembled src = " + src)
      print("base          = " + base)

      # --- SOUNDNESS: assembled path is valid + verifies + correct content
      machine.succeed("nix path-info " + src + " >/dev/null")
      machine.succeed("nix store verify --no-trust " + src)
      machine.succeed("test \"$(cat " + src + "/pkgs/f1.dat)\" = EDITED-ONCE")
      machine.succeed("test \"$(cat " + base + "/pkgs/f30.dat | head -c 8)\" = \"$(cat " + src + "/pkgs/f30.dat | head -c 8)\"")

      # --- THE WIN: unchanged files share inodes with the base (hardlinked,
      # not re-copied); the changed file does not. ------------------------
      shared = int(machine.succeed(
          "shared=0; for i in $(seq 1 ${toString nFiles}); do "
          "si=$(stat -c %i " + src + "/pkgs/f$i.dat); bi=$(stat -c %i " + base + "/pkgs/f$i.dat); "
          "[ \"$si\" = \"$bi\" ] && shared=$((shared+1)); done; echo $shared"
      ).strip())
      print("shared inodes (hardlinked from base): " + str(shared) + " / ${toString nFiles}")

      f1_shared = machine.succeed(
          "si=$(stat -c %i " + src + "/pkgs/f1.dat); bi=$(stat -c %i " + base + "/pkgs/f1.dat); "
          "[ \"$si\" = \"$bi\" ] && echo yes || echo no"
      ).strip()
      assert f1_shared == "no", "the CHANGED file f1 must be rewritten, not linked from base"

      # Allow a tiny slack but require the vast majority hardlinked. A full
      # re-copy would make this 0.
      assert shared >= ${toString (nFiles - 2)}, \
          f"expected >= ${toString (nFiles - 2)} hardlinked-from-base files, got {shared} (regressed to a full copy?)"

      # --- EDIT a DIFFERENT file; base is now cached → assembly reuses it,
      # no second base materialisation. -----------------------------------
      machine.succeed("echo EDITED-TWO > /tmp/repo/pkgs/f2.dat")
      machine.succeed(
          "cd /tmp/repo && nix eval 'git+file:///tmp/repo#drv.drvPath' "
          "--impure --raw -vvvv 2> /tmp/eval2.log > /dev/null"
      )
      machine.succeed("grep -q \"assembled '\" /tmp/eval2.log")
      # The committed base tree-OID is unchanged across both edits, so the
      # second edit must NOT copy the base again (no second '-base' path with
      # a different hash for the same tree).
      nbase = int(machine.succeed("ls -d " + store + "/*-source-base 2>/dev/null | wc -l").strip())
      print("distinct materialised base paths after 2 edits: " + str(nbase))
      assert nbase == 1, f"expected the committed base to be materialised once and reused, found {nbase} base paths"

      print("dirty-tree-assemble: OK")
    '';
}
