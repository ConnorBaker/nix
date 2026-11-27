#!/usr/bin/env bash
# usage: check-trace.sh <strace output of `nix --version`>
set -eu
trace=$1
enabled=/sys/kernel/mm/transparent_hugepage/enabled

# Without reading the setting, mimalloc would not advise either way.
if ! grep -Eq "\"$enabled\", O_RDONLY[^)]*\) = [0-9]+$" "$trace"; then
  echo "mimalloc did not read $enabled" >&2
  cat "$trace" >&2
  exit 1
fi

# mimalloc's first arena is 1 GiB; Nix's own BumpMemoryResource asks for huge
# pages for 8 GiB regions, which this does not match.
if grep -E 'madvise\(0x[0-9a-f]+, 1073741824, MADV_HUGEPAGE\)|prctl\(PR_SET_THP_DISABLE' "$trace"; then
  echo "mimalloc asked for transparent huge pages or disabled them for the process" >&2
  exit 1
fi
