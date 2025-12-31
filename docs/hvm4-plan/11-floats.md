# 11. Floats

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).
>
> Status (2025-12-28): Float literals are supported via `#Flt{lo, hi}` encoding.
> Float arithmetic and comparisons are not implemented and will fall back to the
> standard evaluator.

Nix has double-precision floats. HVM4 only has 32-bit integers.

## Option A: IEEE 754 Encoding

Encode doubles as pairs of 32-bit integers.

```hvm4
// Float = #Float{lo, hi}
// lo, hi are the low/high 32 bits of IEEE 754 double

// Operations require reimplementing float arithmetic
// Very complex, probably not worth it
```

## Option B: Fixed-Point

Use fixed-point representation for limited precision.

```hvm4
// Fixed = #Fixed{mantissa, scale}
// value = mantissa / 10^scale

@fixed_add = λa.λb.
  // Align scales, add mantissas
```

## Option C: External Float Handling

Defer float operations to external handler.

```hvm4
// FloatOp = #FloatAdd{a, b, continuation} | ...

// Float operations become effects handled outside HVM4
```

## Current Implementation: Encode Float Literals Only

**Rationale:**
- Literal support unblocks parsing and extraction of float values
- Arithmetic/comparison still falls back due to missing operator support

**Implementation:**

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprFloat*>(&expr)) {
    return false;  // Floats not supported - fallback to standard evaluator
}

// All float-related primops also return false:
// - builtins.ceil, builtins.floor
// - Any arithmetic with float operands
```

**Future Option (Phase 2):**
Could add effect-based external handling if needed:
```hvm4
// FloatOp = #FloatAdd{a, b, continuation} | ...
// External handler pauses HVM4, computes in native floats, continues
```

---
