#!/usr/bin/env bash

# Lazy narHash thunk end-to-end (Item 2) + defer-past-mount (§6.1.1).
#
# Part A (LazyAttr coalescing, pre-existing): when a flake input is
# referenced only via `outPath`, the lazy `narHash` thunk is never
# forced; when a sibling reads `narHash`, the value resolves correctly.
#
# Part B (defer-past-mount, this change): for an UNLOCKED input read
# metadata-only (`.rev` / `.lastModified` / the `.outPath` *string*,
# but NOT file bytes and NOT as a derivation src), `mountInput` no
# longer forces the dryRun NAR walk at mount time, so the walk count
# is EXACTLY 0. The sibling that reads bytes / uses the input as a
# derivation src still materialises correctly (the fake mount is
# devirtualised to the real CA path at the demand boundary), and an
# explicit-but-wrong narHash still throws on materialisation.
#
# Walk metric: the dryRun NAR walk logs `hashing '<path>'` at -vvvv
# (`src/libfetchers/fetch-to-store.cc`); a Copy logs `copying ... to
# the store` instead, so this grep counts *only* dryRun walks.

source ../common.sh

requireGit

clearStore
clearCache

repo=$TEST_ROOT/repo
createGitRepo "$repo"
echo "content A" > "$repo/file.txt"
git -C "$repo" add -A
git -C "$repo" commit -m initial
rev=$(git -C "$repo" rev-parse HEAD)

###############################################################################
# Part A — LazyAttr coalescing (pre-existing contract).
###############################################################################

# Flake whose only consumer of `inputs.foo` reads `outPath`.
mkdir -p "$TEST_ROOT/flake-outpath-only"
cat > "$TEST_ROOT/flake-outpath-only/flake.nix" <<EOF
{
  inputs.foo.url = "git+file://$repo?ref=master&rev=$rev";
  inputs.foo.flake = false;
  outputs = { self, foo }: { out = builtins.readFile "\${foo.outPath}/file.txt"; };
}
EOF

log1=$TEST_ROOT/eval1.log
nix eval --json "path:$TEST_ROOT/flake-outpath-only#out" -vvvv 2> "$log1" > /dev/null

# This consumer reads bytes (readFile), so it materialises; the
# meaningful bound is that the lazy narHash thunk is not *additionally*
# forced. At most one dryRun walk for the input.
walks=$(grep -c "hashing '.*foo'" "$log1" || true)
[[ "$walks" -le 1 ]] || { echo "log:" >&2; cat "$log1" >&2; fail "expected at most 1 walk for outpath-only consumer, got $walks"; }

# Sibling flake that *does* read narHash.
mkdir -p "$TEST_ROOT/flake-narhash"
cat > "$TEST_ROOT/flake-narhash/flake.nix" <<EOF
{
  inputs.foo.url = "git+file://$repo?ref=master&rev=$rev";
  inputs.foo.flake = false;
  outputs = { self, foo }: { out = foo.narHash; };
}
EOF

log2=$TEST_ROOT/eval2.log
nix eval --json "path:$TEST_ROOT/flake-narhash#out" -vvvv 2> "$log2" > "$TEST_ROOT/result2.json"

narHash=$(cat "$TEST_ROOT/result2.json")
[[ "$narHash" =~ ^\"sha256-.+\"$ ]] || fail "expected SRI sha256 narHash, got '$narHash'"

###############################################################################
# Part B — defer-past-mount for an UNLOCKED input (the 0-walk axis).
#
# An unlocked input cannot be reached in *pure* eval (it is rejected
# before `mountInput`), so the beneficiary is `--impure`. We use a
# `builtins.fetchTree` of a git working tree WITHOUT a pinned `rev`
# (hence unlocked); the fetcher resolves HEAD's rev into the locked
# input, so `.rev` / `.lastModified` are available *without* a walk.
# No flake input is involved, so no lockfile is written (a lockfile
# write would force the narHash and defeat the test).
###############################################################################

clearStore
clearCache

# ISOLATION (load-bearing for non-vacuity): Part A above fetched `$repo`
# with a `rev`-pinned flake input, warming the persistent fetcher cache
# (`sourcePathToHash`, under $HOME/.cache/nix — NOT removed by
# `clearCache`, which only wipes the binary-cache dir). If Part B reused
# `$repo`, a *forced* `mountInput` would find that warm row and skip the
# dryRun walk too — making the 0-walk assertion pass whether or not the
# defer fires (a vacuous test). So Part B uses a SEPARATE repo with
# UNIQUE content that nothing has fetched, guaranteeing a cold cache: the
# only way to reach 0 walks is the defer actually firing. Verified
# non-vacuous: forcing the slow path (`deferStorePath=false` in
# `paths.cc`) makes this metadata-only read walk once (`metaWalks==1`).
repoB=$TEST_ROOT/repoB
createGitRepo "$repoB"
echo "content B unlocked $(date +%s%N)" > "$repoB/file.txt"   # unique → cold cache
git -C "$repoB" add -A
git -C "$repoB" commit -m init >/dev/null 2>&1
revB=$(git -C "$repoB" rev-parse HEAD)

# Metadata-only: read .rev and .lastModified, and force the .outPath
# *string* (discarding its store context via `stringLength`, so the
# CLI's display-time `ensureLazyPathsCopied` is never handed an Opaque
# element — i.e. we observe the path value without demanding its
# bytes). EXACTLY 0 dryRun walks.
metaExpr="let t = builtins.fetchTree { type = \"git\"; url = \"file://$repoB\"; }; in { inherit (t) rev lastModified; outPathLen = builtins.stringLength t.outPath; }"

logMeta=$TEST_ROOT/eval-meta.log
nix eval --impure --json --expr "$metaExpr" -vvvv 2> "$logMeta" > "$TEST_ROOT/meta.json"

# The headline assertion: zero walks for metadata-only.
metaWalks=$(grep -c "hashing '" "$logMeta" || true)
[[ "$metaWalks" -eq 0 ]] || { echo "meta log:" >&2; cat "$logMeta" >&2; fail "defer-past-mount: expected EXACTLY 0 walks for unlocked metadata-only read, got $metaWalks"; }

# The resolved rev must be HEAD's rev (proves .rev came from the locked
# input, not a placeholder), and outPath must be a non-empty store path.
metaRev=$(jq -r .rev "$TEST_ROOT/meta.json")
[[ "$metaRev" == "$revB" ]] || fail "expected resolved rev '$revB', got '$metaRev'"
metaOutPathLen=$(jq -r .outPathLen "$TEST_ROOT/meta.json")
[[ "$metaOutPathLen" -gt 0 ]] || fail "expected non-empty outPath string, got length '$metaOutPathLen'"

# Sibling that DOES read bytes: must materialise correctly. The fake
# mount is devirtualised to the real CA path on demand; bytes are the
# committed file's bytes. (This walks — that's expected.)
bytesExpr="builtins.readFile ((builtins.fetchTree { type = \"git\"; url = \"file://$repoB\"; }) + \"/file.txt\")"
bytes=$(nix eval --impure --raw --expr "$bytesExpr")
[[ "$bytes" == "content B unlocked"* ]] || fail "expected file bytes starting 'content B unlocked', got '$bytes'"

# Sibling that uses the input as a derivation src: the deferred fake
# path must be rewritten to the real CA path in inputSrcs + the
# builder env (otherwise the build references a non-existent path).
# We don't run the build; instantiating is enough to exercise the
# derivationStrictInternal devirtualisation. Assert the instantiated
# drv's input source is a *valid* store path (the real CA path).
drvExpr="derivation { name = \"uses-src\"; system = builtins.currentSystem; builder = \"/bin/sh\"; args = [ \"-c\" \"echo hi > \$out\" ]; src = (builtins.fetchTree { type = \"git\"; url = \"file://$repoB\"; }); }"
drvPath=$(nix eval --impure --raw --expr "($drvExpr).drvPath")
# Every input source of the drv must be a valid store path (no fake
# stand-in). `nix derivation show` emits
# `{version, derivations: {<drvPath>: {inputs: {srcs: [...]}}}}` and the
# srcs are base names (`<hash>-<name>`), so prepend the store dir.
storeDir=$(nix eval --impure --raw --expr "builtins.storeDir")
nix derivation show "$drvPath" > "$TEST_ROOT/drv.json"
srcs=$(jq -r '.derivations[].inputs.srcs[]' "$TEST_ROOT/drv.json")
[[ -n "$srcs" ]] || { cat "$TEST_ROOT/drv.json" >&2; fail "drv has no input srcs (expected the fetchTree src)"; }
for src in $srcs; do
    nix path-info "$storeDir/$src" > /dev/null 2>&1 || fail "drv inputSrc '$src' is not a valid store path (fake deferred-mount path leaked into the derivation)"
done

# narHash mismatch still throws: an unlocked fetchTree with an explicit
# but WRONG narHash takes the slow path with an expected hash; the
# mismatch must fire when the bytes are demanded (materialisation).
wrongHash="sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
mismatchExpr="builtins.readFile ((builtins.fetchTree { type = \"git\"; url = \"file://$repo\"; narHash = \"$wrongHash\"; }) + \"/file.txt\")"
if nix eval --impure --raw --expr "$mismatchExpr" 2> "$TEST_ROOT/mismatch.log"; then
    fail "expected narHash mismatch to throw, but it succeeded"
fi
grep -qi "NAR hash mismatch\|hash mismatch" "$TEST_ROOT/mismatch.log" || { cat "$TEST_ROOT/mismatch.log" >&2; fail "expected a NAR hash mismatch error"; }

echo "lazy-narhash: ok"
