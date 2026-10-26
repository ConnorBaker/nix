#!/usr/bin/env bash

source common.sh

# Traversals evaluate each element of their root in an unforce job of its
# own (builds without Boehm GC), and print what they print without.

clearStoreIfPossible

# The number of unforce jobs that `$@` ran, or "-" in a build with Boehm GC,
# which has none: its statistics have "gc" in place of "unforce".
jobsOf() {
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats.json" "$@" > "$TEST_ROOT/out"
    jq -r 'if has("gc") then "-" else .unforce.jobs end' "$TEST_ROOT/stats.json"
}

expectJobs() {
    [[ $1 == - || $1 == "$2" ]] || fail "expected $2 unforce jobs, got $1"
}

# A set printed in an earlier element's job is «repeated» in a later one's.
expr='let s = { x = 1; }; in { a = s; b = s; c = [ 1 2 3 ]; }'
expectJobs "$(jobsOf nix eval --expr "$expr")" 3
[[ $(< "$TEST_ROOT/out") == '{ a = { x = 1; }; b = «repeated»; c = [ 1 2 3 ]; }' ]]
expectJobs "$(jobsOf nix eval --json --expr "$expr")" 3
[[ $(< "$TEST_ROOT/out") == '{"a":{"x":1},"b":{"x":1},"c":[1,2,3]}' ]]
expectJobs "$(jobsOf nix-instantiate --eval --strict --xml -E "$expr")" 3
expectJobs "$(jobsOf nix repl <<< ":p $expr")" 3

# --strict forces each element in a job before anything is printed: what
# it forced stays forced (a reverted cell prints as `<CODE>`), and what an
# element built besides is freed.
expectJobs "$(jobsOf nix-instantiate --eval --strict -E "$expr")" 3
[[ $(< "$TEST_ROOT/out") == '{ a = { x = 1; }; b = «repeated»; c = [ 1 2 3 ]; }' ]]
expectJobs "$(jobsOf nix-instantiate --eval --strict -E '{ n = builtins.length (builtins.genList (x: x) 100); }')" 1
[[ $(< "$TEST_ROOT/out") == '{ n = 100; }' ]]
# Only on 64-bit Linux, without Boehm GC, do jobs have a region, from which
# they free.
if [[ $system == *-linux && $system != i686-linux ]] && ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(jq -r '.unforce.bytesFreed' "$TEST_ROOT/stats.json") -gt 0 ]]
fi

# A trace is shown once: the cell whose evaluation showed it keeps its
# value, and the job reverts the rest.
nix eval --expr 'let s = builtins.trace "s" { x = 1; }; in { a = s; b = s; }' 2> "$TEST_ROOT/err"
[[ $(grep -c '^trace: s$' "$TEST_ROOT/err") == 1 ]]

# flake check checks each output in a job of its own.
flakeDir=$TEST_ROOT/flake
mkdir -p "$flakeDir"
cp "${config_nix}" "$flakeDir/"
cat > "$flakeDir/flake.nix" <<EOF
{
  outputs = { self }: with import ./config.nix; {
    packages.$system.x = mkDerivation { name = "x"; builder = "/b"; };
    overlays.default = final: prev: { };
  };
}
EOF
expectJobs "$(jobsOf nix flake check "$flakeDir")" 2
# flake show shows each package in one.
expectJobs "$(jobsOf nix flake show "$flakeDir")" 1

# A derivation found in an earlier job is dropped in a later one; one job
# per attribute, then one per derivation found (its name, its drvPath).
cat > "$TEST_ROOT/drvs.nix" <<EOF
with import "${config_nix}";
rec {
  x = mkDerivation { name = "x"; builder = "/b"; };
  y = x;
  z = mkDerivation { name = "z"; builder = "/b"; };
}
EOF
expectJobs "$(jobsOf nix-env -f "$TEST_ROOT/drvs.nix" -qa)" 5
[[ $(< "$TEST_ROOT/out") == $'x\nz' ]]
expectJobs "$(jobsOf nix-instantiate "$TEST_ROOT/drvs.nix")" 5
[[ $(wc -l < "$TEST_ROOT/out") == 2 ]]
# search visits each attribute in a job.
expectJobs "$(jobsOf nix search -f "$TEST_ROOT/drvs.nix" '' ^)" 3
