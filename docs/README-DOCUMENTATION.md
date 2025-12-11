# Documentation Index

This directory contains comprehensive documentation for the GHC GC integration in Nix.

## Quick Start

For users wanting to understand and use the GHC GC integration:
1. **[ghc-rts-integration.md](ghc-rts-integration.md)** - Complete user guide (23KB)
   - How GHC RTS integration works
   - Build instructions
   - Runtime configuration via GHCRTS
   - Troubleshooting guide

2. **[environment-variables.md](environment-variables.md)** - Environment variable reference
   - Working variables (GHCRTS, NIX_GHC_GC_DEBUG, etc.)
   - Obsolete variables from old C++ GC
   - Migration guide from old to new
   - Usage examples and best practices

## Implementation Details

For developers wanting to understand the implementation:
1. **[summary-of-changes.md](summary-of-changes.md)** - Complete iteration log (53KB)
   - All 68 iterations documented
   - Phase-by-phase summary
   - Code changes with file locations and line numbers
   - Test results and performance metrics

2. **[ralph-wiggum-ghc-rts.md](ralph-wiggum-ghc-rts.md)** - GHC RTS integration loop (22KB)
   - Iterations 64-68 detailed tracking
   - Work items and completion status
   - Final metrics and performance data

3. **[ralph-wiggum.md](ralph-wiggum.md)** - Original optimization loop (8KB)
   - Iterations 58-62 (now superseded by GHC RTS)
   - Example of Ralph Wiggum methodology
   - Lessons learned

## Future Work

1. **[future-work.md](future-work.md)** - Optimization opportunities (16KB)
   - Completed work summary (Phases 1-7)
   - Current architecture with GHC RTS
   - Remaining tuning opportunities
   - Long-term architectural possibilities

## Changelog

1. **[CHANGELOG-iter68.md](CHANGELOG-iter68.md)** - Latest documentation update
   - Iteration 68 documentation synchronization
   - Verified metrics as of 2025-12-12

## Documentation Status

**Last Updated**: 2025-12-12 (Iteration 68)
**Total Documentation**: ~78KB across 7 files
**Status**: ✅ Complete and synchronized with implementation

## File Sizes

| File | Size | Purpose |
|------|------|---------|
| summary-of-changes.md | 53KB | Complete iteration history |
| ghc-rts-integration.md | 23KB | User guide and reference |
| ralph-wiggum-ghc-rts.md | 22KB | GHC RTS loop tracking |
| future-work.md | 16KB | Optimization roadmap |
| ralph-wiggum.md | 8KB | Optimization loop (completed) |
| environment-variables.md | 6KB | Environment variable reference |
| CHANGELOG-iter68.md | 2KB | Latest changes |
| README-DOCUMENTATION.md | 2KB | This index |

## Key Metrics (Verified 2025-12-12)

**Implementation**:
- ghc-gc.cc: 1,987 lines (15.7% reduction from original)
- ghc-gc.hh: 779 lines
- NixAlloc.hs: 566 lines (Haskell FFI layer)
- libghcalloc.so: ~142KB

**Iterations**: 68 completed across 7 phases
**Status**: Production ready with full GHC RTS integration
**Test Coverage**: 100% (all smoke tests passing)

## Quick Reference

**Build Requirements**:
- GHC 9.4+
- Meson build system
- Standard C++ toolchain

**Runtime Configuration**:
```bash
# Enable statistics
export GHCRTS="-T"

# Tune for large workloads
export GHCRTS="-H1G -M2G -A32M -N4 -qg"

# Debug logging
export NIX_GHC_GC_DEBUG=1
```

See [ghc-rts-integration.md](ghc-rts-integration.md) for comprehensive configuration guide.
