#!/usr/bin/env bash

# The build trace an overlay store goes by is its own if that names an
# output it has, in either layer; else the lower store's if that does; else
# our stale one, or the lower store's if we have none, and then a different
# realisation replaces it. Each row sets up both traces of one derivation,
# then either checks which one counts and realises it, or copies in a
# conflicting realisation first, and checks the outcome.

set -eu -o pipefail

set -x

source common.sh

# Avoid store dir being inside sandbox build-dir
unset NIX_STORE_DIR
unset NIX_STATE_DIR

setupStoreDirs
# A plain store that builds the conflicting outputs
storeC="$storeVolume/store-c"

mountOverlayfs

instantiate () {
    nix-instantiate --store "$1" ./ca-nondet.nix --arg busybox "$busybox" --argstr tag "$2" "${@:3}"
}

# $1: the upper trace: none, valid, lower-only (its output is only in the
#     lower store) or stale
# $2: the lower trace: none, same (as the upper one), valid, upper-only (its
#     output is only in the upper layer) or stale
# $3: what happens first: realise, or copy (a conflicting realisation)
# $4: the trace that counts: upper, lower or none
row () {
    local drv up='' low='' wanted='' before conflicting realised err
    drv=$(instantiate "$storeB" "$1-$2-$3")
    instantiate "$storeA" "$1-$2-$3"
    instantiate "$storeC" "$1-$2-$3"
    if [[ $1 != none ]]; then
        up=$(nix-store --store "$storeB" --realise "$drv")
        if [[ $2 == same ]]; then
            nix copy --from "$storeB" --to "$storeA" --no-check-sigs "$drv^out"
            low=$up
        fi
        case $1 in
            lower-only)
                nix copy --from "$storeB" --to "$storeA" --no-check-sigs "$up"
                nix store delete --store "$storeB&remount-hook=$PWD/remount.sh" "$up" ;;
            stale)
                nix store delete --store "$storeB" "$up"
                [[ $2 != same ]] || nix store delete --store "$storeA" "$up" ;;
        esac
    fi
    case $2 in
        valid|upper-only|stale)
            low=$(nix-store --store "$storeA" --realise "$drv") ;;&
        upper-only)
            nix copy --from "$storeA" --to "$storeC" --no-check-sigs "$low" ;;&
        upper-only|stale)
            nix store delete --store "$storeA" "$low" ;;
    esac
    remountOverlayfs
    [[ $2 != upper-only ]] || nix copy --from "$storeC" --to "$storeB" --no-check-sigs "$low"
    case $4 in
        upper) wanted=$up ;;
        lower) wanted=$low ;;
    esac
    conflicting=$(nix-store --store "$storeC" --realise "$drv")
    if [[ $3 == realise ]]; then
        # The trace that counts, or else a stale one: ours, else theirs.
        # Not before a copy: the query can list lower outputs in the upper
        # database, which would hide what the copy does without them.
        before=${wanted:-${up:-$low}}
        if [[ -n $before ]]; then
            nix store build-trace info --store "$storeB" "$drv^out" | grepQuiet -Fx "$(basename "$drv")^out $before"
        else
            expect 1 nix store build-trace info --store "$storeB" "$drv^out"
        fi
        realised=$(nix-store --store "$storeB" --realise "$drv")
        if [[ -n $wanted ]]; then
            [[ $realised == "$wanted" ]]
        else
            [[ $realised != "$up" && $realised != "$low" ]]
        fi
        wanted=$realised
    fi
    if [[ -n $wanted ]]; then
        err=$(expectStderr 1 nix copy --from "$storeC" --to "$storeB" --no-check-sigs "$drv^out")
        grepQuiet -F "Local:  $wanted" <<< "$err"
        grepQuiet -F "Remote: $conflicting" <<< "$err"
    else
        nix copy --from "$storeC" --to "$storeB" --no-check-sigs "$drv^out"
        wanted=$conflicting
    fi
    [[ $(nix-store --store "$storeB" --realise "$drv") == "$wanted" ]]
    nix store build-trace info --store "$storeB" "$drv^out" | grepQuiet -Fx "$(basename "$drv")^out $wanted"
}

for first in copy realise; do
    row none none "$first" none
    row none valid "$first" lower
    row none upper-only "$first" lower
    row none stale "$first" none
    row valid none "$first" upper
    row valid same "$first" upper
    row valid valid "$first" upper
    row valid upper-only "$first" upper
    row valid stale "$first" upper
    row lower-only none "$first" upper
    row lower-only same "$first" upper
    row lower-only valid "$first" upper
    row lower-only upper-only "$first" upper
    row lower-only stale "$first" upper
    row stale none "$first" none
    row stale same "$first" none
    row stale valid "$first" lower
    row stale upper-only "$first" lower
    row stale stale "$first" none
done

# Rows left by versions that registered without a transaction: the first
# names an output in neither layer, the second one only in the lower store,
# which has no trace. The second counts.
drv=$(instantiate "$storeB" rows)
instantiate "$storeA" rows
instantiate "$storeC" rows
up=$(nix-store --store "$storeB" --realise "$drv")
nix copy --from "$storeB" --to "$storeA" --no-check-sigs "$up"
nix store delete --store "$storeB&remount-hook=$PWD/remount.sh" "$up"
remountOverlayfs
key=$(basename "$drv")
gone=$(dirname "$up")/00000000000000000000000000000000-ca-nondet-rows
sqlite3 "$storeBRoot/nix/var/nix/db/db.sqlite" "update BuildTraceV3 set outputPath = '$gone' where drvPath = '$key';
    insert into BuildTraceV3 (drvPath, outputName, outputPath, signatures) values ('$key', 'out', '$up', '')"
nix-store --store "$storeC" --realise "$drv"
err=$(expectStderr 1 nix copy --from "$storeC" --to "$storeB" --no-check-sigs "$drv^out")
grepQuiet -F "Local:  $up" <<< "$err"

# For an output the upper database has no row for, the lower store's trace
# counts, and registering the same output keeps its signatures.
nix-store --generate-binary-cache-key k1.example.org "$TEST_ROOT/sk1" "$TEST_ROOT/pk1"
nix-store --generate-binary-cache-key k2.example.org "$TEST_ROOT/sk2" "$TEST_ROOT/pk2"
drv=$(instantiate "$storeB" sigs --arg deterministic true)
instantiate "$storeA" sigs --arg deterministic true
instantiate "$storeC" sigs --arg deterministic true
out=$(nix-store --store "$storeA" --realise "$drv" --option secret-key-files "$TEST_ROOT/sk1")
[[ $(nix-store --store "$storeC" --realise "$drv" --option secret-key-files "$TEST_ROOT/sk2") == "$out" ]]
remountOverlayfs
nix copy --from "$storeC" --to "$storeB" --no-check-sigs "$drv^out"
sigs=$(sqlite3 "$storeBRoot/nix/var/nix/db/db.sqlite" "select signatures from BuildTraceV3 where drvPath = '$(basename "$drv")'")
grepQuiet "k1.example.org:" <<< "$sigs"
grepQuiet "k2.example.org:" <<< "$sigs"
