#!/usr/bin/env bash

source common.sh

# nix started through the dynamic loader named in its program headers
# (`ld.so /path/to/nix`), where /proc/self/exe is the loader, still finds
# its own program, which the default build hook runs. A copy outside the
# installation tells it apart from the installed nix.
nix=$(type -P nix)
loader=$(head -c 4096 "$nix" | tr '\0' '\n' | grep -a -o -m1 '/[[:alnum:]/._+-]*/ld[-6][[:alnum:]._+-]*' || true)
[[ -n $loader ]] || skipTest "no dynamic loader named in $nix (a static nix?)"

# A newline in the path, which /proc/self/maps escapes.
newline=$TEST_ROOT/new$'\n'line
mkdir -p "$newline"
cp -L "$nix" "$newline/nix"
[[ $("$loader" "$newline/nix" config show build-hook) = "$newline/nix __build-remote" ]]
# A build runs the hook, which declines, having no remote builders. The
# daemon on NixOS would run its own hook instead.
if ! isTestOnNixOS; then
    NIX_REMOTE=local "$loader" "$newline/nix" build --debug --no-link --builders '' --impure \
        --expr "with import ${config_nix}; mkDerivation { name = \"through-loader-hook\"; buildCommand = \"echo > \$out\"; }" 2>&1 \
        | grepQuiet "hook reply is 'decline-permanently'"
fi
