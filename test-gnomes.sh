#!/usr/bin/env bash
# Phase 4 Test: Verify cached thunk problem is fixed (Iteration 50)
# This test imports the same file 10 times via genList, triggering caching.
# Before the fix, this would crash with use-after-free errors when GC runs.

set -e

echo "========================================="
echo "Phase 4: Testing Cached Thunk Fix"
echo "========================================="
echo ""

# Test 1: Fast mode (default) - should always work
echo "Test 1: Fast mode (no GC)"
echo "Command: ./build/src/nix/nix eval --file cached-import-test.nix"
echo ""
if timeout 10 ./build/src/nix/nix eval --file cached-import-test.nix 2>&1; then
    echo ""
    echo "✅ Test 1 PASSED: Fast mode works"
else
    echo ""
    echo "❌ Test 1 FAILED: Fast mode crashed"
    exit 1
fi
echo ""

# Test 2: Tracked mode with GC - the critical test
echo "Test 2: Tracked mode with GC (tests Env preservation fix)"
echo "Command: NIX_GHC_INIT_RTS=1 NIX_GHC_GC_TRACK=1 NIX_GHC_GC_THRESHOLD=5000 ./build/src/nix/nix eval --file cached-import-test.nix"
echo ""
if timeout 10 env NIX_GHC_INIT_RTS=1 NIX_GHC_GC_TRACK=1 NIX_GHC_GC_THRESHOLD=5000 \
   ./build/src/nix/nix eval --file cached-import-test.nix 2>&1; then
    echo ""
    echo "✅ Test 2 PASSED: Tracked mode with GC works (cached thunk problem is FIXED!)"
else
    echo ""
    echo "❌ Test 2 FAILED: Tracked mode crashed (cached thunk problem still exists)"
    exit 1
fi
echo ""

echo "========================================="
echo "All tests PASSED!"
echo "Cached thunk problem is RESOLVED!"
echo "========================================="
