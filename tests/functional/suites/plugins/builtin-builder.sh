# shellcheck shell=bash

# A plugin's builtin builder runs where the plugin is loaded, also when Nix
# was started through the dynamic loader.

needLocalStore "the plugin is loaded by the process that builds"
plugin=$(findPlugin libplugintest)
drv() { echo "derivation { name = \"$1\"; system = builtins.currentSystem; builder = \"builtin:plugintest\"; }"; }
[[ $(cat "$(nix-build --option plugin-files "$plugin" --no-out-link -E "$(drv plugintest)")") = from-plugin ]]
loader=$(head -c 4096 "$(type -P nix)" | tr '\0' '\n' | grep -a -o -m1 '/[[:alnum:]/._+-]*/ld-[[:alnum:]._+-]*' || true)
[[ -z $loader ]] || [[ $(cat "$("$loader" "$(type -P nix-build)" --option plugin-files "$plugin" --no-out-link -E "$(drv through-loader)")") = from-plugin ]]
