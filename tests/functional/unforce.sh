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
# nix-instantiate evaluates its root, and then each attribute path, in a job
# of its own first.
expectJobs "$(jobsOf nix-instantiate --eval --strict --xml -E "$expr")" 5
expectJobs "$(jobsOf nix repl <<< ":p $expr")" 3

# --strict forces each element in a job before anything is printed: what
# it forced stays forced (a reverted cell prints as `<CODE>`), and what an
# element built besides is freed.
expectJobs "$(jobsOf nix-instantiate --eval --strict -E "$expr")" 5
[[ $(< "$TEST_ROOT/out") == '{ a = { x = 1; }; b = «repeated»; c = [ 1 2 3 ]; }' ]]
expectJobs "$(jobsOf nix-instantiate --eval --strict -E '{ n = builtins.length (builtins.genList (x: x) 100); }')" 3
[[ $(< "$TEST_ROOT/out") == '{ n = 100; }' ]]
# Only on 64-bit Linux, without Boehm GC, do jobs have a region, from which
# they free.
if [[ $system == *-linux && $system != i686-linux ]] && ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(jq -r '.unforce.bytesFreed' "$TEST_ROOT/stats.json") -gt 0 ]]
fi

# Without --strict, no element runs in a job of its own: an element's job end
# would revert what it forced (a's outPath for JSON, a's type for XML), which
# a later element reads as it is.
pexpr='let t = "x" + "y"; a = { outPath = t; }; in builtins.seq a { inherit a; b = t; }'
expectJobs "$(jobsOf nix-instantiate --eval --json -E "$pexpr")" 2
[[ $(< "$TEST_ROOT/out") == '{"a":"xy","b":"xy"}' ]]
expectJobs "$(jobsOf nix-instantiate --eval --xml -E "${pexpr/outPath/type}")" 2
[[ $(grep -c '<string value="xy" />' "$TEST_ROOT/out") == 2 ]]

# A trace keeps nothing: re-forcing the cell whose evaluation showed it may
# show it again, and shows no other trace.
nix eval --expr 'let s = builtins.trace "s" { x = 1; }; in { a = s; b = s; }' 2> "$TEST_ROOT/err"
[[ $(grep '^trace: ' "$TEST_ROOT/err" | sort -u) == 'trace: s' ]]

# An effect, here a filtered copy, which nothing remembers, keeps the cell
# whose evaluation ran it: a later element that reads the cell does not run
# it again (the filter traces once per file, as when one element reads it),
# and the job is not kept whole.
for d in src src2; do
    mkdir -p "$TEST_ROOT/$d"
    for f in a b c d e; do echo "$d $f" > "$TEST_ROOT/$d/$f"; done
done
copy() {
    echo "let p = builtins.path { path = $TEST_ROOT/$1; filter = _: _: builtins.trace \"f\" true; }; in"
}
nix eval --impure --expr "$(copy src2) { a = builtins.seq p 1; }" 2> "$TEST_ROOT/err1" > /dev/null
jobsOf nix eval --impure --expr "$(copy src) { a = builtins.seq p 1; b = builtins.seq p 2; q = { x = 1; }; }" 2> "$TEST_ROOT/err" > /dev/null
[[ $(grep -c '^trace: f$' "$TEST_ROOT/err") == $(grep -c '^trace: f$' "$TEST_ROOT/err1") ]]
grepQuiet '^trace: f$' "$TEST_ROOT/err"
if [[ $system == *-linux && $system != i686-linux ]] && ! jq -e 'has("gc")' "$TEST_ROOT/stats.json" > /dev/null; then
    [[ $(jq -r '.unforce.jobsKeptWhole' "$TEST_ROOT/stats.json") == 0 ]]
fi

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
# So in each of nix-env's channels (a directory of expressions).
mkdir -p "$TEST_ROOT/channels"
cp "$TEST_ROOT/drvs.nix" "$TEST_ROOT/channels/"
expectJobs "$(jobsOf nix-env -f "$TEST_ROOT/channels" -qa)" 5
[[ $(< "$TEST_ROOT/out") == $'x\nz' ]]
expectJobs "$(jobsOf nix-instantiate "$TEST_ROOT/drvs.nix")" 7
[[ $(wc -l < "$TEST_ROOT/out") == 2 ]]
# search visits each attribute in a job.
expectJobs "$(jobsOf nix search -f "$TEST_ROOT/drvs.nix" '' ^)" 3
