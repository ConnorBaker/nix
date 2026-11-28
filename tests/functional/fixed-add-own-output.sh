#!/usr/bin/env bash

source common.sh

# A fixed-output derivation's builder adds its own output path to the store,
# which the build has already locked. Adding it must not lock it again: the
# process would wait for itself forever. Concurrent adds must still not
# overwrite each other.

TODO_NixOS

requireDaemonNewerThan "2.37.0pre20261120"

enableFeatures 'recursive-nix dynamic-derivations ca-derivations'
restartDaemon

NIX_BIN_DIR=$(dirname "$(type -p nix)")
export NIX_BIN_DIR

for feature in recursive-nix builder-rpc-v0; do
    # Add, number of files, concurrent adds. Concurrent scanning adds never raced.
    for case in "nar 3000 8" "dump 3000 8" "scan 1 1"; do
        read -r add files par <<< "$case"
        ref=$TEST_ROOT/$feature-$add/own-output
        mkdir -p "$ref"
        printf %s "$feature $add" > "$ref/salt"
        for ((i = 0; i < files; i++)); do echo $i > "$ref/$i"; done
        args=(--file ./fixed-add-own-output.nix --argstr feature "$feature" --argstr add "$add"
              --arg files "$files" --arg par "$par" --argstr hash "$(nix hash path "$ref")")

        out=$(nix eval --raw "${args[@]}" outPath)
        nix build -L --no-link "${args[@]}" &
        pid=$!
        # Instead of hanging, fail once a process waits for the output lock it holds.
        while [[ -e /proc/locks ]] && kill -0 "$pid" 2>/dev/null; do
            sleep 0.1
            ino=$(stat -c %i "$out.lock" 2>/dev/null) || continue
            self=$(awk -v ino="$ino" '
                $2 == "FLOCK" { split($6, a, ":"); if (a[3] == ino) held[$5] }
                $2 == "->" && $3 == "FLOCK" { split($7, a, ":"); if (a[3] == ino) waiting[$6] }
                END { for (p in waiting) if (p in held) print p }' /proc/locks)
            if [[ -n $self ]]; then
                kill -9 "$self" "$pid"
                fail "$feature $add: process $self waits for the lock it holds on $out"
            fi
        done
        wait "$pid"
        nix-store --verify-path "$out"
    done
done
