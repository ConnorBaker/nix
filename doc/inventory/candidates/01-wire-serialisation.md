# Duplicated wire / serialisation logic

Candidates 1-10. All ten validate as VALID; corroborated by direct reading of
`src/libstore/worker-protocol*.{cc,hh}`, `serve-protocol*.{cc,hh}`,
`common-protocol.hh`, `src/libutil/hash.cc`, `signature/local-keys.cc`, and
the JSON adl_serializer scaffolding under `src/libutil/include/nix/util/`.

| # | Verdict | Effort |
| - | ------- | ------ |
| 1 | VALID | small |
| 2 | VALID | medium |
| 3 | VALID | small |
| 4 | VALID | trivial |
| 5 | VALID | trivial |
| 6 | VALID | trivial |
| 7 | VALID | small |
| 8 | VALID | medium |
| 9 | VALID | medium |
| 10 | VALID | medium |

The cluster sits on top of an already-factored substrate (`CommonProto`,
`LengthPrefixedProtoHelper`, the `DECLARE_*_SERIALISER` macros), so
candidates 1-7 are mostly local consolidations. Candidates 8-10 cross
libutil/libstore boundaries and are higher effort despite the smaller
per-site change.

---

1. **`BuildResult` serialiser duplicated across worker and serve protocols.** Both `WorkerProto::Serialise<BuildResult>` and `ServeProto::Serialise<BuildResult>` are near-byte-identical (same status / errorMsg / timing / builtOutputs sequence, only the version-cutoff literals differ — worker uses `{1,29}`/`{1,37}`/feature `realisation-with-path-not-hash`/`{1,28}`; serve uses `{2,3}`/`{2,8}`/`{2,6}`). Worker has an extra cpu-timing branch and a feature gate; serve does not. The shared `common = [&](errorMsg, isNonDeterministic, builtOutputs) { ... }` lambda is duplicated almost verbatim. The single biggest refactor target in the protocol layer.
   - ../verified/09-libstore-protocol.md
   - **Validation:** VALID. The lambda capture lists are byte-identical; only the version literals and the cpu-timing branch differ. Lifts cleanly into `CommonProto` once the per-protocol version gate is parameterised. Effort: small.

2. **`UnkeyedValidPathInfo` serialiser duplicated across worker and serve protocols.** Both protocols hand-roll the deriver/refs/narHash/narSize/sigs/ca format with cosmetic differences: worker uses `Serialise<std::optional<StorePath>>` for the deriver; serve uses an empty-string sentinel inline; worker uses Base16 narHash without prefix; serve uses Nix32 narHash with prefix; serve emits narSize twice (the second as the obsolete "downloadSize"); worker emits an `ultimate` flag, serve does not.
   - ../verified/09-libstore-protocol.md
   - **Validation:** VALID, but the differences listed are *semantic*, not cosmetic — the hash format, the deriver encoding, the `narSize`-twice "downloadSize lie" branch, and the `ultimate` flag are all on-the-wire choices. Refactor needs an explicit per-protocol policy adapter rather than a templated common body. Effort: medium.

3. **`DrvOutput`/`UnkeyedRealisation`/`Realisation` serialisers identical between worker and serve.** Each pair is byte-identical except for the version-gate (`featureRealisationWithPath` for worker, `>= 2.8` for serve). Obvious candidate for promotion to `CommonProto` once a way to thread the per-protocol gate through is found.
   - ../verified/09-libstore-protocol.md
   - **Validation:** VALID. Once the version-gate threading is solved (see #179 for the broader version-gate refactor), this lifts. Effort: small.

4. **Length-prefixed container serialiser macros are three near-identical copies.** `WORKER_USE_LENGTH_PREFIX_SERIALISER`, `SERVE_USE_LENGTH_PREFIX_SERIALISER`, and `COMMON_USE_LENGTH_PREFIX_SERIALISER` each emit `Serialise<vector<T>>`/`Serialise<set<T>>`/`Serialise<tuple<Ts...>>`/`Serialise<map<K,V>>` specialisations delegating to `LengthPrefixedProtoHelper<Proto, T>`. The macros could be a single template parametrised on the protocol struct.
   - ../verified/09-libstore-protocol.md
   - **Validation:** VALID. The three macro families differ only by the `WorkerProto`/`ServeProto`/`CommonProto` token — collapsing to one Proto-parameterised template is mechanical. Effort: trivial.
   - **Branch:** `vibe-coding/cleanup/libstore`

5. **`DECLARE_*_SERIALISER` declaration macros are three near-identical copies.** `DECLARE_COMMON_SERIALISER`, `DECLARE_WORKER_SERIALISER`, `DECLARE_SERVE_SERIALISER` differ only in the namespace prefix on `Serialise<T>` and (cosmetically) in the parameter name. Same shape as #4.
   - ../verified/09-libstore-protocol.md
   - **Validation:** VALID. Effort: trivial.
   - **Branch:** `vibe-coding/cleanup/libstore`

6. **`GET_PROTOCOL_MAJOR`/`GET_PROTOCOL_MINOR` macros duplicated.** Defined identically in both `worker-protocol.hh` and `serve-protocol.hh` (`(x) & 0xff00` and `(x) & 0x00ff`). Including both headers in the same TU works only because the second `#define` produces an identical token sequence.
   - ../verified/09-libstore-protocol.md
   - **Validation:** VALID. Promote to `common-protocol.hh` (or to a shared `proto-version.hh`). Effort: trivial.
   - **Branch:** `vibe-coding/cleanup/libstore` (initially hoisted to `common-protocol.hh`; post-cleanup adversarial review found zero call sites tree-wide, so the macros were deleted entirely in a follow-up commit)

7. **Protocol handshake logic is parallel between worker and serve.** `WorkerProto::BasicClientConnection::handshake` and `ServeProto::BasicClientConnection::handshake` both send magic-1, read magic-2, exchange version numbers, take the min. Worker additionally exchanges and intersects a `FeatureSet` (≥1.38) via private `intersectFeatures`; serve has no such step. Server-side mirrors are likewise parallel.
   - ../verified/09-libstore-protocol.md
   - **Validation:** VALID. The serve handshake also takes an extra `host` string for error context; that's the only other meaningful divergence. Combines with #185's `BasicConnection<Proto>` template proposal. Effort: small.

8. **Three encoders for the same `<algo>:<base>` shape.** `Hash::to_string`/`Hash::parseAny*` use `<algo>:<base*>` (and SRI `<algo>-<base64>`); `Signature` and `Key` use `<name>:<base64>` via the anon-namespace `parseColonBase64`/`serializeColonBase64`. Each module rolls its own. A shared "colon-prefixed Base-N" helper would consolidate these.
   - ../verified/02-libutil-data.md
   - **Validation:** VALID. The SRI variant uses `-` instead of `:` so a single helper has to be parameterised on separator + codec, not a one-helper-fits-all. `Hash::parseAny` additionally falls back to base-from-length when no prefix is present (signature/key do not) — keep that branch out of the shared helper. Effort: medium (cross-library touch).

9. **JSON adl_serializer scaffolding is split across multiple files.** `json-impls.hh` provides the macros, `json-non-null.hh` provides the `json_avoids_null<T>` trait, `json-utils.hh` provides accessor helpers and the generic `adl_serializer<std::optional<T>>`, `abstract-setting-to-json.hh` provides `BaseSetting<T>::toJSONObject`. Each consumer (`hash.cc`, `compression-settings.cc`, `signature/local-keys.cc`, plus most files in shard 05) writes near-identical adl_serializer boilerplate.
   - ../verified/02-libutil-data.md, ../verified/05-libstore-core.md
   - **Validation:** VALID. Boost.PFR or `boost::describe` (neither currently in tree) would let record-shaped consumers autogenerate, with opt-outs for `Hash`/`Signature`/`ContentAddress` which do non-trivial transforms. Effort: medium.

10. **`dumpPath`/`restorePath` overload sets scattered across compilation units.** `archive.hh/.cc` exposes `dumpPath(path, Sink, PathFilter)` plus `dumpPathAndGetMtime`; `source-path.hh/.cc` adds `SourcePath::dumpPath`; `source-accessor.hh/.cc` adds `SourceAccessor::dumpPath` (the actual NAR algorithm); `file-content-address.hh/.cc` adds method-dispatched `dumpPath(SourcePath, Sink, FileSerialisationMethod, PathFilter)`. Same shape for `restorePath`. No single header documents the relationship.
    - ../verified/01-libutil-io.md
    - **Validation:** VALID. The algorithmic `SourceAccessor::dumpPath` legitimately stays on `SourceAccessor`; the rest collapse behind a single `FileSerialisation::{dump,restore}(path, sink, Method)` entry point. `dumpPathAndGetMtime` (used by `BinaryCacheStore::addToStore`-type paths) is a side-channel return that the consolidation must preserve. Effort: medium.
