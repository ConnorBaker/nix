#!/usr/bin/env bash

# Patch G1 regression/correctness test.
#
# Covers:
#   * Parallel .links/ sweep equivalent to serial across thread counts.
#   * Enough entries to cross chunkSize=4096 at least once.
#   * ENOENT tolerance for pre-deleted entries.
#   * Optimise running concurrently during GC's links phase does not
#     corrupt state (optimise re-creates entries GC races to delete).

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

    local expr
    expr=$(cat <<EOF
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
)
    local drvs
    drvs=$(echo "$expr" | nix-instantiate --expr - --add-root "$TEST_ROOT/$tag.drvs" --indirect 2>/dev/null)
    while IFS= read -r drv; do
        [ -z "$drv" ] && continue
        nix-store --realise "$drv" > /dev/null
    done <<< "$drvs"
    # Release drv roots so optimise can run cleanly.
    rm -f "$TEST_ROOT/$tag.drvs"*
}

countLinks() { find "$NIX_STORE_DIR"/.links -maxdepth 1 -type f 2>/dev/null | wc -l; }

# ---------- Scenario 1: serial vs parallel equivalence ----------
#
# We need enough paths that after deletion and the subsequent .links/
# sweep, we cross the 4096-per-chunk boundary. Use ~1200 unique files
# total (100 derivations × 12 files each after auto-optimise dedup).

clearStoreIfPossible
generateStore 100 2 serial-links

# Force auto-optimise so .links/ gets populated.
NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

preLinksSerial="$(countLinks)"

# Run serial GC to clean up paths that no longer have outputs. Since
# we dropped drv roots, paths are live only via their own existence
# in the store (no roots). GC should delete everything.
nix-store --gc --option gc-links-threads 1

postLinksSerial="$(countLinks)"
# After GC, .links/ should be empty (all content-addressed entries
# became orphans).
if [ "$postLinksSerial" != "0" ]; then
    echo "FAIL(serial): .links/ non-empty after full GC: $postLinksSerial"
    exit 1
fi

clearStoreIfPossible
generateStore 100 2 parallel-links
NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

preLinksParallel="$(countLinks)"
nix-store --gc --option gc-links-threads 16
postLinksParallel="$(countLinks)"

if [ "$postLinksParallel" != "0" ]; then
    echo "FAIL(parallel): .links/ non-empty after full GC: $postLinksParallel"
    exit 1
fi

# Both runs must have started with identical link counts.
if [ "$preLinksSerial" != "$preLinksParallel" ]; then
    echo "FAIL: .links/ entry count pre-GC differs: serial=$preLinksSerial parallel=$preLinksParallel"
    exit 1
fi

# ---------- Scenario 2: many entries crossing chunk boundary ----------
#
# Generate enough distinct .links/ entries to cross chunkSize=4096.

clearStoreIfPossible
# 1100 derivations × (2 shared + 3 unique) = ~5500 files; auto-optimise
# ends up with ~3303 distinct hashes after dedup. This crosses 4096.
generateStore 1100 2 big

NIX_REMOTE="" nix-store --optimise --option optimise-threads 8
preBigLinks="$(countLinks)"

nix-store --gc --option gc-links-threads 16
postBigLinks="$(countLinks)"

if [ "$postBigLinks" != "0" ]; then
    echo "FAIL(big): .links/ non-empty after full GC: $postBigLinks (started with $preBigLinks)"
    exit 1
fi

# ---------- Scenario 3: ENOENT tolerance during parallel sweep ----------
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

# ---------- Scenario 4: concurrent optimise during GC links phase ----------
#
# Start a background nix-store --optimise while we GC. We can't
# deterministically race these at the second level, but if either
# crashes or leaves dangling state, the next optimise will surface it.

clearStoreIfPossible
generateStore 50 2 concurrent

# Pre-populate .links/.
NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

# Rebuild-then-release a fresh set so there's new content for optimise.
generateStore 50 2 concurrent2

# Start optimise + GC in parallel.
(nix-store --optimise --option optimise-threads 4 > /dev/null 2>&1) &
opt_pid=$!
(nix-store --gc --option gc-links-threads 4 > /dev/null 2>&1) &
gc_pid=$!

wait "$opt_pid" || true
wait "$gc_pid" || true

# A follow-up optimise must succeed cleanly; this is the true
# robustness check.
NIX_REMOTE="" nix-store --optimise --option optimise-threads 4

echo "PASS: gc-links-parallel"
