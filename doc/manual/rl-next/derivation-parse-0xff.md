---
synopsis: "Derivations whose strings contain the byte 0xFF can be read back"
prs: []
---

The store derivation parser read bytes as signed characters, so a byte 0xFF in a builder, an argument or an environment variable was taken for the end of the input and the derivation failed to parse with "unterminated string in derivation", although Nix had written it.
A derivation depending on it could not be instantiated, since instantiation reads the input derivations back, and it could not be built.
Bytes are now read as unsigned values.
