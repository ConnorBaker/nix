#!/usr/bin/env bash

# LD-S6 — frontier ↔ build consistency (TOCTOU). In this implementation there is
# no separate "flush pre-pass" whose substitutability decision could go stale:
# the build reads `.drv`s virtually (LazyDrvStore) and decides at realisation,
# materialising a `.drv` only when it commits to building it. So a target whose
# substitutability changes between runs is handled cleanly — substituted when
# the cache has it, built from source when it does not — with no crash and no
# missing-`.drv` assertion.

source ../common.sh

needLocalStore "“--no-require-sigs” can’t be used with the daemon"

export NIX_PATH=config="${config_nix}"
cacheDir="$TEST_ROOT/cache"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
mkDerivation { name = "s6"; buildCommand = "echo s6 > $out"; }
EOF
# `-j0` forbids local building, forcing the output from the substituter.
SUB="--substitute --substituters file://$cacheDir --no-require-sigs -j0"

# Seed the cache with the output.
out=$(nix build --no-link --print-out-paths -f "$TEST_ROOT/t.nix")
nix copy --to "file://$cacheDir" "$out"

# Run 1: substitutable (cache hit) — output substituted, .drv elided.
clearStore
nix build --no-link --option lazy-derivations true $SUB -f "$TEST_ROOT/t.nix" \
    || fail "substitutable build failed"

# Run 2: substitutability flips — evict the cache, then build. Must fall back to
# building from source cleanly (no stale-frontier crash).
clearStore
rm -rf "$cacheDir"
nix build --no-link --option lazy-derivations true -f "$TEST_ROOT/t.nix" \
    || fail "fell over when substitutability flipped (LD-S6)"
nix-store --check-validity "$out" || fail "output not built after cache eviction"

echo "substitutability-flips: ok"
