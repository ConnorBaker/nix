#!/usr/bin/env bash

# LD-V5 / LD-X3 — read-only invariance: under read-only evaluation nothing is
# written (the `.drv` is only minted, never enqueued), and lazy-derivations on
# vs off are byte-identical. `nix-instantiate --eval` is itself read-only
# (evalOnly && !read-write-mode), minting the drvPath purely in memory.

source ../common.sh
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
(mkDerivation { name = "ro"; buildCommand = "echo hi > $out"; }).drvPath
EOF

clearStore
drv=$(nix-instantiate --eval --expr "import $TEST_ROOT/t.nix" | tr -d '"')
# Read-only must not have written the .drv.
if nix-store --check-validity "$drv" 2>/dev/null; then
    fail "read-only evaluation wrote the .drv $drv (LD-V5 violated)"
fi

drv2=$(nix-instantiate --eval --option lazy-derivations true --expr "import $TEST_ROOT/t.nix" | tr -d '"')
[ "$drv" = "$drv2" ] || fail "read-only result differs lazy on vs off ($drv vs $drv2)"

echo "read-only-unchanged: ok (no writes, lazy on == off)"
