#!/usr/bin/env bash

# Adversarial gap #4: every other source test uses a trivial filter
# (`p: type: true`) over synthetic toy trees. The README's HEADLINE workload is
# `mkDerivation { src = lib.cleanSource ./.; }` — a REALISTIC, non-trivial
# predicate (reject .git, editor swap/backup files, .o/.so, result symlinks)
# over a real-shaped source tree. This test runs the ACTUAL nixpkgs
# `cleanSourceFilter` logic (inlined verbatim from nixpkgs lib/sources.nix, so
# no nixpkgs dependency) over a git-backed tree that CONTAINS each junk class,
# and asserts:
#   1. the filter actually drops the junk (the result store path excludes it),
#   2. ours keeps the filtered source CACHEABLE (warm re-eval = no re-copy),
#      where master/DetSys hit the fetch-to-store.cc:41 filter-bypass.
#
# This is the realistic-filter counterpart to filtered-cache.sh's trivial one.

source ../common.sh

requireGit
clearStore
clearCache

count_matches() {
    local pattern=$1 file=$2 n
    n=$(command grep -cE "$pattern" "$file" 2>/dev/null || true)
    echo "${n:-0}"
}

repo=$TEST_ROOT/repo
createGitRepo "$repo"
mkdir -p "$repo/src" "$repo/.svn"
# Real source files (kept):
echo 'int main(){return 0;}' > "$repo/src/main.c"
echo 'header' > "$repo/src/lib.h"
echo 'README' > "$repo/README.md"
# Junk each cleanSourceFilter class drops:
echo 'objfile'   > "$repo/src/main.o"          # .o → dropped
echo 'sharedlib' > "$repo/src/libfoo.so"       # .so → dropped
echo 'backup'    > "$repo/src/main.c~"         # ~ suffix → dropped
echo 'swap'      > "$repo/src/.main.c.swp"     # .*.sw[a-z] → dropped
echo 'svnjunk'   > "$repo/.svn/entries"        # .svn dir → dropped
git -C "$repo" add -A
git -C "$repo" commit -qm init
rev=$(git -C "$repo" rev-parse HEAD)

# The REAL nixpkgs cleanSourceFilter, inlined (lib/sources.nix) — lib.hasSuffix /
# hasPrefix / match expressed with builtins so there's no nixpkgs dependency.
clean_filter='(name: type:
        let baseName = baseNameOf (toString name);
            hasSuffix = suf: s: let ls = builtins.stringLength s; lf = builtins.stringLength suf;
                                 in ls >= lf && builtins.substring (ls - lf) lf s == suf;
            hasPrefix = pre: s: builtins.substring 0 (builtins.stringLength pre) s == pre;
        in !(
             (baseName == ".git" || (type == "directory" &&
               (baseName == ".svn" || baseName == "CVS" || baseName == ".hg")))
          || hasSuffix "~" baseName
          || builtins.match "^\\.sw[a-z]$" baseName != null
          || builtins.match "^\\..*\\.sw[a-z]$" baseName != null
          || hasSuffix ".o" baseName
          || hasSuffix ".so" baseName
          || (type == "symlink" && hasPrefix "result" baseName)
          || (type == "unknown")))'

expr="
let
  t = builtins.fetchGit { url = \"file://$repo\"; rev = \"$rev\"; };
in builtins.path { path = t.outPath; name = \"cleaned\"; filter = $clean_filter; }
"

log1=$TEST_ROOT/eval1.log
out=$(nix eval --impure --raw --expr "$expr" -vvvv 2> "$log1")
echo "cleaned store path: $out"

# (1) The realistic filter dropped every junk class, kept the real files.
test -f "$out/src/main.c"   || fail "cleanSource dropped a real file (src/main.c)"
test -f "$out/README.md"    || fail "cleanSource dropped a real file (README.md)"
test ! -e "$out/src/main.o"   || fail "cleanSource KEPT a .o file"
test ! -e "$out/src/libfoo.so" || fail "cleanSource KEPT a .so file"
test ! -e "$out/src/main.c~"   || fail "cleanSource KEPT an editor backup"
test ! -e "$out/src/.main.c.swp" || fail "cleanSource KEPT an editor swap file"
test ! -e "$out/.svn"          || fail "cleanSource KEPT the .svn dir"
echo "cleansource-idiom: realistic filter dropped all junk classes, kept real files"

# (2) Warm re-eval: ours folds ;shape into the fingerprint, so the filtered
# source is CACHEABLE — the second eval re-copies nothing. (On master/DetSys
# this hits the fetch-to-store filter-bypass and re-copies; this test runs in
# OUR tree, so we assert the cacheable behaviour.)
log2=$TEST_ROOT/eval2.log
out2=$(nix eval --impure --raw --expr "$expr" -vvvv 2> "$log2")
[[ "$out" == "$out2" ]] || fail "warm re-eval produced a different store path: $out vs $out2"

copies2=$(count_matches "copying '" "$log2")
[[ "$copies2" -eq 0 ]] \
    || fail "warm re-eval of a realistic cleanSource re-copied ($copies2 'copying' lines); the filtered source is not cacheable. Log:
$(cat "$log2")"
echo "cleansource-idiom: warm re-eval of the realistic filtered source did 0 copies (cacheable)"

echo "cleansource-idiom: ok"
