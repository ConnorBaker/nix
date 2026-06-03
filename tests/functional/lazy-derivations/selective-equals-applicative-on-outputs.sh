#!/usr/bin/env bash

# LD-S7 — selective-flush-then-realise yields the same OUTPUT closure as
# applicative-flush-everything-then-realise; the difference is only in which
# `.drv`s are written. Concretely: building a substitutable target with
# lazy-derivations OFF (applicative: writes the `.drv` during eval) and ON
# (selective: elides the substitutable target's `.drv`) gives byte-identical
# outputs, but the valid-`.drv` sets differ (ON ⊂ OFF).

source ../common.sh

needLocalStore "“--no-require-sigs” can’t be used with the daemon"

export NIX_PATH=config="${config_nix}"
cacheDir="$TEST_ROOT/cache"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
mkDerivation { name = "s7"; buildCommand = "echo s7-output > $out"; }
EOF

# Seed a cache with the target's OUTPUT only.
out=$(nix build --no-link --print-out-paths -f "$TEST_ROOT/t.nix")
nix copy --to "file://$cacheDir" "$out"
drv=$(nix-instantiate --readonly-mode "$TEST_ROOT/t.nix")
# `-j0` forbids local building, forcing the output to come from the substituter.
SUB="--substitute --substituters file://$cacheDir --no-require-sigs -j0"

# Applicative (lazy off): output substitutes, but the .drv is written eagerly.
clearStore
nix build --no-link $SUB -f "$TEST_ROOT/t.nix"
hashOff=$(nix-store --query --hash "$out")
nix-store --check-validity "$drv" 2>/dev/null && drvOff=Y || drvOff=N

# Selective (lazy on): output substitutes, the substitutable target's .drv is elided.
clearStore
nix build --no-link --option lazy-derivations true $SUB -f "$TEST_ROOT/t.nix"
hashOn=$(nix-store --query --hash "$out")
nix-store --check-validity "$drv" 2>/dev/null && drvOn=Y || drvOn=N

[ "$hashOff" = "$hashOn" ] || fail "output closures differ (selective != applicative on outputs)"
[ "$drvOff" = "Y" ] || fail "applicative (lazy off) should write the .drv"
[ "$drvOn" = "N" ] || fail "selective (lazy on) should elide the substitutable target's .drv"

echo "selective-equals-applicative-on-outputs: ok (same outputs, drv-set ON ⊂ OFF)"
