#!/usr/bin/env bash

# LD-S3 — deferral is observationally transparent: a build with lazy-derivations
# ON produces byte-identical results (output path AND output contents) to the
# same build with it OFF. The deferral is a pure optimisation of *when* `.drv`s
# are written, never *what* is produced.

source ../common.sh
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
let dep = mkDerivation { name = "trans-dep"; buildCommand = "echo dep > $out"; };
in mkDerivation {
  name = "trans-top";
  buildCommand = "echo top-content > $out; cat ${dep} >> $out";
}
EOF

clearStore
off=$(nix build --no-link --print-out-paths -f "$TEST_ROOT/t.nix")
offHash=$(nix-store --query --hash "$off")

clearStore
on=$(nix build --no-link --print-out-paths --option lazy-derivations true -f "$TEST_ROOT/t.nix")
onHash=$(nix-store --query --hash "$on")

[ "$off" = "$on" ] || fail "lazy build output path differs from eager ($off vs $on)"
[ "$offHash" = "$onHash" ] || fail "lazy build output contents differ from eager"

echo "deferral-is-observationally-transparent: ok"
