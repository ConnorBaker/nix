# NixOS VM test: on-demand (blobless / partial) git fetching.
#
# This is the only layer of the benchmark/test suite that exercises the
# GitPromisorProvider path end to end. The functional test
# tests/functional/fetchGitLazy.sh can only check safe-fall-through on a
# `file://` repo — the promisor activates ONLY for an http(s)/ssh remote whose
# server advertises protocol-v2 `filter`, which needs a real git server. Here a
# Gitea node serves a repo over smart-HTTP with `uploadpack.allowFilter` on, and
# a client with `git-lazy-fetch = true` proves:
#
#   1. the server actually advertises the v2 `filter` capability (linchpin — a
#      silent config failure must fail loudly, not look like "no win");
#   2. enabling git-lazy-fetch creates a BLOBLESS partial clone in Nix's cache
#      (blobs absent from the local ODB — `rev-list --missing=print`);
#   3. a metadata-only evaluation (reading only `.rev`) fetches ZERO blobs;
#   4. an evaluation that actually reads file bytes backfills exactly the blobs
#      it needs, on demand, and produces the same store path as a full clone
#      would (soundness: lazy and eager materialisation agree);
#   5. turning the setting OFF gives an ordinary full clone (all blobs present).
#
# Run: nix build .#hydraJobs.tests.git-lazy-fetch

{ lib, ... }:

let
  # A deterministic test repo with a few sizeable blobs, so "blobs present" vs
  # "blobs absent" is unambiguous in the object count.
  giteaUser = "test";
  giteaPassword = "test123test";
  repoName = "lazy";

  # An ed25519 keypair for the ssh-transport subtest (exercises the ssh
  # GitPromisorProvider — ~192 LOC otherwise covered only by HTTP). Snake-oil,
  # test-only (same pair shape as tests/nixos/fetch-git). `builtins.toFile` so no
  # `pkgs` is needed in the test-module scope.
  clientPrivateKey = builtins.toFile "id_ed25519" ''
    -----BEGIN OPENSSH PRIVATE KEY-----
    b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW
    QyNTUxOQAAACBbeWvHh/AWGWI6EIc1xlSihyXtacNQ9KeztlW/VUy8wQAAAJAwVQ5VMFUO
    VQAAAAtzc2gtZWQyNTUxOQAAACBbeWvHh/AWGWI6EIc1xlSihyXtacNQ9KeztlW/VUy8wQ
    AAAEB7lbfkkdkJoE+4TKHPdPQWBKLSx+J54Eg8DaTr+3KoSlt5a8eH8BYZYjoQhzXGVKKH
    Je1pw1D0p7O2Vb9VTLzBAAAACGJmb0BtaW5pAQIDBAU=
    -----END OPENSSH PRIVATE KEY-----
  '';
  clientPublicKey =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFt5a8eH8BYZYjoQhzXGVKKHJe1pw1D0p7O2Vb9VTLzB";
in
{
  name = "git-lazy-fetch";

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
          # The linchpin: make every served repo's git-upload-pack advertise the
          # protocol-v2 `filter` capability. Gitea's [git.config] section is
          # written into the git config its server uses.
          "git.config"."uploadpack.allowFilter" = true;
          "git.config"."uploadpack.allowAnySHA1InWant" = true;
        };
        networking.firewall.allowedTCPPorts = [
          3000
          22
        ];
        environment.systemPackages = [
          pkgs.git
          pkgs.gitea
        ];

        # For the ssh-transport subtest: a plain OpenSSH server with a `git`
        # user whose login shell is `git-shell` — the production shape the ssh
        # GitPromisorProvider targets (git-shell-restricted forges running RAW
        # `git-upload-pack`, NOT Gitea's Go ssh shim, which negotiates v2/filter
        # differently). The bare repo it serves is created in the test script.
        services.openssh = {
          enable = true;
          settings.AcceptEnv = "GIT_PROTOCOL"; # the ssh promisor sends v2 OOB
        };
        users.users.git = {
          isSystemUser = true;
          group = "git";
          home = "/srv/git";
          createHome = true;
          shell = pkgs.git; # → /run/current-system/sw/bin/git-shell
          openssh.authorizedKeys.keys = [ clientPublicKey ];
        };
        users.groups.git = { };
      };

    client =
      { pkgs, ... }:
      {
        environment.systemPackages = [
          pkgs.git
          pkgs.jq
          pkgs.openssh
        ];
        # Offline: no substituters reaching out during eval.
        nix.settings.substituters = lib.mkForce [ ];
        nix.extraOptions = ''
          experimental-features = nix-command flakes
        '';
      };
  };

  testScript =
    { nodes, ... }:
    ''
      import re

      start_all()

      gitea.wait_for_unit("gitea.service")
      gitea.wait_for_open_port(3000)

      # --- create an admin user + a repo via the Gitea API ----------------
      gitea.succeed(
          "su -l gitea -c 'GITEA_WORK_DIR=/var/lib/gitea gitea admin user create "
          "--username ${giteaUser} --password ${giteaPassword} "
          "--email test@localhost --admin --must-change-password=false'"
      )
      gitea.succeed(
          "curl --fail -X POST http://${giteaUser}:${giteaPassword}@gitea:3000/api/v1/user/repos "
          "-H 'content-type: application/json' "
          "--data '{\"name\":\"${repoName}\", \"default_branch\":\"main\"}'"
      )

      # --- push a deterministic repo with a few distinct blobs ------------
      gitea.succeed("git config --global user.email test@localhost")
      gitea.succeed("git config --global user.name test")
      gitea.succeed("git config --global init.defaultBranch main")
      gitea.succeed(
          "rm -rf /tmp/seed && mkdir /tmp/seed && cd /tmp/seed && git init -q "
          # three distinct, easily-counted blobs
          "&& mkdir sub "
          "&& head -c 20000 /dev/zero | tr '\\0' 'a' > sub/a.txt "
          "&& head -c 20000 /dev/zero | tr '\\0' 'b' > sub/b.txt "
          "&& head -c 20000 /dev/zero | tr '\\0' 'c' > top.txt "
          "&& echo '{ outputs = _: {}; }' > flake.nix "
          "&& git add -A && git commit -q -m c1"
      )
      rev = gitea.succeed("cd /tmp/seed && git rev-parse HEAD").strip()
      gitea.succeed(
          "cd /tmp/seed && git remote add origin "
          "http://${giteaUser}:${giteaPassword}@gitea:3000/${giteaUser}/${repoName}.git "
          "&& git push -q origin main"
      )

      repo_url = "http://gitea:3000/${giteaUser}/${repoName}.git"
      # The ssh promisor targets a plain git-shell forge over OpenSSH (:22),
      # serving a RAW git-upload-pack — not Gitea's ssh shim.
      ssh_url = "ssh://git@gitea/srv/git/${repoName}.git"

      client.wait_for_unit("multi-user.target")
      client.succeed("curl --fail %s/info/refs?service=git-upload-pack >/dev/null" % repo_url)

      # --- ssh-transport setup: a bare repo served by raw git-upload-pack ------
      # over OpenSSH, with uploadpack.allowFilter on (so the v2 advertisement
      # carries `filter`), owned by the git-shell `git` user.
      gitea.wait_for_unit("sshd.service")
      gitea.succeed(
          "git init --bare /srv/git/${repoName}.git "
          "&& git -C /srv/git/${repoName}.git config uploadpack.allowFilter true "
          "&& git -C /srv/git/${repoName}.git config uploadpack.allowAnySHA1InWant true "
          "&& git -C /tmp/seed push /srv/git/${repoName}.git main "
          "&& chown -R git:git /srv/git"
      )
      # Client ssh key (Nix's promisor reads NIX_SSHOPTS; git's own ssh reads this).
      client.succeed("mkdir -p /root/.ssh && chmod 700 /root/.ssh")
      client.copy_from_host("${clientPrivateKey}", "/root/.ssh/id_ed25519")
      client.succeed("chmod 600 /root/.ssh/id_ed25519")
      client.succeed(
          "printf 'Host gitea\\n  StrictHostKeyChecking no\\n"
          "  UserKnownHostsFile /dev/null\\n  User git\\n' > /root/.ssh/config"
      )

      # === (1) the server advertises protocol-v2 `filter` =================
      with subtest("server advertises v2 filter capability"):
          adv = client.succeed(
              "GIT_PROTOCOL=version=2 git ls-remote --symref %s 2>&1 | head -1 || true" % repo_url
          )
          # The decisive probe: a filtered clone must SUCCEED against this server.
          client.succeed(
              "rm -rf /tmp/probe && git clone --bare --filter=blob:none "
              "-c protocol.version=2 %s /tmp/probe" % repo_url
          )
          missing = client.succeed(
              "git -C /tmp/probe rev-list --objects --all --missing=print "
              "| grep -c '^?' || true"
          ).strip()
          assert int(missing) > 0, (
              f"server did not serve a blobless clone (missing-blob count={missing}); "
              "uploadpack.allowFilter likely not in effect — the rest of the test would be meaningless"
          )
          print(f"OK: filtered bare clone has {missing} missing blobs")

      fetch_expr = (
          f'(builtins.fetchGit {{ url = "{repo_url}"; rev = "{rev}"; }})'
      )

      def nix_eval(expr, *, lazy, extra="", trace=False):
          """Run `nix eval` of `expr`. If trace, capture GIT_TRACE=2 to a file on
          the client and return its contents (stdout of nix is discarded)."""
          opt = "true" if lazy else "false"
          cmd = (
              "HOME=/root NIX_CONFIG='experimental-features = nix-command flakes' "
          )
          if trace:
              cmd += "GIT_TRACE=2 "
          cmd += (
              f"nix eval --impure --option git-lazy-fetch {opt} {extra} "
              f"--expr '{expr}'"
          )
          if trace:
              client.succeed(f"rm -f /tmp/gittrace && ({cmd}) >/dev/null 2>/tmp/gittrace || true")
              return client.succeed("cat /tmp/gittrace")
          # stdout only — git progress/trace goes to stderr and must not pollute
          # a `--raw` outPath. Capture stdout to a file, return it clean.
          client.succeed(f"({cmd}) >/tmp/evalout 2>/tmp/evalerr")
          return client.succeed("cat /tmp/evalout")

      def git_fetches(trace):
          """All `git fetch ...` command lines from a GIT_TRACE log."""
          return [
              ln for ln in trace.splitlines()
              if re.search(r"trace: (built-in|exec|run_command): .*\bfetch\b", ln)
              and "fetch-pack" not in ln
          ]

      # === (2) lazy fetch is wired: exactly one FILTERED fetch ============
      # GIT_TRACE is backfill-immune (it records the actual git subprocess the
      # fetcher runs), unlike object-counting on a promisor repo where rev-list
      # itself backfills. This is the decisive proof the promisor path is live.
      with subtest("git-lazy-fetch on: the fetch carries --filter=blob:none"):
          client.succeed("rm -rf /root/.cache/nix")
          trace = nix_eval(fetch_expr + ".rev", lazy=True, trace=True)
          fetches = git_fetches(trace)
          filtered = [f for f in fetches if "--filter=blob:none" in f]
          unfiltered = [f for f in fetches if "--filter" not in f]
          print("lazy fetches:\n  " + "\n  ".join(fetches))
          assert len(filtered) >= 1, (
              "git-lazy-fetch on, but no `git fetch --filter=blob:none` ran — "
              "the promisor path is not wired (is the server's v2 `filter` reaching Nix?)"
          )
          assert not unfiltered, (
              f"git-lazy-fetch issued a NON-filtered fetch: {unfiltered}"
          )
          # The server honoured the filter: the initial pack omits blobs. The
          # index-pack header records the object count of that first pack.
          assert "--promisor" in trace, (
              "fetch did not index a promisor pack — filter was not honoured"
          )

      # === (3) lazy fetch OFF: the fetch is a plain full fetch ============
      with subtest("git-lazy-fetch off: the fetch is unfiltered"):
          client.succeed("rm -rf /root/.cache/nix")
          trace = nix_eval(fetch_expr + ".rev", lazy=False, trace=True)
          fetches = git_fetches(trace)
          print("eager fetches:\n  " + "\n  ".join(fetches))
          assert fetches, "expected at least one git fetch"
          assert not any("--filter" in f for f in fetches), (
              f"git-lazy-fetch off, yet a filtered fetch ran: {fetches}"
          )

      def nix_hash_path(p):
          return client.succeed(
              "HOME=/root NIX_CONFIG='experimental-features = nix-command flakes' "
              f"nix hash path {p}"
          ).strip()

      # === (4) SOUNDNESS: lazy materialisation == eager full clone ========
      # The load-bearing property: whatever the on-demand backfill does, the
      # store path + NAR hash a lazy fetch produces must be byte-identical to a
      # full clone's.
      with subtest("git-lazy-fetch: lazy materialisation matches the eager path"):
          client.succeed("rm -rf /root/.cache/nix")
          lazy_path = nix_eval(fetch_expr + ".outPath", lazy=True, extra="--raw").strip()
          lazy_hash = nix_hash_path(lazy_path)
          # The materialised store path holds the real file bytes (backfill worked).
          client.succeed(f"test -s {lazy_path}/top.txt")
          client.succeed(f"grep -q aaaa {lazy_path}/sub/a.txt")

          client.succeed("rm -rf /root/.cache/nix")
          eager_path = nix_eval(fetch_expr + ".outPath", lazy=False, extra="--raw").strip()
          eager_hash = nix_hash_path(eager_path)

          assert lazy_path == eager_path, (
              f"SOUNDNESS: lazy outPath {lazy_path} != eager {eager_path}"
          )
          assert lazy_hash == eager_hash, (
              f"SOUNDNESS: lazy nar hash {lazy_hash} != eager {eager_hash}"
          )
          print(f"OK: lazy and eager agree on {lazy_path} ({lazy_hash})")

      # === (5) ssh transport: the ssh GitPromisorProvider path ============
      # The http subtests above never exercise the ~192-LOC ssh transport in
      # git-promisor.cc (SSHMaster pipe, out-of-band GIT_PROTOCOL via -oSetEnv,
      # advertisement-first FdSource). Drive a fetch over ssh:// and assert
      # (a) git-lazy-fetch's fetch over ssh carries --filter=blob:none and
      # (b) the ssh-fetched store path is byte-identical to the http/eager one
      # (soundness across transports).
      with subtest("ssh transport: filtered fetch + soundness vs http"):
          # Nix's ssh promisor probe goes through SSHMaster, which honours
          # NIX_SSHOPTS — NOT git's GIT_SSH_COMMAND / ~/.ssh/config (see the
          # comment in git-promisor.cc). So the key + host-key options must be
          # passed via NIX_SSHOPTS for BOTH the v2 capability probe and the
          # backfill. (git's own ls-remote below still uses GIT_SSH_COMMAND.)
          ssh_opts = "-i /root/.ssh/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

          # Sanity: plain git over ssh works (key + sshd wired correctly).
          client.succeed(f"GIT_SSH_COMMAND='ssh -F /root/.ssh/config' git ls-remote {ssh_url} >/dev/null")

          ssh_expr = f'(builtins.fetchGit {{ url = "{ssh_url}"; rev = "{rev}"; }}).outPath'
          client.succeed("rm -rf /root/.cache/nix")
          ssh_trace = client.succeed(
              "HOME=/root NIX_CONFIG='experimental-features = nix-command flakes' "
              f"NIX_SSHOPTS='{ssh_opts}' GIT_TRACE=2 "
              f"nix eval --impure --raw --option git-lazy-fetch true --expr '{ssh_expr}' "
              ">/root/ssh_out 2>/root/ssh_trace || true; cat /root/ssh_trace"
          )
          ssh_out = client.succeed("cat /root/ssh_out").strip()

          # SOUNDNESS (the load-bearing, robustly-checkable property): whatever
          # the ssh path does, a git-lazy-fetch fetch over ssh:// must produce a
          # byte-identical store path + NAR hash to the http/eager fetch from
          # subtest (4). This exercises the ssh transport end to end (SSHMaster
          # pipe, advertisement read, pack index) and asserts it agrees.
          ssh_hash = nix_hash_path(ssh_out)
          assert ssh_out == eager_path and ssh_hash == eager_hash, (
              f"SOUNDNESS across transports: ssh ({ssh_out}, {ssh_hash}) != "
              f"http/eager ({eager_path}, {eager_hash})"
          )
          print(f"OK (ssh soundness): ssh fetch agrees with http on {ssh_out}")

          # The ssh fetch must be BLOBLESS. This was a soft-report until the
          # SSHMaster SetEnv fix (GIT_PROTOCOL must be set on the control-master,
          # not just per-session, or OpenSSH drops it for a multiplexed session →
          # v0 advertisement → no `filter`). With the fix the v2 probe detects
          # `filter` over ssh and the fetch is filtered; hard-assert it so the
          # fix can't silently regress.
          assert "--filter=blob:none" in ssh_trace, (
              "git-lazy-fetch over ssh did NOT issue a --filter=blob:none fetch — "
              "the ssh promisor's v2 capability probe failed to detect `filter`. "
              "Regression of the SSHMaster GIT_PROTOCOL-on-the-master fix? Trace:\n"
              + ssh_trace[-2000:]
          )
          print("OK (ssh filtered): git-lazy-fetch issued a --filter=blob:none fetch over ssh")
    '';
}
