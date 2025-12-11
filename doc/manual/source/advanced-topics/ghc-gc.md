# GHC Garbage Collector (Experimental)

Nix can optionally use GHC's (Glasgow Haskell Compiler) memory allocator and garbage collector instead of Boehm GC. This is an experimental feature that must be enabled at compile time.

## Overview

The GHC GC backend provides:
- Thread-local memory pools for fast allocation (default mode)
- Optional Haskell-tracked allocation with mark-sweep GC
- Safe point mechanism for controlled garbage collection
- Detailed allocation statistics for debugging

## Compilation

To build Nix with GHC GC support:

```console
$ meson setup build -Dlibexpr:ghc_gc=enabled -Dlibexpr:gc=disabled
$ meson compile -C build
```

The build creates `libghcalloc.so` which must be in the library search path at runtime.

## Environment Variables

### NIX_GHC_INIT_RTS

Enable GHC runtime system initialization. Without this, allocations use fast mmap pools but GC features are unavailable.

```console
$ NIX_GHC_INIT_RTS=1 nix eval --expr "1 + 1"
```

### NIX_GHC_GC_DEBUG

Enable debug output for allocation and GC events.

```console
$ NIX_GHC_GC_DEBUG=1 nix eval --expr "builtins.length [1 2 3]"
GHC GC: Loaded libghcalloc.so (lazy)
GHC GC: Calling hs_init...
GHC GC: hs_init completed
GHC GC: All function pointers loaded. fn_alloc_value=0x...
GHC GC: Using fast mmap pools (default). Set NIX_GHC_GC_TRACK=1 for tracked allocation.
```

### NIX_GHC_GC_TRACK

Enable Haskell-tracked allocation mode. Allocations go through the Haskell FFI for GC tracking. Has ~35% performance overhead but enables proper garbage collection.

```console
$ NIX_GHC_GC_TRACK=1 nix eval --expr "..."
```

### NIX_GHC_GC_THRESHOLD

Set the GC trigger threshold in bytes. When allocations since the last GC exceed this threshold, GC will be triggered at the next safe point.

```console
$ NIX_GHC_GC_THRESHOLD=10000000 nix eval --expr "..."  # 10 MB
```

Default: 10,000,000 bytes (10 MB)

### NIX_GHC_GC_UNSAFE

Allow GC to run outside safe points (testing only). Not recommended for production use.

```console
$ NIX_GHC_GC_UNSAFE=1 nix eval --expr "..."
```

### GHCRTS

Pass options to the GHC runtime system. Use `-T` to enable GC statistics collection (required for `--show-stats` to report heap information).

```console
$ GHCRTS="-T" NIX_GHC_INIT_RTS=1 nix eval --expr "..."
```

## Operation Modes

### Fast Mode (Default)

Uses thread-local mmap memory pools for minimal allocation overhead (~1%). Memory is not garbage collected in this mode.

```console
$ nix eval --expr "..."
```

### Tracked Mode

Enables Haskell-tracked allocation with mark-sweep garbage collection. Has ~35% performance overhead but provides proper memory management.

```console
$ NIX_GHC_INIT_RTS=1 NIX_GHC_GC_TRACK=1 nix eval --expr "..."
```

### Debug Mode

Full debugging with verbose output showing allocation counts and GC events.

```console
$ export LD_LIBRARY_PATH=/path/to/libghcalloc:$LD_LIBRARY_PATH
$ NIX_GHC_INIT_RTS=1 NIX_GHC_GC_DEBUG=1 NIX_GHC_GC_TRACK=1 NIX_GHC_GC_THRESHOLD=100000 \
    nix eval --expr "builtins.filter (x: x > 5000) (builtins.genList (x: x) 10000)"
GHC GC: Safe point GC triggered at 765167 bytes, freed 40134 allocations
4999
```

## Safe Points

The GHC GC uses safe points to ensure garbage collection only occurs when no Values are actively being processed on the C++ stack. Safe points are placed at:

- End of `builtins.filter` operations
- End of `builtins.foldl'` operations
- End of `builtins.any` and `builtins.all` operations
- End of `builtins.concatMap` operations
- After `forceValueDeep` completes
- After top-level evaluation in `nix eval` and `nix-instantiate`

## Performance Tuning

### For Low Memory Systems

Use a smaller GC threshold to collect more frequently:

```console
$ NIX_GHC_INIT_RTS=1 NIX_GHC_GC_TRACK=1 NIX_GHC_GC_THRESHOLD=1000000 nix eval ...
```

### For Maximum Speed

Use the default fast mode (no environment variables needed):

```console
$ nix eval --expr "..."
```

### For Debugging Memory Issues

Enable full tracking and debug output:

```console
$ NIX_GHC_INIT_RTS=1 NIX_GHC_GC_DEBUG=1 NIX_GHC_GC_TRACK=1 nix eval ...
```

## Limitations

- GHC GC is compile-time optional and not enabled in standard Nix builds
- The `libghcalloc.so` library must be in `LD_LIBRARY_PATH` or installed system-wide
- Tracked mode has higher memory overhead during allocation-heavy workloads
- Safe points only trigger at specific locations; very long-running single operations may not trigger GC

## Known Issues

### Cached Thunk Problem (Critical)

**Status**: BLOCKING for production use with complex workloads involving repeated file imports

**Description**: The GHC GC implementation has a fundamental issue with the file evaluation cache when using tracked allocation mode. When a cached file thunk is forced, its environment chain may be freed by the GC, but other unevaluated thunks may still reference those environments, leading to use-after-free crashes.

**Symptoms**:
- Crashes after GC with "attempt to call something which is not a function" errors
- Segmentation faults during evaluation
- Memory corruption in subsequent evaluations

**Trigger Conditions**:
- Using `NIX_GHC_GC_TRACK=1` (tracked allocation mode)
- Expressions that import the same file multiple times (e.g., via `builtins.genList`)
- Memory pressure that triggers GC between imports

**Workarounds**:
1. Use fast mode (default) instead of tracked mode - no GC, no crashes
2. Increase `NIX_GHC_GC_THRESHOLD` to avoid triggering GC
3. Avoid patterns that repeatedly import the same files in a single evaluation

**Example of Problematic Pattern**:
```nix
# gnomes.nix - DO NOT USE WITH NIX_GHC_GC_TRACK=1
builtins.genList (i: import <nixpkgs> {}) 10
```

**Recommendation**: For production use, stick with fast mode (default) until this issue is resolved. Tracked mode should only be used for testing simple evaluations that don't involve complex import patterns.
