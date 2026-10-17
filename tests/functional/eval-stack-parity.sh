#!/usr/bin/env bash

# Stack parity with base Nix. Unforcing must not change where Nix overflows
# the native stack: a program that base evaluates, or stops with the
# catchable max-call-depth error, must do the same here, and one that
# overflows in base must overflow here too, with and without `eval-unforce`.
# Larger frames in the evaluator (an inlined hook in `forceValue`, say) or in
# a recursive traversal break the first; smaller ones the second.
#
# Base is not in the test suite, so its edges are pinned
# (eval-stack-parity/edges.txt): for each program
# (eval-stack-parity/programs.tsv), the largest N (chain length, nesting
# depth, ...) at which base (master a9f38ea0e) still behaves as below its
# edge, under `run` below with a 16 MiB stack. Each program runs at
# N = edge - low margin, where it must behave as base, and at
# edge + high margin, where it must overflow as base does. The margins are
# 1 KiB and 2 KiB of stack (and one level): the frames that run once (the
# prefix) may be smaller than base's (by at most the high margin) but not
# larger (by more than the low one).
#
# Frame sizes, and so the edges, depend on the compiler and the build
# configuration, so the test skips unless the build is the pinned one
# (aarch64-linux, 4 KiB pages, GCC 15.2.0, debugoptimized, no LTO, no unity
# build, no sanitizers, no Boehm GC). Other configurations are checked
# against base built the same way, with the same programs.
#
# To re-pin, run this test against base (NIX_CLIENT_PACKAGE=<base, with
# bin/nix and bin/nix-instantiate>) with STACK_PARITY_CALIBRATE=1, which
# writes the edges to eval-stack-parity/edges.txt in the source directory,
# then with STACK_PARITY_WRITE_EXPECTED=1, which writes base's catchable
# errors (eval-stack-parity/*.err.exp).

source common.sh

pinnedConfig="gcc-15.2.0 debugoptimized lto=false unity=off sanitize=none"
[[ "$(uname -sm)" == "Linux aarch64" ]] || skipTest "the stack-parity pins are for aarch64-linux"
[[ "${_NIX_TEST_BUILD_CONFIG:-unknown}" == "$pinnedConfig" ]] \
    || skipTest "the stack-parity pins are for a '$pinnedConfig' build, not '${_NIX_TEST_BUILD_CONFIG:-unknown}'"
# The stack is randomised, and its mapping rounded, by up to a page.
[[ "$(getconf PAGESIZE)" == 4096 ]] || skipTest "the stack-parity pins are for 4 KiB pages"
nix --version -v > "$TEST_ROOT/stack-parity-version" 2>&1 || true
if grep -q '^Features: .*\bgc\b' "$TEST_ROOT/stack-parity-version"; then
    skipTest "the stack-parity pins are for Nix without Boehm GC"
fi

# Soft and hard limit: Nix raises its soft limit to 64 MiB itself when the hard
# one allows, so setting both makes the edges independent of `ulimit -s`.
# 16 MiB: deep enough that one 16-byte slot more per level moves an edge by
# far more than the margins, shallow enough to be quick.
stackKiB=16384
(ulimit -s "$stackKiB") 2> /dev/null || skipTest "cannot set a $stackKiB KiB stack limit"

# Everything the kernel puts on the stack (the program's path, its arguments
# and environment) has the same size for every build and test directory: a
# fixed-length directory, with links to the binaries.
spDir=$(mktemp -d /tmp/nix-stack-parity.XXXXXXXX)
ln -s "$(type -p nix)" "$spDir/nix"
ln -s "$(type -p nix-instantiate)" "$spDir/nix-instantiate"
mkdir "$spDir/h"

# Without address-space randomisation where possible: the edges are then
# exact, and the margins only cover differences in the kernel's stack setup.
# Without it, the stack moves by up to a page.
setarch=()
loMargin=1024 hiMargin=2048
if type -p setarch > /dev/null && setarch -R true 2> /dev/null; then
    setarch=(setarch -R)
else
    loMargin=8192 hiMargin=8192
fi

# run TOOL MODE MAXCALLDEPTH EXPR: evaluate EXPR in a fixed, minimal
# environment; sets `rc`, and leaves the output in $spDir/out and err.
run() {
    local tool=$1 mode=$2 depth=$3 expr=$4 cmd
    case $tool in
        strict) cmd=("$spDir/nix-instantiate" --eval --strict -E "$expr") ;;
        lazy) cmd=("$spDir/nix-instantiate" --eval -E "$expr") ;;
        ijson) cmd=("$spDir/nix-instantiate" --eval --strict --json -E "$expr") ;;
        ixml) cmd=("$spDir/nix-instantiate" --eval --strict --xml -E "$expr") ;;
        eval) cmd=("$spDir/nix" eval --extra-experimental-features nix-command --expr "$expr") ;;
        ejson) cmd=("$spDir/nix" eval --extra-experimental-features nix-command --json --expr "$expr") ;;
    esac
    set +e
    (
        ulimit -c 0
        ulimit -s "$stackKiB"
        exec env -i HOME="$spDir/h" NIX_CONF_DIR="$spDir/h" NIX_STATE_DIR="$spDir/h/s" \
            NIX_CONFIG=$'eval-unforce = '"$mode"$'\nmax-call-depth = '"$depth" \
            "${setarch[@]}" "${cmd[@]}" --store dummy:// > "$spDir/out" 2> "$spDir/err"
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

# The programs: name, tool, max-call-depth, class, class arguments, expression.
programs=()
while IFS= read -r line; do
    [[ -z "$line" || "$line" == "#"* ]] || programs+=("$line")
done < "$_NIX_TEST_SOURCE_DIR/eval-stack-parity/programs.tsv"

declare -A edge
while read -r name e; do
    [[ -z "$name" || "$name" == "#"* ]] || edge[$name]=$e
done < "$_NIX_TEST_SOURCE_DIR/eval-stack-parity/edges.txt"

# inClass N CLASS ARGS...: whether the last run, at N, gave what base gives
# below its edge (see programs.tsv for the classes).
inClass() {
    # Without tracing: the outputs are long.
    local - n=$1 class=$2 want
    set +x
    shift 2
    case $class in
        int) [[ $rc == 0 && $(< "$spDir/out") == "$(($1))" ]] ;;
        nat) [[ $rc == 0 && $(< "$spDir/out") =~ ^[0-9]+$ ]] ;;
        fmt)
            printf -v want "$1" "$(($2))"
            [[ $rc == 0 && $(< "$spDir/out") == "$want" ]]
            ;;
        has)
            printf -v want "$1" "$(($2))"
            [[ $rc == 0 ]] && grep -qF -- "$want" "$spDir/out"
            ;;
        count) [[ $rc == 0 && $(grep -cF -- "$1" "$spDir/out") == "$(($2))" ]] ;;
        rep) [[ $rc == 0 && $(< "$spDir/out") == "$(repeat "$1" "$n")$2$(repeat "$3" "$n")" ]] ;;
        catch) [[ $rc == 1 ]] && grep -q 'error: stack overflow; max-call-depth exceeded' "$spDir/err" ;;
        *) fail "unknown class $class" ;;
    esac
}

# runAt N: run the current program at N, and check its class.
runAt() {
    local n=$1
    run "$tool" "$mode" "${depth//@N@/$n}" "${expr//@N@/$n}"
    inClass "$n" "$class" "${args[@]}"
}

# The bytes of stack, in units of the program's stack use per step.
levels() {
    local per=$((stackKiB * 1024 / edge[$name]))
    echo $((($1 + per - 1) / per))
}

# Programs whose edges this branch inherits below base's from markers
# (eval-stack-parity/inherited.txt: the JSON, XML and `nix eval` printers'
# frames; review identity-owners-v1, N5). They are not checked here until
# markers takes the stack-parity branch's printer fix; every other program
# is. Calibration measures them all.
declare -A inherited
while read -r name _; do
    [[ -z "$name" || "$name" == "#"* ]] || inherited[$name]=1
done < "$_NIX_TEST_SOURCE_DIR/eval-stack-parity/inherited.txt"

calibrated=()
for p in "${programs[@]}"; do
    IFS=$'\t' read -r -a f <<< "$p"
    name=${f[0]} tool=${f[1]} depth=${f[2]} class=${f[3]} expr=${f[-1]}
    if [[ -z "${STACK_PARITY_CALIBRATE:-}" && -n "${inherited[$name]:-}" ]]; then
        echo "$name: inherited from markers, not checked (eval-stack-parity/inherited.txt)"
        continue
    fi
    args=("${f[@]:4:${#f[@]}-5}")
    for mode in true false; do
        if [[ -n "${STACK_PARITY_CALIBRATE:-}" ]]; then
            # Largest N in the class, by bisection around the pin (or 1000).
            guess=${edge[$name]:-1000}
            lo=$((guess / 2)) hi=$((guess * 3 / 2))
            while ! runAt "$lo"; do
                hi=$lo lo=$((lo / 2))
                ((lo > 0)) || fail "calibration: $name not in class at 1"
            done
            while runAt "$hi"; do lo=$hi hi=$((hi * 2)); done
            while ((hi - lo > 1)); do
                mid=$(((lo + hi) / 2))
                if runAt "$mid"; then lo=$mid; else hi=$mid; fi
            done
            echo "edge $name $lo (eval-unforce = $mode)"
            [[ $mode == false ]] || calibrated+=("$name $lo")
            continue
        fi
        [[ -n "${edge[$name]:-}" ]] || fail "$name: no pinned edge"
        # Above: one level more, as a prefix smaller by up to the margin may
        # complete the level base left unfinished.
        lo=$(levels "$loMargin") hi=$(($(levels "$hiMargin") + 1))
        n=$((edge[$name] - lo))
        runAt "$n" \
            || fail "$name (eval-unforce = $mode): base's result at $n (edge ${edge[$name]} - $lo), but rc $rc: $(tail -c 300 "$spDir/err")"
        if [[ $class == catch ]]; then
            # The catchable error is base's, byte for byte (but for base's
            # warning about the setting it does not know).
            grep -v "^warning: unknown setting 'eval-unforce'$" "$spDir/err" > "$spDir/err.cmp" || true
            exp=$_NIX_TEST_SOURCE_DIR/eval-stack-parity/$name.err.exp
            if [[ -n "${STACK_PARITY_WRITE_EXPECTED:-}" ]]; then
                cp "$spDir/err.cmp" "$exp"
            else
                diff -u "$exp" "$spDir/err.cmp" || fail "$name (eval-unforce = $mode): error differs from base's"
            fi
        fi
        # Upper check (base overflows here -> this Nix must too) is the
        # EXACT-EDGE half of the contract. On the PARALLEL line
        # (vibe-coding/unforce-parallel-*) the contract is deliberately relaxed
        # to "never overflow SOONER than single-threaded base": frames <= base
        # on every recursion path, so this evaluator can never reject a program
        # base accepts. Overflowing LATER than base (frames smaller) is then
        # allowed and must NOT fail. The dangerous direction -- overflowing
        # SOONER (frames larger) -- is still caught by the lower check above
        # (`runAt "$n"` at edge-lo fails when this Nix overflows below base's
        # edge). See STACK-PARITY.md: this relaxation is parallel-line-only and
        # must NOT migrate to the shipping branch (vibe-coding/unforce), which
        # keeps strict exact-edge parity. User-approved (parallel inc2a review).
        #
        # STACK_PARITY_EXACT_EDGE=1 restores the strict upper check (used by the
        # teeth self-check to prove a grown/sooner-overflow frame is flagged,
        # and available for the shipping branch).
        n=$((edge[$name] + hi))
        if [[ -n "${STACK_PARITY_EXACT_EDGE:-}" ]]; then
            ! runAt "$n" \
                || fail "$name (eval-unforce = $mode): base overflows at $n (edge ${edge[$name]} + $hi), this Nix does not (exact-edge mode)"
        elif runAt "$n"; then
            echo "$name (eval-unforce = $mode): base overflows at $n, this Nix does not -- allowed (parallel-line: frames <= base, overflows no sooner)"
        fi
    done
done

if [[ -n "${STACK_PARITY_CALIBRATE:-}" ]]; then
    {
        echo "# Base's edges for eval-stack-parity.sh (written by STACK_PARITY_CALIBRATE=1)"
        echo "# base: ${STACK_PARITY_BASE:-?}  build: $pinnedConfig  stack: $stackKiB KiB  setarch: ${setarch[*]:-none}"
        printf '%s\n' "${calibrated[@]}"
    } > "$_NIX_TEST_SOURCE_DIR/eval-stack-parity/edges.txt"
fi

rm -rf "$spDir"
