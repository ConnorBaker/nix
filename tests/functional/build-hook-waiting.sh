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

# How many hooks Nix had started when it first logged a line that matches $1.
startedBefore() {
    sed "/$1/q" <<< "$2" | grep -c "starting build hook"
}

# Nix does not wait while a hook connects to its machine: it starts the hook
# of the second build before the first hook has connected and accepted.
log=$(build unix a b --builders "$machine - - 2") || fail "$log"
[[ $(startedBefore "hook reply is 'accept'" "$log") == 2 ]] || fail "$log"

# The same with the derivations in an evaluation store only. The hook reads
# its derivation from the store that asks for the build, so Nix copies it
# there once the hook has accepted, also when the goal waited for that hook
# as for a child. Without the copy the hook finds no derivation and the
# build fails.
log=$(build evalstore a b --eval-store "$TEST_ROOT/eval" --builders "$machine - - 2") || fail "$log"
[[ $(startedBefore "hook reply is 'accept'" "$log") == 2 ]] || fail "$log"

# A hook that may connect to an ssh machine never says that it is connecting,
# also not to another machine that it tries first: an ssh server refuses
# connections while too many are not yet authenticated. Nix waits for such a
# hook as before, and its build has started when the next hook is asked.
log=$(build ssh a b --builders "unix://$TEST_ROOT/no-machine - - 2; ssh-ng://localhost - - 2") || fail "$log"
[[ $(startedBefore "^building '.*' on " "$log") == 1 ]] || fail "$log"

# Two machines that cannot be connected to. Both hooks try both at the same
# time and say before each that they are connecting; a goal ignores that from
# the hook it holds. They give their builds back, so `a` and `b` are built
# here. The worker keeps a hook that has given up on the machines: asked
# about `both`, it tries nothing.
log=$(NIX_REMOTE='' nix build -f build-hook-waiting.nix --no-link --argstr salt refuse both --max-jobs 2 \
    --builders "unix://$TEST_ROOT/no-machine - - 2; unix://$TEST_ROOT/no-machine2 - - 2" 2>&1) || fail "$log"
[[ $(grep -c "cannot build on" <<< "$log") == 4 ]] || fail "$log"

# A hook of one's own that says that it is connecting. While we wait for it,
# it takes none of our build slots: of `local` and `local2`, which are built
# here in the only one, one has to wait for the slot, not both. What the
# hook says then, we read in the pieces that arrive: a line longer than one
# piece is whole, and what follows the name of the machine is not lost. (The
# hook builds nothing, so `a` fails.)
cat > "$TEST_ROOT/hook" << EOF
#!/bin/sh
printf '@nix {"action":"connecting"}\n$(printf %05000d 0)\n# accept\nsome-machine\n@nix {"action":"msg","level":0,"msg":"said after the name"}\n' >&2
cat > /dev/null
EOF
chmod +x "$TEST_ROOT/hook"
log=$(NIX_REMOTE='' expectStderr 1 nix build -f build-hook-waiting.nix --no-link --argstr salt hook a local local2 \
    --max-jobs 1 -vvv --option build-hook "$TEST_ROOT/hook")
[[ $(grep -c "\.drv': wait for build slot" <<< "$log") == 1 ]] || fail "$log"
grepQuiet -x "0\{5000\}" <<< "$log" || fail "$log"
grepQuiet "said after the name" <<< "$log" || fail "$log"

# A hook that dies while it is connecting is an error, as one that dies
# before it has answered is. (Something still reads what Nix writes to it:
# a hook that is gone before Nix asks is declined instead.)
cat > "$TEST_ROOT/hook" << EOF
#!/bin/sh
exec 3<&0
cat <&3 > /dev/null 2>&1 &
echo '@nix {"action":"connecting"}' >&2
EOF
log=$(expectStderr 1 build dies a --option build-hook "$TEST_ROOT/hook")
grepQuiet "unexpected EOF reading a line" <<< "$log" || fail "$log"
