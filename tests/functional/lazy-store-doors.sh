#!/usr/bin/env bash

source common.sh

clearStoreIfPossible

# Eager text publication must precede context-free reads. The literal is
# learned from an earlier run and deleted, as a stale lock file could supply
# it; neither a fetcher nor exec may see its name before the producer writes.

script='builtins.toFile "script" "echo \"{ a = 1; }\""'

p=$(nix eval --raw --expr "$script")
nix store delete "$p"
[[ ! -e $p ]]

# The text producer precedes the fetcher read.
out=$(nix eval --impure --raw --expr "let t = $script; in builtins.seq t (builtins.readFile (builtins.fetchTree { type = \"path\"; path = \"$p\"; }).outPath)")
[[ $out == 'echo "{ a = 1; }"' ]]

nix store delete "$p"
[[ ! -e $p ]]

# The text producer precedes a program run during evaluation.
out=$(nix eval --option allow-unsafe-native-code-during-evaluation true --expr "let t = $script; in builtins.seq t (builtins.exec [ \"/bin/sh\" \"$p\" ])")
[[ $out == '{ a = 1; }' ]]
