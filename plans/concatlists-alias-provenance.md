# concatLists alias-path provenance: remaining work

## Status

- **Fix 1 (comment rewrite):** landed.  The doc-comment above
  `EvalState::concatLists` in `src/libexpr/eval.cc` now accurately
  describes the content-based `ContainerRef` lookup, the tListN vs
  tListSmall publication behaviour, and the bitwise-copy invariant.
- **Fix 2 (runtime assert):** NOT yet landed (confirmed 2026-04-29).
  The function body in `eval.cc` contains only the invariant comment
  block; the `assert([&]() -> bool { ... }());` block that would compare
  `lookupTracedContainer(&v)` against `lookupTracedContainer(nonEmpty)`
  under `traceActiveDepth && TraceAccess::current()` is not present.

## Invariant recap

`EvalState::concatLists` aliases the result when exactly one input list
is non-empty and its length equals the total:

```cpp
if (nonEmpty && len == nonEmpty->listSize()) {
    v = *nonEmpty;
    return;
}
```

Correctness depends on two facts:

1. `ContainerRef` is keyed by element-pointer content (not by enclosing
   `Value*`) for non-empty lists, so `lookupTracedContainer(&v)` resolves
   to the same provenance as the source.
2. `v = *nonEmpty` is a bitwise copy, preserving both element pointers
   and (for `tListN`) `List::publication`.

If the alias is ever rewritten as a manual rebuild (`buildList` + copy),
the element-pointer identity is preserved but the publication pointer
for `tListN` would not transfer — silently regressing traced-container
warm replay.

## Remaining work: Fix 2 — runtime assert after `v = *nonEmpty`

**Location:** `src/libexpr/eval.cc`, immediately after the `v = *nonEmpty;`
assignment (currently around eval.cc:2835-2838).

Add an assertion that, when tracing is active, the alias resolves to the
same `TracedContainerProvenance*` as the source:

```cpp
if (nonEmpty && len == nonEmpty->listSize()) {
    v = *nonEmpty;
    assert([&]() -> bool {
        if (!traceActiveDepth) return true;
        auto access = eval_trace::TraceAccess::current();
        if (!access) return true;
        if (v.listSize() == 0) return true;
        auto * sourceProv = access->lookupTracedContainer(nonEmpty);
        auto * aliasProv  = access->lookupTracedContainer(&v);
        return sourceProv == aliasProv;
    }());
    return;
}
```

The lambda is evaluated inside `assert()` so it is elided in NDEBUG
builds. This is a regression guard, not a correctness fix: it will fire
immediately in debug builds if someone changes the alias implementation
in a way that breaks content-based lookup or publication transfer.

## Verification steps

1. Run debug-build tests covering a JSON array concatenated with `[]`
   (the `(xs ++ [])` alias path).
2. Confirm byte-for-byte identical cold vs warm eval for a traced JSON
   array through the alias — the canonical case is
   `nixpkgs asciidoc.nativeBuildInputs`.
