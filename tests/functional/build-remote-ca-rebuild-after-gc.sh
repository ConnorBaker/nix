#!/usr/bin/env bash

# A non-reproducible two-output CA derivation x, built on a builder, loses an output and is built
# there again: the client takes outputs of one build of x, or refuses them.

source common.sh

enableFeatures "ca-derivations"
requireSandboxSupport
requiresUnprivilegedUserNamespaces
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"
unset NIX_STORE_DIR

# shellcheck disable=SC2016
expr='with import ./config.nix; let
  ca = name: out: derivation { inherit name system; builder = '"$busybox"'; __contentAddressed = true;
    outputHashMode = "recursive"; outputHashAlgo = "sha256"; outputs = [ "bin" "out" ];
    args = [ "sh" "-c" "read -r r < /proc/sys/kernel/random/uuid; echo $r > $bin; echo ${out} > $out" ]; };
in { x = ca "remote-x" "$r"; y = ca "remote-y" "y"; }'
build() {
    nix build -L --no-link --print-out-paths --impure --expr "$expr" --max-jobs 0 --store "$store" \
        --builders "${proto-ssh-ng://localhost?remote-program=$prog&}remote-store=$builder%3Fsystem-features=ca-derivations - - 1 1 ca-derivations" "$@"
}
drvOf() { drv=$(nix eval --store "$store" --impure --raw --expr "($expr).$1.drvPath"); }
traced() { nix store build-trace info --store "$store" "$drv^$1" --json | jq -r '.[] | .opaquePath | select(.)'; }
# The traces of x name outputs of one build: x writes the same line to both.
oneBuild() { [[ $(cat "$store$(traced out)") == $(cat "$store$(traced bin)") ]]; }

# A new builder builds x again for the bin the client lost: its build answers for both outputs,
# and the client takes them, also out2 for the out1 it keeps.
store=$TEST_ROOT/fresh/client builder=$TEST_ROOT/fresh/builder prog=nix-daemon
build 'x^*'
drvOf x; out1=$(traced out)
nix store delete --store "$store" "$(traced bin)"
builder+=-new
build 'x^bin'
[[ $(traced out) != "$out1" ]]; oneBuild
nix path-info --store "$store" "$out1" > /dev/null

# A builder finds x's bin valid, then loses it (to a GC: nothing roots a floating output) before
# it builds x for out: it answers with that build, which the client takes, also bin2 for its bin1.
store=$TEST_ROOT/race/client builder=$TEST_ROOT/race/builder prog=nix-daemon
build 'x^*'
drvOf x; out1=$(traced out)
nix store delete --store "$store" "$out1"; nix store delete --store "$builder" "$out1"
printf '#!%s\nnix store delete --store %s %s >&2\n' "$SHELL" "$builder" "$(traced bin)" > "$TEST_ROOT/hook"
prog=$TEST_ROOT/front; chmod +x "$TEST_ROOT/hook"
# shellcheck disable=SC2016
printf '#!%s\nexec nix-daemon "$@" --option pre-build-hook %s\n' "$SHELL" "$TEST_ROOT/hook" > "$prog"; chmod +x "$prog"
bin1=$(traced bin)
build 'x^out'
[[ $(traced bin) != "$bin1" ]]; oneBuild
nix path-info --store "$store" "$bin1" > /dev/null

# A builder that kept y's out builds y again for bin, reproducing out: its build answers for both
# outputs, also for the out it found valid, and the client takes them.
store=$TEST_ROOT/rebuilt/client builder=$TEST_ROOT/rebuilt/builder prog=nix-daemon
build 'y^*'
drvOf y; bin1=$(traced bin)
nix store delete --store "$store" "$bin1"; nix store delete --store "$builder" "$bin1"
bin2=$(build 'y^bin')
[[ $bin2 != "$bin1" && $(traced bin) == "$bin2" ]]

# A builder whose traces of x name outputs of two builds (it lost its bin and took another
# builder's, as a copied trace is taken) finds x valid and does not say that a build answers for
# all outputs: the client, with a trace of x's out only, refuses that builder's bin.
store=$TEST_ROOT/mixed/other builder=$TEST_ROOT/mixed/builder-other prog=nix-daemon
build 'x^*'
store=$TEST_ROOT/mixed/client builder=$TEST_ROOT/mixed/builder
build 'x^*'
drvOf x; bin1=$(traced bin)
# The builders hold only the BasicDerivation they were sent: give them the derivation, by which
# their traces are copied.
nix copy --no-check-sigs --from "$store" --to "$builder" "$drv"
nix copy --no-check-sigs --from "$store" --to "$TEST_ROOT/mixed/builder-other" "$drv"
nix store delete --store "$builder" "$bin1"; nix store build-trace delete --store "$builder" "$drv^bin"
nix copy --no-check-sigs --from "$TEST_ROOT/mixed/builder-other" --to "$builder" "$drv^bin"
nix store delete --store "$store" "$bin1"; nix store build-trace delete --store "$store" "$drv^bin"
build 'x^bin' 2> "$TEST_ROOT/log" && fail "x^bin of another build than x^out taken"
grepQuiet "did not say that it built them together" "$TEST_ROOT/log"

# A builder that does not say that a build answers for all outputs (here, one that found y valid;
# under daemonCompat, a released daemon): the client, with a trace of y's out only, refuses.
store=$TEST_ROOT/old/client builder=$TEST_ROOT/old/builder prog=${NIX_DAEMON_PACKAGE:+$NIX_DAEMON_PACKAGE/bin/}nix-daemon
build 'y^*'
drvOf y; bin1=$(traced bin)
nix store delete --store "$store" "$bin1"; nix store build-trace delete --store "$store" "$drv^bin"
build 'y^bin' 2> "$TEST_ROOT/log" && fail "y^bin taken"; grepQuiet "did not say that it built them together" "$TEST_ROOT/log"

# A binary cache as the client's store takes traces one at a time, so as if mixed: it refuses a
# new builder's build of y for the bin whose trace and narinfo it lost, for out's trace there.
store=file://$TEST_ROOT/bc builder=$TEST_ROOT/bc-builder proto='ssh-ng://localhost?'
build 'y^*'
drvOf y; bin1=$(traced bin)
mv "$TEST_ROOT/bc/$(basename "$bin1" | cut -c1-32).narinfo" "$TEST_ROOT/bc/build-trace-v2/$(basename "$drv")/bin.doi" "$TEST_ROOT"
builder+=-new
build 'y^bin' --option narinfo-cache-positive-ttl 0 2> "$TEST_ROOT/log" && fail "y^bin taken"
grepQuiet "differs from the build traces in" "$TEST_ROOT/log"
