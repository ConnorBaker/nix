#!/usr/bin/env bash

source common.sh

# `nix build` evaluates nothing once its installables are resolved, so it
# must not keep the values evaluation left reachable through its build wait.

[[ $(uname) == Linux ]] || skipTest "reads the client's resident memory from /proc"

clearStore

# nix's own messages from a build's stderr, without the collector's log.
nixMessages() {
    grep -v -E '^(GC|-->|In-use|Grow|Initiating|World|Finalize|Complete|Immediately|Adding|[0-9]+ final)' "$1" || true
}

# Print the resident memory (KiB) of `nix build` while it waits on a build of
# an expression that keeps a list of `size` strings reachable.
rssDuringBuildWait() {
    local size=$1 dir="$TEST_ROOT/release-$1" pid rss i
    mkdir -p "$dir"
    # Not GC_PRINT_STATS: the collector's logging overwrites the stack it
    # scans and so hides the retention this test is for.
    RELEASE_SIZE=$size RELEASE_DIR=$dir \
        nix build --no-link --impure -f ./build-release-evaluator.nix drv 2> "$dir/stderr" &
    pid=$!
    # Well inside the harness's own 300 s limit for a test.
    for ((i = 0; i < 2400; i++)); do
        [[ -e "$dir/started" ]] && break
        kill -0 "$pid" 2> /dev/null || break
        sleep 0.1
    done
    # The failure paths stop the build themselves: an EXIT trap here would
    # replace the one that stops the test daemon.
    if [[ ! -e "$dir/started" ]]; then
        kill "$pid" 2> /dev/null || true
        nixMessages "$dir/stderr" >&2
        fail "the build of size $size never started"
    fi
    if ! rss=$(awk '/^VmRSS:/ { print $2 }' "/proc/$pid/status"); then
        kill "$pid" 2> /dev/null || true
        fail "could not read the resident memory of nix build"
    fi
    cp "/proc/$pid/smaps" "$dir/smaps"
    touch "$dir/go"
    if ! wait "$pid"; then
        nixMessages "$dir/stderr" >&2
        fail "nix build of size $size failed"
    fi
    echo "$rss"
}

small=$(rssDuringBuildWait 1)
# Strings and a list, all reachable when evaluation ends.
large=$(rssDuringBuildWait 3000000)
echo "resident during the build wait: small $small KiB, large $large KiB"

# Measured on x86_64-linux: a client that keeps the evaluator holds about
# 414 MiB more than the small case; one that lets it go, about 31 MiB.
if ! (( large < small + 100 * 1024 )); then
    dir="$TEST_ROOT/release-3000000"
    echo "largest resident mappings (KiB, name):"
    # awk, unlike head, reads all its input, so sort never sees SIGPIPE.
    awk '/^[0-9a-f]+-[0-9a-f]+ / { name = $6 } /^Rss:/ { print $2, name }' "$dir/smaps" | sort -rn | awk 'NR <= 5'
    fail "nix build kept $(( (large - small) / 1024 )) MiB of evaluation through its build wait"
fi
