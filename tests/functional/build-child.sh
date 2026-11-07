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

# Settings that need an experimental feature are applied after it is
# enabled ("sandbox setup:" is the build child's output, at --debug).
out=$(nix-build --no-out-link --debug --extra-experimental-features auto-allocate-uids -E "
    with import ${config_nix}; mkDerivation { name = \"xp-settings\"; buildCommand = \"echo > \$out\"; }" 2>&1)
[[ $out != *"sandbox setup: warning: Ignoring setting"* ]] || { echo "$out" >&2; fail "the build child ignored a setting"; }

# Only argv[0] makes Nix a build child.
expectStderr 1 nix __build-child < /dev/null | grepQuiet "'__build-child' is not a recognised command"

# The build child refuses what a program from another Nix build sends.
str() { printf "\\x$(printf %02x ${#1})\\0\\0\\0\\0\\0\\0\\0%s" "$1"; head -c $(( (8 - ${#1} % 8) % 8 )) /dev/zero; }
str "nix-build-child 0" > "$TEST_ROOT/other-build"
if (exec -a __build-child nix) < "$TEST_ROOT/other-build" 2> "$TEST_ROOT/other-build.err"; then
    fail "the build child ran for another Nix build"
fi
grepQuiet -a "is from a different Nix build" "$TEST_ROOT/other-build.err"

# Nix started through a copy of the dynamic loader, because the one the
# programs name is missing (masked here), runs its build child through it
# too.
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
