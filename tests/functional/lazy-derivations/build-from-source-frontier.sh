#!/usr/bin/env bash

# LD-S4 — daemon-safety / frontier sufficiency: when a target is built from
# source, every `.drv` the build goal reads back as a file (the target via its
# trampoline, and each built input) is valid before it is read — no goal hits
# `assert(false)` on a deferred `.drv`. After a from-source build the whole
# built `.drv` closure is materialised.

source ../common.sh
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
let dep = mkDerivation { name = "frontier-dep"; buildCommand = "echo dep > $out"; };
in mkDerivation {
  name = "frontier-top";
  buildCommand = "echo top > $out; cat ${dep} >> $out";
}
EOF

clearStore
nix build --no-link --option lazy-derivations true -f "$TEST_ROOT/t.nix" \
    || fail "from-source build hit a deferred-.drv error (frontier insufficient)"

# The target .drv and each input .drv it references are now materialised.
topDrv=$(nix-instantiate --readonly-mode "$TEST_ROOT/t.nix")
nix-store --check-validity "$topDrv" || fail "target .drv not materialised"

inputDrvs=$(nix-store --query --references "$topDrv" | grep '\.drv$' || true)
[ -n "$inputDrvs" ] || fail "target .drv has no input .drv references (expected the dep)"
for d in $inputDrvs; do
    nix-store --check-validity "$d" || fail "input .drv $d not materialised (frontier insufficient)"
done

echo "build-from-source-frontier: ok"
