#!/usr/bin/env bash
#
# MANUAL benchmark (NOT gate-registered): the DAEMON elision-forfeit magnitude.
# A substitutable target with a BIG fingerprintable source. LOCAL lazy elides the
# source copy; a DAEMON build must ship the whole .drv closure (incl. the source)
# to the daemon before building (LD-S6, no reverse channel), so it forfeits the
# elision — it copies the 100MB source the local build skips, then substitutes
# the output anyway. The delta = the wasted ship. Result to /tmp/ld-forfeit.txt.
# To run: temporarily add 'bench-forfeit.sh' to this dir's meson.build, then
# `meson test -C build --suite lazy-derivations bench-forfeit`.
source ../common.sh
requireGit
TODO_NixOS
enableFeatures "flakes"
clearStore; clearCache
echo "require-sigs = false" >> "$NIX_CONF_DIR/nix.conf"

cacheDir="$TEST_ROOT/cache"; flake="$TEST_ROOT/flk"
createGitRepo "$flake"; mkdir -p "$flake/src"
head -c 104857600 /dev/urandom > "$flake/src/big.dat"   # 100MB
cat > "$flake/flake.nix" <<'EOF'
{ outputs = { self }: let src = builtins.path { path = ./src; name = "bigsrc"; }; in {
    packages.SYSTEM.default = derivation { name = "forfeit-bench"; system = "SYSTEM"; builder = "/bin/sh";
      args = [ "-c" "echo done > $out" "${src}" ]; }; }; }
EOF
sed -i "s|SYSTEM|$system|g" "$flake/flake.nix"
git -C "$flake" add -A >/dev/null 2>&1; git -C "$flake" commit -qm init >/dev/null 2>&1
ref="git+file://$flake#packages.$system.default"
SUB="--substitute --substituters file://$cacheDir -j0"
out=$(nix build --no-link --print-out-paths --option eval-cache false "$ref")
nix copy --to "file://$cacheDir" "$out"

OUT=/tmp/ld-forfeit.txt; : > "$OUT"
echo "daemon elision-forfeit: 100MB source, substitutable target" >> "$OUT"
clearStore
t0=$(date +%s.%N)
nix build --no-link --option eval-cache false --option lazy-derivations true $SUB "$ref" >/dev/null 2>&1
t1=$(date +%s.%N)
ls "$NIX_STORE_DIR"/*bigsrc >/dev/null 2>&1 && bs=COPIED || bs=ELIDED
printf 'LOCAL  lazy  %.3fs  bigsrc=%s\n' "$(echo "$t1-$t0"|bc -l)" "$bs" >> "$OUT"

killDaemon 2>/dev/null || true; clearStore; startDaemon
t0=$(date +%s.%N)
timeout -k 5 90 nix build --no-link --option eval-cache false --option lazy-derivations true $SUB "$ref" >/dev/null 2>&1
rc=$?
t1=$(date +%s.%N)
ls "$NIX_STORE_DIR"/*bigsrc >/dev/null 2>&1 && bs=COPIED || bs=ELIDED
printf 'DAEMON lazy  %.3fs  bigsrc=%s  rc=%d\n' "$(echo "$t1-$t0"|bc -l)" "$bs" "$rc" >> "$OUT"
killDaemon 2>/dev/null || true
echo "bench-forfeit done" >> "$OUT"
cat "$OUT"
