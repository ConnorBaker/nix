#!/usr/bin/env bash
#
# Run each test under two modes and compare exit codes and every store snapshot.
#
# Usage: differential.sh <modeA.env> <labelA> <modeB.env> <labelB> <suite> <script.sh>...
#
# Verdicts (one line per test, appended to $HARNESS_TMPDIR/out/summary-<A>-vs-<B>-<suite>.tsv):
#   SAME-PASS  identical snapshots and exit 0 on both sides
#   SAME-SKIP  identical, exit 77 (the suite's skip code)
#   SAME-FAIL  identical snapshots and the same non-zero exit on both sides (environment failure)
#   DIFF       a snapshot or the exit code differs; unified diffs are left beside the B-side snapshots
# With A = B = reference, DIFF means the test's reference outcome is nondeterministic.
# HARNESS_TMPDIR must name a resolved, short directory (the daemon socket path
# must stay under the 104-byte limit on macOS); everything is written under it.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
modeA=$1 labelA=$2 modeB=$3 labelB=$4 suite=$5
if (( BASH_VERSINFO[0] < 5 )) && [[ ${HARNESS_REEXEC-} != 1 ]]; then
    # shellcheck disable=SC1090
    source "$modeA"
    HARNESS_REEXEC=1 exec "$HARNESS_BASH" "$0" "$@"
fi
shift 5
TMP=${HARNESS_TMPDIR:?set HARNESS_TMPDIR to a resolved, short directory}
mkdir -p "$TMP/out"
summary=$TMP/out/summary-$labelA-vs-$labelB-$suite.tsv
[[ -e $summary ]] || printf 'suite\ttest\texitA\texitB\tverdict\tsnapshots\tdetail\n' > "$summary"

for script in "$@"; do
    name=${script%.sh}
    "$here/run-one.sh" "$modeA" "$labelA" "$suite" "$script"
    "$here/run-one.sh" "$modeB" "$labelB" "$suite" "$script"
    A=$TMP/out/$labelA/$suite/$name
    B=$TMP/out/$labelB/$suite/$name
    ea=$(cat "$A/exit")
    eb=$(cat "$B/exit")
    verdict=SAME
    detail=""
    labels=$(find "$A" "$B" -maxdepth 1 -name 'snap-*.txt' -exec basename {} \; | LC_ALL=C sort -u)
    n=0
    for l in $labels; do
        n=$((n + 1))
        if [[ ! -e $A/$l || ! -e $B/$l ]]; then
            verdict=DIFF
            detail+="missing:$l "
            continue
        fi
        if ! cmp -s "$A/$l" "$B/$l"; then
            verdict=DIFF
            detail+="differs:$l "
            diff -u "$A/$l" "$B/$l" > "$B/$l.diff" || true
        fi
    done
    if [[ $ea != "$eb" ]]; then
        verdict=DIFF
        detail+="exit:$ea/$eb "
    fi
    if [[ $verdict == SAME ]]; then
        case $ea in
            0) verdict=SAME-PASS ;;
            77) verdict=SAME-SKIP ;;
            *) verdict=SAME-FAIL ;;
        esac
    fi
    for l in $(find "$A" "$B" -maxdepth 1 -name 'snap-*.strict' -exec basename {} \; | LC_ALL=C sort -u); do
        if [[ -e $A/$l && -e $B/$l ]] && ! cmp -s "$A/$l" "$B/$l"; then
            detail+="content-differs:${l%.strict} "
            diff -u "$A/$l" "$B/$l" > "$B/$l.diff" || true
        fi
    done
    cmp -s "$A/stdout" "$B/stdout" || detail+="stdout-differs "
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$suite" "$name" "$ea" "$eb" "$verdict" "$n" "$detail" | tee -a "$summary"
done
