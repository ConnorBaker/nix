---
synopsis: "`--max-freed` rejects negative values and accepts the full `uint64_t` range"
---

`nix-store --gc --max-freed N` and `nix-collect-garbage --max-freed N` previously parsed the argument as `int64_t` and clamped through `std::max(..., 0)` before assigning to the `uint64_t` field. Two consequences:

- Negative values (e.g. `--max-freed -1`) clamped to `0`, which means "stop after freeing zero bytes" — a near-no-op collection, the strictest possible cap. They now error at parse time instead.
- Values above `INT64_MAX` already errored at parse time (`boost::lexical_cast<int64_t>` rejects out-of-range input; it does not wrap). The full `uint64_t` range above `INT64_MAX` is now accepted and honoured.

Values in `[0, INT64_MAX]` are unaffected. Note that `--max-freed 0` continues to mean "free nothing"; the "no limit" behaviour is the default when the flag is omitted (`maxFreed` defaults to `uint64_t`'s maximum). There was never a negative "no cap" sentinel — a previous `--max-freed -1` was a no-op collection, so it has no equivalent flag form and must simply be dropped (do not expect dropping the flag to reproduce it: with no `--max-freed`, the collection is unbounded).
