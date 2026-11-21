#!/usr/bin/env bash

# Build hooks on one host, of any client, must not upload the same missing
# path to a machine at the same time, which would divide the bandwidth
# between them; anything else they do concurrently. Nor may the machine
# collect a hook's inputs before its build, if its store keeps temporary
# roots, and it substitutes what it can when asked to.

source common.sh

TODO_NixOS

currentLoad=$NIX_STATE_DIR/current-load

# Two clients; `a`'s hooks read NARs that block until we feed them.
clientA="file://$TEST_ROOT/client-a?compression=none"
clientB="file://$TEST_ROOT/client-b?compression=none"

# How long, in seconds, any one wait of this test may take before it fails,
# so that a hook stuck on a lock or a NAR fails the test instead of hanging it.
bound=60

# On failure, show what the hooks were doing; on any exit, stop the builds
# still running, so that none outlives the test.
cleanUp() {
    local status=$?
    if ((status != 0)); then
        echo "/proc/locks:"; cat /proc/locks
        echo "$currentLoad:"; ls -li "$currentLoad" || true
        echo "processes:"
        for cmdline in /proc/[0-9]*/cmdline; do
            echo "${cmdline//[^0-9]/}: $(tr '\0' ' ' < "$cmdline" 2> /dev/null)"
        done
        for log in "$TEST_ROOT"/*.log; do echo "$log:"; cat "$log"; done
    fi
    # The daemon is one of the jobs: stop it first, since `killDaemon`
    # fails on a daemon that is already gone.
    killDaemon
    # shellcheck disable=SC2046
    kill $(jobs -p) 2> /dev/null || true
}
trap cleanUp EXIT

# Restart the test's daemon, if any, which reads the configuration again.
# Starting it sets its own exit trap, so set ours again.
reconfigure() {
    restartDaemon
    trap cleanUp EXIT
}

instantiate() {
    local store=$1 name=$2; shift 2
    nix-instantiate --store "$store" build-remote-upload-lock.nix --argstr name "$name" "$@"
}

inputOf() {
    nix-store --store "$clientA" -q --references "$1" | grep "upload-lock-$2\$"
}

# The NAR file of a path in client `a`.
narOf() {
    local narInfo
    narInfo=$TEST_ROOT/client-a/$(basename "$1" | cut -c1-32).narinfo
    echo "$TEST_ROOT/client-a/$(sed -n 's/^URL: //p' "$narInfo")"
}

# Replace the NAR of a path in client `a` with a FIFO, kept in $1.real.
blockNar() {
    local nar
    nar=$(narOf "$1")
    mv "$nar" "$nar.real"
    mkfifo "$nar"
    echo "$nar"
}

# Let hooks read the NAR behind the FIFO $1, each exactly once. Opening the
# FIFO for reading and writing doesn't block, and lets a hook blocked
# opening it proceed; that hook reads the NAR written once into it, and any
# later open finds the NAR itself.
release() {
    exec 3<> "$1"
    mv "$1.real" "$1"
    cat "$1" >&3
    exec 3>&-
}

# Whether a thread of the build $1 or of its descendants (its hook) is
# blocked opening a FIFO: about to read a NAR that `blockNar` replaced.
# Only these processes count: other tests may block on FIFOs of their own.
fifoOpening() {
    local pid
    while read -r pid; do
        command grep -qsx wait_for_partner /proc/"$pid"/task/*/wchan && return
    done < <(awk -v root="$1" '
        { split(FILENAME, f, "/"); sub(/.*\) /, ""); parent[f[3]] = $2 }
        END {
            ours[root] = 1
            do { more = 0; for (p in parent) if (!(p in ours) && parent[p] in ours) { ours[p] = 1; more = 1 } } while (more)
            for (p in ours) print p
        }' /proc/[0-9]*/stat 2> /dev/null)
    return 1
}

# Whether some process holds (`held`) or waits for (`waited`) a lock on one
# of the files $2..., from /proc/locks; waiters are listed with `->`.
locked() {
    local mode=$1 inodes; shift
    inodes=$(stat -c '%i' -- "$@" 2> /dev/null | tr '\n' ' ' || true)
    awk -v mode="$mode" -v inodes="$inodes" '
        BEGIN { split(inodes, list, " "); for (i in list) want[list[i]] = 1 }
        { waiter = $2 == "->"; n = split($(waiter ? 7 : 6), f, ":") }
        (mode == "waited") == waiter && f[n] in want { found = 1 }
        END { exit !found }' /proc/locks
}

uploadLocked() {
    local files
    mapfile -t files < <(find "$currentLoad" -type f -name '*upload*')
    locked "$1" "${files[@]}"
}

# Build $3 from client $2 in the background, logging to $1.log.
start() {
    local name=$1 store=$2 drv=$3; shift 3
    nix-build --store "$store" -j0 --no-out-link "$drv" --builders "$builders" "$@" \
        > "$TEST_ROOT/$proto-$name.log" 2>&1 &
}

# Wait for the build $2 started as $1 to succeed.
finish() {
    local i
    for ((i = 0; i < 10 * bound; i++)); do
        kill -0 "$2" 2> /dev/null || break
        sleep 0.1
    done
    kill -0 "$2" 2> /dev/null && fail "$proto: $1 did not finish within ${bound}s"
    wait "$2" || fail "$proto: $1 failed"
}

# Start building $3 from client $2 as $1, and wait until its hook is
# uploading: it holds an upload lock, or is about to read a blocked NAR.
startBlocked() {
    local i
    start "$@"
    blockedPid=$!
    for ((i = 0; i < 10 * bound; i++)); do
        kill -0 "$blockedPid" 2> /dev/null || fail "$proto: $1 ended before uploading"
        uploadLocked held && return
        fifoOpening "$blockedPid" && return
        sleep 0.1
    done
    fail "$proto: $1 was not uploading within ${bound}s"
}

# Start building $3 from client $2 as $1, and record in `waited` what it
# waits on before its build ends: an upload lock (`upload`), or the
# machine's lock on the store path $4 (`store`), taken to import it.
startWatched() {
    local i storeLock=${4:+$4.lock}
    start "$1" "$2" "$3"
    watchedPid=$!
    waited=
    for ((i = 0; i < 10 * bound; i++)); do
        kill -0 "$watchedPid" 2> /dev/null || return 0
        if uploadLocked waited; then waited=upload; return; fi
        if [[ -n $storeLock ]] && locked waited "$storeLock"; then waited=store; return; fi
        sleep 0.1
    done
    fail "$proto: $1 neither finished nor waited on a lock within ${bound}s"
}

# Every machine is this test's store; `ssh://localhost` and
# `ssh-ng://localhost` bypass ssh (see build-remote.sh).
for proto in local ssh ssh-ng; do
    if [[ $proto == local ]]; then
        builders="auto - - 2 1"
    else
        builders="$proto://localhost - - 2 1"
    fi

    aDrv=$(instantiate "$clientA" "$proto-a" --arg inputs "[\"$proto-c\"]")
    bDrv=$(instantiate "$clientB" "$proto-b" --arg inputs "[\"$proto-c\"]")
    a2Drv=$(instantiate "$clientA" "$proto-a2" --arg inputs "[\"$proto-d\"]")
    b2Drv=$(instantiate "$clientB" "$proto-b2" --arg inputs "[\"$proto-d\"]")
    a3Drv=$(instantiate "$clientA" "$proto-a3" --arg inputs "[\"$proto-e\"]")
    a4Drv=$(instantiate "$clientA" "$proto-a4" --arg inputs "[\"$proto-f\"]")

    # `c` and `e` are already on the machine.
    nix copy --from "$clientA" --no-check-sigs "$(inputOf "$aDrv" "$proto-c")" "$(inputOf "$a3Drv" "$proto-e")"

    # 1. While `a` uploads its own input, `b` (sharing only `c`) is not held up.
    aNar=$(blockNar "$(inputOf "$aDrv" "$proto-a.sh")")
    startBlocked a "$clientA" "$aDrv"
    startWatched b "$clientB" "$bDrv"
    [[ -z $waited ]] || fail "$proto: b waited (on a lock: $waited) while a uploaded other paths"
    finish b "$watchedPid"
    release "$aNar"
    finish a "$blockedPid"

    # 2. While `a2` uploads `d`, `b2` waits to upload it too, rather than
    # sending it as well, then finds it there.
    d=$(inputOf "$a2Drv" "$proto-d")
    dNar=$(blockNar "$d")
    startBlocked a2 "$clientA" "$a2Drv"
    startWatched b2 "$clientB" "$b2Drv" "$d"
    [[ $waited != store ]] || fail "$proto: b2 uploaded d while a2 was uploading it"
    [[ $waited == upload ]] || fail "$proto: b2 did not wait for a2's upload of d"
    release "$dNar"
    finish a2 "$blockedPid"
    finish b2 "$watchedPid"

    # 3. Once a hook has found `e` on the machine, a garbage collection there
    # keeps it until the build, if the machine's store keeps temporary roots:
    # a daemon does so only from 2.36 (`featureAddTempRoots`). Upstream
    # 2.36.0pre builds between eb1602c861 (2026-07-06) and the merge
    # 94b9bd1e12 (2026-09-17) lack it too.
    if [[ $NIX_REMOTE != daemon ]] || isDaemonNewer 2.36.0pre; then
        e=$(inputOf "$a3Drv" "$proto-e")
        a3Nar=$(blockNar "$(inputOf "$a3Drv" "$proto-a3.sh")")
        startBlocked a3 "$clientA" "$a3Drv"
        timeout "$bound" nix-store --gc || fail "$proto: the collection did not finish within ${bound}s"
        nix path-info "$e" || fail "$proto: the machine collected the input $e of a running hook"
        release "$a3Nar"
        finish a3 "$blockedPid"
    fi

    # 4. With builders-use-substitutes, the machine substitutes `f` instead of
    # it being uploaded: client `a`'s NAR of it is garbage. Only the ssh://
    # and ssh-ng:// machines substitute when asked which paths they have. The
    # substituter is in the configuration that the machine's store (perhaps
    # the test's daemon) reads itself. A daemon from before 2.36.0 (2.36.0pre
    # ones included) is started with `substitute = true` in NIX_CONFIG, where
    # it overrides: offline, its acceptor turns substitution off, and before
    # it re-executed itself for each connection its workers kept that.
    if [[ $proto != local ]]; then
        f=$(inputOf "$a4Drv" "$proto-f")
        nix copy --from "$clientA" --to "file://$TEST_ROOT/cache-$proto" "$f"
        echo garbage > "$(narOf "$f")"
        cp "$test_nix_conf" "$TEST_ROOT/nix.conf.saved"
        printf 'substituters = file://%s\nrequire-sigs = false\n' "$TEST_ROOT/cache-$proto" >> "$test_nix_conf"
        daemonConfig=
        isDaemonNewer 2.36.0 || daemonConfig="substitute = true"
        NIX_CONFIG=$daemonConfig reconfigure
        start a4 "$clientA" "$a4Drv" --builders-use-substitutes
        finish "a4 (whose input $f the machine can substitute)" "$!"
        nix path-info "$f"
        cp "$TEST_ROOT/nix.conf.saved" "$test_nix_conf"
        reconfigure
    fi
done

# Upload lock files go away with their uploads.
[[ -z $(find "$currentLoad" -name '*.upload.lock') ]]
