---
synopsis: "`eval-attrset-update-layer-rhs-threshold` is deprecated and has no effect"
---

The [attribute set update operator `//`](@docroot@/language/operators.md#update) used to copy both of its operands into a new attribute set when the right operand had more attributes than [`eval-attrset-update-layer-rhs-threshold`](@docroot@/command-ref/conf-file.md#conf-eval-attrset-update-layer-rhs-threshold) (16 by default, 8192 on 32-bit systems), or when the left operand already had 8 layers.
Updates of large attribute sets with large ones, common in NixOS configurations, then copied both every time.

`//` now keeps every attribute set of *n* attributes within log2(*n*) + 1 layers, whatever the operands' sizes.
The right operand becomes a new layer over the left one, sharing the right operand's attributes instead of copying them where it can, and merged with the left operand's top layers where that is needed to stay within the bound.
Such updates now copy less, and use less memory.
The result is unchanged: the same attributes, in the same order, with the same positions, comparing equal as before.

The setting has nothing left to tune.
It is still accepted, so that existing configurations keep working, but it has no effect, and setting it prints a warning:

```
warning: 'eval-attrset-update-layer-rhs-threshold' is deprecated and has no effect
```
