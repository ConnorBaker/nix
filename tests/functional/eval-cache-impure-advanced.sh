#!/usr/bin/env bash

# Advanced impure eval cache tests: partial tree invalidation via thunk mutation,
# origExpr deep chain, FullAttrs shared dep, nested FullAttrs, no infinite
# recursion, deep origExpr O(2^K), dep suspension (fat parent).

source common.sh

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

testDir="$TEST_ROOT/eval-cache-impure"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: Partial tree invalidation via thunk mutation
###############################################################################
clearStoreIndex

cat >"$testDir/partial-a.txt" <<'EOF'
hello
EOF
cat >"$testDir/partial-b.txt" <<'EOF'
world
EOF
cat >"$testDir/test-partial.nix" <<'EOF'
let
  a = builtins.readFile ./partial-a.txt;
  b = builtins.readFile ./partial-b.txt;
in {
  inherit a b;
  c = a + b;
}
EOF

# Cold cache: evaluate all three attrs
[[ "$(nix eval --json --impure -f "$testDir/test-partial.nix" a)" == '"hello\n"' ]]
[[ "$(nix eval --json --impure -f "$testDir/test-partial.nix" b)" == '"world\n"' ]]
[[ "$(nix eval --json --impure -f "$testDir/test-partial.nix" c)" == '"hello\nworld\n"' ]]

# Warm cache: all should be served from cache
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-partial.nix" a)" == '"hello\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-partial.nix" b)" == '"world\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-partial.nix" c)" == '"hello\nworld\n"' ]]

# Modify only a.txt — b should still be valid
echo "modified" > "$testDir/partial-a.txt"

# a and c should be re-evaluated (their deps changed)
[[ "$(nix eval --json --impure -f "$testDir/test-partial.nix" a)" == '"modified\n"' ]]
[[ "$(nix eval --json --impure -f "$testDir/test-partial.nix" c)" == '"modified\nworld\n"' ]]

# After re-evaluation, all should be cached again
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-partial.nix" a)" == '"modified\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-partial.nix" b)" == '"world\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-partial.nix" c)" == '"modified\nworld\n"' ]]

echo "Test 1 passed: partial tree invalidation via thunk mutation"

####################################################################
# Test 2: Child origExpr wrapping enables deep warm cache hits
####################################################################
testDir2="$TEST_ROOT/deep-chain"
mkdir -p "$testDir2"

cat > "$testDir2/default.nix" <<'EOF'
let
  inner = {
    leaf = builtins.readFile ./data.txt;
    other = "constant";
  };
in {
  a = inner.leaf;
  b = inner.other;
}
EOF
echo "hello" > "$testDir2/data.txt"

# Cold cache: evaluate both to populate
[[ $(nix eval --impure -f "$testDir2/default.nix" a --json) = '"hello\n"' ]]
[[ $(nix eval --impure -f "$testDir2/default.nix" b --json) = '"constant"' ]]

# Warm cache: both served from cache
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir2/default.nix" a --json) = '"hello\n"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir2/default.nix" b --json) = '"constant"' ]]

# Invalidate: change data.txt
echo "world" > "$testDir2/data.txt"

# a depends on data.txt -> invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir2/default.nix" a --json \
    | grepQuiet "not everything is cached"

# Re-evaluate a (re-caches with new value)
[[ $(nix eval --impure -f "$testDir2/default.nix" a --json) = '"world\n"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir2/default.nix" a --json) = '"world\n"' ]]

echo "Test 2 passed: child origExpr wrapping enables deep warm cache hits"

####################################################################
# Test 3: FullAttrs warm cache serves shared dependency
####################################################################
testDir3="$TEST_ROOT/shared-dep"
mkdir -p "$testDir3"

cat > "$testDir3/default.nix" <<'EOF'
let
  sharedDep = {
    version = builtins.readFile ./version.txt;
    name = "shared";
  };
in {
  first = "${sharedDep.name}-${sharedDep.version}";
  second = "${sharedDep.name}-${sharedDep.version}-extra";
}
EOF
echo -n "1.0" > "$testDir3/version.txt"

# Cold cache: evaluate both
[[ $(nix eval --impure -f "$testDir3/default.nix" first --json) = '"shared-1.0"' ]]
[[ $(nix eval --impure -f "$testDir3/default.nix" second --json) = '"shared-1.0-extra"' ]]

# Warm cache: both served from cache
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir3/default.nix" first --json) = '"shared-1.0"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir3/default.nix" second --json) = '"shared-1.0-extra"' ]]

# Invalidate version.txt -> both should invalidate (shared dep set)
echo -n "2.0" > "$testDir3/version.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir3/default.nix" first --json \
    | grepQuiet "not everything is cached"

# Re-evaluate
[[ $(nix eval --impure -f "$testDir3/default.nix" first --json) = '"shared-2.0"' ]]
[[ $(nix eval --impure -f "$testDir3/default.nix" second --json) = '"shared-2.0-extra"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir3/default.nix" first --json) = '"shared-2.0"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir3/default.nix" second --json) = '"shared-2.0-extra"' ]]

echo "Test 3 passed: FullAttrs warm cache serves shared dependency"

####################################################################
# Test 4: Nested FullAttrs chain with warm cache
####################################################################
testDir4="$TEST_ROOT/nested-chain"
mkdir -p "$testDir4"

cat > "$testDir4/default.nix" <<'EOF'
let
  inner = {
    value = builtins.readFile ./data.txt;
  };
  outer = {
    inner = inner;
    summary = "outer-${inner.value}";
  };
in {
  deep = outer.inner.value;
  shallow = outer.summary;
}
EOF
echo -n "deep-val" > "$testDir4/data.txt"

# Cold cache: evaluate both
[[ $(nix eval --impure -f "$testDir4/default.nix" deep --json) = '"deep-val"' ]]
[[ $(nix eval --impure -f "$testDir4/default.nix" shallow --json) = '"outer-deep-val"' ]]

# Warm cache
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir4/default.nix" deep --json) = '"deep-val"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir4/default.nix" shallow --json) = '"outer-deep-val"' ]]

# Invalidation
echo -n "new-val" > "$testDir4/data.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir4/default.nix" deep --json \
    | grepQuiet "not everything is cached"
[[ $(nix eval --impure -f "$testDir4/default.nix" deep --json) = '"new-val"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir4/default.nix" deep --json) = '"new-val"' ]]

echo "Test 4 passed: nested FullAttrs chain with warm cache"

####################################################################
# Test 5: FullAttrs warm cache doesn't cause infinite recursion
####################################################################
testDir5="$TEST_ROOT/no-recursion"
mkdir -p "$testDir5"

cat > "$testDir5/default.nix" <<'EOF'
let
  dep = {
    a = "val-a";
    b = "val-b";
    c = "val-c";
  };
in {
  useA = dep.a;
  useB = dep.b;
  useC = dep.c;
}
EOF

# Cold cache: evaluate all (populates dep as FullAttrs with all children)
[[ $(nix eval --impure -f "$testDir5/default.nix" useA --json) = '"val-a"' ]]
[[ $(nix eval --impure -f "$testDir5/default.nix" useB --json) = '"val-b"' ]]
[[ $(nix eval --impure -f "$testDir5/default.nix" useC --json) = '"val-c"' ]]

# Warm cache: all served from cache
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir5/default.nix" useA --json) = '"val-a"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir5/default.nix" useB --json) = '"val-b"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir5/default.nix" useC --json) = '"val-c"' ]]

echo "Test 5 passed: FullAttrs warm cache doesn't cause infinite recursion"

####################################################################
# Test 6: Deep nested origExpr chain completes without dep explosion
####################################################################
testDir6="$TEST_ROOT/deep-origexpr"
mkdir -p "$testDir6"

# Create 6 files — each level of nesting reads a different file
for i in $(seq 1 6); do
    echo "file-$i-content" > "$testDir6/dep-$i.txt"
done

cat > "$testDir6/default.nix" <<'EOF'
let
  level1 = {
    data = builtins.readFile ./dep-1.txt;
    child = {
      data = builtins.readFile ./dep-2.txt;
      child = {
        data = builtins.readFile ./dep-3.txt;
        child = {
          data = builtins.readFile ./dep-4.txt;
          child = {
            data = builtins.readFile ./dep-5.txt;
            child = {
              data = builtins.readFile ./dep-6.txt;
            };
          };
        };
      };
    };
  };
in {
  deepLeaf = level1.child.child.child.child.child.data;
  midLeaf = level1.child.child.data;
  topLeaf = level1.data;
}
EOF

# Cold cache: traverse the full chain
[[ $(nix eval --impure -f "$testDir6/default.nix" deepLeaf --json) = '"file-6-content\n"' ]]
[[ $(nix eval --impure -f "$testDir6/default.nix" midLeaf --json) = '"file-3-content\n"' ]]
[[ $(nix eval --impure -f "$testDir6/default.nix" topLeaf --json) = '"file-1-content\n"' ]]

# Warm cache: all served from cache
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir6/default.nix" deepLeaf --json) = '"file-6-content\n"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir6/default.nix" midLeaf --json) = '"file-3-content\n"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir6/default.nix" topLeaf --json) = '"file-1-content\n"' ]]

# Modify a deep file — only deepLeaf should invalidate
echo "file-6-changed" > "$testDir6/dep-6.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir6/default.nix" deepLeaf --json \
    | grepQuiet "not everything is cached"
# Re-evaluate deepLeaf
[[ $(nix eval --impure -f "$testDir6/default.nix" deepLeaf --json) = '"file-6-changed\n"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir6/default.nix" deepLeaf --json) = '"file-6-changed\n"' ]]

echo "Test 6 passed: deep nested origExpr chain completes without dep explosion"

####################################################################
# Test 7: ExprOrigChild doesn't inherit parent's file deps
####################################################################
testDir7="$TEST_ROOT/fat-parent"
mkdir -p "$testDir7"

# Create many files that the parent reads (simulates a "fat" parent)
for i in $(seq 1 20); do
    echo "parent-dep-$i" > "$testDir7/parent-$i.txt"
done
echo "child-data" > "$testDir7/child-dep.txt"

cat > "$testDir7/default.nix" <<'EOF'
let
  parent = {
    p1 = builtins.readFile ./parent-1.txt;
    p2 = builtins.readFile ./parent-2.txt;
    p3 = builtins.readFile ./parent-3.txt;
    p4 = builtins.readFile ./parent-4.txt;
    p5 = builtins.readFile ./parent-5.txt;
    p6 = builtins.readFile ./parent-6.txt;
    p7 = builtins.readFile ./parent-7.txt;
    p8 = builtins.readFile ./parent-8.txt;
    p9 = builtins.readFile ./parent-9.txt;
    p10 = builtins.readFile ./parent-10.txt;
    p11 = builtins.readFile ./parent-11.txt;
    p12 = builtins.readFile ./parent-12.txt;
    p13 = builtins.readFile ./parent-13.txt;
    p14 = builtins.readFile ./parent-14.txt;
    p15 = builtins.readFile ./parent-15.txt;
    p16 = builtins.readFile ./parent-16.txt;
    p17 = builtins.readFile ./parent-17.txt;
    p18 = builtins.readFile ./parent-18.txt;
    p19 = builtins.readFile ./parent-19.txt;
    p20 = builtins.readFile ./parent-20.txt;
    child = builtins.readFile ./child-dep.txt;
  };
in {
  childVal = parent.child;
  parentVal = parent.p1;
}
EOF

# Cold cache
[[ $(nix eval --impure -f "$testDir7/default.nix" childVal --json) = '"child-data\n"' ]]
[[ $(nix eval --impure -f "$testDir7/default.nix" parentVal --json) = '"parent-dep-1\n"' ]]

# Warm cache
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir7/default.nix" childVal --json) = '"child-data\n"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir7/default.nix" parentVal --json) = '"parent-dep-1\n"' ]]

# Modify a parent file that the child does NOT depend on
echo "parent-dep-10-changed" > "$testDir7/parent-10.txt"

# Child should still be cached — its dep set should NOT include parent-10.txt
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir7/default.nix" childVal --json) = '"child-data\n"' ]]

# Parent value that reads parent-1.txt should also still be cached
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir7/default.nix" parentVal --json) = '"parent-dep-1\n"' ]]

# Now modify the child's own dep — should invalidate
echo "child-data-changed" > "$testDir7/child-dep.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir7/default.nix" childVal --json \
    | grepQuiet "not everything is cached"

# Re-evaluate
[[ $(nix eval --impure -f "$testDir7/default.nix" childVal --json) = '"child-data-changed\n"' ]]
[[ $(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir7/default.nix" childVal --json) = '"child-data-changed\n"' ]]

echo "Test 7 passed: ExprOrigChild doesn't inherit parent's file deps"

echo "All eval-cache-impure-advanced tests passed!"
