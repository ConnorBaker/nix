#!/usr/bin/env bash

source ./common.sh

requireGit

flake1Dir="$TEST_ROOT/eval-cache-flake"

createGitRepo "$flake1Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$flake1Dir/"
git -C "$flake1Dir" add simple.nix simple.builder.sh config.nix
git -C "$flake1Dir" commit -m "config.nix"

cat >"$flake1Dir/flake.nix" <<EOF
{
  description = "Fnord";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    foo.bar = throw "breaks";
    drv = mkDerivation {
      name = "build";
      buildCommand = ''
        echo true > \$out
      '';
    };
    stack-depth =
      let
        f = x: if x == 0 then true else f (x - 1);
      in
        assert (f 100); self.drv;
    ifd = assert (import self.drv); self.drv;
  };
}
EOF

git -C "$flake1Dir" add flake.nix
git -C "$flake1Dir" commit -m "Init"

expect 1 nix build "$flake1Dir#foo.bar" 2>&1 | grepQuiet 'error: breaks'
expect 1 nix build "$flake1Dir#foo.bar" 2>&1 | grepQuiet 'error: breaks'

# Stack overflow: fails at depth 50, then succeeds at the default limit.
# The eval cache does not memoise failures, so the second call re-evaluates.
expect 1 nix build --max-call-depth 50 "$flake1Dir#stack-depth" 2>&1 \
  | grepQuiet 'error: stack overflow; max-call-depth exceeded'
nix build --no-link "$flake1Dir#stack-depth"

# IFD: fails with allow-import-from-derivation disabled, then succeeds with
# it enabled. Same fail-then-succeed shape via re-evaluation.
expect 1 nix build "$flake1Dir#ifd" --option allow-import-from-derivation false 2>&1 \
  | grepQuiet 'error: cannot build .* during evaluation because the option '\''allow-import-from-derivation'\'' is disabled'
nix build --no-link "$flake1Dir#ifd"
