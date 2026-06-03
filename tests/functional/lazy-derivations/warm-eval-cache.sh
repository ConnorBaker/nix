#!/usr/bin/env bash

# LD-V3 / C2 — warm flake eval-cache + lazy-derivations on the build path. On a
# warm eval-cache hit, `AttrCursor::forceDerivation` returns the cached drvPath
# WITHOUT re-running `derivationStrict`; under naive deferral the `.drv` is
# neither on disk nor pending, so the cache path would throw "don't know how to
# recreate store derivation". The fix: `forceDerivation` re-forces (re-defers)
# and TOLERATES a still-pending `.drv` (it is materialised at the build
# boundary). The §5 soundness rests on build goals reading `.drv` *bytes*, never
# the cold `drvHashes` modulo memo (asserted structurally by meta-audit.sh).
#
# This builds a flake twice with the store cleared in between but the eval cache
# warm; the second build must succeed from the cache hit.

source ../common.sh

requireGit
enableFeatures "flakes"

clearStore

flake="$TEST_ROOT/flk"
createGitRepo "$flake"
cat > "$flake/flake.nix" <<'EOF'
{
  outputs = { self }: {
    packages.SYSTEM.default = derivation {
      name = "warm-cache-target";
      system = "SYSTEM";
      builder = "/bin/sh";
      args = [ "-c" "echo warm-cache-ok > $out" ];
    };
  };
}
EOF
sed -i "s|SYSTEM|$system|g" "$flake/flake.nix"
git -C "$flake" add flake.nix
git -C "$flake" commit -m init

ref="git+file://$flake#packages.$system.default"

# 1. First build with lazy on — populates the flake eval cache (drvPath/outPath)
#    and builds the output.
out=$(nix build --no-link --print-out-paths --option lazy-derivations true "$ref")
nix-store --check-validity "$out" || fail "first build's output missing"

# 2. Clear the STORE (the .drv and output are gone) but NOT the eval cache, so
#    the next build takes the warm-eval-cache `forceDerivation` path.
clearStore

# 3. Second build, lazy on: warm eval-cache hit ⇒ forceDerivation returns the
#    cached drvPath; the deferred `.drv` must be tolerated and materialised at
#    the build boundary. Must NOT throw "don't know how to recreate store
#    derivation".
out2=$(nix build --no-link --print-out-paths --option lazy-derivations true "$ref") \
    || fail "warm-eval-cache build failed under lazy-derivations (forceDerivation tolerance / cold drvHashes)"

[ "$out" = "$out2" ] || fail "warm-cache rebuild produced a different output path"
nix-store --check-validity "$out2" || fail "warm-cache rebuild's output missing"
[ "$(cat "$out2")" = "warm-cache-ok" ] || fail "warm-cache rebuild's output has wrong contents"

echo "warm-eval-cache: ok (lazy build succeeds on a warm eval-cache hit)"
