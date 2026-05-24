---
synopsis: "`legacy-ssh://` `log-fd=N` URL parameter is now honoured on Windows"
---

The `legacy-ssh://` store URL accepted a `log-fd=N` parameter on Unix but silently dropped it on Windows: the underlying `LegacySSHStoreConfig::logFD` was a `Setting<int>` on Unix and a plain `Descriptor` field on Windows, with no parser to bridge from the URL parameter to the field on the Windows side.

`logFD` is now a `Setting<Descriptor>` on both platforms, so a Windows user-supplied `log-fd=N` is parsed into a native handle via the same `toDescriptor` conversion the rest of the codebase uses elsewhere. Behaviour on Unix is unchanged.

If you have a Windows configuration that previously set `log-fd=N` expecting it to be ignored, it will now be honoured.
