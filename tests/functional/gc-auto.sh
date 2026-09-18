#!/usr/bin/env bash

source common.sh

# shellcheck disable=SC1111
needLocalStore "“min-free” and “max-free” are daemon options"

TODO_NixOS

# Three paths of distinct contents, each over the 1948 bytes one collection
# round frees before it stops.  Under the object store a path's files are
# hard links to the store's blobs, and the collector counts a deletion as
# freeing a file's bytes only when the file has one link or two (its own and
# the blob's: src/libutil/unix/file-system.cc, `deletePath`); three copies
# of one script share one blob, so deleting them frees nothing until the
# last, and one round would take all three.
# shellcheck disable=SC2034
garbage1=$(nix store add-path --name garbage1 ./nar-access.sh)
# shellcheck disable=SC2034
garbage2=$(nix store add-path --name garbage2 ./optimise-store.sh)
# shellcheck disable=SC2034
garbage3=$(nix store add-path --name garbage3 ./shell.sh)

ls -l "$garbage3"
POSIXLY_CORRECT=1 du "$garbage3"

fake_free=$TEST_ROOT/fake-free
export _NIX_TEST_FREE_SPACE_FILE=$fake_free
echo 1100 > "$fake_free"

fifoLock=$TEST_ROOT/fifoLock
mkfifo "$fifoLock"

expr=$(cat <<EOF
with import ${config_nix}; mkDerivation {
  name = "gc-A";
  buildCommand = ''
    set -x
    [[ \$(ls \$NIX_STORE/*-garbage? | wc -l) = 3 ]]

    mkdir \$out
    echo foo > \$out/bar

    # Pretend that we run out of space
    echo 100 > ${fake_free}.tmp1
    mv ${fake_free}.tmp1 $fake_free

    # Wait for the GC to run
    for i in {1..20}; do
        echo ''\${i}...
        if [[ \$(ls \$NIX_STORE/*-garbage? | wc -l) = 1 ]]; then
            exit 0
        fi
        sleep 1
    done
    exit 1
  '';
}
EOF
)

expr2=$(cat <<EOF
with import ${config_nix}; mkDerivation {
  name = "gc-B";
  buildCommand = ''
    set -x
    mkdir \$out
    echo foo > \$out/bar

    # Wait for the first build to finish
    cat "$fifoLock"
  '';
}
EOF
)

nix build --impure -v -o "$TEST_ROOT"/result-A -L --expr "$expr" \
    --min-free 1K --max-free 2K --min-free-check-interval 1 &
pid1=$!

nix build --impure -v -o "$TEST_ROOT"/result-B -L --expr "$expr2" \
    --min-free 1K --max-free 2K --min-free-check-interval 1 &
pid2=$!

# Once the first build is done, unblock the second one.
# If the first build fails, we need to postpone the failure to still allow
# the second one to finish
wait "$pid1" || FIRSTBUILDSTATUS=$?
echo "unlock" > "$fifoLock"
( exit "${FIRSTBUILDSTATUS:-0}" )
wait "$pid2"

[[ foo = $(cat "$TEST_ROOT"/result-A/bar) ]]
[[ foo = $(cat "$TEST_ROOT"/result-B/bar) ]]
