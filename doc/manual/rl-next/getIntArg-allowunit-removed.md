---
synopsis: "`getIntArg<N>`'s `allowUnit` parameter has been removed (libmain shipped header)"
---

The function template `nix::getIntArg<N>` in `src/libmain/include/nix/main/shared.hh` (shipped via `install_headers`) previously accepted a trailing `bool allowUnit` parameter. The parameter was unused by the body — the inner `string2IntWithUnitPrefix` always parses unit suffixes — and both in-tree call sites passed `true`, so it was dead surface.

The signature is now `template<class N> N getIntArg(const std::string & opt, Strings::iterator & i, const Strings::iterator & end)`. Out-of-tree consumers (Hydra, Lix, plugins) that pass a fourth `allowUnit` argument will need to drop it on rebuild.

The related `nix::blockInt` extern declaration in the same header — never defined anywhere — is also removed.
