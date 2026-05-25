---
synopsis: "`ServeProto::BasicConnection::remoteVersion` renamed to `protoVersion` in shipped `serve-protocol-connection.hh`"
---

The serve-protocol connection base now mirrors `WorkerProto::BasicConnection`'s field naming: `remoteVersion` is renamed to `protoVersion`. The header `src/libstore/include/nix/store/serve-protocol-connection.hh` ships via `install_headers`.

Out-of-tree consumers (Hydra, Lix, plugins, anyone embedding `ServeProto::BasicClientConnection`) that read or write the field by name must update on rebuild. Semantically the field always held the *negotiated* version (the `std::min(remoteVersion, localVersion)` result, not the raw value reported by the peer); the new name matches the existing `WorkerProto` convention and removes the false suggestion that `remoteVersion` is the peer-reported value.

The accompanying refactor extracts a `ServeProto::BasicConnection` base struct holding `to`, `from`, `protoVersion`, and the operator coercions to `ReadConn`/`WriteConn`, mirroring `WorkerProto::BasicConnection`. `ServeProto::BasicClientConnection` now inherits from it. No wire format changed.
