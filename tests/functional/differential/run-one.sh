#!/usr/bin/env bash
#
# Run one functional test the way tests/functional/meson.build does, under a
# mode, and snapshot the store afterwards.
#
# Usage: run-one.sh <mode.env> <run-label> <suite> <script.sh>
#
# A mode file sets:
#   HARNESS_TREE           prepared tree (see prepare-tree.sh)
#   HARNESS_TOOLS_PATH     PATH prefix with GNU tools
#   HARNESS_BASH           the bash that runs the script (>= 4.4)
#   HARNESS_NIX_PACKAGE    nix package root; its bin/ is what the tree's bindir points at
#   HARNESS_CLIENT_PACKAGE optional: a different nix package to put first on PATH (NIX_CLIENT_PACKAGE)
#   HARNESS_NIX_CONFIG     optional: extra settings, applied through NIX_CONFIG after the files
#   HARNESS_DAEMON         optional: 1 runs the suite through a daemon from HARNESS_NIX_PACKAGE
# Output: $HARNESS_TMPDIR/out/<label>/<suite>/<name>/{stdout,stderr,exit,seconds,snap-*.txt};
# HARNESS_TMPDIR must be set to a resolved, short directory.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mode=$1 label=$2 suite=$3 script=$4
# shellcheck disable=SC1090
source "$mode"
# The shebang resolves before the mode is known; macOS ships bash 3.2, which
# cannot run the snapshot library.  Re-execute under the mode's bash.
if (( BASH_VERSINFO[0] < 5 )) && [[ ${HARNESS_REEXEC-} != 1 ]]; then
    HARNESS_REEXEC=1 exec "$HARNESS_BASH" "$0" "$@"
fi
name=${script%.sh}

export HARNESS_DIR=$here
# Short: the daemon socket path must stay under the 104-byte limit on macOS.
export TMPDIR=${HARNESS_TMPDIR:?set HARNESS_TMPDIR to a resolved, short directory}
# Both the tree and TMPDIR must be given as resolved paths: the suite's characterisation
# tests normalise the working directory to `/pwd`, and on macOS `/tmp` resolves to
# `/private/tmp`, so an unresolved path yields `/private/pwd` and every such test fails.
export HARNESS_OUT=$TMPDIR/out/$label/$suite/$name
rm -rf "$HARNESS_OUT"
mkdir -p "$HARNESS_OUT"

# Exactly the environment meson gives a test (tests/functional/meson.build, test()).
export _NIX_TEST_SOURCE_DIR=$HARNESS_TREE/tests/functional
export _NIX_TEST_BUILD_DIR=$HARNESS_TREE/build
export TEST_SUITE_NAME=$suite
export TEST_NAME=$name
export NIX_REMOTE=''
# The suite normally runs inside stdenv (a Nix build or dev shell), which defines
# NIX_STORE and a lowercase `shell`; common/vars.sh tests the former under `set -u`
# and formatter.sh expands the latter.  A plain shell defines neither.
export NIX_STORE=${NIX_STORE-}
export shell=$HARNESS_BASH
# Pin the timestamps git records, so that commit hashes and the lastModified of git
# inputs in lock files are functions of content rather than of the wall clock.  The
# suite only ever compares lastModified against git's own value (flakes/flakes.sh:69).
export GIT_AUTHOR_DATE='@1000000000 +0000' GIT_COMMITTER_DATE='@1000000000 +0000'
export PS4='+(${BASH_SOURCE[0]-$0}:$LINENO) '
export PATH=$HARNESS_TOOLS_PATH:$PATH
if [[ -n "${HARNESS_NIX_CONFIG-}" ]]; then export NIX_CONFIG=$HARNESS_NIX_CONFIG; else unset NIX_CONFIG; fi
if [[ -n "${HARNESS_CLIENT_PACKAGE-}" ]]; then export NIX_CLIENT_PACKAGE=$HARNESS_CLIENT_PACKAGE; else unset NIX_CLIENT_PACKAGE; fi
# Daemon mode as the packaging runs it (tests/functional/package.nix, `test-daemon`): only the
# daemon package is set; common.sh starts the daemon after init.sh has initialised the store
# locally, and only then does the suite switch NIX_REMOTE to the daemon.
if [[ "${HARNESS_DAEMON-}" == 1 ]]; then
    export NIX_DAEMON_PACKAGE=${HARNESS_DAEMON_PACKAGE:-$HARNESS_NIX_PACKAGE}
else
    unset NIX_DAEMON_PACKAGE
fi
unset NIX_REMOTE_
unset HARNESS_EPOCH

# Start from nothing.  common/init.sh wipes TEST_ROOT for the tests that source it,
# but not every test does (derivation-advanced-attributes.sh sources only
# test-root.sh and paths.sh), and the two runs must start from the same state.
mkdir -p "$TMPDIR/nix-test"
test_root=$(realpath "$TMPDIR/nix-test")/$suite/$name
if [[ -e $test_root ]]; then chmod -R u+w "$test_root"; rm -rf "$test_root"; fi

workdir=$_NIX_TEST_SOURCE_DIR
[[ $suite == main ]] || workdir=$_NIX_TEST_SOURCE_DIR/$suite

start=$(date +%s)
set +e
( cd "$workdir" && timeout 300 "$HARNESS_BASH" -x -e -u -o pipefail "$script" > "$HARNESS_OUT/stdout" 2> "$HARNESS_OUT/stderr" )
rc=$?
set -e
echo "$rc" > "$HARNESS_OUT/exit"
echo $(( $(date +%s) - start )) > "$HARNESS_OUT/seconds"

# Final snapshot with the variables the test derived (TEST_ROOT and the store
# locations are functions of TMPDIR, suite and name).  By now any daemon the
# test started has been stopped by its EXIT trap, so this reads the local store.
# shellcheck disable=SC2031 # name and label are this script's own (lines 19, 27); what the subshell sources is meant to stay inside it
( source "$_NIX_TEST_SOURCE_DIR/common/vars.sh" && source "$here/lib.sh" && harness_snapshot final ) \
    || echo "final snapshot failed for $suite/$name" >&2
# shellcheck disable=SC2031 # as above
echo "$label $suite/$name exit=$rc seconds=$(cat "$HARNESS_OUT/seconds")"
