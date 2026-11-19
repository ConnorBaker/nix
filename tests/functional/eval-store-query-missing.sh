#!/usr/bin/env bash

source common.sh

TODO_NixOS

needLocalStore "starts its own daemon"

# `queryMissing` against a remote build store, with a separate evaluation
# store as the default (the `eval-store` setting), must ask the build store a
# number of questions that grows with the depth of the graph, not with the
# number of derivations in it, and must still report the derivations that
# only the evaluation store has.

depth=4
width=10
all=$((depth * width + 1))
file=./eval-store-query-missing.nix
eval_store=$TEST_ROOT/eval-store
graph=(--arg depth "$depth" --arg width "$width")

# Dry-run the graph against store $1 and check that it reports $2
# derivations to build, nothing unknown, and that the daemon logging to $3
# received at most $4 ops. The ops are left in $TEST_ROOT/ops. A daemon logs
# each op it receives at the debug level ("received daemon op N"), the only
# per-op count it gives. It takes the client's verbosity with SetOptions, so
# the client runs with -vvvvv; SetOptions itself is received before that and
# not counted.
dryRun() {
    local store=$1 expected=$2 log=$3 maxOps=$4 mark
    mark=$(wc -l < "$log")
    nix -vvvvv build -f "$file" top "${graph[@]}" \
        --option eval-store "$eval_store" --store "$store" --dry-run 2> "$TEST_ROOT/dry-run.log"
    grep -E "will be built|will be fetched|don't know how to build" "$TEST_ROOT/dry-run.log"
    grepQuietInverse "don't know how to build" "$TEST_ROOT/dry-run.log"
    grepQuiet "these $expected derivations will be built" "$TEST_ROOT/dry-run.log"
    # Not counted: QueryPathInfo (26), with which printMissing sorts the
    # derivations to build in the build store, once per derivation.
    tail -n +"$((mark + 1))" "$log" | sed -n 's/.*received daemon op \([0-9]*\).*/\1/p' | grep -vx 26 > "$TEST_ROOT/ops"
    echo "daemon ops: $(sort -n "$TEST_ROOT/ops" | uniq -c | tr '\n' ' ')"
    (($(wc -l < "$TEST_ROOT/ops") <= maxOps))
}

# An untrusted daemon, without the substituter that only this process uses:
# what that substituter has is fetched, as this process decides.
cache=$TEST_ROOT/query-missing-cache
untrusted=$TEST_ROOT/query-missing-untrusted.sh
cat > "$untrusted" <<EOF2
#!$SHELL
exec nix-daemon --force-untrusted "\$@"
EOF2
chmod +x "$untrusted"
remote=$TEST_ROOT/query-missing-untrusted-remote
lone=$(nix-build "$file" -A lone "${graph[@]}" --option eval-store "" --no-out-link)
nix copy --to "file://$cache" "$lone"
res=$(nix build -f "$file" lone "${graph[@]}" --option eval-store "$eval_store" \
    --store "ssh-ng://localhost?remote-program=$untrusted&remote-store=$remote" \
    --substituters "file://$cache" --no-require-sigs --option substitute true --dry-run 2>&1)
echo "$res"
[[ $res != *"will be built"* && $res != *"don't know how to build"* ]]
[[ $res == *"will be fetched"*"$lone"* ]]

# A daemon whose store is a binary cache answers QueryValidPaths by reading
# the narinfo of `lone`'s output, where its IsValidPath, which this walk
# asked before, only checks that it exists. So a corrupt narinfo is an
# error; before, it took the output as valid.
bc=$TEST_ROOT/query-missing-bc
loneDrv=$(nix-instantiate "$file" -A lone "${graph[@]}" --option eval-store "")
nix copy --to "file://$bc" "$lone" "$loneDrv"
narinfo() { echo "$bc/$(basename "$1" | cut -c1-32).narinfo"; }
cp "$(narinfo "$lone")" "$TEST_ROOT/lone.narinfo"
mv "$(narinfo "$loneDrv")" "$TEST_ROOT/lone-drv.narinfo"
NIX_CONFIG="store = file://$bc" startDaemon
bcDryRun() {
    nix build -f "$file" lone "${graph[@]}" --option eval-store "$eval_store" --dry-run
}
echo corrupt > "$(narinfo "$lone")"
expectStderr 1 bcDryRun | grepQuiet "is corrupt"
# A narinfo for `lone`'s derivation that names another path: the daemon
# does not have the derivation, so its static outputs are taken, and the
# dry run succeeds; before, the daemon's IsValidPath said that it had it,
# and asking it for the outputs failed.
cp "$TEST_ROOT/lone.narinfo" "$(narinfo "$lone")"
sed 's|^StorePath: \(.*\)\.drv$|StorePath: \1-other.drv|' "$TEST_ROOT/lone-drv.narinfo" > "$(narinfo "$loneDrv")"
[[ $(grep '^StorePath:' "$(narinfo "$loneDrv")") == *-other.drv ]]
bcDryRun
killDaemon

# The unix-socket daemon, which could substitute from the cache.
NIX_CONFIG="substituters = file://$cache
require-sigs = false
extra-experimental-features = ca-derivations" startDaemon
daemon_log=$TEST_ROOT/daemon-log
dryRun daemon "$all" "$daemon_log" $((depth + 1))

# With CA derivations, the output of the floating `unbuilt`, which is built
# nowhere and which only the evaluation store has, is unknown: it is built.
ca=(--extra-experimental-features ca-derivations)
res=$(nix build -f "$file" unbuilt "${graph[@]}" "${ca[@]}" --option eval-store "$eval_store" --dry-run 2>&1)
echo "$res"
[[ $res == *"this derivation will be built"* ]]
# Without CA derivations here, the daemon, which has `unbuilt`, is asked for
# its output, as before; the output is unknown, which needs them.
unbuilt=$(nix-instantiate "$file" -A unbuilt "${graph[@]}" "${ca[@]}" --option eval-store "")
expectStderr 1 nix build "$unbuilt^out" --option eval-store "$eval_store" --dry-run \
    | grepQuiet "experimental Nix feature 'ca-derivations' is disabled"

# A dry run fetches nothing: the daemon is asked without substitution.
nix-store --delete "$lone" > /dev/null
nix build -f "$file" lone "${graph[@]}" --option eval-store "$eval_store" --store daemon \
    --substituters "file://$cache" --no-require-sigs --option substitute true --dry-run
expectStderr 1 nix path-info --store daemon "$lone" > /dev/null

killDaemon
