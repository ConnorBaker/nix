#!/usr/bin/env bash

# A derivation output has one row in the build trace table: registering it
# again merges the signatures of all its rows into one, of two
# concurrent registrations of different outputs only one succeeds, and of
# rows left by older versions one naming a valid output counts.

source common.sh

needLocalStore "the test reads and locks the store database"
[[ -n $(type -p sqlite3) ]] || skipTest "needs sqlite3"

db="$NIX_STATE_DIR/db/db.sqlite"
rows () {
    sqlite3 "$db" "select outputPath, signatures from BuildTraceV3 where drvPath = '$(basename "$1")' and outputName = 'out'"
}

clearStore
nix-store --generate-binary-cache-key cache1.example.org "$TEST_ROOT/sk1" "$TEST_ROOT/pk1"
nix-store --generate-binary-cache-key cache2.example.org "$TEST_ROOT/sk2" "$TEST_ROOT/pk2"

# shellcheck disable=SC2016 # $out is the builder's
expr='with import ./config.nix; mkDerivation {
  name = "deterministic";
  buildCommand = "echo hello > $out";
  __contentAddressed = true;
  outputHashMode = "recursive";
  outputHashAlgo = "sha256";
}'
drv=$(nix-instantiate -E "$expr")
path=$(nix-build -E "$expr" --no-out-link --secret-key-files "$TEST_ROOT/sk1")
nix copy --to "file://$TEST_ROOT/signed1" --impure --expr "$expr"
nix store delete "$path"
nix store build-trace delete "$drv^out"
[[ $(nix-build -E "$expr" --no-out-link --secret-key-files "$TEST_ROOT/sk2") == "$path" ]]
# A second row for the output, with a signature of its own
sqlite3 "$db" "insert into BuildTraceV3 (drvPath, outputName, outputPath, signatures)
    select drvPath, outputName, outputPath, replace(signatures, 'cache2.example.org:', 'cache3.example.org:')
    from BuildTraceV3"
[[ $(rows "$drv" | wc -l) == 2 ]]
nix copy --from "file://$TEST_ROOT/signed1" --no-check-sigs --impure --expr "$expr"
[[ $(rows "$drv" | wc -l) == 1 ]]
rows "$drv" | grepQuiet "cache1.example.org:"
rows "$drv" | grepQuiet "cache2.example.org:"
rows "$drv" | grepQuiet "cache3.example.org:"

# Two registrations of different outputs while another connection holds
# the database's write lock. Should either start only after the lock is
# released, it reads the other's trace and conflicts anyway: timing can let
# a broken implementation pass, but cannot fail a correct one.
drv=$(nix-instantiate ./nondeterministic-ns.nix -A singleOut)
for i in 1 2; do
    p[i]=$(nix-build ./nondeterministic-ns.nix -A singleOut --no-out-link)
    nix copy --to "file://$TEST_ROOT/race$i" --file ./nondeterministic-ns.nix singleOut
    nix store build-trace delete "$drv^out"
done
sqlite3 "$db" > "$TEST_ROOT/lock.log" 2>&1 <<EOF &
begin immediate;
.shell touch $TEST_ROOT/locked
.shell sleep 10
commit;
EOF
lock=$!
until [[ -e $TEST_ROOT/locked ]]; do sleep 0.1; done
for i in 1 2; do
    nix copy --from "file://$TEST_ROOT/race$i" --no-check-sigs --file ./nondeterministic-ns.nix singleOut \
        2> "$TEST_ROOT/race$i.err" &
    copy[i]=$!
done
wait "$lock"
failed=0
for i in 1 2; do
    wait "${copy[i]}" || failed=$((failed + 1))
done
[[ $failed == 1 ]]
cat "$TEST_ROOT"/race*.err | grepQuiet "but we already have another one locally"
# Neither waited in Nix's retry loop for more than a second, after which it
# warns that the database is busy
cat "$TEST_ROOT"/race*.err | grepQuietInverse "is busy"
[[ $(rows "$drv" | wc -l) == 1 ]]

# Versions that registered without a transaction may have left several
# rows. A valid output counts even when a row naming an output not in the
# store comes first.
key=$(basename "$drv")
gone=$(dirname "${p[1]}")/00000000000000000000000000000000-time-single-out
sqlite3 "$db" "delete from BuildTraceV3 where drvPath = '$key';
    insert into BuildTraceV3 (drvPath, outputName, outputPath, signatures)
    values ('$key', 'out', '$gone', ''), ('$key', 'out', '${p[1]}', '')"
err=$(expectStderr 1 nix copy --from "file://$TEST_ROOT/race2" --no-check-sigs --file ./nondeterministic-ns.nix singleOut)
grepQuiet -F "Local:  ${p[1]}" <<< "$err"
[[ $(rows "$drv" | wc -l) == 2 ]]
