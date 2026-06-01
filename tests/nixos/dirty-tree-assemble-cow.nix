# NixOS VM test: dirty-tree base-plus-overlay assembly (item (b)) on a
# COPY-ON-WRITE filesystem (btrfs), where `tryCloneFile`'s `FICLONE`
# reflink actually fires — the case the host gbench cannot exercise (a
# tmpfs/extN store has no reflink, so unchanged files are *hardlinked*
# there; the dramatic, tree-size-independent win needs CoW).
#
# Why a VM and not the host: making a btrfs filesystem requires
# `mkfs.btrfs` + a privileged loop/mount, which must not touch the host.
# A NixOS VM gives root on an isolated machine with an empty disk
# (`/dev/vdb`) we can format btrfs and mount, exactly as `fsync.nix` does.
#
# What it proves, that the hardlink path cannot: after editing one file in
# a large tree, the assembled store path and the materialised base SHARE
# PHYSICAL EXTENTS (reflink), not merely inodes. We verify this by the
# disk-usage invariant — `du` counts shared extents once, so the combined
# apparent usage of (base ∪ assembled) is ≈ one tree, not two — and by
# confirming the unchanged files are DISTINCT inodes (a reflink is a new
# inode sharing extents, unlike a hardlink which is the same inode).
#
# Run: nix build .#hydraJobs.tests.dirty-tree-assemble-cow

{ lib, ... }:

let
  nFiles = 200;
  # Each file large enough that a real byte-copy would dominate disk usage,
  # so reflink-vs-copy is unambiguous in `du`.
  fileKiB = 64; # ~12.5 MiB of distinct bytes
in
{
  name = "dirty-tree-assemble-cow";

  nodes.machine =
    { pkgs, ... }:
    {
      virtualisation.emptyDiskImages = [ 2048 ]; # /dev/vdb for the btrfs store
      boot.supportedFilesystems = [ "btrfs" ];
      environment.systemPackages = [
        pkgs.git
        pkgs.coreutils
        pkgs.btrfs-progs # `btrfs filesystem du` for extent-sharing measurement
      ];
      nix.settings.substituters = lib.mkForce [ ];
      nix.extraOptions = "experimental-features = nix-command flakes";
    };

  testScript = ''
    machine.wait_for_unit("multi-user.target")

    # --- a btrfs (CoW) filesystem to host a Nix store ------------------
    machine.succeed("mkfs.btrfs -f /dev/vdb")
    machine.succeed("mkdir -p /mnt/cow")
    machine.succeed("mount -t btrfs /dev/vdb /mnt/cow")
    # Confirm reflink works at all on this mount (sanity: cp --reflink).
    machine.succeed("echo hello > /mnt/cow/a && cp --reflink=always /mnt/cow/a /mnt/cow/b")
    machine.succeed("rm -f /mnt/cow/a /mnt/cow/b")

    machine.succeed("git config --global user.email test@localhost")
    machine.succeed("git config --global user.name test")
    machine.succeed("git config --global init.defaultBranch main")
    machine.succeed("git config --global commit.gpgsign false")

    system = machine.succeed("nix eval --impure --raw --expr builtins.currentSystem").strip()

    # --- a large git working tree + a flake using it as src -----------
    machine.succeed("rm -rf /tmp/repo && mkdir -p /tmp/repo/pkgs && cd /tmp/repo && git init -q")
    # Distinct, incompressible content per file. Write raw /dev/urandom
    # directly (NOT `… | base64 | head -c`, whose closing `head` sends
    # SIGPIPE to `base64` and fails under `set -o pipefail`).
    machine.succeed(
        "cd /tmp/repo && for i in $(seq 1 ${toString nFiles}); do "
        "head -c ${toString (fileKiB * 1024)} /dev/urandom > pkgs/f$i.dat; "
        "done"
    )
    machine.succeed(
        "cd /tmp/repo && printf '%s' "
        "'{ outputs = { self }: { drv = derivation { name = \"uses-src\"; system = \"" + system + "\"; "
        "builder = \"/bin/sh\"; args = [ \"-c\" \":\" ]; src = self; }; }; }' > flake.nix"
    )
    machine.succeed("cd /tmp/repo && git add -A && git commit -q -m init")

    # The store lives on the btrfs mount, so base + assembled paths are on
    # the same CoW filesystem and `tryCloneFile` (FICLONE) can reflink.
    store = "local?root=/mnt/cow"

    # --- edit one file, instantiate against the btrfs store -----------
    machine.succeed("echo EDITED-ONCE > /tmp/repo/pkgs/f1.dat")
    machine.succeed(
        "cd /tmp/repo && nix eval --store '" + store + "' 'git+file:///tmp/repo#drv.drvPath' "
        "--impure --raw -vvvv 2> /tmp/eval1.log > /dev/null"
    )
    machine.succeed("grep -q \"assembled '\" /tmp/eval1.log")

    # --- locate the assembled source + the materialised base ----------
    realStore = "/mnt/cow/nix/store"
    src = machine.succeed(
        "cd /tmp/repo && nix derivation show --store '" + store + "' 'git+file:///tmp/repo#drv' --impure "
        "| grep -oE '/nix/store/[a-z0-9]+-source' | grep -v -- '-base' | head -1"
    ).strip()
    # Map the logical store path to its real on-disk location under the root.
    srcReal = "/mnt/cow" + src
    baseReal = machine.succeed("ls -d " + realStore + "/*-source-base | head -1").strip()
    print("assembled (real) = " + srcReal)
    print("base      (real) = " + baseReal)

    # --- SOUNDNESS first: valid + verifies + correct content ----------
    machine.succeed("nix path-info --store '" + store + "' " + src + " >/dev/null")
    machine.succeed("nix store verify --store '" + store + "' --no-trust " + src)
    machine.succeed("test \"$(cat " + srcReal + "/pkgs/f1.dat)\" = EDITED-ONCE")

    # --- THE COW WIN: unchanged files are REFLINKED (distinct inode,
    # shared extents), not hardlinked and not byte-copied. ------------
    # 1) distinct inode (reflink != hardlink):
    same_inode = machine.succeed(
        "si=$(stat -c %i " + srcReal + "/pkgs/f50.dat); bi=$(stat -c %i " + baseReal + "/pkgs/f50.dat); "
        "[ \"$si\" = \"$bi\" ] && echo yes || echo no"
    ).strip()
    assert same_inode == "no", "on a CoW store the reflink must be a DISTINCT inode (got a hardlink)"

    # 2) shared extents (reflink != copy). Plain `du` CANNOT detect this:
    #    a reflink is a distinct inode, and du counts blocks per-inode, so
    #    two reflinked trees report 2× even though the extents are shared.
    #    `btrfs filesystem du` is extent-aware and reports Total / Exclusive
    #    / Shared. After reflinking, the assembled tree's bytes should be
    #    almost entirely SHARED (with the base) and minimally EXCLUSIVE
    #    (just the one changed file + metadata).
    out = machine.succeed("btrfs filesystem du -s --raw " + srcReal)
    print("btrfs fi du -s (assembled):\n" + out)
    # Output columns: "     Total   Exclusive  Set shared  Filename"
    # Parse the data row (last line).
    cols = out.strip().splitlines()[-1].split()
    total = int(cols[0])
    exclusive = int(cols[1])
    print("assembled: total={} exclusive={} (shared≈total-exclusive)".format(total, exclusive))
    # The exclusive (not-shared-with-base) portion must be a small fraction
    # of the total: only the 1 changed file of ${toString nFiles} + slack.
    # If everything were byte-copied, exclusive ≈ total.
    assert exclusive < total // 4, \
        f"expected most extents SHARED with base via reflink, but exclusive={exclusive} of total={total} — looks byte-copied"

    print("dirty-tree-assemble-cow: OK (reflinked, {} of {} bytes shared with base)".format(total - exclusive, total))
  '';
}
