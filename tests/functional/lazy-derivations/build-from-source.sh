#!/usr/bin/env bash

# Build-path flush (PROPOSAL-LAZY-DERIVATIONS.md §4.2 item 1 / Increment 5).
# `nix build` of a target with a from-source input: under lazy-derivations both
# the target `.drv` and its input `.drv` are deferred at eval time. The
# client-side build-path flush in `Installable::build2` must materialise the
# pending closure before the Worker reads those `.drv`s back as files (the
# target via its trampoline, the input via the building goal). Non-vacuity:
# without the flush, the build fails with "path '…lazy-dep.drv' is not valid".

source ../common.sh

clearStore

export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/build.nix" <<'EOF'
with import <config>;
let
  dep = mkDerivation {
    name = "lazy-dep";
    buildCommand = "echo dep-content > $out";
  };
in mkDerivation {
  name = "lazy-top";
  # Referencing `dep` gives `top` a from-source input `.drv` that the
  # build-path flush must materialise alongside the target.
  buildCommand = "echo top-content > $out; cat ${dep} >> $out";
}
EOF

out=$(nix build --no-link --print-out-paths --option lazy-derivations true -f "$TEST_ROOT/build.nix") \
    || fail "nix build failed under lazy-derivations (build-path flush missing?)"

grep -q top-content "$out" || fail "missing top-content in build output"
grep -q dep-content "$out" || fail "from-source input was not built (input .drv not flushed)"

echo "build-from-source: ok"
