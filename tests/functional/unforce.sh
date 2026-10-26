#!/usr/bin/env bash

source common.sh

# Traversals evaluate each element of their root in an unforce job of its
# own (builds without Boehm GC), and print what they print without.

clearStoreIfPossible

# The number of unforce jobs that `$@` ran, or "-" in a build that has none.
jobsOf() {
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats.json" "$@" > "$TEST_ROOT/out"
    jq -r '.unforce.jobs // "-"' "$TEST_ROOT/stats.json"
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
