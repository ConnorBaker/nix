#!/usr/bin/env bash

# Patch G1 regression/correctness test.
#
# Covers:
#   * Parallel .links/ sweep equivalent to serial across thread counts.
#   * ENOENT tolerance for pre-deleted entries.
#
# Concurrent optimise + GC interaction is covered by
# gc-optimise-concurrent.sh.

source common.sh

TODO_NixOS

# ---------- helpers ----------

# Build N derivations with distinct per-derivation content plus K
# shared files (so .links/ ends up with a mixture of deduped and
# soon-to-be-unique entries).
generateStore() {
    local n="$1"
    local shared="$2"
    local tag="$3"

    realiseFromExpr "$tag" <<EOF
let
  cfg = import ${config_nix};
  mkN = i: cfg.mkDerivation {
    name = "${tag}-\${toString i}-pkg";
    builder = builtins.toFile "b" ''
      mkdir \$out
      for j in \$(seq 1 ${shared}); do
        echo shared-\$j > \$out/s\$j
      done
      for j in \$(seq 1 3); do
        echo unique-\${toString i}-\$j > \$out/u\$j
      done
    '';
  };
in builtins.listToAttrs (
     map (i: { name = "d\${toString i}"; value = mkN i; })
     (builtins.genList (x: x) ${n}))
EOF
}

countLinks() { find "$NIX_STORE_DIR"/.links -maxdepth 1 -type f 2>/dev/null | wc -l; }

# ---------- Scenario 1: serial vs parallel equivalence ----------
#
# Use 1100 derivations × (2 shared + 3 unique) ≈ 3300 distinct
# hashes after dedup, enough to span multiple gc-links-threads
# chunks. Both gc-links-threads=1 and gc-links-threads=16 must
# empty `.links/` from an identical pre-GC fixture.

run_links_gc() {
    local tag="$1" threads="$2"
    clearStoreIfPossible >&2
    generateStore 1100 2 "$tag" >&2
    NIX_REMOTE="" nix-store --optimise --option optimise-threads 8 >&2
    local pre; pre="$(countLinks)"
    nix-store --gc --option gc-links-threads "$threads" >&2
    local post; post="$(countLinks)"
    if [ "$post" != "0" ]; then
        echo "FAIL($tag): .links/ non-empty after full GC: $post" >&2
        exit 1
    fi
    echo "$pre"
}

preLinksSerial="$(run_links_gc serial-links 1)"
preLinksParallel="$(run_links_gc parallel-links 16)"

# Both runs must have started with identical link counts.
if [ "$preLinksSerial" != "$preLinksParallel" ]; then
    echo "FAIL: .links/ entry count pre-GC differs: serial=$preLinksSerial parallel=$preLinksParallel"
    exit 1
fi

# ---------- Scenario 2: ENOENT tolerance during parallel sweep ----------
#
# Rebuild, pre-delete half the .links/ entries, then run GC. The
# workers must tolerate the missing entries.

clearStoreIfPossible
generateStore 50 2 enoent

NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

# Pre-delete every third entry.
i=0
for f in "$NIX_STORE_DIR"/.links/*; do
    if [ "$((i % 3))" = "0" ]; then
        rm -f "$f"
    fi
    i=$((i + 1))
done

# GC must complete cleanly despite the pre-deleted entries.
nix-store --gc --option gc-links-threads 4

# Concurrent optimise during GC's links phase is covered by
# `gc-optimise-concurrent.sh` Phase 2.

echo "PASS: gc-links-parallel"
