#!/usr/bin/env bash
#
# MANUAL benchmark (NOT a gate test — it copies 100MB and timing is noisy, so it
# is deliberately not in this directory's meson.build). It measures the ELISION
# win — the headline value of the selective/lazy design — which the standalone
# instantiate harness (doc/tecnix-survey/bench-lazy-derivations.sh) cannot reach
# (elision needs the build+substitute path, hence the functional harness). A
# substitutable target with a BIG fingerprintable source: lazy OFF copies the
# source eagerly during eval and then substitutes the output — wasting the copy;
# lazy ON elides it. Result + elision status to /tmp/ld-elision-bench.txt.
#
# To run: temporarily add 'bench-elision.sh' to this directory's meson.build
# 'tests' list, then `meson test -C build --suite lazy-derivations bench-elision`,
# then revert.
#
# Representative result (best-of-N, verified counts, PAGE-CACHE-WARM):
#     eager (lazy off)  bigsrc=COPIED   (the copy is wasted — output substitutes)
#     lazy  (on)        bigsrc=ELIDED   → faster, copy avoided
# MEASURED RANGE (ledger A7, 2026-06-03): ~1.4-1.7x best-of-5 on 8 sources
# (160-720MB), page-cache-WARM. The earlier "~5x" here was an n=1 single-source
# eyeball — DO NOT trust a single run: this multiplier is f(source-size /
# eval-size) (experimenter-set, a byte-throughput tautology), it is page-cache
# warm (the source was just written; `drop_caches` needs root, so the cold-disk
# win — larger — is unmeasured here), and it requires the REQUESTED TARGET to be
# fully substitutable (ledger A6): a must-build target over substitutable inputs
# materialises its WHOLE input .drv closure (referential integrity) and elides
# NOTHING. Report absolute ms-saved + the verified COPIED/ELIDED status, not a
# multiplier; a remote daemon forfeits the elision (LD-S6).
source ../common.sh
requireGit
needLocalStore "“--no-require-sigs” can’t be used with the daemon"
enableFeatures "flakes"
clearStore; clearCache

cacheDir="$TEST_ROOT/cache"; flake="$TEST_ROOT/flk"
createGitRepo "$flake"; mkdir -p "$flake/src"
head -c 104857600 /dev/urandom > "$flake/src/big.dat"   # 100MB incompressible source
cat > "$flake/flake.nix" <<'EOF'
{ outputs = { self }: let src = builtins.path { path = ./src; name = "bigsrc"; }; in {
    packages.SYSTEM.default = derivation { name = "elide-bench"; system = "SYSTEM"; builder = "/bin/sh";
      args = [ "-c" "echo done > $out" "${src}" ]; }; }; }
EOF
sed -i "s|SYSTEM|$system|g" "$flake/flake.nix"
git -C "$flake" add -A >/dev/null 2>&1; git -C "$flake" commit -qm init >/dev/null 2>&1
ref="git+file://$flake#packages.$system.default"
SUB="--substitute --substituters file://$cacheDir --no-require-sigs -j0"

# Seed the cache with the OUTPUT only (the build echoes; bigsrc is not in the
# output closure).
out=$(nix build --no-link --print-out-paths --option eval-cache false "$ref")
nix copy --to "file://$cacheDir" "$out"

OUT=/tmp/ld-elision-bench.txt; : > "$OUT"
echo "elision bench: 100MB source, substitutable target" >> "$OUT"
measure() {  # $1 label $2 opts
    clearStore
    local t0 t1 bs
    t0=$(date +%s.%N)
    nix build --no-link --option eval-cache false $2 $SUB "$ref" >/dev/null 2>&1
    t1=$(date +%s.%N)
    if ls "$NIX_STORE_DIR"/*bigsrc >/dev/null 2>&1; then bs=COPIED; else bs=ELIDED; fi
    printf '%-16s %.3fs  bigsrc=%s\n' "$1" "$(echo "$t1-$t0"|bc -l)" "$bs" >> "$OUT"
}
for r in 1 2 3; do measure "eager(lazy off)" ""; done
for r in 1 2 3; do measure "lazy(on)" "--option lazy-derivations true"; done
echo "_bench_elision done" >> "$OUT"
cat "$OUT"
