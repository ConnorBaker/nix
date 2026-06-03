#!/usr/bin/env bash

# LD-S9 — `queryMissing` must classify a deferred `.drv` correctly (will-build /
# will-substitute), not as "unknown". The proposal proposed teaching
# `queryMissing` to read the pending queue (the `misc.cc:208` FIXME); this
# implementation achieves the same via the lazy-`.drv` store overlay, so
# `nix build --dry-run` reports a deferred target as buildable — and, being a
# dry run, writes nothing.

source ../common.sh
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/t.nix" <<'EOF'
with import <config>;
mkDerivation { name = "s9"; buildCommand = "echo hi > $out"; }
EOF

clearStore
report=$(nix build --dry-run --option lazy-derivations true -f "$TEST_ROOT/t.nix" 2>&1)

echo "$report" | grep -q "will be built" \
    || fail "dry-run did not report the deferred .drv as buildable (LD-S9): $report"
if echo "$report" | grep -qi "don't know how to build"; then
    fail "dry-run misreported the deferred .drv as unknown (LD-S9): $report"
fi

# A dry run must not write the .drv.
drv=$(nix-instantiate --readonly-mode "$TEST_ROOT/t.nix")
if nix-store --check-validity "$drv" 2>/dev/null; then
    fail "dry-run wrote the .drv $drv (should be a no-op)"
fi

echo "dry-run-reports-deferred: ok"
