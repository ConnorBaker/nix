#!/usr/bin/env bash

source common.sh

nix-store --generate-binary-cache-key cache1.example.org "$TEST_ROOT"/sk1 "$TEST_ROOT"/pk1
pk1=$(cat "$TEST_ROOT"/pk1)
nix-store --generate-binary-cache-key cache2.example.org "$TEST_ROOT"/sk2 "$TEST_ROOT"/pk2
pk2=$(cat "$TEST_ROOT"/pk2)

# Build a path.
outPath=$(nix-build dependencies.nix --no-out-link --secret-key-files "$TEST_ROOT/sk1 $TEST_ROOT/sk2")

# Verify that the path got signed.  Signing at registration is under
# fingerprint 2 only (no NAR hash is in hand there): one signature per
# key.  `nix store sign` and the binary-cache upload add a fingerprint-1
# signature beside it; see below.
info=$(nix path-info --json --json-format 2 "$outPath")
echo "$info" | jq -e '.info.[] | .ultimate == true'
TODO_NixOS # looks like an actual bug? Following line fails on NixOS:
echo "$info" | jq -e '.info.[] | .signatures.[] | select(startswith("cache1.example.org"))'
echo "$info" | jq -e '.info.[] | .signatures.[] | select(startswith("cache2.example.org"))'
echo "$info" | jq -e '.info.[] | .signatures | length == 2'

# Test "nix store verify".
nix store verify -r "$outPath"

expect 2 nix store verify -r "$outPath" --sigs-needed 1

nix store verify -r "$outPath" --sigs-needed 1 --trusted-public-keys "$pk1"

expect 2 nix store verify -r "$outPath" --sigs-needed 2 --trusted-public-keys "$pk1"

nix store verify -r "$outPath" --sigs-needed 2 --trusted-public-keys "$pk1 $pk2"

nix store verify --all --sigs-needed 2 --trusted-public-keys "$pk1 $pk2"

# Build something unsigned.
outPath2=$(nix-build simple.nix --no-out-link)

nix store verify -r "$outPath"

# Verify that the path did not get signed but does have the ultimate bit.
info=$(nix path-info --json --json-format 2 "$outPath2")
echo "$info" | jq -e '.info.[] | .ultimate == true'
echo "$info" | jq -e '.info.[] | .signatures == []'

# Test "nix store verify".
nix store verify -r "$outPath2"

expect 2 nix store verify -r "$outPath2" --sigs-needed 1

expect 2 nix store verify -r "$outPath2" --sigs-needed 1 --trusted-public-keys "$pk1"

# Test "nix store sign": one key, two signatures (both fingerprints).
nix store sign --key-file "$TEST_ROOT"/sk1 "$outPath2"

nix store verify -r "$outPath2" --sigs-needed 1 --trusted-public-keys "$pk1"
info=$(nix path-info --json --json-format 2 "$outPath2")
echo "$info" | jq -e '.info.[] | .signatures | length == 2 and all(startswith("cache1.example.org:"))'
echo "$info" | jq -e '.info.[] | .signatures | unique | length == 2'

# Build something content-addressed.
outPathCA=$(IMPURE_VAR1=foo IMPURE_VAR2=bar nix-build ./fixed.nix -A good.0 --no-out-link)

nix path-info --json --json-format 2 "$outPathCA" | jq -e '.info.[].ca | .method == "flat" and (.hash | startswith("md5-"))'

# Content-addressed paths don't need signatures, so they verify
# regardless of --sigs-needed.
nix store verify "$outPathCA"
nix store verify "$outPathCA" --sigs-needed 1000

# Check that signing a content-addressed path doesn't overflow validSigs
nix store sign --key-file "$TEST_ROOT"/sk1 "$outPathCA"
nix store verify -r "$outPathCA" --sigs-needed 1000 --trusted-public-keys "$pk1"

# Copy to a binary cache.
nix copy --to file://"$cacheDir" "$outPath2"

# Verify that signatures got copied: both of cache1's, none of cache2's.
info=$(nix path-info --store file://"$cacheDir" --json --json-format 2 "$outPath2")
echo "$info" | jq -e '.info.[] | .ultimate == false'
echo "$info" | jq -e '.info.[] | .signatures.[] | select(startswith("cache1.example.org"))'
echo "$info" | jq -e '.info.[] | [.signatures.[] | select(startswith("cache1.example.org:"))] | length == 2'
echo "$info" | expect 4 jq -e '.info.[] | .signatures.[] | select(startswith("cache2.example.org"))'

# The `.narinfo` carries both of cache1's signatures (a client that knows
# only fingerprint 1 verifies one of them), and both verify here.
for file in "$cacheDir"/*.narinfo; do
    [[ $(grep -c '^Sig: cache1.example.org:' "$file") == 2 ]]
done
nix store verify --store file://"$cacheDir" "$outPath2" --sigs-needed 1 --trusted-public-keys "$pk1"
# `--sigs-needed` counts keys ("signed by at least *n* different keys"):
# two signatures from one key are one key.
expect 2 nix store verify --store file://"$cacheDir" "$outPath2" --sigs-needed 2 --trusted-public-keys "$pk1"

# Verify that adding a signature to a path in a binary cache works.
nix store sign --store file://"$cacheDir" --key-file "$TEST_ROOT"/sk2 "$outPath2"
info=$(nix path-info --store file://"$cacheDir" --json --json-format 2 "$outPath2")
echo "$info" | jq -e '.info.[] | .signatures.[] | select(startswith("cache1.example.org"))'
echo "$info" | jq -e '.info.[] | .signatures.[] | select(startswith("cache2.example.org"))'
echo "$info" | jq -e '.info.[] | [.signatures.[] | select(startswith("cache2.example.org:"))] | length == 2'
echo "$info" | jq -e '.info.[] | .signatures | length == 4'
nix store verify --store file://"$cacheDir" "$outPath2" --sigs-needed 2 --trusted-public-keys "$pk1 $pk2"

# Copying to a diverted store should fail due to a lack of signatures by trusted keys.
chmod -R u+w "$TEST_ROOT"/store0 || true
rm -rf "$TEST_ROOT"/store0

# Fails or very flaky only on GHA + macOS:
#     expectStderr 1 nix copy --to $TEST_ROOT/store0 $outPath | grepQuiet -E 'cannot add path .* because it lacks a signature by a trusted key'
# but this works:
(! nix copy --to "$TEST_ROOT"/store0 "$outPath")

# But succeed if we supply the public keys.
nix copy --to "$TEST_ROOT"/store0 "$outPath" --trusted-public-keys "$pk1"

expect 2 nix store verify --store "$TEST_ROOT"/store0 -r "$outPath"

nix store verify --store "$TEST_ROOT"/store0 -r "$outPath" --trusted-public-keys "$pk1"
nix store verify --store "$TEST_ROOT"/store0 -r "$outPath" --sigs-needed 2 --trusted-public-keys "$pk1 $pk2"

# It should also succeed if we disable signature checking.
(! nix copy --to "$TEST_ROOT"/store0 "$outPath2")
nix copy --to "$TEST_ROOT"/store0?require-sigs=false "$outPath2"

# But signatures should still get copied.
nix store verify --store "$TEST_ROOT"/store0 -r "$outPath2" --trusted-public-keys "$pk1"

# Content-addressed stuff can be copied without signatures.
nix copy --to "$TEST_ROOT"/store0 "$outPathCA"

# Test multiple signing keys: the upload signs under both fingerprints
# with each key (its fingerprint-2 signatures coincide with the ones made
# at the build), four `Sig:` lines per `.narinfo`.
nix copy --to "file://$TEST_ROOT/storemultisig?secret-keys=$TEST_ROOT/sk1,$TEST_ROOT/sk2" "$outPath"
for file in "$TEST_ROOT/storemultisig/"*.narinfo; do
    if [[ "$(grep -cE  '^Sig: cache[1,2]\.example.org:' "$file")" -ne 4 \
       || "$(grep -c '^Sig: cache1.example.org:' "$file")" -ne 2 \
       || "$(grep -c '^Sig: cache2.example.org:' "$file")" -ne 2 ]]; then
        echo "ERROR: expected two cache1.example.org and two cache2.example.org signatures in ${file}"
        cat "${file}"
        exit 1
    fi
done
