#!/usr/bin/env bash

source common.sh

TODO_NixOS
needLocalStore "the builds are scheduled by this process"

# The machine is this test's store, behind a daemon. The builds are asked for
# from another store, which never builds: the store that asks holds the locks
# on the outputs, so it cannot also be the machine.
startDaemon
machine=unix://$NIX_DAEMON_SOCKET_PATH

build() {
    local salt=$1
    shift
    nix build -f build-hook-waiting.nix --no-link --argstr salt "$salt" \
        --store "$TEST_ROOT/client" --max-jobs 0 -vvv "$@" 2>&1
}

# A build that ends frees the build slot of its machine. One of the goals
# that wait for a machine asks again then, and only one. Nothing else lets
# a goal ask here: the poll interval is longer than any test runs. Each build
# says "started" and goes on when it has read a line from a fifo, which we
# write when Nix shows us that. Nix shows it only once every goal that can
# ask the hook has asked, so a build holds the slot while the hook postpones
# the others: at first two of them. Later each of those is woken alone when
# the slot is free, so the hook postpones no build again, and is started
# three times.
mkfifo "$TEST_ROOT/log" "$TEST_ROOT/go"
build wake a b c -L --argstr go "$TEST_ROOT/go" --builders "$machine - - 1" \
    --option build-poll-interval 1000000 > "$TEST_ROOT/log" &
exec 3< "$TEST_ROOT/log" 4<> "$TEST_ROOT/go"
log=''
while read -r line <&3; do
    log+=$line$'\n'
    [[ $line != *"> started" ]] || echo >&4
done
wait "$!"
exec 3<&- 4>&-
[[ $(grep -c "hook reply is 'postpone'" <<< "$log") == 2 ]] || fail "$log"
[[ $(grep -c "starting build hook" <<< "$log") == 3 ]] || fail "$log"
