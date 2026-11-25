#!/usr/bin/env bash

# A build ends once the builder has exited and its log has been closed,
# not as soon as the log is closed.

source common.sh

clearStoreIfPossible

# Every build here takes a few seconds at most; the timeout turns a
# regression that makes Nix wait forever into a failure.
build() {
    nix-build --no-out-link --timeout 120 "$@"
}

# A builder that closes stdout and stderr and then keeps working is not
# killed, and its own exit status is what counts. Without a way to wait
# for the builder's exit, it is killed when it closes its log instead.
if canWaitForBuilderExit; then
    build builder-exit.nix -A closesLogThenSucceeds

    # (The exit code of a failed build depends on whether a daemon is in between.)
    if out=$(build builder-exit.nix -A closesLogThenFails 2>&1); then
        fail "closesLogThenFails should have failed"
    fi
    grepQuiet "builder failed with exit code 3" <<< "$out"
else
    if out=$(build builder-exit.nix -A closesLogThenSucceeds 2>&1); then
        fail "closesLogThenSucceeds should have been killed"
    fi
    grepQuiet "builder failed due to signal" <<< "$out"
fi

# On NixOS the ambient daemon sandboxes the build: the builder can't
# write to $TEST_ROOT, its process IDs mean nothing out here, and the
# processes it leaves behind die with it (it is PID 1 of its namespace).
if ! isTestOnNixOS; then
    # Processes the builder leaves behind are still killed once it exits.
    build builder-exit.nix -A leavesChild
    leftover=$(< "$TEST_ROOT/leftover.pid")
    for ((i = 0; ; i++)); do
        if [[ -e /proc/$leftover/stat ]]; then
            # The process may linger as an unreaped zombie of whoever
            # inherited it; that's fine.
            read -r _ _ state _ < "/proc/$leftover/stat" || break
            [[ $state != Z ]] || break
        else
            kill -0 "$leftover" 2> /dev/null || break
        fi
        if ((i == 100)); then
            fail "process $leftover left behind by the builder is still running"
        fi
        sleep 0.1
    done

    # But one that still holds the log keeps the build going until it
    # closes it: the builder's exit alone doesn't end the build.
    drvPath=$(nix-instantiate builder-exit.nix -A exitsBeforeLogCloses)
    build "$drvPath"
    grepQuiet '^written after the builder exited' <<< "$(nix-store --read-log "$drvPath")" \
        || fail "lost the log written after the builder exited"
fi

# The log of a builder that writes a lot and then exits is kept whole.
drvPath=$(nix-instantiate builder-exit.nix -A fastLogThenFails)
if out=$(build "$drvPath" 2>&1); then
    fail "fastLogThenFails should have failed"
fi
grepQuiet "last line" <<< "$out"
log=$(nix-store --read-log "$drvPath")
[[ $(grep -c '^line ' <<< "$log") == 10000 ]]
grepQuiet '^last line' <<< "$log"
