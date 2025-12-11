#!/usr/bin/env bash
# Build libghcalloc.so - GHC RTS allocator for Nix
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building libghcalloc.so (GHC RTS Allocator for Nix) ==="
echo

# Find GHC
GHC="${GHC:-ghc}"
GHC_PKG="${GHC_PKG:-ghc-pkg}"

# Get GHC version and paths
echo "Finding GHC..."
GHC_VERSION=$($GHC --numeric-version)
GHC_LIBDIR=$($GHC --print-libdir)
echo "  GHC version: $GHC_VERSION"
echo "  GHC libdir: $GHC_LIBDIR"
echo

# Create build directory
mkdir -p build dist

# Step 1: Compile Haskell module with FFI exports
echo "Step 1: Compiling NixAlloc.hs..."
$GHC -c \
    -O2 \
    -threaded \
    -fPIC \
    -dynamic \
    -outputdir build \
    -o build/NixAlloc.o \
    NixAlloc.hs

echo "  Generated: build/NixAlloc.o"
echo "  Generated: build/NixAlloc_stub.h (FFI stub)"
echo

# Step 2: Build shared library
echo "Step 2: Building libghcalloc.so..."
# Explicitly link against the threaded RTS library
# GHC libraries are in lib/x86_64-linux-ghc-<version>/
RTS_LIB_PATH="$GHC_LIBDIR/../lib/x86_64-linux-ghc-$GHC_VERSION"
echo "  RTS library path: $RTS_LIB_PATH"

# Link with the threaded RTS library (libHSrts-<version>_thr-ghc<version>.so)
$GHC -shared \
    -dynamic \
    -threaded \
    -rtsopts \
    -o dist/libghcalloc.so \
    build/NixAlloc.o \
    -fPIC \
    -L"$RTS_LIB_PATH" \
    -lHSrts-1.0.2_thr-ghc$GHC_VERSION

echo "  Generated: dist/libghcalloc.so"
echo

# Step 3: Verify exports
echo "Step 3: Verifying FFI exports..."
if command -v nm &> /dev/null; then
    echo "  Exported symbols:"
    nm -D dist/libghcalloc.so | grep "nix_ghc" | head -10
    echo "  ... (showing first 10, run 'nm -D dist/libghcalloc.so | grep nix_ghc' for all)"
else
    echo "  (nm not available, skipping symbol check)"
fi
echo

echo "=== Build complete ==="
echo
echo "Library: $SCRIPT_DIR/dist/libghcalloc.so"
echo "Header:  $SCRIPT_DIR/build/NixAlloc_stub.h"
echo
echo "To use:"
echo "  1. Link C++ code with: -L$SCRIPT_DIR/dist -lghcalloc"
echo "  2. Include FFI header: #include \"NixAlloc_stub.h\""
echo "  3. Initialize GHC RTS: hs_init(&argc, &argv)"
echo "  4. Set RTS options: GHCRTS=\"-T -H1G\" (example)"
echo
