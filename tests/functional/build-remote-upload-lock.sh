#!/usr/bin/env bash

source common.sh

requireSandboxSupport
requiresUnprivilegedUserNamespaces
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"
command -v flock > /dev/null || skipTest "no flock"

# Avoid store dir being inside sandbox build-dir
unset NIX_STORE_DIR

file="build-remote-upload-lock.nix"
cp "$test_nix_conf" "$TEST_ROOT/nix.conf.orig"

evalAttr () {
    nix eval --raw -f "$file" --store "$localStore" \
      --arg busybox "$busybox" --argstr inputSalt "$1" --argstr salt "$2" "$3"
}

buildRemote () {
    nix build -vv -f "$file" --no-link --max-jobs 0 \
      --arg busybox "$busybox" --argstr inputSalt "$1" --argstr salt "$2" \
      --store "$localStore" --builders "$builders"
}

# Hold the upload lock, as another hook uploading to the same machine would.
holdLock () {
    local locked=$TEST_ROOT/locked-$RANDOM-$RANDOM
    (
        exec 9> "$1"
        flock -x 9
        touch "$locked"
        exec sleep 600
    ) > /dev/null 2>&1 &
    holder=$!
    for _ in {1..100}; do
        [[ -e "$locked" ]] && break
        sleep 0.1
    done
    [[ -e "$locked" ]]
}

# Note: ssh{-ng}://localhost bypasses ssh. See build-remote.sh.
for proto in local ssh ssh-ng; do
    localStore=$TEST_ROOT/$proto/local
    remoteStore=$TEST_ROOT/$proto/remote
    if [[ $proto = local ]]; then
        builders="$remoteStore - - 4 1"
    else
        builders="$proto://localhost?remote-store=$remoteStore - - 4 1"
    fi
    cp "$TEST_ROOT/nix.conf.orig" "$test_nix_conf"

    # The first build has to upload its inputs, which creates the upload lock.
    buildRemote a a
    nix path-info --store "$remoteStore" "$(evalAttr a a input)"
    lockFile=$(find "$localStore" -name '*.upload-lock')
    [[ -f "$lockFile" ]]

    holdLock "$lockFile"

    # All inputs are already valid on the remote, so the build must not wait
    # for the upload lock.
    timeout 60 nix build -f "$file" --no-link --max-jobs 0 \
      --arg busybox "$busybox" --argstr inputSalt a --argstr salt b \
      --store "$localStore" --builders "$builders"
    nix path-info --store "$localStore" "$(evalAttr a b outPath)"

    # A build with a missing input must still wait for the upload lock.
    buildRemote c c 2> "$TEST_ROOT/log-$proto-c" &
    build=$!
    for _ in {1..300}; do
        grepQuiet "waiting for the upload lock" "$TEST_ROOT/log-$proto-c" && break
        sleep 0.1
    done
    grepQuiet "waiting for the upload lock" "$TEST_ROOT/log-$proto-c"
    # Give a hook that ignored the lock time to upload and build.
    sleep 1
    expect 1 nix path-info --store "$remoteStore" "$(evalAttr c c input)"
    expect 1 nix path-info --store "$localStore" "$(evalAttr c c outPath)"

    # Releasing the lock lets the upload and the build proceed.
    kill "$holder"
    wait "$build"
    nix path-info --store "$remoteStore" "$(evalAttr c c input)"
    nix path-info --store "$localStore" "$(evalAttr c c outPath)"

    # With builders-use-substitutes, an input the remote can substitute is not
    # uploaded, so the build does not wait for the upload lock either. Only
    # the ssh:// and ssh-ng:// remotes substitute when queried.
    [[ $proto = local ]] && continue
    cache=$TEST_ROOT/$proto/cache
    nix copy --from "$localStore" --to "file://$cache" "$(evalAttr d d input)"
    cat >> "$test_nix_conf" << EOF
substituters = file://$cache
require-sigs = false
builders-use-substitutes = true
EOF
    holdLock "$lockFile"
    timeout 60 nix build -f "$file" --no-link --max-jobs 0 \
      --arg busybox "$busybox" --argstr inputSalt d --argstr salt d \
      --store "$localStore" --builders "$builders"
    nix path-info --store "$remoteStore" "$(evalAttr d d input)"
    nix path-info --store "$localStore" "$(evalAttr d d outPath)"
    kill "$holder"
done
