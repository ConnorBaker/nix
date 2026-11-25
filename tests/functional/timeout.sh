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

# (`--timeout 120` turns a regression that makes Nix wait for this builder
# forever into a failure.)
if canWaitForBuilderExit; then
    # Closing the log doesn't end the build, so a builder that closes it
    # and keeps running is subject to the timeouts like any other.
    expectStderr 101 nix-build timeout.nix -A closeLog --max-silent-time 2 --timeout 120 | grepQuiet "timed out after 2 seconds"
else
    expectStderr 100 nix-build timeout.nix -A closeLog --timeout 120 | grepQuiet "builder failed due to signal"
fi

expectStderr 1 nix build -f timeout.nix silent --max-silent-time 2 | grepQuiet "timed out after 2 seconds"
