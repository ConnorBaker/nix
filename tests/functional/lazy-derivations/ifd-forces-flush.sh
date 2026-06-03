#!/usr/bin/env bash

# LD-S5 — the monadic-escape flush. Import-from-derivation builds a derivation
# *during* evaluation (`realiseContext` → `buildPaths`,
# PROPOSAL-LAZY-DERIVATIONS.md §4.2 item 3). Under `lazy-derivations` the inner
# derivation's `.drv` write is deferred, so the `realiseContext` guard must
# flush the write-queue before the in-eval validity check + build — otherwise
# the context-realisation finds the target `.drv` missing.
#
# `nix eval` is used (not `nix-instantiate --eval`, which sets read-only mode
# and never writes a `.drv` at all). Non-vacuity: with the flush removed, this
# evaluation fails with "path '…ifd-inner.drv' is not valid".

source ../common.sh

clearStore

export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/ifd.nix" <<'EOF'
with import <config>;
# `import <derivation>` forces a mid-eval build of the inner derivation and
# imports its output as a Nix expression (here, the string "hello-ifd").
import (mkDerivation {
  name = "ifd-inner";
  buildCommand = "echo '\"hello-ifd\"' > $out";
})
EOF

result=$(nix eval --raw --option lazy-derivations true -f "$TEST_ROOT/ifd.nix") \
    || fail "IFD evaluation failed under lazy-derivations (realiseContext did not flush the deferred .drv)"

[ "$result" = "hello-ifd" ] || fail "expected IFD result 'hello-ifd', got: '$result'"

echo "ifd-forces-flush: ok"
