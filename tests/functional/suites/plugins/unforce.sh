# shellcheck shell=bash

# A plugin's primop is native code, whose effects unforcing could not
# revert: front ends open no unforce jobs (in builds without Boehm GC), and
# the statistics say why.

# The jobs that `$@` ran and why there were none, or "-" in a build that has
# no unforcing.
unforcing() {
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats.json" "$@" > "$TEST_ROOT/out"
    jq -r 'if .unforce then "\(.unforce.jobs) \(.unforce.off // "on")" else "-" end' "$TEST_ROOT/stats.json"
}

# a allocates nothing, which is no rise in memory in use: b runs in a's job.
res=$(unforcing nix eval --expr '{ a = null; b = [ 1 ]; }')
[[ $res == - || $res == "1 on" ]]

plugin=$(findPlugin libplugintest)
res=$(unforcing nix --option plugin-files "$plugin" eval --expr '{ a = builtins.anotherNull; b = [ 1 ]; }')
[[ $(< "$TEST_ROOT/out") == '{ a = false; b = [ 1 ]; }' ]]
[[ $res == - || $res == "0 plugin primop" ]]
