#!/usr/bin/env bash

# A path substituted from a cache that describes it by its NAR hash alone --
# a cache an older Nix wrote: `NarHash:`, no `ObjectHash:`, one version-1
# `Sig:` -- copied on to a daemon that requires signatures, by a user the
# daemon does not trust.
#
# The row the substitution leaves holds the object hash and no NAR hash
# (doc/lazy-store/01-specification.md section 9.11, *Signatures*;
# doc/manual/rl-next/one-address.md, *Signatures*), so the version-1
# signature the sender carries is over a NAR hash that is in nobody's hand
# when the description arrives.  The receiving store hashes the stream it is
# already reading and verifies the signature against that
# (`LocalStore::addToStore`, src/libstore/local-store.cc), which is what makes
# the copy succeed.  The unit form of this is
# `LocalStoreObjectsTest.versionOneSignatureWithoutAssertedNarHashIsCheckedOnTheStream`
# and the local-store-to-local-store form is in `object-hash.sh` (store5 to
# store6); what is driven here is the copy through a daemon with an untrusted
# client, where the daemon and not the client decides to check.

source common.sh

TODO_NixOS

needLocalStore "the daemon is this test's own, and the second store is a local one"

clearStore

nix-store --generate-binary-cache-key cache1.example.org "$TEST_ROOT/sk1" "$TEST_ROOT/pk1"
pk1=$(cat "$TEST_ROOT/pk1")

# Two paths: one this test signs, one it leaves unsigned as the control.
signed=$(nix-build simple.nix --no-out-link)
unsigned=$(nix-build dependencies.nix --no-out-link)
nix store sign --key-file "$TEST_ROOT/sk1" "$signed"

# `nix store sign` on a local store makes both fingerprints' signatures.
nix path-info --json --json-format 4 "$signed" | jq -e --arg b "$(basename "$signed")" \
    '.info[$b].signatures | length == 2 and all(.keyName == "cache1.example.org")'

cache=$TEST_ROOT/narhash-only-cache
rm -rf "$cache"
nix copy --to "file://$cache" "$signed" "$unsigned"

# The cache an older Nix would have written: no `ObjectHash:`, no `Sig:`, and
# then one version-1 signature, which is all `nix store sign` can make against
# a cache whose descriptions carry a NAR hash and no object hash.
for p in "$signed" "$unsigned"; do
    narinfo=$cache/$(basename "$p" | cut -c1-32).narinfo
    [[ -e $narinfo ]]
    chmod u+w "$narinfo"
    grep -v -e '^ObjectHash: ' -e '^Sig: ' "$narinfo" > "$narinfo.old"
    mv "$narinfo.old" "$narinfo"
    grepQuietInverse '^ObjectHash: ' "$narinfo"
    grepQuietInverse '^Sig: ' "$narinfo"
    grepQuiet '^NarHash: sha256:' "$narinfo"
done
nix store sign --store "file://$cache" --key-file "$TEST_ROOT/sk1" "$signed"
signedNarinfo=$cache/$(basename "$signed" | cut -c1-32).narinfo
[[ $(grep -c '^Sig: ' "$signedNarinfo") == 1 ]]
grepQuiet '^Sig: cache1.example.org:' "$signedNarinfo"
grepQuietInverse '^ObjectHash: ' "$signedNarinfo"

# The second store, filled by substitution from that cache: `nix-store
# --realise` with the cache as the only substituter, the signed path under the
# key and the control without a signature check.
storeB=$TEST_ROOT/substituted-store
chmod -R u+w "$storeB" 2> /dev/null || true
rm -rf "$storeB"
nix-store --store "$storeB" --realise "$signed" \
    --option substitute true \
    --option substituters "file://$cache" \
    --option trusted-public-keys "$pk1"
nix-store --store "$storeB" --realise "$unsigned" \
    --option substitute true \
    --option substituters "file://$cache" \
    --option require-sigs false
nix-store --store "$storeB" --check-validity "$signed" "$unsigned"

# What the substitution left: the object hash, no NAR hash, and the one
# version-1 signature.
nix path-info --store "$storeB" --json --json-format 4 "$signed" \
    | jq -e --arg b "$(basename "$signed")" \
    '.info[$b] | (has("narHash") | not) and (.objectHash | startswith("git:sha256:"))
     and (.signatures | length == 1) and (.signatures[0].keyName == "cache1.example.org")'
nix path-info --store "$storeB" --json --json-format 4 "$unsigned" \
    | jq -e --arg b "$(basename "$unsigned")" '.info[$b].signatures == []'

# The daemon: it requires signatures, it trusts that key, and it does not
# trust this user, so it is the daemon that decides to check (a trusted client
# may ask for `--no-check-sigs`; an untrusted one may not).
sed -i -e '/^trusted-users =/d' "$test_nix_conf"
cat >> "$test_nix_conf" <<EOF
require-sigs = true
trusted-public-keys = $pk1
EOF

# The daemon's store must not already hold the paths, or the copy is a no-op.
clearStore
startDaemon

# The claim: the copy succeeds, on a description whose version-1 signature has
# no NAR hash beside it.
nix copy --from "$storeB" "$signed"
nix-store --check-validity "$signed"
[[ $(cat "$signed") == "$(cat "$storeB$signed")" ]]
nix path-info --json --json-format 4 "$signed" | jq -e --arg b "$(basename "$signed")" \
    '.info[$b].signatures | length == 1 and all(.keyName == "cache1.example.org")'

# The control: the same route for a path with no signature at all is refused,
# so the acceptance above is the signature's and not the check's absence.
expectStderr 1 nix copy --from "$storeB" "$unsigned" > "$TEST_ROOT/unsigned-copy.log"
grepQuiet 'lacks a signature by a trusted key' "$TEST_ROOT/unsigned-copy.log"
expect 1 nix-store --check-validity "$unsigned"

killDaemon
