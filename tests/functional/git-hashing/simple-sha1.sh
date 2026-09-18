#!/usr/bin/env bash

# The git content-address method admits SHA-256 only (doc/lazy-store/
# 01-specification.md, section 9.9 "The algorithm"; section 10, "The git
# method is SHA-256 only").
# This file was the SHA-1 twin of simple-sha256.sh.  It now pins the refusal
# of the SHA-1 form at every entry that creates a git-addressed object or
# name, and what happens to data an older Nix made under that form.

source common.sh

refused='the git content-address method admits SHA-256 only'

rm -rf "$TEST_ROOT/hash-path"
mkdir "$TEST_ROOT/hash-path"
echo "Hello World" > "$TEST_ROOT/hash-path/hello"

# `nix hash path --mode git --algo sha1` (src/nix/hash.cc).
expectStderr 1 nix hash path --mode git --algo sha1 "$TEST_ROOT/hash-path" | grepQuiet "$refused"
# SHA-256 is the method's algorithm, named or not.
[[ $(nix hash path --mode git --algo sha256 --format base16 "$TEST_ROOT/hash-path") \
    == $(nix hash path --mode git --format base16 "$TEST_ROOT/hash-path") ]]

# `nix store add --mode git --hash-algo sha1`: the store's entry
# (`Store::addToStoreSlow`) and the dry run's (`computeStorePath` over
# `hashPath`).
expectStderr 1 nix store add --mode git --hash-algo sha1 "$TEST_ROOT/hash-path" | grepQuiet "$refused"
expectStderr 1 nix store add --dry-run --mode git --hash-algo sha1 "$TEST_ROOT/hash-path" | grepQuiet "$refused"
p=$(nix store add --mode git --hash-algo sha256 "$TEST_ROOT/hash-path")
[[ $(nix path-info --json --json-format 2 "$p" | jq -r '.info.[].ca.hash') == sha256-* ]]

# A fixed-output derivation with `outputHashMode = "git"` and a SHA-1 hash
# is refused at instantiation (primops.cc); the SHA-256 one instantiates.
expectStderr 1 nix-instantiate ../fixed.nix -A git-sha1 | grepQuiet "$refused"
nix-instantiate ../fixed.nix -A git-sha256 > /dev/null

# A derivation that names the form and reached the store by another route
# (`nix derivation add`, or an older Nix) is read -- nothing that reads a
# derivation refuses it, since the collector reads every derivation it
# visits under `ca-derivations` -- and refused when it is built
# (derivation-builder-impl.cc), before its output is hashed.  The hash is
# git's SHA-1 tree id of what ../fixed.builder2.sh produces, so that the
# build succeeded under the older Nix.
drv=$(nix derivation add <<EOF
{ "name": "fixed-git-sha1", "version": 4, "system": "$system", "builder": "$SHELL",
  "args": ["-c", "mkdir \$out \$out/bla; echo 'Hello World!' > \$out/foo; ln -s foo \$out/bar"],
  "env": { "PATH": "$coreutils" }, "inputs": { "drvs": {}, "srcs": [] },
  "outputs": { "out": { "method": "git", "hash": "sha1-zUS682kV1d7IN0Iy6n4gV/O0SU4=" } } }
EOF
)
nix derivation show "$drv" | jq -e '.derivations[].outputs.out.hash == "sha1-zUS682kV1d7IN0Iy6n4gV/O0SU4="'
expectStderr 1 nix-build "$drv" --no-out-link | grepQuiet "$refused"

# Old data.  A row an older Nix made under the SHA-1 form is read, verified
# and collected -- the collector reads every row it visits (gc.cc,
# `topoSortPaths`), so the reader of a content address does not refuse the
# form -- and is not copied on, since the receiving store cannot check the
# address (01 section 10, "The git method is SHA-256 only").  The row is
# planted: no Nix this test can drive writes one.
if type -p sqlite3 > /dev/null; then
    db=$NIX_STATE_DIR/db/db.sqlite
    sha1Nix32=$(nix hash convert --hash-algo sha1 --to nix32 e5c0a11a556801a5c9dcf330ca9d7e2c572697f4)
    sqlite3 "$db" "update ValidPaths set ca = 'fixed:git:sha1:$sha1Nix32' where path = '$p'"

    # Read: the row parses and the address is shown as the row holds it.
    [[ $(nix path-info --json --json-format 2 "$p" | jq -r '.info.[].ca | .method + " " + .hash') == "git sha1-"* ]]
    # Verified: the object hash is what the row is checked against.  (`nix
    # store verify` also asks whether the address names the path, which a
    # planted address does not; a row an older Nix wrote passes that too.)
    nix-store --verify --check-contents
    nix-store --verify-path "$p"
    # Collected: rooted, the row is visited and the path kept; unrooted, deleted.
    nix-store --add-root "$TEST_ROOT/sha1-root" -r "$p" > /dev/null
    nix-store --gc
    nix path-info "$p" > /dev/null
    rm "$TEST_ROOT/sha1-root"
    nix-store --gc
    expect 1 nix path-info "$p"
else
    echo "sqlite3 not on PATH; skipping the planted-row checks" >&2
fi

# The form on a cache an older Nix wrote (master's `nix`, given through
# NIX_REFERENCE_BIN, under its `git-hashing` feature): the description is
# read, and the copy into this Nix's store is refused with the remedy.
if [[ -n "${NIX_REFERENCE_BIN:-}" && -x "$NIX_REFERENCE_BIN/nix" ]]; then
    ref() {
        # The loader environment is the branch's; the reference binary must
        # find its own.
        env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
            "$NIX_REFERENCE_BIN/nix" --extra-experimental-features 'nix-command git-hashing' "$@"
    }
    oldStore=$TEST_ROOT/old-store
    oldCache=$TEST_ROOT/old-cache
    rm -rf "$oldStore" "$oldCache"
    oldPath=$(ref store add --store "$oldStore" --mode git --hash-algo sha1 "$TEST_ROOT/hash-path")
    ref copy --from "$oldStore" --to "file://$oldCache" "$oldPath"
    grepQuiet '^CA: fixed:git:sha1:' "$oldCache"/*.narinfo
    nix path-info --store "file://$oldCache" --json --json-format 2 "$oldPath" \
        | jq -e '.info.[].ca.method == "git" and (.info.[].ca.hash | startswith("sha1-"))'
    expectStderr 1 nix copy --from "file://$oldCache" --to "$TEST_ROOT/new-store" "$oldPath" | grepQuiet "$refused"
    [[ ! -e "$TEST_ROOT/new-store$oldPath" ]]
else
    echo "NIX_REFERENCE_BIN not set; skipping the old-cache check" >&2
fi
