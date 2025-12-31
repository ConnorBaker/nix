# 1. Attribute Sets

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).
>
> Status (2025-12-28): Implemented encoding is `#Ats{spine}` with sorted `#Atr{key_id, value}`
> nodes in a `#Nil/#Con` spine. `//` eagerly merges two spines (`mergeAttrs`, O(n+m)) and does
> not use layering. Lookup is linear and returns ERA on missing attributes; `hasAttr` returns
> NUM 0/1. Dynamic attribute names and layered updates are not implemented. Cyclic `rec`
> attrsets are rejected.
>
> Note: This file is intentionally short and reflects current behavior plus
> near-term TODOs.

Nix attribute sets are central to the language. They are **sorted maps from symbols to values** with:
- O(log n) lookup via binary search
- Lazy values (thunks)
- The `//` (update) operator with layering optimization
- Dynamic attribute names (`"${expr}" = value`)

## Nix Implementation Details

```cpp
// Bindings: sorted array with layering for efficient //
class Bindings {
    Attr attrs[0];              // Flexible array, sorted by Symbol
    const Bindings* baseLayer;  // For // optimization (up to 8 layers)
};

struct Attr { Symbol name; Value* value; PosIdx pos; };
```

## Current Implementation (HVM4 backend)

The backend currently uses a wrapped sorted list, not the layered representation below.

```hvm4
// AttrSet = #Ats{spine}
// spine = #Nil{} | #Con{#Atr{key_id, value}, tail}

// Example: { a = 1; b = 2; }
#Ats{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}

// Example: { a = 1; } // { b = 2; } (merged eagerly)
#Ats{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}
```

Implementation notes:
- `mergeAttrs` merges two sorted spines and rewraps with `#Ats{}`.
- `emitAttrLookup` does a linear search and returns ERA on missing attributes.
- `emitOpHasAttr` uses the same search and returns NUM 0/1.
- Dynamic attribute names (`${expr}` keys) are not supported yet.


## Future Work

- Dynamic attribute names (`${expr}` keys).
- Layered `//` with bounded depth and flattening.
- Structured error propagation for missing attrs and type errors.
