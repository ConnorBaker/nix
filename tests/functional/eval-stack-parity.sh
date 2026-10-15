#!/usr/bin/env bash

# PROTOTYPE (unforce): stack parity with base Nix. Unforcing must not make Nix
# overflow the native stack earlier than base: a program that base evaluates,
# or stops with the catchable max-call-depth error, must do the same here, with
# and without `eval-unforce` (see notes/identity/STACK-PARITY.md in the
# unforce work notes). Larger frames in the evaluator (an inlined hook in
# `forceValue`, say) or in a recursive traversal break this.
#
# Base is not in the test suite, so its edges are pinned below: for each
# program, the largest N (chain length, nesting depth, ...) at which base
# (master a9f38ea0e; GCC 15.2.0 -O2 -g, no GC, aarch64-linux) still succeeds
# or stops with the catchable error, under `run` below with a 16 MiB stack.
# Each program runs at N = edge - margin, where it must behave as base, and at
# edge + margin, where it must overflow as base does (frames smaller than
# base's would also move the edge). The margin (16 KiB of stack) covers stack
# randomisation and the different paths of the binaries.
#
# To re-pin, run this test against base (NIX_CLIENT_PACKAGE=<base, with
# bin/nix and bin/nix-instantiate>) with STACK_PARITY_CALIBRATE=1, which
# prints the edges, then with STACK_PARITY_WRITE_EXPECTED=<file>, which
# writes base's catchable error at the new edge (eval-stack-parity/cedge.err.exp).

source common.sh

[[ "$(uname -sm)" == "Linux aarch64" ]] || skipTest "the stack-parity pins are for aarch64-linux"

# Soft and hard limit: Nix raises its soft limit to 64 MiB itself when the hard
# one allows, so setting both makes the edges independent of `ulimit -s`.
# 16 MiB: deep enough that one 16-byte slot more per level moves an edge by
# far more than the margin, shallow enough to be quick.
stackKiB=16384
(ulimit -s "$stackKiB") 2> /dev/null || skipTest "cannot set a $stackKiB KiB stack limit"

nixBin=$(type -p nix)
nixInstantiate=$(type -p nix-instantiate)
# Without address-space randomisation where possible: the edges are then exact.
setarch=()
if type -p setarch > /dev/null && setarch -R true 2> /dev/null; then
    setarch=(setarch -R)
fi
spHome=$TEST_ROOT/stack-parity
mkdir -p "$spHome"

# run TOOL MODE MAXCALLDEPTH EXPR: evaluate EXPR in a fixed, minimal
# environment; sets `rc`, and leaves the output in $spHome/out and err.
run() {
    local tool=$1 mode=$2 depth=$3 expr=$4 cmd
    case $tool in
        strict) cmd=("$nixInstantiate" --eval --strict -E "$expr") ;;
        lazy) cmd=("$nixInstantiate" --eval -E "$expr") ;;
        ijson) cmd=("$nixInstantiate" --eval --strict --json -E "$expr") ;;
        ixml) cmd=("$nixInstantiate" --eval --strict --xml -E "$expr") ;;
        eval) cmd=("$nixBin" eval --extra-experimental-features nix-command --expr "$expr") ;;
        ejson) cmd=("$nixBin" eval --extra-experimental-features nix-command --json --expr "$expr") ;;
    esac
    set +e
    (
        ulimit -c 0
        ulimit -s "$stackKiB"
        exec env -i HOME="$spHome" NIX_CONF_DIR="$spHome" NIX_STATE_DIR="$spHome/state" \
            NIX_CONFIG=$'eval-unforce = '"$mode"$'\nmax-call-depth = '"$depth" \
            "${setarch[@]}" "${cmd[@]}" --store dummy:// > "$spHome/out" 2> "$spHome/err"
    )
    rc=$?
    set -e
}

repeat() {
    local - s
    set +x
    printf -v s '%*s' "$2" ''
    printf '%s' "${s// /$1}"
}

chain='let chain = n: let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = if i == 0 then 0 else s.${toString (i - 1)} + 1; }) n); in s.${toString (n - 1)}; in chain @N@'
cedge='let chain = n: base: let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = if i == 0 then base else s.${toString (i - 1)} + 1; }) n); in s.${toString (n - 1)}; wrap = n: v: if n == 0 then v else 0 + wrap (n - 1) v; in chain @N@ (wrap 2501 0)'
deepList="builtins.foldl' (acc: _: [ acc ]) [ ] (builtins.genList (x: x) @N@)"
deepAttrs="builtins.foldl' (acc: _: { a = acc; }) { } (builtins.genList (x: x) @N@)"

# name tool max-call-depth expression (@N@ stands for N)
programs=(
    "chain strict 10000 $chain"
    "cedge strict 2500 $cedge"
    "tojson strict 1000000 builtins.stringLength (builtins.toJSON ($deepList))"
    "regex strict 10000 builtins.length (builtins.match \"(a|b)*\" (builtins.concatStringsSep \"\" (builtins.genList (_: \"ab\") @N@)))"
    "recur strict 1000000 let f = n: if n == 0 then 0 else 1 + f (n - 1); in f @N@"
    "foldl strict 1000000 builtins.foldl' (f: x: y: f (y + x)) (y: y) (builtins.genList (x: 1) @N@) 0"
    "deepseq strict 1000000 builtins.deepSeq ($deepList) 1"
    "printi strict 1000000 $deepList"
    "printl lazy 1000000 $deepList"
    "printe eval 1000000 $deepList"
    "printea eval 1000000 $deepAttrs"
    "ijson ijson 1000000 $deepList"
    "ejson ejson 1000000 $deepList"
    "ixml ixml 1000000 $deepList"
)

# Base's edges, and whether this Nix must overflow just above them too (all:
# the per-level frames are base's, and the prefix is at most a few levels
# smaller, well within the margin).
declare -A edge exact
edge[chain]=9612 exact[chain]=1
edge[cedge]=5826 exact[cedge]=1
edge[tojson]=19769 exact[tojson]=1
edge[regex]=15410 exact[regex]=1
edge[recur]=6350 exact[recur]=1
edge[foldl]=5790 exact[foldl]=1
edge[deepseq]=15879 exact[deepseq]=1
edge[printi]=15880 exact[printi]=1
edge[printl]=65511 exact[printl]=1
edge[printe]=52409 exact[printe]=1
edge[printea]=15190 exact[printea]=1
edge[ijson]=19775 exact[ijson]=1
edge[ejson]=19777 exact[ejson]=1
edge[ixml]=24374 exact[ixml]=1

# Whether the result of program NAME at N is what base gives below its edge.
inClass() {
    # Without tracing: the outputs are long.
    local - name=$1 n=$2
    set +x
    case $name in
        cedge)
            [[ $rc == 1 ]] && grep -q 'error: stack overflow; max-call-depth exceeded' "$spHome/err" ;;
        chain) [[ $rc == 0 && $(< "$spHome/out") == "$((n - 1))" ]] ;;
        tojson) [[ $rc == 0 && $(< "$spHome/out") == "$((2 * n + 2))" ]] ;;
        regex | deepseq) [[ $rc == 0 && $(< "$spHome/out") == 1 ]] ;;
        recur | foldl) [[ $rc == 0 && $(< "$spHome/out") == "$n" ]] ;;
        printi | printl | printe) [[ $rc == 0 && $(< "$spHome/out") == "$(repeat '[ ' "$n")[ ]$(repeat ' ]' "$n")" ]] ;;
        printea) [[ $rc == 0 && $(< "$spHome/out") == "$(repeat '{ a = ' "$n"){ }$(repeat '; }' "$n")" ]] ;;
        ijson | ejson) [[ $rc == 0 && $(< "$spHome/out") == "$(repeat '[' "$n")[]$(repeat ']' "$n")" ]] ;;
        ixml) [[ $rc == 0 && $(grep -c '<list>' "$spHome/out") == $((n + 1)) ]] ;;
    esac
}

# 16 KiB of stack, in units of the program's stack use per step.
margin() {
    local per=$((stackKiB * 1024 / edge[$1]))
    echo $(((16384 + per - 1) / per))
}

runProgram() {
    local name=$1 tool=$2 depth=$3 mode=$4 n=$5
    run "$tool" "$mode" "$depth" "${6//@N@/$n}"
}

for p in "${programs[@]}"; do
    read -r name tool depth expr <<< "$p"
    for mode in true false; do
        if [[ -n "${STACK_PARITY_CALIBRATE:-}" ]]; then
            # Largest N in the class, by bisection from the pin.
            lo=$((edge[$name] * 8 / 10)) hi=$((edge[$name] * 5 / 4))
            runProgram "$name" "$tool" "$depth" "$mode" "$lo" "$expr"
            inClass "$name" "$lo" || fail "calibration: $name not in class at $lo"
            runProgram "$name" "$tool" "$depth" "$mode" "$hi" "$expr"
            ! inClass "$name" "$hi" || fail "calibration: $name still in class at $hi"
            while ((hi - lo > 1)); do
                mid=$(((lo + hi) / 2))
                runProgram "$name" "$tool" "$depth" "$mode" "$mid" "$expr"
                if inClass "$name" "$mid"; then lo=$mid; else hi=$mid; fi
            done
            echo "edge[$name]=$lo (eval-unforce = $mode)"
            continue
        fi
        m=$(margin "$name")
        n=$((edge[$name] - m))
        runProgram "$name" "$tool" "$depth" "$mode" "$n" "$expr"
        inClass "$name" "$n" \
            || fail "$name (eval-unforce = $mode): base's result at $n (edge ${edge[$name]} - $m), but rc $rc: $(tail -c 300 "$spHome/err")"
        if [[ $name == cedge ]]; then
            # The catchable error is base's, byte for byte (but for base's
            # warning about the setting it does not know).
            grep -v "^warning: unknown setting 'eval-unforce'$" "$spHome/err" > "$spHome/cedge.err" || true
            if [[ -n "${STACK_PARITY_WRITE_EXPECTED:-}" ]]; then
                cp "$spHome/cedge.err" "$STACK_PARITY_WRITE_EXPECTED"
            else
                diff -u "$_NIX_TEST_SOURCE_DIR/eval-stack-parity/cedge.err.exp" "$spHome/cedge.err" \
                    || fail "cedge (eval-unforce = $mode): error differs from base's"
            fi
        fi
        if ((exact[$name])); then
            n=$((edge[$name] + m))
            runProgram "$name" "$tool" "$depth" "$mode" "$n" "$expr"
            ! inClass "$name" "$n" \
                || fail "$name (eval-unforce = $mode): base overflows at $n (edge ${edge[$name]} + $m), this Nix does not: its frames are smaller than base's"
        fi
    done
done
