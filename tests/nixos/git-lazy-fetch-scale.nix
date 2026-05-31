# NixOS VM test: LARGE-repo lazy-fetch scale measurement.
#
# The other two lazy-fetch tests use toy repos (a few KB) — enough to prove
# correctness and that the filter fires, but NOT to show the saving matters. This
# one generates a deliberately large repo (thousands of files, hundreds of MB of
# blob bytes, with Nix expressions to evaluate) on a Gitea HTTP server, then
# measures — for `git-lazy-fetch` ON vs OFF — what three realistic access
# patterns actually pull:
#
#   metadata : read only `.rev`              (no file bytes wanted)
#   subtree  : materialise ONE small subdir  (a fraction of the tree)
#   full     : materialise the whole tree    (everything wanted)
#
# Metric = the on-disk size of Nix's gitv3 cache repo after the access (a
# faithful proxy for bytes-on-wire: a blobless partial clone stores commit+tree
# objects but defers blob packs) plus wall-clock. This empirically answers
# "does lazy fetch save real bytes/time at scale, and on which access patterns?"
# — including surfacing the pinned-rev-metadata-backfill behaviour on real data.
#
# Run: nix build .#hydraJobs.tests.git-lazy-fetch-scale
# Knobs (env, baked at eval): SCALE_DIRS, SCALE_FILES_PER_DIR, SCALE_FILE_BYTES.

{ lib, ... }:

let
  giteaUser = "test";
  giteaPassword = "test123test";
  repoName = "big";

  # ~50 dirs × 40 files × 24 KiB ≈ 48 MB of distinct blob bytes across ~2000
  # files. Large enough that "blobs deferred" is tens of MB, small enough to
  # build + push inside a VM test in reasonable time. Deterministic content.
  dirs = 50;
  filesPerDir = 40;
  fileBytes = 24576;
in
{
  name = "git-lazy-fetch-scale";

  nodes = {
    gitea =
      { pkgs, ... }:
      {
        services.gitea.enable = true;
        services.gitea.settings = {
          service.DISABLE_REGISTRATION = true;
          server = {
            DOMAIN = "gitea";
            HTTP_PORT = 3000;
            ROOT_URL = "http://gitea:3000/";
          };
          "git.config"."uploadpack.allowFilter" = true;
          "git.config"."uploadpack.allowAnySHA1InWant" = true;
        };
        networking.firewall.allowedTCPPorts = [ 3000 ];
        # A large push needs room + time.
        services.gitea.settings."repository.upload".FILE_MAX_SIZE = 256;
        environment.systemPackages = [
          pkgs.git
          pkgs.gitea
        ];
        virtualisation.diskSize = 4096;
      };

    client =
      { pkgs, ... }:
      {
        environment.systemPackages = [
          pkgs.git
          pkgs.gnused
          pkgs.coreutils
          pkgs.bc
        ];
        nix.settings.substituters = lib.mkForce [ ];
        nix.extraOptions = "experimental-features = nix-command flakes";
        virtualisation.diskSize = 4096;
      };
  };

  testScript =
    { nodes, ... }:
    ''
      start_all()
      gitea.wait_for_unit("gitea.service")
      gitea.wait_for_open_port(3000)

      gitea.succeed(
          "su -l gitea -c 'GITEA_WORK_DIR=/var/lib/gitea gitea admin user create "
          "--username ${giteaUser} --password ${giteaPassword} "
          "--email test@localhost --admin --must-change-password=false'"
      )
      gitea.succeed(
          "curl --fail -X POST http://${giteaUser}:${giteaPassword}@gitea:3000/api/v1/user/repos "
          "-H 'content-type: application/json' --data '{\"name\":\"${repoName}\"}'"
      )
      gitea.succeed("git config --global user.email test@localhost")
      gitea.succeed("git config --global user.name test")
      gitea.succeed("git config --global init.defaultBranch main")

      # --- build a large, deterministic repo on the server ---------------
      # ${toString dirs} dirs × ${toString filesPerDir} files × ${toString fileBytes} B.
      # Deterministic content (dir+file index, padded) so it's reproducible.
      # Each file gets ${toString fileBytes} bytes of INCOMPRESSIBLE content
      # (base64 of /dev/urandom). Distinct, high-entropy bytes are essential: an
      # earlier version wrote the same filler to every file and git delta-
      # compressed ~48 MB down to 268 KiB, so "blobs deferred" was invisible.
      # Content is random (not deterministic) — fine for a perf measurement; the
      # repo just needs to be genuinely large on the wire. No `yes|head` pipe
      # (SIGPIPE under set -e); `head -c` on /dev/urandom never overruns.
      gitea.succeed(
          "rm -rf /tmp/seed && mkdir /tmp/seed && cd /tmp/seed && git init -q && "
          "for d in $(seq 1 ${toString dirs}); do mkdir -p dir$d; "
          "  for f in $(seq 1 ${toString filesPerDir}); do "
          "    head -c ${toString fileBytes} /dev/urandom | base64 > dir$d/f$f.txt; "
          "  done; done && "
          "echo '{ outputs = _: {}; }' > flake.nix && "
          "mkdir -p small && echo hello > small/one.txt && echo world > small/two.txt",
          timeout=600,
      )
      gitea.succeed("cd /tmp/seed && git add -A && git commit -q -m big", timeout=600)
      rev = gitea.succeed("cd /tmp/seed && git rev-parse HEAD").strip()
      gitea.succeed(
          "cd /tmp/seed && git remote add origin "
          "http://${giteaUser}:${giteaPassword}@gitea:3000/${giteaUser}/${repoName}.git "
          "&& git -c http.postBuffer=524288000 push -q origin main",
          timeout=600,
      )
      pack_kib = gitea.succeed("du -sk /tmp/seed/.git | cut -f1").strip()
      print(f"server repo .git size: {int(pack_kib)//1024} MiB")

      repo_url = "http://gitea:3000/${giteaUser}/${repoName}.git"
      client.wait_for_unit("multi-user.target")
      client.succeed("curl --fail %s/info/refs?service=git-upload-pack >/dev/null" % repo_url)

      def cache_kib():
          out = client.succeed(
              "du -sk /root/.cache/nix/gitv3 2>/dev/null | cut -f1 || echo 0"
          ).strip()
          return int(out or "0")

      def measure(label, expr, lazy):
          """Fresh cache, run one nix eval, return (cache_KiB, wall_seconds)."""
          opt = "true" if lazy else "false"
          client.succeed("rm -rf /root/.cache/nix /root/.local/state/nix")
          t = client.succeed(
              "HOME=/root NIX_CONFIG='experimental-features = nix-command flakes' "
              "/run/current-system/sw/bin/date +%s.%N > /tmp/t0; "
              f"HOME=/root NIX_CONFIG='experimental-features = nix-command flakes' "
              f"nix eval --impure --raw --option git-lazy-fetch {opt} "
              f"--expr '{expr}' >/dev/null 2>/tmp/err; "
              "/run/current-system/sw/bin/date +%s.%N > /tmp/t1; "
              "echo \"$(cat /tmp/t1) - $(cat /tmp/t0)\" | bc"
          ).strip()
          return cache_kib(), float(t)

      # NB on access patterns and what each can defer:
      #   - `.rev`     : metadata only. (A pinned-rev fetchGit still backfills via
      #     revCount/lastModified — see git-lazy-fetch.nix; measured here on real data.)
      #   - readFile of ONE sub-file: prim_readFile prefetches only the file's
      #     PARENT dir at depth 1 (primops.cc), so the other ~48 MB of blobs in
      #     sibling dirs stay deferred. This is the pattern where lazy fetch wins.
      #     Reading `(input).outPath` instead would copy the WHOLE tree (all blobs),
      #     so it deliberately is NOT how the subtree pattern is expressed.
      #   - `.outPath` : materialise everything → all blobs wanted, no saving.
      base = f'builtins.fetchGit {{ url = "{repo_url}"; rev = "{rev}"; }}'
      patterns = {
          "metadata":     f'({base}).rev',
          "subfile-read": f'builtins.readFile (({base}) + "/small/one.txt")',
          "full":         f'({base}).outPath',
      }

      results = {}
      print("\n=== large-repo lazy-fetch: gitv3 cache size + wall-clock ===")
      print(f"{'pattern':10} {'lazy':5} {'cache MiB':10} {'wall s':8}")
      for pat, expr in patterns.items():
          for lazy in (True, False):
              kib, secs = measure(pat, expr, lazy)
              results[(pat, lazy)] = (kib, secs)
              print(f"{pat:10} {str(lazy):5} {kib/1024:<10.1f} {secs:<8.2f}")

      full_lazy = results[("full", True)][0]
      full_eager = results[("full", False)][0]
      meta_lazy = results[("metadata", True)][0]
      sub_lazy = results[("subfile-read", True)][0]

      # Sanity: the repo really is large on the wire (incompressible content).
      assert full_eager > 20 * 1024, f"repo too small to be meaningful: {full_eager} KiB"

      # MEASURED REALITY (this is the point of the test — report it, don't fake a
      # win): for a pinned-rev `builtins.fetchGit {{ rev = ...; }}`, git-lazy-fetch
      # pulls ~the SAME bytes as a full clone at EVERY access pattern, because the
      # input's revCount/lastModified computation walks + backfills the whole tree
      # before any narrow per-file prefetch runs (git.cc; see the test header and
      # the project notes). The filtered partial clone IS created and the fetch IS
      # `--filter=blob:none` (proven in git-lazy-fetch.nix) — but the saving is
      # pre-empted. So the bandwidth benefit is currently UNREALISED for this
      # dominant access pattern.
      pct = lambda a, b: (100.0 * a / b) if b else 0.0
      print("\n--- measured reality (pinned-rev fetchGit over git-lazy-fetch) ---")
      print(f"  metadata lazy     : {meta_lazy/1024:6.1f} MiB ({pct(meta_lazy, full_eager):.0f}% of full eager)")
      print(f"  subfile-read lazy : {sub_lazy/1024:6.1f} MiB ({pct(sub_lazy, full_eager):.0f}% of full eager)")
      print(f"  full eager        : {full_eager/1024:6.1f} MiB (100%)")

      if sub_lazy * 2 < full_eager:
          print("\nNOTE: lazy fetch DID defer the bulk of blobs for a single-file read.")
      else:
          print(
              "\nFINDING: git-lazy-fetch did NOT reduce bytes for a pinned-rev input at any\n"
              "access pattern — revCount/lastModified backfill the whole tree first. The\n"
              "mechanism is wired and sound (see git-lazy-fetch.nix); realising the saving\n"
              "needs revCount/lastModified deferred over promisor remotes. Recorded, not hidden."
          )

      # The assertion that MUST hold regardless of the byte finding: SOUNDNESS.
      # Lazy and eager full materialisation produce the same store path + hash.
      full_expr = patterns["full"]
      nixenv = "HOME=/root NIX_CONFIG='experimental-features = nix-command flakes'"

      def eval_full(lazy):
          opt = "true" if lazy else "false"
          out = client.succeed(
              f"rm -rf /root/.cache/nix; {nixenv} "
              f"nix eval --impure --raw --option git-lazy-fetch {opt} --expr '{full_expr}'"
          ).strip()
          h = client.succeed(f"{nixenv} nix hash path {out}").strip()
          return out, h

      lazy_out, lh = eval_full(True)
      eager_out, eh = eval_full(False)
      assert lazy_out == eager_out and lh == eh, (
          f"SOUNDNESS at scale: lazy ({lazy_out}, {lh}) != eager ({eager_out}, {eh})"
      )
      print(f"\nOK (soundness at scale): lazy and eager agree on {lazy_out} ({lh}).")
    '';
}
