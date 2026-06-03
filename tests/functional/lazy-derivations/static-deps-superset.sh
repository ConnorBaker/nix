#!/usr/bin/env bash

# LD-S8 — static-dependency over-approximation: the statically-named input-`.drv`
# edges (`inputDrvs`, populated at eval time and recorded in the `.drv`) are a
# superset of what realisation demands. IFD-free means no un-named dependency is
# ever introduced. Here: a target depending on a dep records that dep's `.drv`
# as a static `inputDrvs` edge, observable in the materialised `.drv`.

source ../common.sh
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
let dep = mkDerivation { name = "s8-dep"; buildCommand = "echo dep > $out"; };
in mkDerivation {
  name = "s8-top";
  buildCommand = "echo top > $out; echo ${dep}";
}
EOF

clearStore
topDrv=$(nix-instantiate --option lazy-derivations true "$TEST_ROOT/t.nix")
depDrv=$(nix-instantiate --readonly-mode "$TEST_ROOT/t.nix" -A nonexistent 2>/dev/null || true)

# The materialised top .drv statically names the dep's .drv as an input edge.
refDrvs=$(nix-store --query --references "$topDrv" | grep 's8-dep.*\.drv$' || true)
[ -n "$refDrvs" ] || fail "top .drv does not statically name its dep .drv (LD-S8 superset violated)"

# And `nix derivation show` exposes it under inputDrvs.
nix derivation show --option lazy-derivations true -f "$TEST_ROOT/t.nix" \
    | grep -q "s8-dep" || fail "dep .drv missing from inputDrvs in derivation show"

echo "static-deps-superset: ok"
