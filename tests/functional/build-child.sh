#!/usr/bin/env bash

source common.sh

# A local build's child process is a fresh exec of Nix.

needLocalStore "the build child is started by the process that builds"
# (A daemon ignores hashed-mirrors from a user it does not trust.)
if isTestOnNixOS && ! isTrustedByAmbientStore; then skipTest "Can't run through the daemon"; fi

# The settings reach it: builtin:fetchurl finds this file only through
# hashed-mirrors (a store path, so that a sandbox shows it too).
echo mirrored > "$TEST_ROOT/mirrored"
hash=$(nix-hash --flat --type sha256 "$TEST_ROOT/mirrored")
mkdir -p "$TEST_ROOT/mirror/sha256" && mv "$TEST_ROOT/mirrored" "$TEST_ROOT/mirror/sha256/$hash"
mirror=$(nix-store --add "$TEST_ROOT/mirror")
nix-build --no-out-link --option hashed-mirrors "file://$mirror" -E "derivation {
    name = \"mirrored\"; system = builtins.currentSystem; builder = \"builtin:fetchurl\"; url = \"file://$TEST_ROOT/missing\";
    outputHashMode = \"flat\"; outputHashAlgo = \"sha256\"; outputHash = \"$hash\"; mirror = builtins.storePath \"$mirror\"; }"

# Nix started through a copy of the dynamic loader, because the one the
# programs name is missing (masked here), runs the helper through it too.
# A builtin builder, so that nothing else needs the interpreter.
loader=$(head -c 4096 "$(type -P nix)" | tr '\0' '\n' | grep -a -o -m1 '/[[:alnum:]/._+-]*/ld-[[:alnum:]._+-]*' || true)
[[ -n $loader ]] || skipTest "no dynamic loader named in nix (a static build?)"
cp "$loader" "$TEST_ROOT/ld.so"
echo through-loader > "$TEST_ROOT/fetched"
cat > "$TEST_ROOT/through-loader.sh" <<EOF
mount --bind /dev/null $loader
exec $TEST_ROOT/ld.so $(type -P nix-build) --no-out-link --option build-hook '' --option sandbox false -E 'derivation {
    name = "through-loader"; system = builtins.currentSystem; builder = "builtin:fetchurl"; url = "file://$TEST_ROOT/fetched";
    outputHashMode = "flat"; outputHashAlgo = "sha256"; outputHash = "$(nix-hash --flat --type sha256 "$TEST_ROOT/fetched")"; }'
EOF
# (As the root of a user namespace, Nix could not make a read-only store
# writable.)
userns=(--map-root-user)
if [[ $(id -u) = 0 ]]; then userns=(); else requiresUnprivilegedUserNamespaces; fi
unshare --mount "${userns[@]}" "$SHELL" "$TEST_ROOT/through-loader.sh"
