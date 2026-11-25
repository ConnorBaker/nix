#!/usr/bin/env bash

# Test the `--timeout' option.

source common.sh

# XXX: This shouldn’t be, but #4813 cause this test to fail
needLocalStore "see #4813"

# FIXME: https://github.com/NixOS/nix/issues/4813
expectStderr 101 nix-build -Q timeout.nix -A infiniteLoop --timeout 2 | grepQuiet "timed out" \
    || skipTest "Do not block CI until fixed"

expectStderr 1 nix-build -Q timeout.nix -A infiniteLoop --max-build-log-size 100 | grepQuiet "killed after writing more than 100 bytes of log output"

expectStderr 101 nix-build timeout.nix -A silent --max-silent-time 2 | grepQuiet "timed out after 2 seconds"

# On Linux, closing its log doesn't end the build; the builder's exit does.
if [[ $(uname) == Linux ]]; then
    # Without pidfd_open() (Linux < 5.3), the builder is still killed when it closes its log.
    ! command -v enosys > /dev/null || expectStderr 100 enosys --syscall pidfd_open -- nix-build --no-out-link timeout.nix -A closeLogThenExit --timeout 10 | grepQuiet "signal 9"
    nix-build --no-out-link timeout.nix -A closeLogThenExit
    out=$(expectStderr 100 nix-build --no-out-link timeout.nix -A leavesProcesses)
    grepQuiet "late$" <<< "$out"
    sleep 5
    [[ ! -e "$TEST_ROOT/marker" ]]
    expectStderr 101 nix-build timeout.nix -A closeLog --max-silent-time 2 | grepQuiet "timed out after 2 seconds"
    expectStderr 101 nix-build timeout.nix -A closeLog --timeout 2 | grepQuiet "timed out after 2 seconds"
else
    expectStderr 100 nix-build timeout.nix -A closeLog | grepQuiet "builder failed due to signal"
fi

expectStderr 1 nix build -f timeout.nix silent --max-silent-time 2 | grepQuiet "timed out after 2 seconds"
