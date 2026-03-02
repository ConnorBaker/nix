#!/usr/bin/env bash

# Impure eval trace dependency tracking tests: getEnv, currentSystem,
# currentTime, pathExists/readDir, pathExists true->false, shared dependency
# traces, file deletion, dir removal, readFileType, hashFile, addPath, filterSource.
# Each test verifies that oracle dependencies (Shake) are correctly recorded
# in traces and that trace verification (BSàlC) detects dependency changes.

source common.sh

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

testDir="$TEST_ROOT/eval-trace-impure"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: builtins.getEnv dependency in trace (Shake: environment oracle)
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

# Record trace with MY_TEST_VAR=hello (env var recorded as oracle dependency)
MY_TEST_VAR=hello nix build --no-link --impure -f "$testDir/test-getenv.nix" drv

# Same env var -> verification hit (oracle returns same value)
MY_TEST_VAR=hello NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-getenv.nix" drv

# Different env var -> verify miss (oracle returns different value, trace verification fails)
MY_TEST_VAR=world NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-getenv.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 1 passed: builtins.getEnv oracle dependency in trace"

###############################################################################
# Test 2: builtins.currentSystem dependency in trace (Shake: system oracle)
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

# Record trace with default system (system oracle dependency)
nix build --no-link --impure -f "$testDir/test-system.nix" drv

# Same system -> verification hit (oracle returns same value)
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-system.nix" drv

# Different system -> verify miss (oracle returns different value, trace verification fails)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-system.nix" drv --system i686-linux \
  | grepQuiet "not everything is cached"

echo "Test 2 passed: builtins.currentSystem oracle dependency in trace"

###############################################################################
# Test 3: builtins.currentTime — volatile oracle always fails trace verification
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

# Record trace with volatile currentTime dependency
nix build --no-link --impure -f "$testDir/test-currenttime.nix" drv

# Verify miss — volatile oracle (currentTime) always fails trace verification
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-currenttime.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 3 passed: builtins.currentTime volatile oracle always fails verification"

###############################################################################
# Test 4: pathExists / readDir oracle dependencies in trace
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

# Record trace with existence and directory listing dependencies
nix build --no-link --impure -f "$testDir/test-pathexists.nix" drv

# Verification hit — oracle dependencies unchanged
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-pathexists.nix" drv

# Add a new file to the directory -> directory listing oracle returns different value
echo "file2" > "$testDir/subdir/b.txt"

NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-pathexists.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 4 passed: pathExists/readDir oracle dependencies in trace"

###############################################################################
# Test 5: pathExists true-to-false transition — existence oracle changes
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

# Record trace with existence oracle returning true
nix build --no-link --impure -f "$testDir/test-pe-truetofalse.nix" drv

# Verification hit — existence oracle unchanged
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-pe-truetofalse.nix" drv

# Remove the file — existence oracle now returns false
rm "$testDir/optdir/target.txt"

# Verify miss — trace verification fails (existence oracle changed true->false)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-pe-truetofalse.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 5 passed: pathExists existence oracle transition"

###############################################################################
# Test 6: Multiple attributes with shared content dependency in traces
###############################################################################
clearStoreIndex

echo "1.0" > "$testDir/version.txt"

# Each attribute calls readFile independently (not shared thunk),
# so each records its own content dependency in its trace.
cat >"$testDir/test-shared-dep.nix" <<EOF
{
  a = "pkg-a-\${builtins.readFile $testDir/version.txt}";
  b = "pkg-b-\${builtins.readFile $testDir/version.txt}";
}
EOF

# Record traces for both attributes in same session
nix eval --impure -f "$testDir/test-shared-dep.nix" --json

# Both verification hits — traced results valid
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-shared-dep.nix" a
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-shared-dep.nix" b

# Change the shared dependency — dirties both traces
echo "2.0" > "$testDir/version.txt"

# Both verify miss — trace verification fails for both (shared dependency changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-shared-dep.nix" a \
    | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-shared-dep.nix" b \
    | grepQuiet "not everything is cached"

echo "Test 6 passed: shared content dependency invalidates multiple traces"

###############################################################################
# Test 7: File deletion — content dependency in trace becomes unresolvable
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

# Record trace with content dependency on deletable.txt
nix build --no-link --impure -f "$testDir/test-file-delete.nix" drv

# Verification hit — trace valid
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-file-delete.nix" drv

# Delete the file — content dependency in trace becomes unresolvable
rm "$testDir/deletable.txt"

# Verify miss — trace verification fails (content oracle cannot read deleted file)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-file-delete.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 7 passed: file deletion fails trace verification"

###############################################################################
# Test 8: Directory entry removal — directory listing oracle changes
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

# Record trace with directory listing dependency
nix build --no-link --impure -f "$testDir/test-dir-remove.nix" drv

# Verification hit — directory listing oracle unchanged
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-dir-remove.nix" drv

# Remove a file from the directory — directory listing oracle returns different value
rm "$testDir/rmdir-test/b.txt"

# Verify miss — trace verification fails (directory listing dependency changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-dir-remove.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 8 passed: directory entry removal fails trace verification"

###############################################################################
# Test 9: readFileType — file type oracle change fails trace verification
###############################################################################
clearStoreIndex

echo "content" > "$testDir/typed-node"
cat >"$testDir/test-readfiletype.nix" <<'EOF'
builtins.readFileType ./typed-node
EOF

# Fresh evaluation — records trace with file type dependency
[[ "$(nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"regular"' ]]

# Verification hit — file type oracle unchanged
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"regular"' ]]

# Replace file with directory — file type oracle now returns "directory"
rm "$testDir/typed-node"
mkdir "$testDir/typed-node"

# Verify miss then fresh evaluation — records new trace with updated result
[[ "$(nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"directory"' ]]

# Verification hit — new trace valid
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-readfiletype.nix")" == '"directory"' ]]

echo "Test 9 passed: readFileType oracle change fails trace verification"

###############################################################################
# Test 10: hashFile — content dependency in trace detects file change
###############################################################################
clearStoreIndex

echo -n "hello" > "$testDir/hashme.txt"
cat >"$testDir/test-hashfile.nix" <<'EOF'
builtins.hashFile "sha256" ./hashme.txt
EOF

# Fresh evaluation — records trace with content hash dependency
result1="$(nix eval --json --impure -f "$testDir/test-hashfile.nix")"

# Verification hit — traced result replayed (same content hash)
result2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hashfile.nix")"
[[ "$result1" == "$result2" ]]

# Modify file content — dirties the content dependency in trace
echo -n "world" > "$testDir/hashme.txt"

# Verify miss then fresh evaluation — records new trace with new result
result3="$(nix eval --json --impure -f "$testDir/test-hashfile.nix")"
[[ "$result1" != "$result3" ]]

# Verification hit — new trace valid
result4="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hashfile.nix")"
[[ "$result3" == "$result4" ]]

echo "Test 10 passed: hashFile content dependency in trace"

###############################################################################
# Test 11: addPath (unfiltered) — NAR content dependency in trace
###############################################################################
clearStoreIndex

mkdir -p "$testDir/addpath-src"
echo "v1" > "$testDir/addpath-src/file.txt"
cat >"$testDir/test-addpath.nix" <<'EOF'
builtins.path { path = ./addpath-src; name = "test-addpath"; }
EOF

# Fresh evaluation — records trace with NAR content dependency (CopiedPath)
result1="$(nix eval --json --impure -f "$testDir/test-addpath.nix")"

# Verification hit — traced result replayed (NAR content unchanged)
result2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-addpath.nix")"
[[ "$result1" == "$result2" ]]

# Modify file in source directory — NAR content dependency changes
echo "v2" > "$testDir/addpath-src/file.txt"

# Verify miss then fresh evaluation — records new trace with new store path
result3="$(nix eval --json --impure -f "$testDir/test-addpath.nix")"
[[ "$result1" != "$result3" ]]

# Verification hit — new trace valid
result4="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-addpath.nix")"
[[ "$result3" == "$result4" ]]

echo "Test 11 passed: addPath NAR content dependency in trace"

###############################################################################
# Test 12: filterSource / filtered addPath — per-file trace dependencies
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

# Fresh evaluation — records trace with per-file NAR content dependencies
result1="$(nix eval --json --impure -f "$testDir/test-filtersource.nix")"

# Verification hit — traced result replayed (all per-file dependencies unchanged)
result2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result1" == "$result2" ]]

# Modify included file -> trace verification fails, fresh evaluation yields new store path
echo "included v2" > "$testDir/filter-src/included.txt"
result3="$(nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result1" != "$result3" ]]

# Verification hit — new trace valid
result4="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result3" == "$result4" ]]

# Modify only excluded file -> verification hit (no trace dependency recorded for excluded files)
echo "excluded v2" > "$testDir/filter-src/excluded.log"
result5="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result3" == "$result5" ]]

# Add a new file to the directory -> trace verification fails (directory listing dependency changes)
echo "new file" > "$testDir/filter-src/new-file.txt"
result6="$(nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result3" != "$result6" ]]

# Verification hit — new trace valid after directory change
result7="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-filtersource.nix")"
[[ "$result6" == "$result7" ]]

echo "Test 12 passed: filterSource / filtered addPath per-file trace dependencies"

echo "All eval-trace-impure-deps tests passed! (BSàlC: oracle dependencies in verifying traces)"
