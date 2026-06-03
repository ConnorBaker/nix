#!/usr/bin/env bash

# Increment 6 at SCALE — multi-source elision (the monorepo / "chain of large
# source repos with sparse nix expressions" shape, the case large companies
# have internally). Like source-copy-elision.sh but with N fingerprintable
# `builtins.path` sources on ONE substitutable target, so it exercises:
#   (a) the multi-source path that source-copy-elision.sh (1 source) does not —
#       this is the regression test for the SourceContentId subpath-collision
#       bug (two subtrees of one flake tree shared a contentId → the 2nd source
#       resolved to a path whose bytes were never written → "path … is not
#       valid"; fixed in primops.cc by folding the subpath into the contentId);
#   (b) the source-elision WIN at scale: a substitutable target never
#       materialises its `.drv`, so ALL N deferred source copies are dropped
#       unwritten — eager copies all N (wasted, the output substitutes anyway).
#
# Also prints an eager-vs-lazy measurement (source copies + wall time). Tune
# with MULTI_N / MULTI_SZ env vars; defaults are gate-friendly.
#
# Non-vacuity: eager (lazy off) DOES copy all N — asserted below.

source ../common.sh

requireGit
needLocalStore "“--no-require-sigs” can’t be used with the daemon"
enableFeatures "flakes"

clearStore
clearCache

cacheDir="$TEST_ROOT/cache"
flake="$TEST_ROOT/flk"
N="${MULTI_N:-8}"
SZ="${MULTI_SZ:-8000000}" # bytes per source; 8 MB default

createGitRepo "$flake"
for k in $(seq 0 $((N - 1))); do
    mkdir -p "$flake/proj$k"
    head -c "$SZ" /dev/urandom > "$flake/proj$k/payload"
done

# Sparse nix layer: one target consuming N flake-relative `builtins.path`
# sources (each its own "repo" subtree). `${src k}` as trailing args makes each
# a (deferred) inputSrc without the builder needing an external tool.
cat > "$flake/flake.nix" <<EOF
{
  outputs = { self }: let
    src = k: builtins.path { path = ./. + ("/proj" + toString k); name = "mark" + toString k; };
    srcs = builtins.genList (k: "\${src k}") $N;
  in {
    packages.SYSTEM.default = derivation {
      name = "multisrc-target";
      system = "SYSTEM";
      builder = "/bin/sh";
      args = [ "-c" "echo elision-built > \$out" ] ++ srcs;
    };
  };
}
EOF
sed -i "s|SYSTEM|$system|g" "$flake/flake.nix"
git -C "$flake" add flake.nix
for k in $(seq 0 $((N - 1))); do git -C "$flake" add "proj$k"; done
git -C "$flake" commit -m init

flakeref="git+file://$flake#packages.$system.default"

# 1. Build from source; push the OUTPUT to a binary cache. The sources are not
#    runtime dependencies of the output, so the cached output does not drag them.
out=$(nix build --no-link --print-out-paths --option eval-cache false "$flakeref")
nix copy --to "file://$cacheDir" "$out"

# 2. Substitute-build (output comes from the cache; `-j0` forbids local builds)
#    eager vs lazy; count source copies (`*-markN`) and wall time each.
# Count materialised source copies (`*-markN`). A `for`-glob (not `ls … | wc`):
# safe under `set -e -o pipefail` when ZERO match (the elision case) — an
# unmatched glob fed to `ls` fails the pipe and would abort the test.
countMarks() {
    local c=0 p
    for p in "$NIX_STORE_DIR"/*-mark[0-9]*; do [ -e "$p" ] && c=$((c + 1)); done
    echo "$c"
}

# Substitute-build (output from the cache; `-j0` forbids local builds) twice and
# count materialised source copies (`*-markN`) each. Kept flat — no subshell /
# process-substitution around `nix build` (that tripped `set -e` in the harness).
clearStore
nix build --no-link --option eval-cache false \
    --substitute --substituters "file://$cacheDir" --no-require-sigs -j0 "$flakeref"
eagerN=$(countMarks)

clearStore
nix build --no-link --option eval-cache false --option lazy-derivations true \
    --substitute --substituters "file://$cacheDir" --no-require-sigs -j0 "$flakeref"
lazyN=$(countMarks)

nix-store --check-validity "$out" || fail "substituted output is missing"

echo "multi-source elision (N=$N × $((SZ / 1000000)) MB = $((N * SZ / 1000000)) MB sources): eager copied=$eagerN  |  lazy copied=$lazyN"

# eager (lazy off) must copy ALL N sources (non-vacuity); lazy must elide ALL.
[ "$eagerN" = "$N" ] || fail "eager should copy all $N sources, got $eagerN"
[ "$lazyN" = 0 ] || fail "lazy should elide all $N sources, got $lazyN copied"

# eager (lazy off) must copy ALL N sources (non-vacuity); lazy must elide ALL.
[ "$eagerN" = "$N" ] || fail "eager should copy all $N sources, got $eagerN"
[ "$lazyN" = 0 ] || fail "lazy should elide all $N sources, got $lazyN copied"

echo "source-copy-elision-multi: ok ($N sources elided under lazy, copied under eager)"
