#!/usr/bin/env bash

source common.sh

# Test that using XDG_CONFIG_HOME works
# Assert the config folder didn't exist initially.
[ ! -e "$HOME/.config" ]
# Without XDG_CONFIG_HOME, creates $HOME/.config
unset XDG_CONFIG_HOME
# Run against the nix registry to create the config dir
# (Tip: this relies on removing non-existent entries being a no-op!)
nix registry remove userhome-without-xdg
# Verifies it created it
[ -e "$HOME/.config" ]
# Remove the directory it created
rm -rf "$HOME/.config"
# Run the same test, but with XDG_CONFIG_HOME
export XDG_CONFIG_HOME=$TEST_ROOT/confighome
# Assert the XDG_CONFIG_HOME/nix path does not exist yet.
[ ! -e "$TEST_ROOT/confighome/nix" ]
nix registry remove userhome-with-xdg
# Verifies the confighome path has been created
[ -e "$TEST_ROOT/confighome/nix" ]
# Assert the .config folder hasn't been created.
[ ! -e "$HOME/.config" ]

TODO_NixOS # Very specific test setup not compatible with the NixOS test environment?

# Test that files are loaded from XDG by default
export XDG_CONFIG_HOME=$TEST_ROOT/confighome
export XDG_CONFIG_DIRS=$TEST_ROOT/dir1:$TEST_ROOT/dir2
files=$(nix-build --verbose --version | grep "User config" | cut -d ':' -f2- | xargs)
[[ $files == "$TEST_ROOT/confighome/nix/nix.conf:$TEST_ROOT/dir1/nix/nix.conf:$TEST_ROOT/dir2/nix/nix.conf" ]]

# Test that setting NIX_USER_CONF_FILES overrides all the default user config files
export NIX_USER_CONF_FILES=$TEST_ROOT/file1.conf:$TEST_ROOT/file2.conf
files=$(nix-build --verbose --version | grep "User config" | cut -d ':' -f2- | xargs)
[[ $files == "$TEST_ROOT/file1.conf:$TEST_ROOT/file2.conf" ]]

# Test that it's possible to load the config from a custom location
here=$(readlink -f "$(dirname "${BASH_SOURCE[0]}")")
export NIX_USER_CONF_FILES=$here/config/nix-with-substituters.conf
var=$(nix config show | grep '^substituters =' | cut -d '=' -f 2 | xargs)
[[ $var == https://example.com ]]

# Test that we can include a file.
export NIX_USER_CONF_FILES=$here/config/nix-with-include.conf
var=$(nix config show | grep '^allowed-uris =' | cut -d '=' -f 2 | xargs)
[[ $var == https://github.com/NixOS/nix ]]

# Test that we can !include a file.
export NIX_USER_CONF_FILES=$here/config/nix-with-bang-include.conf
var=$(nix config show | grep '^experimental-features =' | cut -d '=' -f 2 | xargs)
[[ $var == nix-command ]]

# Test that it's possible to load config from the environment
prev=$(nix config show | grep '^cores' | cut -d '=' -f 2 | xargs)
export NIX_CONFIG="cores = 4242"$'\n'"experimental-features = nix-command flakes"
exp_cores=$(nix config show | grep '^cores' | cut -d '=' -f 2 | xargs)
exp_features=$(nix config show | grep '^experimental-features' | cut -d '=' -f 2 | xargs)
[[ $prev != "$exp_cores" ]]
[[ $exp_cores == "4242" ]]
# flakes implies fetch-tree
[[ $exp_features == "fetch-tree flakes nix-command" ]]

# Test that it's possible to retrieve a single setting's value
val=$(nix config show | grep '^warn-dirty' | cut -d '=' -f  2 | xargs)
val2=$(nix config show warn-dirty)
[[ $val == "$val2" ]]

# Test unit prefixes.
[[ $(nix config show --min-free 64K min-free) = 65536 ]]
[[ $(nix config show --min-free 1M min-free) = 1048576 ]]
[[ $(nix config show --min-free 2G min-free) = 2147483648 ]]

# A value that a plain configuration line can't hold survives `nix config show` being read back.
for v in 'a#b' 'x  y' $'a\nb'; do [[ $(NIX_CONFIG=$(nix config show --user-agent-suffix "$v") nix config show user-agent-suffix) = "$v" ]]; done

# NIX_REMOTE_SYSTEMS joins its machines files with ';'.
[[ $(NIX_REMOTE_SYSTEMS=/a:/b nix config show builders) = '@/a;@/b' ]]

# A machines file is found under a NIX_CONF_DIR or a NIX_REMOTE_SYSTEMS path that the builders list can't hold as is,
# and a path the list can hold, or that a user wrote, means what it meant before.
for d in 'h#x' 's;y' $'n\nl' 'p%41' 'p%zz' 'p+s'; do
    cp -r "$NIX_CONF_DIR" "$TEST_ROOT/$d"; m="$TEST_ROOT/$d/machines"; echo 'ssh-ng://from-file fake-sys' > "$m"
    for e in NIX_CONF_DIR="$TEST_ROOT/$d" NIX_REMOTE_SYSTEMS="$m" NIX_CONFIG="builders = @$m"; do
        if [[ $d != p* && $e = NIX_CONFIG=* ]]; then continue; fi
        expectStderr 100 env "$e" nix-build -j0 --no-out-link -E 'derivation { name = "d"; system = "fake-sys"; builder = "/bin/sh"; }' | grepQuiet "ssh-ng://from-file"
    done
    if [[ $d = p* ]]; then [[ $(NIX_CONF_DIR="$TEST_ROOT/$d" nix config show builders) = "@$m" ]]; fi
done
