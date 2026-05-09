#!/usr/bin/env bash

# Regression test for the `sharded-links` experimental feature.
#
# Verifies:
#   * Fresh store with `experimental-features = sharded-links` places
#     new canonical `.links/<pfx>/<hash>` entries into 2-char shard
#     subdirectories rather than the flat `.links/<hash>` layout.
#   * A legacy flat store lazily migrates entries into shards when
#     `nix-store --optimise` runs with the feature enabled.
#   * Mixed layouts co-exist during migration (some hashes flat,
#     others sharded) and `nix-store --gc` correctly handles both.

source common.sh

TODO_NixOS

# ---------- helpers ----------

# Build `n` derivations whose shared file rotates by group id so the
# hashes are distinct but there is still some sharing.
buildStore() {
    local n="$1"
    local tag="$2"

    local expr
    expr=$(cat <<EOF
let
  cfg = import ${config_nix};
  mkN = i: cfg.mkDerivation {
    name = "${tag}-\${toString i}";
    builder = builtins.toFile "b" ''
      mkdir \$out
      echo shared-\$((i % 3)) > \$out/shared
      echo unique-\${toString i} > \$out/unique
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
    rm -f "$TEST_ROOT/$tag.drvs"*
}

# Count files directly under `.links/` (flat entries).
countFlat() {
    find "$NIX_STORE_DIR"/.links -maxdepth 1 -type f 2>/dev/null | wc -l
}

# Count files inside shard subdirectories (sharded entries).
countSharded() {
    find "$NIX_STORE_DIR"/.links -mindepth 2 -type f 2>/dev/null | wc -l
}

# Total `.links/` entries regardless of layout.
countAll() {
    find "$NIX_STORE_DIR"/.links -type f 2>/dev/null | wc -l
}

# Count top-level shard subdirectories (should be 1024 when flag is on).
countShards() {
    find "$NIX_STORE_DIR"/.links -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l
}

# ---------- Scenario 1: fresh store with flag on ----------

clearStoreIfPossible

buildStore 10 shard-fresh

NIX_REMOTE="" nix-store --optimise \
    --extra-experimental-features sharded-links \
    --option optimise-threads 1

# Should be 1024 shard subdirs pre-created at store open.
shards=$(countShards)
if [ "$shards" -ne 1024 ]; then
    echo "FAIL: expected 1024 shard subdirs, got $shards"
    exit 1
fi

# All entries should now live inside shard subdirs (new store, so
# nothing pre-existing).
flatCount=$(countFlat)
shardedCount=$(countSharded)

if [ "$flatCount" -gt 0 ]; then
    echo "FAIL: $flatCount entries are flat on a fresh sharded store"
    exit 1
fi
if [ "$shardedCount" -eq 0 ]; then
    echo "FAIL: no sharded entries created"
    exit 1
fi

# Sanity: a sharded entry's first two chars of filename must match
# its parent directory name, and the filename must end with
# `.<NN>` (two zero-padded digits) per the sharded-layout invariant.
mismatched=0
bad_suffix=0
while IFS= read -r f; do
    local_name=$(basename "$f")
    parent=$(basename "$(dirname "$f")")
    prefix=${local_name:0:2}
    if [ "$parent" != "$prefix" ]; then
        mismatched=$((mismatched + 1))
        echo "DEBUG: mismatched shard: $f (parent=$parent, prefix=$prefix)"
    fi
    # Name = <52-char-hash>.<NN>, total 55 chars.
    n=${#local_name}
    if [ "$n" -lt 55 ] || [ "${local_name:$((n-3)):1}" != "." ]; then
        bad_suffix=$((bad_suffix + 1))
        echo "DEBUG: bad replica suffix: $f (len=$n)"
        continue
    fi
    d1=${local_name:$((n-2)):1}
    d2=${local_name:$((n-1)):1}
    case "$d1$d2" in
        [0-9][0-9]) ;;  # ok
        *) bad_suffix=$((bad_suffix + 1));
           echo "DEBUG: non-digit replica suffix: $f" ;;
    esac
done < <(find "$NIX_STORE_DIR"/.links -mindepth 2 -type f)

if [ "$mismatched" -gt 0 ]; then
    echo "FAIL: $mismatched entries are in the wrong shard directory"
    exit 1
fi
if [ "$bad_suffix" -gt 0 ]; then
    echo "FAIL: $bad_suffix entries violate the .NN replica suffix invariant"
    exit 1
fi

# ---------- Scenario 2: legacy flat store, migrate with flag on ----------

clearStoreIfPossible

# Build with flag OFF so entries land flat.
buildStore 10 shard-migrate
NIX_REMOTE="" nix-store --optimise --option optimise-threads 1

preFlat=$(countFlat)
preSharded=$(countSharded)
if [ "$preFlat" -eq 0 ]; then
    echo "FAIL: expected flat entries pre-migration, got $preFlat"
    exit 1
fi
if [ "$preSharded" -ne 0 ]; then
    echo "FAIL: unexpected sharded entries pre-migration: $preSharded"
    exit 1
fi

# Re-run optimise with the flag ON. Existing entries get lazily
# migrated; new ones land sharded.
NIX_REMOTE="" nix-store --optimise \
    --extra-experimental-features sharded-links \
    --option optimise-threads 1

postFlat=$(countFlat)
postSharded=$(countSharded)

if [ "$postSharded" -lt "$preFlat" ]; then
    echo "FAIL: migration did not move all entries (pre-flat=$preFlat, post-sharded=$postSharded, post-flat=$postFlat)"
    exit 1
fi

# ---------- Scenario 3: mixed layout + GC ----------

# Build some additional content with flag OFF, producing new flat
# entries on top of the existing sharded ones.
buildStore 5 shard-mixed
NIX_REMOTE="" nix-store --optimise --option optimise-threads 1

if [ "$(countFlat)" -eq 0 ]; then
    echo "FAIL: expected new flat entries after optimise-without-flag"
    exit 1
fi

# Release all roots.
nix-store --gc --extra-experimental-features sharded-links

# After full GC, all `.links/` entries should be nlink=1 and have
# been unlinked.
finalTotal=$(countAll)
if [ "$finalTotal" -ne 0 ]; then
    echo "FAIL: $finalTotal .links/ entries survived GC"
    exit 1
fi

echo "PASS: sharded-links"
