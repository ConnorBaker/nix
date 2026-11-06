#!/usr/bin/env bash

source common.sh

# nix started through the dynamic loader named in its program headers
# (`ld.so /path/to/nix`), where /proc/self/exe is the loader, still finds
# its own program, which the default build hook runs. A copy outside the
# installation tells it apart from the installed nix.
nix=$(type -P nix)
loader=$(head -c 4096 "$nix" | tr '\0' '\n' | grep -a -o -m1 '/[[:alnum:]/._+-]*/ld-[[:alnum:]._+-]*' || true)
[[ -n $loader ]] || skipTest "no dynamic loader named in $nix (a static nix?)"
copy=$TEST_ROOT/bin/nix
mkdir -p "$TEST_ROOT/bin"
cp -L "$nix" "$copy"

# A newline in the path, which /proc/self/maps escapes.
newline=$TEST_ROOT/new$'\n'line
mkdir -p "$newline"
cp -L "$nix" "$newline/nix"
[[ $("$loader" "$newline/nix" config show build-hook) = "$loader $newline/nix __build-remote" ]]

# nix runs itself through the loader too, so its own interpreter need
# not exist: the copy names one that does not, of the same length.
chmod u+w "$copy"
offset=$(grep -abo -F -m1 "$loader" "$copy")
printf %s "${loader%/*}/x${loader##*/l}" | dd of="$copy" bs=1 seek="${offset%%:*}" conv=notrunc status=none
expect 127 "$copy" --version
# Another nix (NIX_BIN_DIR) does not run through this loader.
[[ $(NIX_BIN_DIR=/other "$loader" "$copy" config show build-hook) = "/other/nix __build-remote" ]]

# nix-channel runs nix-env as nix, through runNixBin.
ln -s nix "$TEST_ROOT/bin/nix-channel"
"$loader" "$TEST_ROOT/bin/nix-channel" --remove through-loader

# The daemon's workers run nix too. The daemon cannot be started on
# NixOS, where the tests share the system's, nor the chroot store below.
isTestOnNixOS && skipTest "the daemon and nix shell cases need their own daemon and store"
# Its own socket, apart from the daemon that the suite may run (and stop on
# exit, so the trap does that too), on the same store, the test's own.
socket=$TEST_ROOT/through-loader-socket
NIX_REMOTE=local NIX_DAEMON_SOCKET_PATH=$socket "$loader" "$copy" daemon > "$TEST_ROOT/through-loader-daemon-log" 2>&1 &
daemon=$!
trap 'kill "$daemon" 2>/dev/null || true; killDaemon' EXIT
for _ in $(seq 100); do [[ -S $socket ]] && break; sleep 0.1; done
NIX_REMOTE=unix://$socket nix-build --no-out-link \
    -E "with import ${config_nix}; mkDerivation { name = \"through-loader-daemon\"; buildCommand = \"echo > \$out\"; }"

# nix shell runs its chroot helper as nix, on a store whose logical and
# physical directories differ.
requireSandboxSupport
requiresUnprivilegedUserNamespaces
"$loader" "$copy" shell --sandbox-build-dir /build-tmp --sandbox-paths '/nix? /bin? /lib? /lib64? /usr?' \
    --store "$TEST_ROOT/store0" -f shell-hello.nix hello -c hello | grepQuiet 'Hello World'
