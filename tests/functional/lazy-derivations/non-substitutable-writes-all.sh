#!/usr/bin/env bash

# LD-S2 (negative control) — the elision must NOT over-fire. With no substituter,
# a target is built from source, and a built `.drv` IS materialised (doc C1:
# the target `.drv` of a built output is unavoidable). This is the control that
# distinguishes selective elision from "elide everything": pair it with
# substitutable-input-elision.sh (where the same `.drv` IS elided).

source ../common.sh

clearStore
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
mkDerivation { name = "nsw-target"; buildCommand = "echo built-from-source > $out"; }
EOF

# Build from source — no substituters.
nix build --no-link --option lazy-derivations true -f "$TEST_ROOT/t.nix"

drv=$(nix-instantiate --readonly-mode "$TEST_ROOT/t.nix")
nix-store --check-validity "$drv" \
    || fail "from-source build must materialise its .drv (LD-S2 negative control / doc C1)"

echo "non-substitutable-writes-all: ok (built .drv written)"
