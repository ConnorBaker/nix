#!/usr/bin/env bash
# Build script for GHC RTS integration test
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Find GHC
GHC="${GHC:-ghc}"
GHC_PKG="${GHC_PKG:-ghc-pkg}"

echo "=== GHC RTS Integration Test Build ==="
echo

# Get GHC version and paths
echo "Finding GHC paths..."
GHC_VERSION=$($GHC --numeric-version)
GHC_LIBDIR=$($GHC --print-libdir)
echo "  GHC version: $GHC_VERSION"
echo "  GHC libdir: $GHC_LIBDIR"

# Get RTS include and library directories
RTS_INCLUDE_DIRS=$($GHC_PKG field rts include-dirs | grep 'rts-' | head -1 | tr -d ' ')
RTS_INCLUDE_DIR="${RTS_INCLUDE_DIRS#include-dirs:}"
echo "  RTS include dir: $RTS_INCLUDE_DIR"

RTS_LIB_DIRS=$($GHC_PKG field rts library-dirs | grep 'rts-' | head -1 | tr -d ' ')
RTS_LIB_DIR="${RTS_LIB_DIRS#library-dirs:}"
echo "  RTS lib dir: $RTS_LIB_DIR"

RTS_VERSION=$($GHC_PKG field rts version | tr -d ' ')
RTS_VERSION="${RTS_VERSION#version:}"
echo "  RTS version: $RTS_VERSION"

echo

# Create build directory
BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"

# Step 1: Compile Haskell module
echo "Step 1: Compiling Haskell module..."
$GHC -c \
    -O2 \
    -outputdir "$BUILD_DIR" \
    -o "$BUILD_DIR/TestAlloc.o" \
    TestAlloc.hs

echo "  Generated: $BUILD_DIR/TestAlloc.o"
echo "  Generated: $BUILD_DIR/TestAlloc_stub.h"
echo

# Step 2: Compile C++ test program
echo "Step 2: Compiling C++ test program..."
CXX="${CXX:-g++}"
$CXX -c \
    -std=c++17 \
    -O2 \
    -I"$BUILD_DIR" \
    -I"$RTS_INCLUDE_DIR" \
    -I"$GHC_LIBDIR/include" \
    -o "$BUILD_DIR/test_ghc_rts.o" \
    test_ghc_rts.cc

echo "  Generated: $BUILD_DIR/test_ghc_rts.o"
echo

# Step 3: Link everything together
echo "Step 3: Linking..."

# Find the threaded RTS library
RTS_LIB=""
for lib in "HSrts-${RTS_VERSION}_thr" "HSrts_thr" "HSrts-${RTS_VERSION}"; do
    if [[ -f "$RTS_LIB_DIR/lib${lib}.a" ]]; then
        RTS_LIB="$lib"
        echo "  Found RTS library: lib${RTS_LIB}.a"
        break
    fi
done

if [[ -z "$RTS_LIB" ]]; then
    echo "Error: Could not find GHC RTS library"
    echo "Available libraries in $RTS_LIB_DIR:"
    ls -la "$RTS_LIB_DIR" || true
    exit 1
fi

# Link with GHC to get all the right libraries
$GHC \
    -no-hs-main \
    -threaded \
    -rtsopts \
    -O2 \
    -o "$BUILD_DIR/test_ghc_rts" \
    "$BUILD_DIR/test_ghc_rts.o" \
    "$BUILD_DIR/TestAlloc.o" \
    -lstdc++

echo "  Generated: $BUILD_DIR/test_ghc_rts"
echo

echo "=== Build complete ==="
echo
echo "Run the test with:"
echo "  $BUILD_DIR/test_ghc_rts"
echo
echo "Or with RTS options:"
echo "  $BUILD_DIR/test_ghc_rts +RTS -T -RTS"
