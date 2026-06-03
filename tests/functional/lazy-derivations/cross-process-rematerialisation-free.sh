#!/usr/bin/env bash

# LD-D5 — cross-process re-materialisation: a `.drv`'s path is content-addressed,
# so a fresh process evaluating the same expression re-mints the identical path
# and (finding it already valid) skips the write. Asserts cross-process
# determinism (same drvPaths) + validity. The "writes zero" half is asserted at
# the unit level (AlreadyValidIsNotRewrittenAcrossWriters); the negative control
# here perturbs the content and gets a different path.

source ../common.sh
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
mkDerivation { name = "d5"; buildCommand = "echo same > $out"; }
EOF

clearStore
a=$(nix-instantiate --option lazy-derivations true "$TEST_ROOT/t.nix")
# Fresh process, identical expression → identical path (re-mint), still valid.
b=$(nix-instantiate --option lazy-derivations true "$TEST_ROOT/t.nix")
[ "$a" = "$b" ] || fail "cross-process drvPaths differ ($a vs $b)"
nix-store --check-validity "$b" || fail "re-minted .drv is not valid"

# Negative control: a different derivation gets a different path.
cat > "$TEST_ROOT/t2.nix" <<'EOF'
with import <config>;
mkDerivation { name = "d5"; buildCommand = "echo DIFFERENT > $out"; }
EOF
c=$(nix-instantiate --option lazy-derivations true "$TEST_ROOT/t2.nix")
[ "$a" != "$c" ] || fail "different content must yield a different drvPath"

echo "cross-process-rematerialisation-free: ok"
