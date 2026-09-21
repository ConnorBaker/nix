#!/usr/bin/env bash
# An isolated store environment for the macOS drivers beside this file (mirrors tests/functional/common/vars.sh);
# sourced, not run.  Parameters: LAZY_STORE_SCRATCH, LAZY_STORE_TREE (its build/src/nix is put on PATH).
# shellcheck shell=bash
# Isolated store environment for the sem experiments (mirrors tests/functional/common/vars.sh).
S=${LAZY_STORE_SCRATCH:?a scratch directory}
B=${LAZY_STORE_TREE:?the worktree}/build
export PATH=$B/src/nix:$PATH
export TEST_ROOT=${TEST_ROOT:-$S/test-root}
export NIX_STORE_DIR=$TEST_ROOT/store
export NIX_LOCALSTATE_DIR=$TEST_ROOT/var
export NIX_LOG_DIR=$TEST_ROOT/var/log/nix
export NIX_STATE_DIR=$TEST_ROOT/var/nix
export NIX_CONF_DIR=$TEST_ROOT/etc
export NIX_DAEMON_SOCKET_PATH=$TEST_ROOT/dSocket
unset NIX_USER_CONF_FILES NIX_PATH XDG_STATE_HOME XDG_DATA_HOME XDG_CONFIG_HOME XDG_CONFIG_DIRS XDG_CACHE_HOME
export _NIX_TEST_NO_SANDBOX=1 _NIX_TEST_NO_LSOF=1 _NIX_TEST_NO_ENVIRONMENT_WARNINGS=1
export NIX_REMOTE=
export HOME=$TEST_ROOT/test-home
export NIX_IGNORE_SYMLINK_STORE=1
freshStore() {
  rm -rf "$TEST_ROOT"; mkdir -p "$NIX_CONF_DIR" "$HOME" "$NIX_STORE_DIR" "$NIX_STATE_DIR"
  cat > "$NIX_CONF_DIR/nix.conf" <<CONF
build-users-group =
keep-derivations = false
sandbox = false
experimental-features = nix-command
substituters =
CONF
}
objects=$NIX_STORE_DIR/.objects
db=$NIX_STATE_DIR/db/db.sqlite
inode() { stat -f %i "$1"; }
nlink() { stat -f %l "$1"; }
blobs() { find "$objects/blobs" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' '; }
blobsx() { find "$objects/blobs-x" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' '; }
trees() { find "$objects/trees" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' '; }
hashColumn() { sqlite3 "$db" "select hash from ValidPaths where path = '$1'"; }
# Plant the schema-10 rendering master's database held for a path (sha256:<base16 NAR hash>).
plantOldRow() { local h; h=$(nix hash path --mode nar --algo sha256 --format base16 "$1"); sqlite3 "$db" "update ValidPaths set hash = 'sha256:$h' where path = '$1'"; }
# Simulate a store an older Nix wrote: objects never entered.  Removing the blob files leaves the
# store paths' files as plain files (link count 1), exactly what an older Nix left.
wipeObjects() { find "$objects/blobs" "$objects/blobs-x" "$objects/trees" -mindepth 1 -delete; }
