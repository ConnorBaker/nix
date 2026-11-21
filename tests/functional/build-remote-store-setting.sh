#!/usr/bin/env bash

# A client's `store` setting names the store that the client uses, not the
# daemon's. A daemon that took it as its own passed it on to its build hook,
# which then imported the outputs of a remote build through another
# connection to the daemon, rather than into the store whose output locks
# the build held while waiting for the hook; that import waited for those
# locks forever.

source common.sh

TODO_NixOS
requireSandboxSupport
requiresUnprivilegedUserNamespaces
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

# The machine's store is a diverted store, whose builds run in the sandbox;
# keep the sandbox's build directory away from the store directory, which
# is in $TEST_ROOT. The setting is in the environment: set in the
# configuration file instead, it did not reach the build hook of a 2.35
# daemon.
export NIX_CONFIG='sandbox-build-dir = /nix-build'

startDaemon

# A build takes well under a second; one that waits for its lock fails.
build() {
    timeout 60 nix build -L --no-link --print-out-paths --max-jobs 0 \
        -f build-remote-store-setting.nix --arg busybox "$busybox" --argstr name "$1" \
        --builders "$TEST_ROOT/machine - - 1 1" "${@:2}"
}

# The client uses the daemon each time; the last two name it, in two
# spellings.
for name in default daemon socket; do
    case $name in
        default) out=$(build "$name") ;;
        daemon) out=$(build "$name" --store daemon) ;;
        socket) out=$(build "$name" --store "unix://$NIX_DAEMON_SOCKET_PATH") ;;
    esac
    # Built on the machine, and imported into the daemon's store.
    nix path-info --store "$TEST_ROOT/machine" "$out"
    [[ $(cat "$out") == "$name" ]]
done
