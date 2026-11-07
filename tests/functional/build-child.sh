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

# The build child server refuses a program from another Nix build, after
# it replied with its own version.
printf "nix-build-child 0" > "$TEST_ROOT/other-build"
if (exec -a __build-child nix) <> "$TEST_ROOT/other-build" 2> "$TEST_ROOT/other-build.err"; then
    fail "the build child server ran for another Nix build"
fi
grepQuiet "is from a different Nix build" "$TEST_ROOT/other-build.err"
grepQuiet "^nix-build-child 0nix-build-child 1 " "$TEST_ROOT/other-build"

# The build children are forked by a build child server, a child of Nix
# with argv[0] `__build-child` that has one thread. When it went away
# between two builds of one goal loop, the second still builds.
cat > "$TEST_ROOT/kill-server.sh" <<'EOF2'
ppid() { while read -r key value; do if [[ $key = PPid: ]]; then echo "$value"; fi; done 2> /dev/null < "$1/status"; }
nix=$(ppid /proc/$$)
for p in /proc/[0-9]*; do
    argv0=
    IFS= read -r -d '' argv0 2> /dev/null < "$p/cmdline" || true
    if [[ $argv0 = __build-child && $(ppid "$p") = "$nix" ]]; then server=${p#/proc/}; fi
done
[[ -n ${server-} ]] || { echo "no build child server"; exit 1; }
threads=("/proc/$server/task/"*)
[[ ${#threads[@]} = 1 ]] || { echo "the build child server has ${#threads[@]} threads"; exit 1; }
kill -9 "$server"
# Until it is a zombie (or reaped), which has closed its socket.
while stat=$(< "/proc/$server/stat") && state=${stat##*") "} && [[ ${state%% *} != Z ]]; do sleep 0.01; done
touch "$out"
EOF2
cat > "$TEST_ROOT/kill-server.nix" <<EOF2
with import ${config_nix};
rec {
  kill-server = mkDerivation { name = "kill-server"; builder = $TEST_ROOT/kill-server.sh; };
  after-kill = mkDerivation { name = "after-kill"; buildCommand = "echo \${kill-server} > \$out"; };
}
EOF2
# Without build users: a builder that runs as one cannot kill the server,
# which runs as Nix.
nix-build --no-out-link --option sandbox false --option build-users-group '' "$TEST_ROOT/kill-server.nix" -A after-kill

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
