# NixOS VM test (opt-in): 3-way real-network comparison of git fetching over a
# blobless-capable HTTP server.
#
# Where git-lazy-fetch.nix proves CORRECTNESS of our promisor path, this measures
# COST against other Nix builds on a real network: for each build it times a cold
# `fetchGit` and counts how many objects the git server actually sent (the
# index-pack header in GIT_TRACE — backfill-immune). This is the in-VM analogue
# of the bash harness in benchmarks/, but over a real git server so the blobless
# saving is observable.
#
# This file is a FUNCTION of `extraNixes`, returning a NixOS-test module — it is
# CALLED (not imported directly as a module), which is how the comparison set is
# parameterised without fighting the module system's `_module.args` resolution.
# tests/nixos/default.nix registers it as:
#
#   git-lazy-fetch-compare = runNixOSTest (import ./git-lazy-fetch-compare.nix { });
#
# By default the matrix is just the just-built Nix with the setting on vs off, so
# it runs in CI without external trees. To add the upstream-master baseline and
# the DeterminateSystems fork — which are NOT inputs of this flake — pass them as
# `extraNixes`. Each entry is { name; package; lazy ? true;
# lazySetting ? "git-lazy-fetch"; }:
#
#   nix build --impure --expr '
#     let f = builtins.getFlake (toString ./.); s = builtins.currentSystem;
#         pkgs = import f.inputs.nixpkgs { system = s; };
#         nixos-lib = import (f.inputs.nixpkgs + "/nixos/lib") { };
#         baseline = (builtins.getFlake "github:NixOS/nix/2d309b18e").packages.${s}.default;
#         detsys   = (builtins.getFlake "git+file:///home/you/ext-sources/nix-src").packages.${s}.default;
#     in (nixos-lib.runTest {
#          hostPkgs = pkgs;
#          imports = [ (import ./tests/nixos/git-lazy-fetch-compare.nix {
#            extraNixes = [
#              { name = "baseline"; package = baseline; lazy = false; }
#              { name = "detsys";   package = detsys;   lazy = true; lazySetting = "lazy-trees"; }
#            ];
#          }) ];
#          _module.args.nixComponents = ...;  # as in tests/nixos/default.nix
#        }).config.result'
#
# Most users will instead read benchmarks/RESULTS.md; this test exists so the
# comparison can be reproduced hermetically when the external trees are pinned.
{ extraNixes ? [ ] }:

{
  lib,
  nixComponents,
  ...
}:

let
  giteaUser = "test";
  giteaPassword = "test123test";
  repoName = "lazy";

  # The matrix of (label, nix package, setting name, on/off) to time.
  ours = nixComponents.nix-cli;
  variants =
    [
      {
        name = "ours-lazy-on";
        package = ours;
        lazy = true;
        lazySetting = "git-lazy-fetch";
      }
      {
        name = "ours-lazy-off";
        package = ours;
        lazy = false;
        lazySetting = "git-lazy-fetch";
      }
    ]
    ++ map (e: {
      inherit (e) name package;
      lazy = e.lazy or true;
      lazySetting = e.lazySetting or "git-lazy-fetch";
    }) extraNixes;

  # Install each comparison nix at /run/nixes/<name>/bin/nix on the client.
  nixesEnv = lib.concatMapStringsSep "\n" (
    v: "ln -sfn ${v.package} /run/nixes/${v.name}"
  ) variants;
in
{
  name = "git-lazy-fetch-compare";

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
        environment.systemPackages = [
          pkgs.git
          pkgs.gitea
        ];
      };

    client =
      { pkgs, ... }:
      {
        environment.systemPackages = [
          pkgs.git
          pkgs.jq
          pkgs.hyperfine
        ];
        nix.settings.substituters = lib.mkForce [ ];
        # The comparison nixes are referenced by store path; keep them alive.
        system.extraDependencies = map (v: v.package) variants;
      };
  };

  testScript =
    { nodes, ... }:
    ''
      import re

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
      # A repo with enough blob bytes that "blobs sent" vs "blobs deferred" is a
      # visible byte difference.
      gitea.succeed(
          "rm -rf /tmp/seed && mkdir /tmp/seed && cd /tmp/seed && git init -q && mkdir sub "
          "&& for i in $(seq 1 40); do head -c 50000 /dev/zero | tr '\\0' a > sub/f$i.txt; done "
          "&& echo '{ outputs = _: {}; }' > flake.nix && git add -A && git commit -q -m c1"
      )
      rev = gitea.succeed("cd /tmp/seed && git rev-parse HEAD").strip()
      gitea.succeed(
          "cd /tmp/seed && git remote add origin "
          "http://${giteaUser}:${giteaPassword}@gitea:3000/${giteaUser}/${repoName}.git "
          "&& git push -q origin main"
      )

      repo_url = "http://gitea:3000/${giteaUser}/${repoName}.git"
      client.wait_for_unit("multi-user.target")
      client.succeed("mkdir -p /run/nixes")
      client.succeed("""${nixesEnv}""")
      client.succeed("curl --fail %s/info/refs?service=git-upload-pack >/dev/null" % repo_url)

      fetch_expr = f'(builtins.fetchGit {{ url = "{repo_url}"; rev = "{rev}"; }}).outPath'

      def run_variant(name, setting, on):
          nix = f"/run/nixes/{name.split('::')[0]}/bin/nix"
          # name encodes the store symlink; setting+on come from the matrix.
          val = "true" if on else "false"
          client.succeed("rm -rf /root/.cache/nix /root/.local/state/nix")
          trace = "/tmp/tr"
          client.succeed(
              f"HOME=/root NIX_CONFIG='experimental-features = nix-command flakes' "
              f"GIT_TRACE2_EVENT=/tmp/ev.json GIT_TRACE=2 "
              f"{nix} eval --impure --raw --option {setting} {val} "
              f"--expr '{fetch_expr}' >/dev/null 2>{trace} || true"
          )
          tr = client.succeed(f"cat {trace}")
          # Objects in the first received pack: index-pack ... --pack_header=2,<N>
          packs = [int(m) for m in re.findall(r"--pack_header=2,(\d+)", tr)]
          first_pack = packs[0] if packs else -1
          total_pack = sum(packs)
          filtered = "--filter=blob:none" in tr
          return dict(name=name, setting=setting, on=on, filtered=filtered,
                      first_pack=first_pack, total_pack=total_pack, n_packs=len(packs))

      results = []
      # The matrix rows are baked into the symlink names + this list, mirrored from Nix.
      matrix = ${
        let
          rows = map (v: ''("${v.name}", "${v.lazySetting}", ${if v.lazy then "True" else "False"})'') variants;
        in
        "[" + lib.concatStringsSep ", " rows + "]"
      }

      print("\n=== 3-way git fetch comparison (objects in server-sent packs) ===")
      print(f"{'variant':22} {'setting':16} {'on':5} {'filtered':9} {'1st pack':9} {'total objs':10}")
      for (name, setting, on) in matrix:
          r = run_variant(name, setting, on)
          results.append(r)
          print(f"{r['name']:22} {r['setting']:16} {str(r['on']):5} "
                f"{str(r['filtered']):9} {r['first_pack']:<9} {r['total_pack']:<10}")

      # Sanity: ours-lazy-on must issue a filtered fetch; ours-lazy-off must not.
      by = {r['name']: r for r in results}
      assert by['ours-lazy-on']['filtered'], "ours-lazy-on did not filter"
      assert not by['ours-lazy-off']['filtered'], "ours-lazy-off filtered unexpectedly"
      # The point of the comparison: the FIRST pack of a filtered fetch carries
      # fewer objects (blobs deferred) than an unfiltered one.
      assert by['ours-lazy-on']['first_pack'] < by['ours-lazy-off']['first_pack'], (
          f"expected lazy first-pack < eager first-pack, got "
          f"{by['ours-lazy-on']['first_pack']} vs {by['ours-lazy-off']['first_pack']}"
      )
      print("\nOK: filtered fetch defers blobs (smaller initial pack) vs full fetch.")
    '';
}
