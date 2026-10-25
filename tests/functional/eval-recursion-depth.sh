#!/usr/bin/env bash

source common.sh

# Recursions that evaluation goes through all the time, each to a depth that
# it must reach in a stack of 8 MiB, a common default limit: a function that
# calls itself, `//`, `?`, `builtins.deepSeq`, and derivations that depend on
# derivations. Each level takes the bytes of the frames on its path, so a
# change that makes one of them much larger fails here.
#
# Each depth leaves room to spare: built with GCC (release, or debugoptimized
# without a unity build; with Boehm GC or without), each program also reaches
# twice its depth, and built with Clang one and a half times. AddressSanitizer
# makes every frame several times larger.

if ASAN_OPTIONS=help=1 nix-instantiate --version 2>&1 | grepQuiet AddressSanitizer; then
    skipTest "AddressSanitizer enlarges every frame"
fi

(ulimit -s 8192) || skipTest "cannot set a stack limit of 8 MiB"

# reaches EXPECTED EXPR: EXPR evaluates to EXPECTED in a stack of 8 MiB.
reaches() {
    local expected=$1 expr=$2 out
    out=$(ulimit -s 8192 && nix-instantiate --eval --strict --readonly-mode --option max-call-depth 100000 -E "$expr")
    [[ $out == "$expected" ]] || fail "expected '$expected', got '$out', from: $expr"
}

# A function's recursion: a call, `if`, `==`, `+` and `-` per level.
reaches 6000 'let f = n: if n == 0 then 0 else 1 + f (n - 1); in f 6000'

# `//`, as the module system nests it.
reaches 1 'let f = n: if n == 0 then { } else { a = n; } // f (n - 1); in builtins.length (builtins.attrNames (f 6000))'

# `?` on the result of the recursion.
reaches true 'let f = n: if n == 0 then true else (if f (n - 1) then { a = 1; } else { }) ? a; in f 6000'

# builtins.deepSeq of nested lists.
reaches 1 'builtins.deepSeq (builtins.foldl'"'"' (acc: _: [ acc ]) [ ] (builtins.genList (x: x) 16000)) 1'

# Derivations, each an input of the next: derivationStrict per level.
reaches 1 'let mk = n: derivation { name = "d"; system = "x"; builder = "/b"; dep = if n == 0 then "" else mk (n - 1); }; in builtins.seq (mk 2000).drvPath 1'
