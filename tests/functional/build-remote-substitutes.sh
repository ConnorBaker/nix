#!/usr/bin/env bash

# With `builders-use-substitutes`, a machine substitutes a build's inputs
# from its own substituters instead of the client uploading them, whether
# the machine's `nix-daemon --stdio` serves the hook itself or forwards it
# to the machine's daemon, and whether or not that daemon trusts the
# machine's user.

source common.sh

TODO_NixOS

client="file://$TEST_ROOT/client?compression=none"
cache="file://$TEST_ROOT/cache"

# The machine's configuration, which the client does not read.
machineConfDir=$TEST_ROOT/machine-conf
mkdir -p "$machineConfDir"

# `ssh://localhost` and `ssh-ng://localhost` bypass ssh (see
# build-remote.sh), so the machine's programs would read the client's
# environment; these read the machine's configuration instead, as over ssh.
mkdir -p "$TEST_ROOT/machine-bin"
for program in nix-daemon nix-store; do
    printf '#!%s\nunset NIX_CONFIG\nexport NIX_CONF_DIR=%s\nexec %s "$@"\n' \
        "$SHELL" "$machineConfDir" "$(type -P "$program")" > "$TEST_ROOT/machine-bin/$program"
    chmod +x "$TEST_ROOT/machine-bin/$program"
done

# Build, from the client, a derivation whose input `f` only the cache has
# intact: the client's NAR of it is garbage, so uploading it fails the build.
# The machine reads its configuration $3..., and the daemon, if any, is
# restarted to read it too.
build() {
    local proto=$1 name=$2; shift 2
    local program=nix-daemon drv f narInfo
    [[ $proto == ssh ]] && program=nix-store
    drv=$(nix-instantiate --store "$client" build-remote-substitutes.nix --argstr name "$proto-$name")
    f=$(nix-store --store "$client" -q --references "$drv" | grep -- "-substitutes-$proto-$name-f\$")
    nix copy --from "$client" --to "$cache" "$f"
    narInfo=$TEST_ROOT/client/$(basename "$f" | cut -c1-32).narinfo
    echo garbage > "$TEST_ROOT/client/$(sed -n 's/^URL: //p' "$narInfo")"

    cp "$test_nix_conf_dir"/nix.conf* "$machineConfDir/"
    printf '%s\n' "$@" >> "$machineConfDir/nix.conf"
    # Offline, `nix daemon` (which starts the test's daemon) sets
    # `substitute = false` unless the setting is overridden. Before 2.36,
    # its workers are forked and keep that, and ssh-ng:// sends no options
    # to change it, so such a daemon would not substitute for ssh-ng://.
    # The override must come from NIX_CONFIG: `loadConfFile` resets
    # `overridden` after reading the system nix.conf, so `substitute` in
    # the machine's nix.conf would not count. Daemons older than 2.36.0,
    # 2.36.0pre ones included, get it. Since 2.36, workers re-exec and read
    # the configuration again; overriding `substitute` there would hide
    # workers that keep the acceptor's setting.
    local daemonConfig=
    isDaemonNewer 2.36.0 || daemonConfig="substitute = true"
    NIX_CONF_DIR=$machineConfDir NIX_CONFIG=$daemonConfig restartDaemon
    # The machine's store, which the caller uses too.
    machine="$proto://localhost?remote-program=$TEST_ROOT/machine-bin/$program"

    nix-build --store "$client" -j0 --no-out-link "$drv" --builders-use-substitutes \
        --builders "$machine - - 1 1" \
        > "$TEST_ROOT/$proto-$name.log" 2>&1
}

for proto in ssh ssh-ng; do
    # Without a substituter, `f` is uploaded: the garbage fails the build.
    if build "$proto" none; then
        cat "$TEST_ROOT/$proto-none.log"
        fail "$proto: uploading a garbage NAR did not fail the build"
    fi
    grepQuiet "bad archive" "$TEST_ROOT/$proto-none.log"

    # With the machine's own substituter, `f` is substituted, whether or
    # not the daemon trusts the machine's user. Only a daemon can distrust
    # it: `nix-daemon --stdio` serving the hook itself trusts its client.
    # (An untrusted user cannot build an input-addressed derivation over
    # ssh://.)
    trusts=(trusted)
    [[ $proto == ssh-ng && ${NIX_REMOTE-} == daemon ]] && trusts+=(untrusted)
    for trust in "${trusts[@]}"; do
        conf=("substituters = $cache" "require-sigs = false")
        [[ $trust == untrusted ]] && conf+=("trusted-users =")
        build "$proto" "$trust" "${conf[@]}" || {
            cat "$TEST_ROOT/$proto-$trust.log"
            fail "$proto ($trust): the machine did not substitute its input"
        }
        if [[ $proto == ssh-ng ]]; then
            info=$(nix store info --json --store "$machine") \
                || fail "$proto ($trust): cannot ask the machine whether it trusts us"
            [[ $(jq '.trusted' <<< "$info") == "$([[ $trust == trusted ]] && echo true || echo false)" ]] \
                || fail "$proto: the machine does not report that it is $trust: $info"
        fi
    done
done
