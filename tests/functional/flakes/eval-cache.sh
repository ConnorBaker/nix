#!/usr/bin/env bash

source ./common.sh

requireGit

# These cache assertions require immutable source identities, so both
# fixtures below are evaluated through their explicitly pinned commits.
export _NIX_TEST_BARF_ON_UNCACHEABLE=1

# Test basic caching: trace should only appear on first evaluation
basicCacheDir="$TEST_ROOT/basic-cache-flake"
createGitRepo "$basicCacheDir" ""
cp "${config_nix}" "$basicCacheDir/"
git -C "$basicCacheDir" add config.nix
git -C "$basicCacheDir" commit -m "config.nix"

cat >"$basicCacheDir/flake.nix" <<EOF
{
  description = "Basic cache test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    # Assert pure-eval is enabled (builtins.currentSystem is unavailable in pure mode)
    packages.$system.default = assert builtins ? currentSystem -> throw "pure-eval not enabled";
      builtins.trace "basic-test-evaluating" (mkDerivation {
        name = "cached-build";
        buildCommand = "echo hello > \\\$out";
      });
  };
}
EOF

git -C "$basicCacheDir" add flake.nix
git -C "$basicCacheDir" commit -m "Init"
basicCacheRef="git+file://$basicCacheDir?rev=$(git -C "$basicCacheDir" rev-parse HEAD)"

clearBinaryCache

# First build should show trace
nix build --no-link "$basicCacheRef" 2>&1 | grepQuiet 'trace: basic-test-evaluating'

# Second build should use cache, no trace output
nix build --no-link "$basicCacheRef" 2>&1 | grepQuietInverse 'trace: basic-test-evaluating'

# Third build should also use cache, no trace output
nix build --no-link "$basicCacheRef" 2>&1 | grepQuietInverse 'trace: basic-test-evaluating'

# Test edge cases with separate flake
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
flake1Ref="git+file://$flake1Dir?rev=$(git -C "$flake1Dir" rev-parse HEAD)"

expect 1 nix build "$flake1Ref#foo.bar" 2>&1 | grepQuiet 'error: breaks'
expect 1 nix build "$flake1Ref#foo.bar" 2>&1 | grepQuiet 'error: breaks'

# Stack overflow error must not be cached
expect 1 nix build --max-call-depth 50 "$flake1Ref#stack-depth" 2>&1 \
  | grepQuiet 'error: stack overflow; max-call-depth exceeded'
# If the SO is cached, the following invocation will produce a cached failure; we expect it to succeed
nix build --no-link "$flake1Ref#stack-depth"

# Conditional error should not be cached
expect 1 nix build "$flake1Ref#ifd" --option allow-import-from-derivation false 2>&1 \
  | grepQuiet 'error: cannot build .* during evaluation because the option '\''allow-import-from-derivation'\'' is disabled'
nix build --no-link "$flake1Ref#ifd"

# A working tree remains mutable even when Git's index flags suppress
# content comparisons. Its current bytes must not reuse a revision's
# cached evaluation; the pinned commit continues to denote its old bytes.
unset _NIX_TEST_BARF_ON_UNCACHEABLE
cp "$basicCacheDir/flake.nix" "$TEST_ROOT/basic-cache-original.nix"
for assume in --no-assume-unchanged --assume-unchanged; do
  for skip in --no-skip-worktree --skip-worktree; do
    cp "$TEST_ROOT/basic-cache-original.nix" "$basicCacheDir/flake.nix"
    git -C "$basicCacheDir" update-index "$assume" flake.nix
    git -C "$basicCacheDir" update-index "$skip" flake.nix
    [[ $(nix eval --raw "$basicCacheDir#packages.$system.default.name") = cached-build ]]
    sed 's/name = "cached-build"/name = "changed-build"/' \
      "$TEST_ROOT/basic-cache-original.nix" > "$basicCacheDir/flake.nix"
    [[ $(nix eval --raw "$basicCacheDir#packages.$system.default.name") = changed-build ]]
    [[ $(nix eval --raw "$basicCacheRef#packages.$system.default.name") = cached-build ]]
  done
done
