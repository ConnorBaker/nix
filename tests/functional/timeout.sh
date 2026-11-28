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

# The timeouts do not apply to a builder that is still setting up its
# environment. The build child of `fetched` reads `netrc-file` during its
# setup: here a FIFO that stays open until Nix reports that the other
# build, `silent`, timed out (which it does at once, because another
# derivation needs `silent`). After that `fetched` must still get through
# its setup, which is when Nix says that it builds it.
mkfifo "$TEST_ROOT/netrc"
expectStderr 101 nix-build --no-out-link --keep-going --max-jobs 2 --max-silent-time 2 --option netrc-file "$TEST_ROOT/netrc" -E '
  with import ./config.nix; let
    silent = mkDerivation { name = "silent"; buildCommand = "sleep 1000000000"; };
  in [
    (mkDerivation { name = "dependent"; inherit silent; })
    (import <nix/fetchurl.nix> { name = "fetched"; url = "file:///dev/null"; hash = "sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="; })
  ]' | {
    exec 3<> "$TEST_ROOT/netrc"
    grep -m 1 "timed out after 2 seconds"
    # End the read, whether or not the build child has opened the FIFO yet.
    : > "$TEST_ROOT/netrc.empty"
    mv "$TEST_ROOT/netrc.empty" "$TEST_ROOT/netrc"
    exec 3>&-
    grep "building '.*-fetched.drv'"
}
