#!/usr/bin/env bash

# LD-S1 / LD-S3 — eval-store mass instantiation (the headline lazy-derivations
# workload, PROPOSAL-LAZY-DERIVATIONS.md §2/§7): evaluate many derivations to
# `.drv` files with no builds. Under `lazy-derivations` the writes are deferred
# and bulk-flushed at the resolution boundary, and the result must be
# OBSERVATIONALLY IDENTICAL to the eager path:
#
#   - the same drvPaths (content-addressing ⇒ deferral cannot change identity),
#   - every deferred `.drv` materialised (valid) once instantiation returns.
#
# Non-vacuity that deferral actually defers (the `.drv` is invalid until the
# flush boundary) is asserted at the unit level by the C++ test
# `LazyDerivationDeferTest.DefersDrvUntilFlushAndRendersAttrset`; here we prove
# the end-to-end pipeline is transparent.

source ../common.sh

clearStore

cat > "$TEST_ROOT/mass.nix" <<'EOF'
let
  mk = n: derivation {
    name = "lazy-mass-${toString n}";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [ "-c" "echo ${toString n} > $out" ];
  };
in builtins.genList mk 10
EOF

# Eager (lazy-derivations off) — the reference.
eager=$(nix-instantiate "$TEST_ROOT/mass.nix" | sort)
[ "$(echo "$eager" | wc -l)" -eq 10 ] || fail "expected 10 eager drvPaths, got: $eager"

clearStore

# Lazy: defer the writes, bulk-flush at the boundary.
lazy=$(nix-instantiate --option lazy-derivations true "$TEST_ROOT/mass.nix" | sort)
[ "$(echo "$lazy" | wc -l)" -eq 10 ] || fail "expected 10 lazy drvPaths, got: $lazy"

# LD-S3 — transparency: identical drvPaths.
if [ "$eager" != "$lazy" ]; then
    diff <(echo "$eager") <(echo "$lazy") || true
    fail "lazy drvPaths differ from eager (deferral changed identity)"
fi

# LD-S1 — every deferred `.drv` is materialised (valid) after instantiation.
for d in $lazy; do
    nix-store --check-validity "$d" \
        || fail "lazy .drv was not materialised at the flush boundary: $d"
done

echo "mass-instantiation: ok (10 drvs, lazy ≡ eager, all materialised)"
