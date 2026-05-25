---
synopsis: "`BaseSetting<T>::overrideIfSet(U & dest)` is now part of the configuration API"
---

`src/libutil/include/nix/util/configuration.hh` (shipped via `install_headers`) gains a new public member function on `BaseSetting<T>`:

```cpp
template<typename U>
void overrideIfSet(U & dest) const;
```

It copies the setting's value into `dest` only when the user explicitly overrode the setting; otherwise `dest` is left untouched. The template parameter `U` lets `dest` be any type assignable from `T`, including `std::optional<T>`.

Out-of-tree consumers (Hydra, Lix, plugins) inherit the new method automatically — this is a purely additive addition with no signature or layout changes elsewhere in the header.

The first planned in-tree consumer is `HttpBinaryCacheStore::makeRequest`, which currently open-codes a four-call lambda doing the same `if (overridden) dest = value` test for the per-substituter retry settings; that migration lands on the libstore cleanup branch once this libutil change reaches upstream.
