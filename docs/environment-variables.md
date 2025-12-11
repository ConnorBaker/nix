# GHC RTS Environment Variables - Reference Guide

**Date**: 2025-12-12
**Status**: Current documentation for GHC RTS integration (experimental)

> **⚠️ EXPERIMENTAL**: The GHC RTS integration is experimental and under active development. This is not backwards compatible with previous versions.

This document provides a comprehensive reference for all environment variables used with Nix's GHC RTS integration.

---

## Working Environment Variables

### GHCRTS (Primary Configuration)
**Status**: ✅ WORKING - This is the main way to configure GHC RTS

Controls all GHC Runtime System options. See [GHC RTS Options](https://downloads.haskell.org/~ghc/latest/docs/html/users_guide/runtime_control.html).

**Common flags**:
```bash
# Enable statistics
GHCRTS="-T"

# Set heap limits
GHCRTS="-H1G -M2G"        # -H = suggested heap, -M = maximum heap

# Set allocation area (affects GC frequency)
GHCRTS="-A32M"            # 32MB allocation area

# Parallel GC
GHCRTS="-N4 -qg"          # 4 capabilities, parallel GC

# Combined example
GHCRTS="-T -H1G -M2G -A32M -N4 -qg"
```

### NIX_GHC_GC_DEBUG
**Status**: ✅ WORKING

Enables debug logging for GHC RTS initialization.

```bash
NIX_GHC_GC_DEBUG=1 nix eval --expr '1+1'
```

**Output**:
- Library loading messages
- RTS initialization details
- Function pointer resolution
- Debug information during execution

### NIX_GHC_GC_STATS
**Status**: ✅ WORKING

Prints allocation statistics at exit.

```bash
NIX_GHC_GC_STATS=1 nix eval --expr '1+1'
```

**Output**:
- Total allocations by type (Value, Env, Bindings, List)
- Allocation counts
- Memory usage

### NIX_LIBGHCALLOC_PATH
**Status**: ✅ WORKING

Override the path to libghcalloc.so (for development/testing).

```bash
NIX_LIBGHCALLOC_PATH=/custom/path/libghcalloc.so nix eval --expr '1+1'
```

---

## Obsolete/Non-functional Variables

These were used by the old custom C++ GC and have NO EFFECT with GHC RTS:

### ❌ NIX_GHC_INIT_RTS
**Status**: DOES NOT EXIST - Never implemented

This variable was referenced in some historical documentation but was never actually implemented in any version of the code.

### ❌ NIX_GHC_GC_TRACK
**Status**: OBSOLETE - From custom C++ GC, no effect with GHC RTS

**Migration**: Use `GHCRTS` instead for GC configuration.

### ❌ NIX_GHC_GC_THRESHOLD
**Status**: OBSOLETE - From custom C++ GC, no effect with GHC RTS

**Migration**: Use `GHCRTS="-M<size>"` to set maximum heap size instead.

### ❌ NIX_GHC_GC_NURSERY_THRESHOLD
**Status**: OBSOLETE - From custom C++ GC, no effect with GHC RTS

**Migration**: Use `GHCRTS="-A<size>"` to set allocation area size instead.

### ❌ NIX_GHC_GC_FULL_GC_INTERVAL
**Status**: OBSOLETE - From custom C++ GC, no effect with GHC RTS

**Migration**: GHC RTS manages GC intervals automatically. No manual configuration needed.

### ❌ NIX_GHC_GC_GEN0_SURVIVAL
**Status**: OBSOLETE - From custom C++ GC, no effect with GHC RTS

**Migration**: GHC RTS manages generational promotion automatically.

### ❌ NIX_GHC_GC_GEN1_SURVIVAL
**Status**: OBSOLETE - From custom C++ GC, no effect with GHC RTS

**Migration**: GHC RTS manages generational promotion automatically.

### ❌ GC_MAXIMUM_HEAP_SIZE
**Status**: LEGACY - From custom C++ GC, may still work but prefer GHCRTS

**Migration**: Use `GHCRTS="-M<size>"` instead.

---

## Recommended Usage Patterns

### Development/Debugging
```bash
NIX_GHC_GC_DEBUG=1 \
NIX_GHC_GC_STATS=1 \
GHCRTS="-T" \
    nix eval --file myfile.nix
```

### Small Evaluations (< 1MB)
```bash
GHCRTS="-H32M -A4M" nix eval --expr 'import <nixpkgs> {}'
```

### Medium Configurations (1-100MB)
```bash
GHCRTS="-H256M -M1G -A16M -T" nix build
```

### Large Evaluations (100MB+)
```bash
GHCRTS="-H1G -M4G -A64M -N4 -qg" nix eval --file large.nix
```

### Profiling GC Behavior
```bash
GHCRTS="-T -s -S" nix eval --file test.nix
# -T: collect stats
# -s: summary at exit
# -S: detailed GC stats
```

---

## GHC RTS Flag Reference

### Memory Management Flags

| Flag | Purpose | Example |
|------|---------|---------|
| `-H<size>` | Suggested heap size (soft limit) | `-H1G` |
| `-M<size>` | Maximum heap size (hard limit) | `-M2G` |
| `-A<size>` | Allocation area size | `-A32M` |
| `-F<factor>` | Heap growth factor (default: 2) | `-F1.5` |
| `-c` | Compact on every GC | `-c` |
| `-c<n>` | Compact every N GCs | `-c5` |

**Size suffixes**: `K` (kilobytes), `M` (megabytes), `G` (gigabytes)

### GC Strategy Flags

| Flag | Purpose | Example |
|------|---------|---------|
| `-N<n>` | Number of capabilities (threads) | `-N4` |
| `-qg` | Enable parallel GC | `-qg` |
| `-qg<n>` | Use N parallel GC threads | `-qg4` |
| `-I<sec>` | Idle GC interval | `-I60` |
| `-G<n>` | Number of generations (default: 2) | `-G3` |

### Statistics and Debugging Flags

| Flag | Purpose | Output |
|------|---------|--------|
| `-T` | Collect GC statistics | Summary at exit |
| `-s` | Print GC summary | One-line summary |
| `-S` | Print detailed GC stats | Per-GC details |
| `-t` | One-line GC stats | To stderr |

### Combined Examples

```bash
# Balanced configuration for production
GHCRTS="-T -H512M -M2G -A16M -N4 -qg"

# Memory-constrained environment
GHCRTS="-M256M -A8M -c5"

# Maximum performance on 8-core system
GHCRTS="-H2G -M8G -A64M -N8 -qg"

# Debug with full statistics
GHCRTS="-T -S -s"
```

---

## Migration Guide

If you have scripts using obsolete environment variables, here's how to migrate:

### Old Custom C++ GC
```bash
# Old (doesn't work with GHC RTS)
NIX_GHC_GC_TRACK=1 \
NIX_GHC_GC_THRESHOLD=100000000 \
NIX_GHC_GC_NURSERY_THRESHOLD=5000000 \
    nix eval --file large.nix
```

### New GHC RTS
```bash
# New (correct for GHC RTS)
GHCRTS="-T -M100M -A5M" \
NIX_GHC_GC_DEBUG=1 \
    nix eval --file large.nix
```

### Mapping Old to New

| Old Variable | New Equivalent | Notes |
|--------------|----------------|-------|
| `NIX_GHC_GC_TRACK=1` | `GHCRTS="-T"` | Enable statistics |
| `NIX_GHC_GC_THRESHOLD=100M` | `GHCRTS="-M100M"` | Maximum heap |
| `NIX_GHC_GC_NURSERY_THRESHOLD=5M` | `GHCRTS="-A5M"` | Allocation area |
| `GC_MAXIMUM_HEAP_SIZE=2G` | `GHCRTS="-M2G"` | Maximum heap |
| `NIX_GHC_GC_DEBUG=1` | `NIX_GHC_GC_DEBUG=1` | Still works! |

---

## Full Documentation

For comprehensive documentation, see:

- [ghc-rts-integration.md](ghc-rts-integration.md) - Complete GHC RTS integration guide
- [GHC User's Guide - Runtime Control](https://downloads.haskell.org/~ghc/latest/docs/html/users_guide/runtime_control.html) - Official GHC RTS documentation

---

**Last updated**: 2025-12-12
**Status**: Current
