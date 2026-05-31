#!/usr/bin/env bash

# Track D (filtered-shape cache) end-to-end test.
#
# §6.6 of doc/tecnix-survey/README.md documents the cache bypass
# `fetch-to-store.cc` performed when a `PathFilter` was supplied: the
# filter forced `fingerprint = nullopt`, so neither `sourcePathToHash`
# was consulted nor written, and every cold eval re-walked the source.
#
# The filtered-shape cache (PROPOSAL.md §5) collects the post-filter
# accepted shape, hashes it, and keys a row on
# `(sourceFingerprint, method, subpath, shapeHash)`. Two evaluations
# of the same `builtins.path { path = ./.; filter = ...; }` against
# the same content-addressed source must hit that row on the second
# call.
#
# Verification path:
#   - Evaluate once: cold, walks the filter, writes a row.
#   - Evaluate again: must hit the cache. We assert via the `--debug`
#     output mentioning "filtered source ... cache hit ...".

source ../common.sh

requireGit

clearStore
clearCache

repo=$TEST_ROOT/repo
createGitRepo "$repo"

# Tiny working tree: one file we keep, several we filter out.
echo "kept" > "$repo/keep.txt"
echo "discard a" > "$repo/skip-a.txt"
echo "discard b" > "$repo/skip-b.txt"
mkdir -p "$repo/sub"
echo "discard c" > "$repo/sub/skip-c.txt"

git -C "$repo" add -A
git -C "$repo" commit -m 'initial'
rev=$(git -C "$repo" rev-parse HEAD)

# Build a flake whose default eval produces a filtered store path.
# The filter accepts only top-level `keep.txt` and the directory
# entries needed to reach it (root). All other paths are dropped.
flakeDir=$TEST_ROOT/flake
mkdir -p "$flakeDir"
cat > "$flakeDir/flake.nix" <<EOF
{
  inputs.foo.url = "git+file://$repo?ref=master&rev=$rev";
  inputs.foo.flake = false;
  outputs = { self, foo }: {
    out = builtins.path {
      path = foo.outPath;
      name = "filtered";
      filter = path: type:
        type == "directory" || baseNameOf path == "keep.txt";
    };
  };
}
EOF

# Cold eval: walks the filter and writes a row.
log1=$TEST_ROOT/eval1.log
storePath1=$(nix eval --raw "path:$flakeDir#out" -vvvv 2> "$log1")
echo "first eval store path: $storePath1"

# Post-Item-1 activation, `addPath` registers a SourceView and emits
# a SourceVirtual placeholder; the cache row that drives subsequent
# evals lives in the `sourceContentToNarHash` domain (keyed on
# SourceContentId), not the legacy `filteredSourcePathToHash` domain
# that pre-activation `fetchToStore`'s filtered branch consulted.
# The contract is the same — cold eval walks once, warm eval reuses
# the cached narHash without walking — but the log line shape and
# probe domain differ.

# The cold eval should NOT report any cache hit (the row didn't
# exist yet — we're keying on SourceContentId now).
if command grep -qE "using cache entry 'sourceContentToNarHash" "$log1"; then
    fail "cold eval unexpectedly reported a sourceContentToNarHash cache hit. Log:
$(cat "$log1")"
fi

# But it should report probing the new domain plus walking the
# source. Either "did not find cache entry for 'sourceContentToNarHash:..."
# (when the lookup misses) or the walk-and-copy log line.
command grep -qE "did not find cache entry for 'sourceContentToNarHash|copying '[^']*' to the store" "$log1" \
    || fail "cold eval didn't probe sourceContentToNarHash or copy to store. Log:
$(cat "$log1")"

# Second eval, with `_NIX_TEST_BARF_ON_UNCACHEABLE=1`. The flag fires
# only on the unfingerprinted, unfiltered branch — `barf && !filter`
# in fetch-to-store.cc — so a filtered cache miss will not by itself
# raise it, but a missing source fingerprint would. Together with
# the cache-hit grep below this confirms the cache is doing the
# work, not the bypass.
export _NIX_TEST_BARF_ON_UNCACHEABLE=1

log2=$TEST_ROOT/eval2.log
storePath2=$(nix eval --raw "path:$flakeDir#out" -vvvv 2> "$log2")
[[ "$storePath1" = "$storePath2" ]] \
    || fail "store paths differ between evaluations: $storePath1 vs $storePath2"

# The second eval must report a sourceContentToNarHash cache hit (or
# an in-process narHashByContent_ memo hit, if the same process
# populated it earlier). Either is a successful result for the
# shape-cache contract post-Item-1.
command grep -qE "using cache entry 'sourceContentToNarHash" "$log2" \
    || fail "second eval did not report a sourceContentToNarHash cache hit. Log:
$(cat "$log2")"

# And the second eval should NOT have re-hashed the filtered tree
# from disk. "copied" means a write into the store via NAR walk;
# "hashing" is the dryRun NAR hash. Either would be a regression.
if command grep -qE "copied '[^']*' to '[^']*-filtered'" "$log2"; then
    fail "second eval re-walked the filtered source. Log:
$(cat "$log2")"
fi

echo "filtered-cache: ok"
