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

# Older clients still send the setting; the daemon ignores it. Such a client
# speaks the protocol here, through `nix daemon --stdio`, which forwards its
# standard input and output to the daemon. A daemon of another version can
# still take the setting, so the case runs only against this version's
# daemon. It exits rather than skipping: the cases above ran against
# whichever daemon this is, and their result still counts.
if [[ -n "${NIX_DAEMON_PACKAGE-}" ]] \
    && [[ $("$NIX_DAEMON_PACKAGE/bin/nix" daemon --version) != $(nix daemon --version) ]]; then
    exit 0
fi

u64() {
    local i bytes=
    for ((i = 0; i < 64; i += 8)); do
        bytes+=$(printf '\\x%02x' $(($1 >> i & 255)))
    done
    printf %b "$bytes"
}
str() {
    u64 "${#1}"
    printf %s "$1"
    head -c $((-${#1} & 7)) /dev/zero
}

drv=$(nix-instantiate build-remote-store-setting.nix --arg busybox "$busybox" --argstr name sent)
out=$(nix-store --query --outputs "$drv")

mkfifo "$TEST_ROOT/client"
# The daemon's end of the forwarding is a pipe, which splice(2) needs.
nix daemon --stdio < "$TEST_ROOT/client" | cat > "$TEST_ROOT/client-replies" &
clientPid=$!
exec {client}> "$TEST_ROOT/client"
{
    # Handshake, as protocol version 1.37, which exchanges no features.
    u64 0x6e697863
    u64 $((1 << 8 | 37))
    u64 0
    u64 0
    # SetOptions: keep-failed, keep-going, fallback, verbosity, max-jobs,
    # max-silent-time, an obsolete field, verbose build (as 0), two obsolete
    # fields, cores, substitute, and the overrides.
    u64 19
    u64 0; u64 0; u64 0; u64 0; u64 0
    u64 0; u64 1; u64 0; u64 0; u64 0; u64 1; u64 0
    u64 3
    str builders; str "$TEST_ROOT/machine - - 1 1"
    # As an older client sends it from NIX_CONFIG; a daemon started before
    # NIX_CONFIG was set needs it.
    str sandbox-build-dir; str /nix-build
    str store; str daemon
    # BuildPaths, in the normal mode.
    u64 9
    u64 1; str "$drv!out"
    u64 0
} >&"$client"

for ((i = 0; i < 600; i++)); do
    nix-store --check-validity "$out" 2> /dev/null && break
    sleep 0.1
done
exec {client}>&-
wait "$clientPid"
# The replies' text says where the hook copied the output.
tr -c '[:print:]' '\n' < "$TEST_ROOT/client-replies" | grep -F copying || true
nix-store --check-validity "$out"
nix path-info --store "$TEST_ROOT/machine" "$out"
[[ $(cat "$out") == sent ]]
