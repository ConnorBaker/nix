#!/usr/bin/env bash

# One address (doc/lazy-store/01-specification.md, section 9.11; 04
# section 1.9, one address): a store object's content hash
# is its object hash, `git:sha256:<hex>`, the same value `nix hash path
# --mode git` computes; path-info JSON version 4 shows it and no NAR hash,
# and the older formats still show a NAR hash through the shim.  A path is
# signed under fingerprint 2 and verifies; it round-trips through
# export/import and through a binary cache, whose `.narinfo` carries both
# `ObjectHash:` and `NarHash:`, with the same object hash at every step;
# verification recomputes the object hash and reports a modified file.  A
# key signs both fingerprints, so an old client verifies a path the new
# Nix signed and uploaded, under version 1.  An old client against the new
# daemon still gets a NAR hash.  A schema-10 row is migrated by `nix store
# migrate` and by a query, and ungates the collector's sweep; a cache
# without `ObjectHash:` is signed under version 1 and verifies.

source common.sh

needLocalStore "the column and the verifier are the local store's"

clearStore

src=$TEST_ROOT/src
rm -rf "$src"
mkdir -p "$src/d/e"
echo one > "$src/a"
echo two > "$src/d/b"
echo one > "$src/d/e/c"
echo one > "$src/x"
chmod +x "$src/x"
ln -s b "$src/d/l"

p=$(nix store add --mode nar "$src")
pBase=$(basename "$p")

# `nix path-info --json-format 4` shows the object hash and no NAR hash;
# the object hash is `nix hash path --mode git`'s under SHA-256.
gitHex=$(nix hash path --mode git --algo sha256 --format base16 "$p")
[[ $gitHex =~ ^[0-9a-f]{64}$ ]]
info4=$(nix path-info --json --json-format 4 "$p")
[[ $(echo "$info4" | jq -r --arg b "$pBase" '.info[$b].objectHash') == "git:sha256:$gitHex" ]]
echo "$info4" | jq -e --arg b "$pBase" '.info[$b] | has("narHash") | not'
echo "$info4" | jq -e --arg b "$pBase" '.info[$b].narSize > 0'
echo "$info4" | jq -e '.version == 4'

# `--json-format 2` still shows the NAR hash, through the shim, equal to
# `nix hash path --mode nar`'s.
narSri=$(nix hash path --mode nar --format sri "$p")
[[ $(nix path-info --json --json-format 2 "$p" | jq -r --arg b "$pBase" '.info[$b].narHash') == "$narSri" ]]
nix path-info --json --json-format 2 "$p" | jq -e --arg b "$pBase" '.info[$b] | has("objectHash") | not'

# A schema-10 row (01 section 9.11, "The database"; src/nix/store-migrate.cc):
# `ValidPaths.hash` holding `sha256:<NAR hash>` as an older Nix wrote it, is
# given its object hash by one walk of the path -- by `nix store migrate`,
# or by the first query.  The row is planted through SQLite, the one
# writer of that form left; until it is migrated the collector's sweep of
# the object store is gated (gc.cc), and afterwards it is not.
if type -p sqlite3 > /dev/null; then
    db=$NIX_STATE_DIR/db/db.sqlite
    hashColumn() { sqlite3 "$db" "select hash from ValidPaths where path = '$1'"; }
    [[ $(hashColumn "$p") == "git:sha256:$gitHex" ]]

    # The rendering master's database held: `narHash.to_string(HashFormat::Base16, true)`.
    narHex=$(nix hash path --mode nar --algo sha256 --format base16 "$p")
    sqlite3 "$db" "update ValidPaths set hash = 'sha256:$narHex' where path = '$p'"
    [[ $(hashColumn "$p") == "sha256:$narHex" ]]

    # A path deleted leaves its blob behind: `nix store delete` sweeps
    # nothing (gc.cc, only a whole-store collection pays the sweep), and it
    # queries no row but the victim's, so `$p`'s row stays old.  (The gate
    # on an old row -- "paths not yet migrated", the sweep removing nothing
    # -- cannot be observed from here any more: a whole-store collection
    # visits every rooted path and queries its info (`computeFSClosure`,
    # `topoSortPaths`), which migrates the row on the way; the gate is
    # checked where it can be driven directly, `LocalStoreObjectsTest`'s
    # sweeps with an enumeration that returns an unmigrated count.)
    nix-store --add-root "$TEST_ROOT/root-p" --indirect -r "$p" > /dev/null
    echo "collect me $RANDOM$RANDOM" > "$TEST_ROOT/victim"
    victim=$(nix store add --mode nar "$TEST_ROOT/victim")
    victimBlob=$NIX_STORE_DIR/.objects/blobs/$(nix hash path --mode git --algo sha256 --format base16 "$TEST_ROOT/victim")
    [[ -e $victimBlob ]]
    nix store delete "$victim"
    [[ -e $victimBlob ]]
    [[ $(hashColumn "$p") == "sha256:$narHex" ]]

    # (a) `nix store migrate`: the one valid path checked, none skipped,
    # none failed (the text is store-migrate.cc's `notice` at HEAD).
    nix store migrate 2> "$TEST_ROOT/migrate.log"
    grepQuiet "1 store paths checked, 0 skipped (collected meanwhile), 0 failed" "$TEST_ROOT/migrate.log"
    # (c) The row is the object hash's rendering, (b) which `--query --hash`
    # prints, equal to `nix hash path --mode git`.
    [[ $(hashColumn "$p") == "git:sha256:$gitHex" ]]
    [[ $(nix-store --query --hash "$p") == "git:sha256:$gitHex" ]]
    # A second run finds the row current and reports the same count.
    nix store migrate 2> "$TEST_ROOT/migrate-again.log"
    grepQuiet "1 store paths checked, 0 skipped (collected meanwhile), 0 failed" "$TEST_ROOT/migrate-again.log"
    [[ $(hashColumn "$p") == "git:sha256:$gitHex" ]]
    # (d) Ungated, every row migrated: the collection's sweep removes the
    # deleted path's blob and keeps `$p`'s.
    nix-store --gc 2> "$TEST_ROOT/gc-swept.log"
    grepQuietInverse "not yet migrated" "$TEST_ROOT/gc-swept.log"
    [[ ! -e $victimBlob ]]
    [[ -e $NIX_STORE_DIR/.objects/trees/$gitHex ]]
    [[ $(cat "$p/a") == one ]]

    # The lazy route: the old row again, in the other rendering the old
    # parser accepts (`Hash::parseAnyPrefixed`: nix32), and one query
    # migrates it.
    narNix32=$(nix hash path --mode nar --algo sha256 --format nix32 "$p")
    sqlite3 "$db" "update ValidPaths set hash = 'sha256:$narNix32' where path = '$p'"
    [[ $(hashColumn "$p") == "sha256:$narNix32" ]]
    [[ $(nix path-info --json --json-format 4 "$p" | jq -r --arg b "$pBase" '.info[$b].objectHash') == "git:sha256:$gitHex" ]]
    [[ $(hashColumn "$p") == "git:sha256:$gitHex" ]]

    # The store an older Nix left, simulated (01 section 10, "a live root
    # without an object keeps nothing beneath it"): a schema-10 row and no
    # objects at all -- the path's files plain,
    # the object store empty.  The first collection migrates the row by its
    # walk and finds a live root with no tree; it says so, names `nix-store
    # --optimise`, and sweeps all the same: a live root without an object
    # keeps nothing beneath it (01 section 9.10 law 5).  A path added and
    # deleted afterwards loses its blob at the next collection.  Before:
    # "the object store was not swept", and the victim's blob survived every
    # collection until `--optimise` (doc/lazy-store/08-store-model.md, the collector;
    # E1 B).
    sqlite3 "$db" "update ValidPaths set hash = 'sha256:$narHex' where path = '$p'"
    find "$NIX_STORE_DIR/.objects/blobs" "$NIX_STORE_DIR/.objects/blobs-x" "$NIX_STORE_DIR/.objects/trees" -mindepth 1 -delete
    [[ $(stat --format=%h "$p/d/b") == 1 ]]
    nix-store --gc 2> "$TEST_ROOT/gc-upgraded.log"
    grepQuiet "1 valid paths have no object in the object store" "$TEST_ROOT/gc-upgraded.log"
    grepQuiet "nix-store --optimise" "$TEST_ROOT/gc-upgraded.log"
    grepQuietInverse "was not swept" "$TEST_ROOT/gc-upgraded.log"
    [[ $(hashColumn "$p") == "git:sha256:$gitHex" ]]
    echo "collect me too $RANDOM$RANDOM" > "$TEST_ROOT/victim2"
    victim2=$(nix store add --mode nar "$TEST_ROOT/victim2")
    victim2Blob=$NIX_STORE_DIR/.objects/blobs/$(nix hash path --mode git --algo sha256 --format base16 "$TEST_ROOT/victim2")
    [[ -e $victim2Blob ]]
    nix store delete "$victim2"
    [[ -e $victim2Blob ]]
    nix-store --gc 2> "$TEST_ROOT/gc-upgraded-2.log"
    grepQuiet "1 valid paths have no object in the object store" "$TEST_ROOT/gc-upgraded-2.log"
    [[ ! -e $victim2Blob ]]
    [[ $(cat "$p/a") == one ]]
    # `nix-store --optimise` is the remedy the warning names: the path is
    # entered again, and the next collection has nothing to report.
    nix-store --optimise
    [[ -e $NIX_STORE_DIR/.objects/trees/$gitHex ]]
    [[ $(stat --format=%h "$p/d/b") == 2 ]]
    nix-store --gc 2> "$TEST_ROOT/gc-entered.log"
    grepQuietInverse "have no object" "$TEST_ROOT/gc-entered.log"

    # `--optimise` on an unmigrated row writes the column as `nix store
    # migrate` does -- the walk that enters the path is the walk the
    # migration needs -- and its report says what it did: the objects
    # entered and the rows migrated, so a migration is told from a no-op.
    # Before: the column stayed `sha256:` and the report read "hard-linking
    # 0 files" (doc/lazy-store/08-store-model.md, the migration).
    sqlite3 "$db" "update ValidPaths set hash = 'sha256:$narHex' where path = '$p'"
    find "$NIX_STORE_DIR/.objects/blobs" "$NIX_STORE_DIR/.objects/blobs-x" "$NIX_STORE_DIR/.objects/trees" -mindepth 1 -delete
    nix-store --optimise 2> "$TEST_ROOT/optimise-migrate.log"
    [[ $(hashColumn "$p") == "git:sha256:$gitHex" ]]
    grepQuiet "1 paths migrated to the object hash" "$TEST_ROOT/optimise-migrate.log"
    grepQuiet "files entered as new objects" "$TEST_ROOT/optimise-migrate.log"
    grepQuietInverse "0 files entered as new objects" "$TEST_ROOT/optimise-migrate.log"
    [[ -e $NIX_STORE_DIR/.objects/trees/$gitHex ]]
    nix-store --optimise 2> "$TEST_ROOT/optimise-noop.log"
    grepQuiet "0.0 KiB freed by hard-linking 0 files; 0 files entered as new objects; 0 paths migrated to the object hash" "$TEST_ROOT/optimise-noop.log"

    # A schema-10 row whose files are gone (01 section 10, "a schema-10 row
    # whose files are gone is answered from the database"): the
    # row is valid and master answered from it; here too -- `nix path-info`
    # and `--query --references` answer, `nix store migrate` counts the path
    # failed and names `nix-store --verify`, and the collector's note, whose
    # sweep the row gates, names it as well.  `nix-store --verify` removes
    # the row and the next collection sweeps.  Before: the query threw
    # "does not exist", `nix store migrate` said "skipped (collected
    # meanwhile), 0 failed" and exited 0, and the note named only `nix store
    # migrate`, which could not help.
    echo "gone $RANDOM$RANDOM" > "$TEST_ROOT/gone"
    q=$(nix store add --mode nar "$TEST_ROOT/gone")
    qBlob=$NIX_STORE_DIR/.objects/blobs/$(nix hash path --mode git --algo sha256 --format base16 "$TEST_ROOT/gone")
    [[ -e $qBlob ]]
    qNarHex=$(nix hash path --mode nar --algo sha256 --format base16 "$q")
    sqlite3 "$db" "update ValidPaths set hash = 'sha256:$qNarHex' where path = '$q'"
    chmod -R u+w "$q"
    rm -rf "$q"
    nix path-info "$q" > /dev/null
    nix-store --query --references "$q" > /dev/null
    [[ $(hashColumn "$q") == "sha256:$qNarHex" ]]
    expectStderr 1 nix store migrate > "$TEST_ROOT/migrate-missing.log"
    grepQuiet "files are missing" "$TEST_ROOT/migrate-missing.log"
    grepQuiet "nix-store --verify" "$TEST_ROOT/migrate-missing.log"
    grepQuiet "1 failed" "$TEST_ROOT/migrate-missing.log"
    grepQuietInverse "1 skipped" "$TEST_ROOT/migrate-missing.log"
    nix-store --gc 2> "$TEST_ROOT/gc-missing.log"
    grepQuiet "1 paths not yet migrated" "$TEST_ROOT/gc-missing.log"
    grepQuiet "nix-store --verify" "$TEST_ROOT/gc-missing.log"
    [[ -e $qBlob ]]
    nix-store --verify 2> "$TEST_ROOT/verify-missing.log"
    grepQuiet "disappeared, removing from database" "$TEST_ROOT/verify-missing.log"
    expect 1 nix-store --check-validity "$q"
    nix-store --gc 2> "$TEST_ROOT/gc-after-missing.log"
    grepQuietInverse "not yet migrated" "$TEST_ROOT/gc-after-missing.log"
    [[ ! -e $qBlob ]]
else
    echo "sqlite3 not on PATH; skipping the schema-10 row checks" >&2
fi

# The object hash agrees with the hashing door for a bare root, too.
echo hello > "$TEST_ROOT/plain"
pPlain=$(nix store add --mode nar "$TEST_ROOT/plain")
[[ $(nix path-info --json --json-format 4 "$pPlain" | jq -r '.info | to_entries[0].value.objectHash') \
   == "git:sha256:$(nix hash path --mode git --algo sha256 --format base16 "$pPlain")" ]]

# Signing under fingerprint 2, and verifying.
nix-store --generate-binary-cache-key cache1.example.org "$TEST_ROOT"/sk1 "$TEST_ROOT"/pk1
pk1=$(cat "$TEST_ROOT"/pk1)
# (`$p` is content-addressed, so it needs no signature to verify; the
# signature is checked through its own presence below.)
nix store sign --key-file "$TEST_ROOT"/sk1 "$p"
nix store verify "$p" --sigs-needed 1 --trusted-public-keys "$pk1"
# One key, two signatures: fingerprint 2 and fingerprint 1 (for clients
# that verify only the latter), the same key name on both.
nix path-info --json --json-format 4 "$p" | jq -e --arg b "$pBase" \
    '.info[$b].signatures | length == 2 and all(.keyName == "cache1.example.org") and (map(.sig) | unique | length == 2)'

# Export/import round-trips into a second store with the same object hash.
store2=$TEST_ROOT/store2
rm -rf "$store2"
nix-store --export "$p" > "$TEST_ROOT/p.export"
nix-store --store "$store2" --import < "$TEST_ROOT/p.export" | grepQuiet "$pBase"
[[ $(nix path-info --store "$store2" --json --json-format 4 "$p" | jq -r --arg b "$pBase" '.info[$b].objectHash') \
   == "git:sha256:$gitHex" ]]
[[ $(nix path-info --store "$store2" --json --json-format 2 "$p" | jq -r --arg b "$pBase" '.info[$b].narHash') \
   == "$narSri" ]]

# A binary cache: the `.narinfo` carries both fields, `NarHash:` from the
# upload's tee, and a copy back into a fresh store has the same object hash.
cache=$TEST_ROOT/cache
rm -rf "$cache"
nix copy --to "file://$cache" "$p"
narinfo=$cache/$(echo "$pBase" | cut -c1-32).narinfo
[[ -e $narinfo ]]
grepQuiet "^ObjectHash: git:sha256:$gitHex\$" "$narinfo"
grepQuiet "^NarHash: sha256:" "$narinfo"
grepQuiet "^NarSize: " "$narinfo"
[[ $(nix hash convert --hash-algo sha256 --to sri "$(grep '^NarHash: ' "$narinfo" | cut -d' ' -f2)") == "$narSri" ]]

store3=$TEST_ROOT/store3
rm -rf "$store3"
nix copy --from "file://$cache" --to "$store3" --no-check-sigs "$p"
[[ $(nix path-info --store "$store3" --json --json-format 4 "$p" | jq -r --arg b "$pBase" '.info[$b].objectHash') \
   == "git:sha256:$gitHex" ]]

# A cache written without `ObjectHash:` (an old cache) is still read: the
# object hash is computed from the NAR on arrival.
oldCache=$TEST_ROOT/old-cache
rm -rf "$oldCache"
cp -r "$cache" "$oldCache"
oldNarinfo=$oldCache/$(basename "$narinfo")
chmod u+w "$oldNarinfo"
grep -v '^ObjectHash: ' "$narinfo" > "$oldNarinfo"
grepQuietInverse "^ObjectHash: " "$oldNarinfo"
store4=$TEST_ROOT/store4
rm -rf "$store4"
nix copy --from "file://$oldCache" --to "$store4" --no-check-sigs "$p"
[[ $(nix path-info --store "$store4" --json --json-format 4 "$p" | jq -r --arg b "$pBase" '.info[$b].objectHash') \
   == "git:sha256:$gitHex" ]]

# Verification recomputes the object hash: clean, then a modified file is
# reported.
nix-store --verify --check-contents
nix store verify "$p"
chmod u+w "$p/d/b"
echo corrupt >> "$p/d/b"
chmod u-w "$p/d/b"
expectStderr 1 nix-store --verify --check-contents | grepQuiet "was modified"
# `nix store verify`: exit bit 1 is "corrupted" (src/nix/verify.cc).
expect 1 nix store verify "$p"
# Repaired from the cache written above, the store is clean again.
nix-store --verify --check-contents --repair \
    --option substituters "file://$cache" --option require-sigs false
nix-store --verify --check-contents
[[ $(cat "$p/d/b") == two ]]
[[ $(nix path-info --json --json-format 4 "$p" | jq -r --arg b "$pBase" '.info[$b].objectHash') \
   == "git:sha256:$gitHex" ]]

# The same modification on a schema-10 row (01 section 10, "a schema-10
# row's migration checks the NAR hash the row asserts"): the
# migration checks the NAR hash the row asserts on the walk that computes
# the object hash and, on a mismatch, writes nothing; a query answers from
# the row, every verifier reports "was modified" through the NAR hash as
# master's did, `nix store migrate` counts the path failed, `--optimise`
# does not enter it, and `--repair` restores it from the cache and the row
# is then migrated.  Before: the first query -- a verifier's included --
# wrote the object hash of the modified bytes into the column, and every
# verifier then passed the path.
if type -p sqlite3 > /dev/null; then
    sqlite3 "$db" "update ValidPaths set hash = 'sha256:$narHex' where path = '$p'"
    # The path's files as an older store holds them: not the object store's.
    find "$NIX_STORE_DIR/.objects/blobs" "$NIX_STORE_DIR/.objects/blobs-x" "$NIX_STORE_DIR/.objects/trees" -mindepth 1 -delete
    [[ $(stat --format=%h "$p/d/b") == 1 ]]
    chmod u+w "$p/d/b"
    printf 'xwo\n' > "$p/d/b"
    chmod u-w "$p/d/b"
    nix path-info "$p" > /dev/null
    [[ $(hashColumn "$p") == "sha256:$narHex" ]]
    expectStderr 1 nix-store --verify --check-contents | grepQuiet "was modified"
    [[ $(hashColumn "$p") == "sha256:$narHex" ]]
    expect 1 nix store verify "$p"
    expect 1 nix-store --verify-path "$p"
    expectStderr 1 nix store migrate | grepQuiet "was modified"
    [[ $(hashColumn "$p") == "sha256:$narHex" ]]
    nix-store --optimise 2> "$TEST_ROOT/optimise-modified.log"
    grepQuiet "was modified" "$TEST_ROOT/optimise-modified.log"
    [[ $(hashColumn "$p") == "sha256:$narHex" ]]
    [[ $(stat --format=%h "$p/d/b") == 1 ]]
    nix-store --verify --check-contents --repair \
        --option substituters "file://$cache" --option require-sigs false
    [[ $(cat "$p/d/b") == two ]]
    [[ $(hashColumn "$p") == "git:sha256:$gitHex" ]]
    nix-store --verify --check-contents
fi

# Signatures an old client can verify.  `$p` is content-addressed and so
# verifies with no signature at all (`checkSignatures` returns `maxSigs`);
# only an input-addressed path discriminates.  Signed under one key it
# carries two signatures, and verifies under that key -- locally, and
# from the cache, which carries both `Sig:` lines -- and not without it.
ia=$(nix-build simple.nix --no-out-link)
iaBase=$(basename "$ia")
nix store sign --key-file "$TEST_ROOT"/sk1 "$ia"
nix path-info --json --json-format 4 "$ia" | jq -e --arg b "$iaBase" \
    '.info[$b].signatures | length == 2 and all(.keyName == "cache1.example.org") and (map(.sig) | unique | length == 2)'
nix store verify "$ia" --sigs-needed 1 --trusted-public-keys "$pk1"
expect 2 nix store verify "$ia" --sigs-needed 1
nix copy --to "file://$cache" "$ia"
iaNarinfo=$cache/$(echo "$iaBase" | cut -c1-32).narinfo
[[ $(grep -c '^Sig: cache1.example.org:' "$iaNarinfo") == 2 ]]
grepQuiet "^ObjectHash: git:sha256:" "$iaNarinfo"
grepQuiet "^NarHash: sha256:" "$iaNarinfo"
nix store verify --store "file://$cache" "$ia" --sigs-needed 1 --trusted-public-keys "$pk1"
expect 2 nix store verify --store "file://$cache" "$ia" --sigs-needed 1

# Signing a cache written without `ObjectHash:` (an old cache): `nix store
# sign` against it knows the NAR hash and not the object hash, so it makes
# the version-1 signature -- one `Sig:` line -- and no other, which verifies
# under that key alone, for the new Nix here and for the old client below.
nix-store --generate-binary-cache-key cache2.example.org "$TEST_ROOT"/sk2 "$TEST_ROOT"/pk2
pk2=$(cat "$TEST_ROOT"/pk2)
rm -rf "$oldCache"
cp -r "$cache" "$oldCache"
oldIaNarinfo=$oldCache/$(basename "$iaNarinfo")
chmod u+w "$oldIaNarinfo"
grep -v -e '^ObjectHash: ' -e '^Sig: ' "$iaNarinfo" > "$oldIaNarinfo"
grepQuietInverse "^ObjectHash: " "$oldIaNarinfo"
grepQuietInverse "^Sig: " "$oldIaNarinfo"
grepQuiet "^NarHash: sha256:" "$oldIaNarinfo"
# Unsigned, it verifies under no key.
expect 2 nix store verify --store "file://$oldCache" --sigs-needed 1 --trusted-public-keys "$pk1 $pk2" "$ia"
nix store sign --store "file://$oldCache" --key-file "$TEST_ROOT"/sk2 "$ia"
[[ $(grep -c '^Sig: ' "$oldIaNarinfo") == 1 ]]
grepQuiet "^Sig: cache2.example.org:" "$oldIaNarinfo"
grepQuietInverse "^ObjectHash: " "$oldIaNarinfo"
nix store verify --store "file://$oldCache" --sigs-needed 1 --trusted-public-keys "$pk2" "$ia"
expect 2 nix store verify --store "file://$oldCache" --sigs-needed 1 --trusted-public-keys "$pk1" "$ia"

# A version-1 signature outlives substitution (01 section 9.11,
# "Signatures"; path-info.cc `checkSignatures`, local-store.cc
# `addToStore`).  `$ia` copied from the old-style cache -- `NarHash:` and
# cache2's version-1 `Sig:` alone -- into a store that requires signatures
# is admitted (the cache asserts the NAR hash the signature is over), and
# its row then holds the object hash and no NAR hash.  From there: `nix
# store verify` gets the NAR hash by one walk of the path and the signature
# verifies; a copy on to a second store that requires signatures is
# admitted once its stream is hashed; under a key that did not sign, both
# refuse.  Before: `verify` reported the path untrusted (exit 2) and the
# copy was refused ("lacks a signature by a trusted key").
store5=$TEST_ROOT/store5
rm -rf "$store5"
nix copy --from "file://$oldCache" --to "$store5" --trusted-public-keys "$pk2" "$ia"
nix path-info --store "$store5" --json --json-format 4 "$ia" | jq -e --arg b "$iaBase" \
    '.info[$b] | (has("narHash") | not) and (.signatures | length == 1) and (.signatures[0].keyName == "cache2.example.org")'
nix store verify --store "$store5" --sigs-needed 1 --trusted-public-keys "$pk2" "$ia"
expect 2 nix store verify --store "$store5" --sigs-needed 1 --trusted-public-keys "$pk1" "$ia"
store6=$TEST_ROOT/store6
rm -rf "$store6"
nix copy --from "$store5" --to "$store6" --trusted-public-keys "$pk2" "$ia"
nix path-info --store "$store6" --json --json-format 4 "$ia" | jq -e --arg b "$iaBase" \
    '.info[$b] | (has("narHash") | not) and (.signatures | length == 1)'
nix store verify --store "$store6" --sigs-needed 1 --trusted-public-keys "$pk2" "$ia"
store7=$TEST_ROOT/store7
rm -rf "$store7"
expectStderr 1 nix copy --from "$store5" --to "$store7" --trusted-public-keys "$pk1" "$ia" \
    | grepQuiet "lacks a signature by a trusted key"
[[ ! -e "$store7$NIX_STORE_DIR/$iaBase" ]]

# `nix-store --dump-db` writes the object hash's rendering, and `--load-db`
# reads a dump an older Nix wrote as well (01 section 9.11, "JSON and
# text"): a record carrying a NAR hash and no object hash is walked, the NAR
# hash checked on the tee that computes the object hash, as the migration
# checks a row.  A record whose path does not hash to it is reported ("was
# modified!", both hashes) and skipped, every other record is registered,
# and the command exits 1 -- per record, as `nix store migrate` reports.
# Before: the first mismatch threw ("NAR hash mismatch for path") before
# anything was registered, so one bad record lost the whole load.  A record
# this Nix wrote carries the object hash and is registered as written.
echo "load one $RANDOM$RANDOM" > "$TEST_ROOT/load1"
echo "load two $RANDOM$RANDOM" > "$TEST_ROOT/load2"
echo "load three $RANDOM$RANDOM" > "$TEST_ROOT/load3"
l1=$(nix store add --mode nar "$TEST_ROOT/load1")
l2=$(nix store add --mode nar "$TEST_ROOT/load2")
l3=$(nix store add --mode nar "$TEST_ROOT/load3")
nix-store --dump-db "$l1" > "$TEST_ROOT/new.dump"
[[ $(sed -n 2p "$TEST_ROOT/new.dump") == "git:sha256:$(nix hash path --mode git --algo sha256 --format base16 "$l1")" ]]
# The record master's `--dump-db` wrote: the NAR hash in bare base-16
# (`narHash.to_string(HashFormat::Base16, false)`), the NAR size, no deriver,
# no references.  The second argument, when given, is the path whose NAR
# hash the record asserts instead of its own: a valid hash, and wrong.
oldRecord() {
    printf '%s\n%s\n%s\n\n0\n' "$1" \
        "$(nix hash path --mode nar --algo sha256 --format base16 "${2:-$1}")" \
        "$(nix path-info --json --json-format 2 "$1" | jq -r '.info[].narSize')"
}
{ oldRecord "$l1"; oldRecord "$l2" "$l3"; oldRecord "$l3"; } > "$TEST_ROOT/old.dump"
(
    # The same store directory under an empty database: what `--load-db`
    # restores into.
    export NIX_STATE_DIR=$TEST_ROOT/load-db-state
    rm -rf "$NIX_STATE_DIR"
    expect 1 nix-store --load-db < "$TEST_ROOT/old.dump" 2> "$TEST_ROOT/load-db.log"
    grepQuiet "path '$l2' was modified! expected hash 'sha256:$(nix hash path --mode nar --algo sha256 --format nix32 "$l3")', got 'sha256:$(nix hash path --mode nar --algo sha256 --format nix32 "$l2")'" "$TEST_ROOT/load-db.log"
    grepQuiet "1 path was not registered" "$TEST_ROOT/load-db.log"
    nix-store --check-validity "$l1" "$l3"
    expect 1 nix-store --check-validity "$l2"
    # The rows the walk registered hold the object hash.
    [[ $(nix-store --query --hash "$l1") == "git:sha256:$(nix hash path --mode git --algo sha256 --format base16 "$l1")" ]]
    # A dump this Nix wrote loads as written.
    rm -rf "$NIX_STATE_DIR"
    nix-store --load-db < "$TEST_ROOT/new.dump"
    nix-store --check-validity "$l1"
    [[ $(nix-store --dump-db "$l1") == "$(cat "$TEST_ROOT/new.dump")" ]]
)

# Interop: an old client (master's `nix`, given through NIX_REFERENCE_BIN)
# against the new daemon gets a NAR hash equal to the shim's, and verifies
# the signed path from the cache under fingerprint 1.
if [[ -n "${NIX_REFERENCE_BIN:-}" && -x "$NIX_REFERENCE_BIN/nix" ]]; then
    # A git-named source, as every source this Nix adds is named (01 section
    # 9.9): master parses `fixed:git:` only under its `git-hashing` feature
    # (`8aad447f0:src/libstore/content-address.cc:193`), so an older peer
    # refuses such a path's description -- from the daemon, from a cache, or
    # from a new client -- unless that feature is set on the old side.  Stated
    # in the release note; pinned here in both directions, without
    # the feature and with it.
    pGit=$(nix store add --mode git "$src")
    nix copy --to "file://$cache" "$pGit"
    gitHashingDisabled="experimental Nix feature 'git-hashing' is disabled"
    startDaemon
    # The harness points the dynamic loader at this build's libraries; the
    # reference binary must find its own.
    expectStderr 1 env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command path-info --json "$pGit" \
        | grepQuiet "$gitHashingDisabled"
    env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features "nix-command git-hashing" path-info --json "$pGit" \
        | grepQuiet '"ca":"fixed:git:sha256:'
    # The same for the cache this Nix wrote: the old client's narinfo parser.
    expectStderr 1 env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command path-info --store "file://$cache" --json "$pGit" \
        | grepQuiet "$gitHashingDisabled"
    env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features "nix-command git-hashing" path-info --store "file://$cache" --json "$pGit" \
        | grepQuiet '"ca":"fixed:git:sha256:'
    refInfo=$(env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command path-info --json "$p")
    refNarHash=$(echo "$refInfo" | jq -r \
        'if type == "array" then .[0].narHash
         elif has("info") then (.info | to_entries[0].value.narHash)
         else (to_entries[0].value.narHash) end')
    [[ $refNarHash == "$narSri" ]]
    # And the new client through the same daemon still sees the object hash.
    [[ $(nix path-info --json --json-format 4 "$p" | jq -r --arg b "$pBase" '.info[$b].objectHash') \
       == "git:sha256:$gitHex" ]]
    # The old client reads the cache's `.narinfo` (its parser ignores
    # `ObjectHash:`) and verifies the input-addressed path under
    # fingerprint 1 with the key, and refuses it without.
    env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command store verify \
        --store "file://$cache" --sigs-needed 1 --trusted-public-keys "$pk1" "$ia"
    expect 2 env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command store verify \
        --store "file://$cache" --sigs-needed 1 "$ia"
    # And the version-1 signature the new Nix put on the old-style cache
    # (no `ObjectHash:`) is one the old client accepts, under that key alone.
    env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command store verify \
        --store "file://$oldCache" --sigs-needed 1 --trusted-public-keys "$pk2" "$ia"
    expect 2 env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command store verify \
        --store "file://$oldCache" --sigs-needed 1 --trusted-public-keys "$pk1" "$ia"
    killDaemon

    # The other direction: this client against the OLD daemon (the
    # reference `nix daemon`, on a fresh store root, since it refuses a
    # schema-11 store).  That daemon describes a path by its NAR hash
    # alone, so what needs the object hash of a path it holds -- a `path:`
    # input, a `file` input, `builtins.path` -- computes it by one walk of
    # the object (`Store::queryObjectHash`, store-api.cc).  Before that
    # every such fetch failed with "has no object hash" (path.cc,
    # tarball.cc).  The sources are git-addressed, so the old daemon needs
    # its `git-hashing` feature to store them: without it, `AddToStore`
    # refuses the address, checked first; then the daemon is
    # started with the feature.
    oldRoot=$TEST_ROOT/old-daemon
    rm -rf "$oldRoot"
    mkdir -p "$oldRoot/store" "$oldRoot/var/nix" "$oldRoot/var/log/nix"
    srcTreeHash=$(nix hash path --mode git --algo sha256 --format sri "$src")
    plainBlobHash=$(nix hash path --mode git --algo sha256 --format sri "$TEST_ROOT/plain")
    (
        export NIX_STORE_DIR=$oldRoot/store NIX_STATE_DIR=$oldRoot/var/nix NIX_LOG_DIR=$oldRoot/var/log/nix \
            NIX_DAEMON_SOCKET_PATH=$oldRoot/socket
        oldPid=
        # The reference daemon does not always exit on SIGTERM (it loops on
        # "interrupted by the user" after a refused operation): poll, then kill.
        stopOldDaemon() {
            kill "$oldPid" 2> /dev/null || true
            for ((i = 0; i < 50; i++)); do
                kill -0 "$oldPid" 2> /dev/null || break
                sleep 0.1
            done
            kill -9 "$oldPid" 2> /dev/null || true
            wait "$oldPid" 2> /dev/null || true
            oldPid=
        }
        trap 'if [[ -n $oldPid ]]; then stopOldDaemon; fi' EXIT
        startOldDaemon() {
            rm -f "$NIX_DAEMON_SOCKET_PATH"
            # `NIX_REMOTE` is set to `daemon` below for the client; the daemon
            # itself must open the local store, not a connection to itself.
            NIX_REMOTE= NIX_CONFIG="extra-experimental-features = $1" \
                env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
                "$NIX_REFERENCE_BIN/nix" --extra-experimental-features nix-command daemon &
            oldPid=$!
            for ((i = 0; i < 100; i++)); do
                [[ -S $NIX_DAEMON_SOCKET_PATH ]] && break
                kill -0 "$oldPid" || fail "the reference daemon died"
                sleep 0.1
            done
            [[ -S $NIX_DAEMON_SOCKET_PATH ]] || fail "the reference daemon did not start"
        }
        export NIX_REMOTE=daemon

        # Without the feature the old daemon refuses the git-named source at
        # `AddToStore`; with it, everything below.
        startOldDaemon "nix-command"
        expectStderr 1 nix eval --impure --raw --expr "builtins.path { path = \"$src\"; }" \
            | grepQuiet "$gitHashingDisabled"
        stopOldDaemon
        startOldDaemon "nix-command git-hashing"

        # `path:`: the tree, named by its tree hash, which the walk gives.
        pathOut=$(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).outPath")
        [[ $(cat "$pathOut/a") == one ]]
        [[ -x $pathOut/x ]]
        [[ $(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"path\"; path = \"$src\"; }).treeHash") \
           == "$srcTreeHash" ]]
        # `file`: a plain file, named by its blob id.
        fileOut=$(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"file\"; url = \"file://$TEST_ROOT/plain\"; }).outPath")
        [[ $(cat "$fileOut") == hello ]]
        [[ $(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"file\"; url = \"file://$TEST_ROOT/plain\"; }).treeHash") \
           == "$plainBlobHash" ]]
        # `builtins.path` (named `src`, so another path than `source` above,
        # of the same tree).
        bpOut=$(nix eval --impure --raw --expr "builtins.path { path = \"$src\"; }")
        [[ $(cat "$bpOut/d/b") == two ]]
        [[ $(nix hash path --mode git --algo sha256 --format sri "$bpOut") == "$srcTreeHash" ]]
        # What the old daemon holds is described by its NAR hash alone:
        # format 2 shows it, and format 4 refuses with the message that
        # names the alternative (path-info.cc, `toJSON`).
        [[ $(nix path-info --json --json-format 2 "$pathOut" | jq -r '.info | to_entries[0].value.narHash') \
           == "$(nix hash path --mode nar --format sri "$pathOut")" ]]
        expectStderr 1 nix path-info --json --json-format 4 "$pathOut" | grepQuiet "use a lower format"
        # What this client *sends* the old daemon as a path info -- a
        # derivation from the write buffer, an imported path -- carries the
        # NAR hash the old form needs, computed by the one wire writer from
        # the description's lazy hash (`CommonProto::writePathInfoHashes`);
        # no producer asks the daemon's version.
        drv=$(nix-instantiate --expr "derivation { name = \"interop\"; system = \"$system\"; builder = \"/bin/sh\"; }")
        [[ $(nix path-info --json --json-format 2 "$drv" | jq -r '.info | to_entries[0].value.narHash') \
           == "$(nix hash path --mode nar --format sri "$drv")" ]]
        nix-store --export "$pathOut" > "$TEST_ROOT/interop.export"
        nix-store --import < "$TEST_ROOT/interop.export" | grepQuiet "$(basename "$pathOut")"
    )
else
    echo "NIX_REFERENCE_BIN not set; skipping the old-client interop check" >&2
fi
