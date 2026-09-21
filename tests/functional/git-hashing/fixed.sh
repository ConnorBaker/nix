#!/usr/bin/env bash

source common.sh

# Store layer needs bugfix
requireDaemonNewerThan "2.27pre20250122"

# The git method admits SHA-256 only (doc/lazy-store/01-specification.md,
# section 10, "The git method is SHA-256 only"); the SHA-1 case is
# refused at instantiation.
# simple-sha1.sh holds the other refusals.
expectStderr 1 nix-instantiate ../fixed.nix -A git-sha1 \
    | grepQuiet 'the git content-address method admits SHA-256 only'

if isDaemonNewer "2.31pre20250724"; then
    nix-build ../fixed.nix -A git-sha256 --no-out-link
fi
