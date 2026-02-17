#!/usr/bin/env bash

# Impure eval cache dependency tracking tests: getEnv, currentSystem,
# currentTime, pathExists/readDir, pathExists true->false, shared thunks,
# file deletion, dir removal, readFileType, hashFile, addPath, filterSource.

source common.sh

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

testDir="$TEST_ROOT/eval-cache-impure"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: builtins.getEnv dep
###############################################################################
clearStoreIndex

cat >"$testDir/test-getenv.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  myVar = builtins.getEnv "MY_TEST_VAR";
in
{
  drv = mkDerivation {
    name = "getenv-cached";
    buildCommand = "echo '${myVar}' > $out";
  };
}
EOF

# Build with MY_TEST_VAR=hello
MY_TEST_VAR=hello nix build --no-link --impure -f "$testDir/test-getenv.nix" drv

# Same env var -> cache hit
MY_TEST_VAR=hello NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-getenv.nix" drv

# Different env var -> cache miss
MY_TEST_VAR=world NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-getenv.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 1 passed: builtins.getEnv dep"

###############################################################################
# Test 2: builtins.currentSystem dep
###############################################################################
clearStoreIndex

cat >"$testDir/test-system.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  sys = builtins.currentSystem;
in
{
  drv = mkDerivation {
    name = "system-cached";
    system = sys;
    buildCommand = "echo ${sys} > $out";
  };
}
EOF

# Build with default system
nix build --no-link --impure -f "$testDir/test-system.nix" drv

# Same system -> cache hit
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-system.nix" drv

# Different system -> cache miss
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-system.nix" drv --system i686-linux \
  | grepQuiet "not everything is cached"

echo "Test 2 passed: builtins.currentSystem dep"

###############################################################################
# Test 3: builtins.currentTime dep (always invalidates)
###############################################################################
clearStoreIndex

cat >"$testDir/test-currenttime.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  t = builtins.currentTime;
in
{
  drv = mkDerivation {
    name = "time-cached-${toString t}";
    buildCommand = "echo ${toString t} > $out";
  };
}
EOF

# Build first time
nix build --no-link --impure -f "$testDir/test-currenttime.nix" drv

# currentTime deps always invalidate
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-currenttime.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 3 passed: builtins.currentTime always invalidates"

###############################################################################
# Test 4: builtins.pathExists / builtins.readDir deps
###############################################################################
clearStoreIndex

mkdir -p "$testDir/subdir"
echo "file1" > "$testDir/subdir/a.txt"

cat >"$testDir/test-pathexists.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  exists = builtins.pathExists ./subdir/a.txt;
  dir = builtins.readDir ./subdir;
  listing = builtins.concatStringsSep "," (builtins.attrNames dir);
in
{
  drv = mkDerivation {
    name = "pathexists-cached";
    buildCommand = "echo '${toString exists} ${listing}' > $out";
  };
}
EOF

# Build first time
nix build --no-link --impure -f "$testDir/test-pathexists.nix" drv

# Should be cached
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-pathexists.nix" drv

# Add a new file to the directory -> readDir invalidates
echo "file2" > "$testDir/subdir/b.txt"

NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-pathexists.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 4 passed: pathExists/readDir deps"

###############################################################################
# Test 5: pathExists true-to-false transition
###############################################################################
clearStoreIndex

mkdir -p "$testDir/optdir"
echo "exists" > "$testDir/optdir/target.txt"

cat >"$testDir/test-pe-truetofalse.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  exists = builtins.pathExists ./optdir/target.txt;
in
{
  drv = mkDerivation {
    name = "pe-truetofalse";
    buildCommand = "echo '${toString exists}' > $out";
  };
}
EOF

# Build with file present
nix build --no-link --impure -f "$testDir/test-pe-truetofalse.nix" drv

# Cache hit
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-pe-truetofalse.nix" drv

# Remove the file — existence changed from true to false
rm "$testDir/optdir/target.txt"

# Cache should be invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-pe-truetofalse.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 5 passed: pathExists true-to-false transition"

###############################################################################
# Test 6: Multiple attributes with same file dep
###############################################################################
clearStoreIndex

echo "1.0" > "$testDir/version.txt"

# Each attribute calls readFile independently (not shared thunk)
# so each records its own Content dep for version.txt.
cat >"$testDir/test-shared-dep.nix" <<EOF
{
  a = "pkg-a-\${builtins.readFile $testDir/version.txt}";
  b = "pkg-b-\${builtins.readFile $testDir/version.txt}";
}
EOF

# Evaluate both attributes in same session (populates cache for both)
nix eval --impure -f "$testDir/test-shared-dep.nix" --json

# Both should be cached
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-shared-dep.nix" a
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-shared-dep.nix" b

# Change the dep
echo "2.0" > "$testDir/version.txt"

# Both cached values should be invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-shared-dep.nix" a \
    | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-shared-dep.nix" b \
    | grepQuiet "not everything is cached"

echo "Test 6 passed: multiple attributes with same file dep"

###############################################################################
# Test 7: File deletion invalidates readFile dep
###############################################################################
clearStoreIndex

echo "content-v1" > "$testDir/deletable.txt"

cat >"$testDir/test-file-delete.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  data = builtins.readFile ./deletable.txt;
in
{
  drv = mkDerivation {
    name = "file-delete-test";
    buildCommand = "echo '${data}' > $out";
  };
}
EOF

# Build to populate cache
nix build --no-link --impure -f "$testDir/test-file-delete.nix" drv

# Verify cache works
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-file-delete.nix" drv

# Delete the file — readFile dep should invalidate
rm "$testDir/deletable.txt"

# Cache should be invalidated since the file was deleted
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-file-delete.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 7 passed: file deletion invalidation"

###############################################################################
# Test 8: Directory entry removal invalidates readDir dep
###############################################################################
clearStoreIndex

mkdir -p "$testDir/rmdir-test"
echo "a" > "$testDir/rmdir-test/a.txt"
echo "b" > "$testDir/rmdir-test/b.txt"

cat >"$testDir/test-dir-remove.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  listing = builtins.concatStringsSep "," (builtins.attrNames (builtins.readDir ./rmdir-test));
in
{
  drv = mkDerivation {
    name = "dir-remove-test";
    buildCommand = "echo '${listing}' > $out";
  };
}
EOF

# Build to populate cache
nix build --no-link --impure -f "$testDir/test-dir-remove.nix" drv

# Verify cache works
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-dir-remove.nix" drv

# Remove a file from the directory — readDir dep should invalidate
rm "$testDir/rmdir-test/b.txt"

# Cache should be invalidated since directory listing changed
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-dir-remove.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 8 passed: directory entry removal"

###############################################################################
# Test 9: readFileType invalidation
###############################################################################
clearStoreIndex

echo "content" > "$testDir/typed-node"
cat >"$testDir/test-readfiletype.nix" <<'EOF'
builtins.readFileType ./typed-node
EOF

# Cold cache
[[ "$(nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"regular"' ]]

# Warm cache
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"regular"' ]]

# Replace file with directory
rm "$testDir/typed-node"
mkdir "$testDir/typed-node"

# Should invalidate and return "directory"
[[ "$(nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"directory"' ]]

# Warm cache after invalidation
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"directory"' ]]

echo "Test 9 passed: readFileType invalidation"

###############################################################################
# Test 10: hashFile invalidation
###############################################################################
clearStoreIndex

echo -n "hello" > "$testDir/hashme.txt"
cat >"$testDir/test-hashfile.nix" <<'EOF'
builtins.hashFile "sha256" ./hashme.txt
EOF

# Cold cache — get initial hash
result1="$(nix eval --json --impure -f "$testDir/test-hashfile.nix")"

# Warm cache — same hash
result2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hashfile.nix")"
[[ "$result1" == "$result2" ]]

# Modify file content
echo -n "world" > "$testDir/hashme.txt"

# Should invalidate and return new hash
result3="$(nix eval --json --impure -f "$testDir/test-hashfile.nix")"
[[ "$result1" != "$result3" ]]

# Warm cache after invalidation — same new hash
result4="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hashfile.nix")"
[[ "$result3" == "$result4" ]]

echo "Test 10 passed: hashFile invalidation"

###############################################################################
# Test 11: addPath (unfiltered) invalidation
###############################################################################
clearStoreIndex

mkdir -p "$testDir/addpath-src"
echo "v1" > "$testDir/addpath-src/file.txt"
cat >"$testDir/test-addpath.nix" <<'EOF'
builtins.path { path = ./addpath-src; name = "test-addpath"; }
EOF

# Cold cache
result1="$(nix eval --json --impure -f "$testDir/test-addpath.nix")"

# Warm cache — same store path
result2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-addpath.nix")"
[[ "$result1" == "$result2" ]]

# Modify file in source directory
echo "v2" > "$testDir/addpath-src/file.txt"

# Should invalidate and return new store path
result3="$(nix eval --json --impure -f "$testDir/test-addpath.nix")"
[[ "$result1" != "$result3" ]]

# Warm cache after invalidation
result4="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-addpath.nix")"
[[ "$result3" == "$result4" ]]

echo "Test 11 passed: addPath (unfiltered) invalidation"

###############################################################################
# Test 12: filterSource / filtered addPath invalidation (per-file deps)
###############################################################################
clearStoreIndex

mkdir -p "$testDir/filter-src"
echo "included v1" > "$testDir/filter-src/included.txt"
echo "excluded v1" > "$testDir/filter-src/excluded.log"
cat >"$testDir/test-filtersource.nix" <<'EOF'
builtins.path {
  path = ./filter-src;
  name = "test-filter";
  filter = path: type: !(builtins.match ".*\\.log" (toString path) != null);
}
EOF

# Cold cache
result1="$(nix eval --json --impure -f "$testDir/test-filtersource.nix")"

# Warm cache — same store path
result2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result1" == "$result2" ]]

# Modify included file -> should invalidate, new store path
echo "included v2" > "$testDir/filter-src/included.txt"
result3="$(nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result1" != "$result3" ]]

# Warm cache after invalidation
result4="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result3" == "$result4" ]]

# Modify only excluded file -> NO invalidation (no per-file dep recorded for it)
echo "excluded v2" > "$testDir/filter-src/excluded.log"
result5="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result3" == "$result5" ]]

# Add a new file to the directory -> invalidation (Directory dep listing changes)
echo "new file" > "$testDir/filter-src/new-file.txt"
result6="$(nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result3" != "$result6" ]]

# Warm cache after directory change
result7="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result6" == "$result7" ]]

echo "Test 12 passed: filterSource / filtered addPath invalidation (per-file deps)"

echo "All eval-cache-impure-deps tests passed!"
