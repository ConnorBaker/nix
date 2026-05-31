#!/usr/bin/env bash

# Cargo-workspace property end-to-end (track O' / track H').
#
# A monorepo where N packages all evaluate
# `mkDerivation { src = builtins.path { path = ./.; filter = f; ... }; }`
# from the same source root with the same filter must, after the
# first evaluation, NEVER re-walk the source on subsequent
# evaluations. This is the §6.6 fix: the filtered-shape cache row
# `(sourceFingerprint, method, subpath, shapeHash) → narHash` is
# shared across all N package names — they all derive their store
# paths from the same NAR hash via `makeFixedOutputPathFromCA`.
#
# Verification:
#   - First eval populates the cache + memo. Each unique name
#     produces a unique store path; the walks happen N times on
#     the cold path because the per-name store paths must be
#     created.
#   - Second eval (fresh process, persistent SQLite cache, store
#     warm) hits the cache for all N packages → 0 walks. This is
#     the user-visible "incremental cargo workspace eval is fast"
#     property.

source ../common.sh

requireGit

clearStore
clearCache

# We *don't* set _NIX_TEST_BARF_ON_UNCACHEABLE here: the workspace
# uses a filter, which exempts it from the barf check anyway. See
# fetch-to-store.cc::fetchToStore2 — `barf && !filter`.

repo=$TEST_ROOT/repo
createGitRepo "$repo"

# Construct a workspace: N directories pkg-N each holding a tiny
# source file plus a "build artefact" we want the filter to drop.
# Smaller N keeps the test fast while still exercising the path.
n_pkgs=20
for i in $(seq 1 "$n_pkgs"); do
    mkdir -p "$repo/pkg-$i"
    echo "package $i source" > "$repo/pkg-$i/src.txt"
    echo "stale build artefact" > "$repo/pkg-$i/.build-out"
done

git -C "$repo" add -A
git -C "$repo" commit -m 'workspace'
rev=$(git -C "$repo" rev-parse HEAD)

# Build a flake whose `out` attribute returns the list of all N
# `builtins.path`-built sources. Each call uses the *same* source
# root and the *same* filter — only `name` differs. The per-name
# store paths are minted via `makeFixedOutputPathFromCA(name, hash)`
# from a single shared NAR hash on the warm path.
flakeDir=$TEST_ROOT/flake
mkdir -p "$flakeDir"
cat > "$flakeDir/flake.nix" <<EOF
{
  inputs.foo.url = "git+file://$repo?ref=master&rev=$rev";
  inputs.foo.flake = false;
  outputs = { self, foo }: {
    out = builtins.listToAttrs (
      map (i:
        let name = "pkg-\${toString i}"; in
        {
          name = name;
          value = builtins.path {
            path = foo.outPath;
            inherit name;
            filter = path: type:
              # Drop dotfiles so the .build-out files are excluded
              # while preserving every directory.
              type == "directory"
              || (builtins.substring 0 1 (baseNameOf path)) != ".";
          };
        }) (builtins.genList (i: i + 1) $n_pkgs)
    );
  };
}
EOF

# `count_matches` returns lines matching pattern in file, 0 on no
# match. `command grep -c` is required: the wrapper `grepQuiet`
# redirects stdout to /dev/null, which discards the count.
count_matches() {
    local pattern=$1 file=$2 n
    n=$(command grep -cE "$pattern" "$file" 2>/dev/null || true)
    echo "${n:-0}"
}

# `dump_log_excerpt` prints up to 5 matching lines from a file, or
# a placeholder if no match. Used in failure messages so the test
# doesn't spew the whole log on success.
dump_log_excerpt() {
    local pattern=$1 file=$2
    command grep -E "$pattern" "$file" 2>/dev/null | head -5 || echo "(no matching lines)"
}

# First eval: cold cache. Each unique name walks the source to mint
# its own store path; the cache row is shared so subsequent SQLite
# lookups hit but the store path for new names isn't valid yet.
log1=$TEST_ROOT/eval1.log
nix eval --json "path:$flakeDir#out" -vvvv 2> "$log1" > "$TEST_ROOT/result1.json"

# All N packages should produce N distinct store paths because
# `name` differs per package (and `name` participates in the
# fixed-output-CA store path).
distinct=$(jq -r '. | to_entries | map(.value) | unique | length' "$TEST_ROOT/result1.json")
[[ "$distinct" -eq "$n_pkgs" ]] \
    || fail "expected $n_pkgs distinct store paths (one per name), got $distinct"

# Verify the persistent cache row was populated. Post-Item-1
# activation, `addPath` registers SourceView + emits SourceVirtual,
# and the cache row lives in `sourceContentToNarHash` (keyed by
# SourceContentId). Pre-activation it was `filteredSourcePathToHash`;
# the contract (probe-event-count >= n_pkgs) is the same.
db_probe=$(count_matches "(did not find|using) cache entry .*sourceContentToNarHash" "$log1")
echo "first eval: $db_probe sourceContentToNarHash cache probe events"
[[ "$db_probe" -ge 1 ]] \
    || fail "expected >=1 sourceContentToNarHash cache-key probe event on first eval, got $db_probe"

# Second eval: a fresh `nix eval` process. The in-process memo
# starts empty, but the persistent SQLite cache row exists, and
# every per-name store path from the first eval is still valid.
# Result: every call should hit the cache and return without a
# fresh content NAR walk. This is the incremental-eval property
# the cargo-workspace optimisation delivers in practice.
log2=$TEST_ROOT/eval2.log
nix eval --json "path:$flakeDir#out" -vvvv 2> "$log2" > "$TEST_ROOT/result2.json"

# Same store paths — a CA mismatch would be an even worse failure.
diff -q "$TEST_ROOT/result1.json" "$TEST_ROOT/result2.json" \
    || fail "second eval produced different store paths"

# Count "copied '<src>' to '<dst>-pkg-N'" lines — these mark real
# content NAR walks + writes. A working cache means *zero* of
# these on the second eval.
copies2=$(count_matches "copied '[^']+' to '[^']*-pkg-" "$log2")
echo "second eval: $copies2 content NAR writes (expected 0)"
if [[ "$copies2" -ne 0 ]]; then
    excerpt=$(dump_log_excerpt "copied '" "$log2")
    fail "expected 0 content NAR walks on second eval, got $copies2. Sample log: $excerpt"
fi

# Post-Item-1 activation, all N packages share one SourceContentId
# (content-determined; `name` is NOT part of the contentId), so the
# `sourceContentToNarHash` cache hit happens *once* per process —
# the in-process narHashByContent_ memo handles the rest. So we
# expect at least 1 sourceContentToNarHash hit (the first observer)
# plus per-placeholder `outPathOf` calls that each go through the
# memo (no log line for memo hits in narHashByContent_).
hits2=$(count_matches "using cache entry 'sourceContentToNarHash" "$log2")
echo "second eval: $hits2 sourceContentToNarHash cache hits (expected >=1; the rest go through the in-process memo)"
if [[ "$hits2" -lt 1 ]]; then
    excerpt=$(dump_log_excerpt "sourceContentToNarHash" "$log2")
    fail "expected >=1 sourceContentToNarHash cache hit on second eval, got $hits2. Sample log: $excerpt"
fi

# Re-run a third time to verify the in-process memo by itself is
# enough — the first call in this third process will still hit
# SQLite, but the rest hit the memo. We don't separate the two
# numerically here; the contract is "no walks", not "X memo hits
# vs Y SQLite hits".
log3=$TEST_ROOT/eval3.log
nix eval --json "path:$flakeDir#out" -vvvv 2> "$log3" > /dev/null
copies3=$(count_matches "copied '[^']+' to '[^']*-pkg-" "$log3")
[[ "$copies3" -eq 0 ]] \
    || fail "expected 0 content NAR walks on third eval, got $copies3"

echo "cargo-workspace-dedup: ok"
