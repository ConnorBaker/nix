#!/usr/bin/env bash

source common.sh

clearStoreIfPossible

# The context-free case at two reads (doc/lazy-store/04-derivation.md, section
# 1.2): a pending object's path arrives as a literal -- here learnt from an
# earlier run and deleted, as a stale lock file would supply it -- and is
# dereferenced by a fetcher and by `builtins.exec`.  Both must find the object,
# written before the read, as the reference wrote it on creation.

script='builtins.toFile "script" "echo \"{ a = 1; }\""'

p=$(nix eval --raw --expr "$script")
nix store delete "$p"
[[ ! -e $p ]]

# A path input naming the pending object: written before the fetcher reads it.
out=$(nix eval --impure --raw --expr "let t = $script; in builtins.seq t (builtins.readFile (builtins.fetchTree { type = \"path\"; path = \"$p\"; }).outPath)")
[[ $out == 'echo "{ a = 1; }"' ]]

nix store delete "$p"
[[ ! -e $p ]]

# The pending object as an argument of a program run during evaluation.
out=$(nix eval --option allow-unsafe-native-code-during-evaluation true --expr "let t = $script; in builtins.seq t (builtins.exec [ \"/bin/sh\" \"$p\" ])")
[[ $out == '{ a = 1; }' ]]
