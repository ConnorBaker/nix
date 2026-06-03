#!/usr/bin/env bash

# LD-S2 — selective elision. When a build target's output is substitutable, the
# build downloads the output and never reads/writes the target's `.drv`. Under
# `lazy-derivations` the deferred `.drv` is read virtually (LazyDrvStore) to
# check substitutability and then dropped unwritten — so it is ELIDED.
#
# Non-vacuity: the negative-control test (no substituters) builds from source,
# which DOES materialise the produced output.

source ../common.sh

needLocalStore "“--no-require-sigs” can’t be used with the daemon"

clearStore
clearCache

export NIX_PATH=config="${config_nix}"
cacheDir="$TEST_ROOT/cache"

cat > "$TEST_ROOT/elision.nix" <<'EOF'
with import <config>;
mkDerivation {
  name = "elision-target";
  buildCommand = "echo target-output > $out";
}
EOF

# 1. Build from source and push the OUTPUT (+ closure) to a binary cache.
out=$(nix build --no-link --print-out-paths -f "$TEST_ROOT/elision.nix")
nix copy --to "file://$cacheDir" "$out"

# The target's `.drv` path, minted read-only (no write).
drv=$(nix-instantiate --readonly-mode "$TEST_ROOT/elision.nix")

clearStore

# 2. Build from the cache with lazy-derivations on: the output substitutes.
#    `-j0` forbids local building, so the output MUST come from the substituter
#    (and the test fails loudly if it cannot, rather than silently building).
nix build --no-link -f "$TEST_ROOT/elision.nix" \
    --option lazy-derivations true \
    --substitute --substituters "file://$cacheDir" --no-require-sigs -j0

# 3. The output is present (substituted)...
nix-store --check-validity "$out" || fail "substituted output is missing"

# ...but the target `.drv` was elided (never written) — the LD-S2 win.
if nix-store --check-validity "$drv" 2>/dev/null; then
    fail "target .drv $drv was materialised even though its output substituted (no elision, LD-S2)"
fi

echo "substitutable-input-elision: ok (.drv elided)"
