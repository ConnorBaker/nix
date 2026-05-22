# E5 — periodic branch verification

This document specifies a process for keeping the quantitative claims
embedded in `doc/inventory/candidates/*.md` and
`doc/inventory/review/05-consolidation-log.md` in sync with the source
tree as upstream lands new commits.

The motivating problem: the consolidation pass discovered that several
counts had drifted between pass-1 and pass-2 reading the same tree at
the same time (#169 friends 13 -> 17 -> 16; #166 forward-decls
9/19/12 -> 9/21/12). Reviewer-judgement drift is bad enough; left
unattended over time, upstream commits will silently invalidate the
catalog body counts further. There is no automation today.

## Inventory of verifiable counts

The table below is exhaustive across the catalog and the consolidation
log. Each row records: the catalog source file (no line numbers per
project policy), the prose claim verbatim or paraphrased, and a
canonical re-derivation command. Commands are written so they can be
pasted into a bash shell at the repo root.

Notation:

- `R` denotes the repo root.
- "Approx" in the count column flags claims that need methodology
  judgement (e.g. "tree-wide vs. `src/`-only"); these still
  re-derive but the script must compare against the methodology cited
  in the body.
- "Manual" flags claims that require qualitative judgement and cannot
  be derived purely by grep; these are listed in the
  "Counts that can't be automated" section below, not here.
- Where the catalog body cites two values for two methodologies (e.g.
  "10 in libutil, 19 tree-wide"), each appears as its own row.
- Numeric ranges in the catalog ("64-65", "21-22") need a tolerance
  band in the verifier (see verify-catalog-counts.sh design).

### Wire / serialisation (section 01)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 01 #1 | "{1,29}/{1,37}/{1,28}" version-cutoff literals in worker `Serialise<BuildResult>` | `grep -nE 'protoVersion (>=|<) +\{1,(28\|29\|37)\}' src/libstore/worker-protocol.cc` |
| 01 #1 | "{2,3}/{2,8}/{2,6}" version-cutoff literals in serve `Serialise<BuildResult>` | `grep -nE 'remoteVersion (>=\|<) +\{2,(3\|6\|8)\}' src/libstore/serve-protocol.cc` |
| 01 #4 | "three near-identical copies" of `*_USE_LENGTH_PREFIX_SERIALISER` | `grep -rEn '#define [A-Z]+_USE_LENGTH_PREFIX_SERIALISER\b' src/libstore` (expect 3) |
| 01 #5 | "three near-identical copies" of `DECLARE_*_SERIALISER` | `grep -rEn '#define DECLARE_(COMMON\|WORKER\|SERVE)_SERIALISER\b' src/libstore` (expect 3) |
| 01 #6 | "two `GET_PROTOCOL_MAJOR`/`MINOR` macros" | `grep -rEn '#define GET_PROTOCOL_(MAJOR\|MINOR)\b' src/libstore/include/nix/store` |

### Parallel stores (section 02)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 02 #11 | "at least ten methods" cross both stores in `LocalOverlayStore` | manual / approx — count distinct method names in `src/libstore/local-overlay-store.cc` |
| 02 #16 | "three SSH-store classes" sharing `CommonSSHStoreConfig` | `grep -rEn ': +(public +)?CommonSSHStoreConfig\b' src/libstore` (expect 3) |

### Repeated boilerplate (section 03)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 03 #17 body | "twelve+ classes" with `anchor()` override (~31 explicit overrides) | `grep -rEn 'void +anchor\(\) +override' src/libstore` (expect ~31) |
| 03 #17 list | enumerated 33+ class names | manual — diff names against grep output |
| 03 #19 body | "21-22 settings structs" via N4's verified count | `grep -rEn '(class\|struct) +\w+ *: *(public\s+)?(virtual\s+)?Config\b' src/` (expect 21-22) |
| 03 #24 | "30+ command files" with `registerCommand<...>` | `grep -rEn 'registerCommand2?<' src/nix` |

### Per-platform symmetry (section 04)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 04 #26 | "`unix/pathlocks.cc` and `windows/pathlocks.cc`" pair exists | `find src/libstore -name pathlocks.cc -path '*unix*' -o -name pathlocks.cc -path '*windows*'` (expect 2) |
| 04 #28 | "five platform/chroot files" included from `unix/build/derivation-builder.cc` | `grep -nE '^#include "(chroot\|linux\|freebsd\|darwin\|external)-derivation-builder.cc"' src/libstore/unix/build/derivation-builder.cc` (expect 5) |

### Inheritance flattening (section 05)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 05 #32 | "nine concrete schemes" plus two abstract bases | `grep -rEn ': +(public\s+)?(virtual\s+)?InputScheme\b' src/libfetchers` (expect 11 ish; 9 concrete + 2 abstract) |
| 05 #33 | "six concrete goal types" | `grep -rEn ': +(public\s+)?Goal\b' src/libstore/build` (expect 6 concrete + Goal base) |
| 05 #35 | "eleven concrete stores" | enumerated; manual — verify list against `grep -rEn ': +(public\s+)?(virtual\s+)?Store\b' src/libstore` |

### Multi-impl base (section 06)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 06 #36 | "five parallel value renderers" | manual — count `Printer::print`, `printAmbiguous`, `printValueAsJSON`, `printValueAsXML`, `Expr::show` |
| 06 #43 | "X-macro setting list ... four near-parallel registers" | manual — locate in `src/libutil/configuration.cc` |

### Dead/stale code (section 07)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 07 #48 | `void check();` in `lockfile.cc` is unused | `grep -rEn '\\bcheck\\b *\\(' src/libflake/lockfile.cc \| wc -l` (expect 0 references after definition) |
| 07 #49 | `blockInt` in `shared.hh` has zero references | `grep -rEn '\\bblockInt\\b' src/` (expect 1 — definition only) |
| 07 #52 | `CurlInputScheme::specialParams` declared but never defined | `grep -rEn 'specialParams' src/libfetchers` (expect 1 — declaration) |
| 07 #57 | "third issue" — `LENGTH_PREFIXED_PROTO_HELPER_X` `#define`d but never `#undef`'d | `grep -rEn 'LENGTH_PREFIXED_PROTO_HELPER_X' src/libstore/include/nix/store/length-prefixed-protocol-helper.hh` |

### Duplicated parsers (section 08)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 08 #66 | "8 type-predicate primops, plus `prim_isAttrs`/`prim_isList`/`prim_isFunction`" | `grep -rEn 'prim_is(Null\|Bool\|Int\|Float\|String\|Path\|Attrs\|List\|Function)' src/libexpr/primops.cc` (expect 9) |
| 08 #67 | "Numeric primops (`__add`/`__sub`/`__mul`/`__div`)" | `grep -rEn 'addPrimOp.*"__(add\|sub\|mul\|div)"' src/libexpr/primops.cc` (expect 4) |

### Cache keys (section 09)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 09 #69 | enumerated cache-key domains | `grep -rEn 'Cache::Key\\b' src/libfetchers` |

### Other (section 10)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 10 #73 | "A dozen commands" with JSON-vs-text dual paths | `grep -rEn 'if +\\(json\\)' src/nix \| wc -l` (expect ~12) |
| 10 #77 | "Eval-cache release ... duplicated four times" | `grep -rEn 'evalCaches\.clear\\(\\)' src/nix` (expect 4) |
| 10 #86 | enumerated registry-pattern sites | covered by N3 (see below) |

### Legacy CLI (section 11)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 11 #89 | "declared three times" — `MyArgs : LegacyArgs, MixEvalArgs` | `grep -rEn 'struct MyArgs +: +LegacyArgs, MixEvalArgs' src/nix` (expect 3) |
| 11 #90 | "12 operation handlers" in nix-env | manual — count `op*` function names in `src/nix/nix-env/nix-env.cc` |
| 11 #90 | "25 handlers" in nix-store | manual — count `op*` function names in `src/nix/nix-store/nix-store.cc` |
| 11 #93 | "Eight subcommand parents enumerate children dynamically via `getCommandsFor`" | `grep -rEn 'getCommandsFor\\b' src/nix` (expect 8) |
| 11 #93 | "Four hand-roll an inline factory list" — `CmdRegistry`, `CmdProfile`, `CmdKey`, `CmdHash` | manual — locate factory lists in those four files |
| 11 #93 | "5/8/2/7" inline children (CmdRegistry/Profile/Key/Hash) | manual — count entries in each factory map |

### libutil + libstore-core extras (section 12)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 12 #98 | "explicit template instantiations for `std::list<std::string>`, `StringSet`, `std::vector<std::string>`" | `grep -nE 'template +std::string +dropEmptyInitThenConcatStringsSep' src/libutil/strings.cc` (expect 3) |
| 12 #98 | "Five callers remain" of `dropEmptyInitThenConcatStringsSep` | `grep -rEn 'dropEmptyInitThenConcatStringsSep\\b' src/` (expect 5 + decl + body = 7+) |

### libexpr extras (section 13)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 13 #105 | "MakeBinOp/MakeBinOpMembers ... Used six times for ExprOpEq/NEq/And/Or/Impl/ConcatLists" | `grep -nE 'MakeBinOp\\(' src/libexpr/nixexpr.hh` (expect 6) |
| 13 #110 | "Four parallel 'value already seen' sets" | manual — verify across `print.cc`, `print-ambiguous.cc`, `get-drvs.cc`, `value-to-xml.cc` |
| 13 #112 | "cite issue 12899 four times" | `grep -rEn '12899\\b' src/libexpr/primops.cc` (expect 4) |
| 13 #114 | "share one shape three times" — `getStringAttr`/`getBoolAttr`/`getStringSetAttr` | `grep -nE '(getStringAttr\|getBoolAttr\|getStringSetAttr)\\b' src/libstore/derivation-options.cc` |
| 13 #115 | "five entry points" — generation-deletion helpers | manual — enumerate in `src/libstore/profiles.cc` |
| 13 #120 | "five sites in `libexpr/eval.cc`" using getConcurrent | `grep -nE '\\bgetConcurrent\\(' src/libexpr/eval.cc` (expect 5) |

### Vestigial / stdlib (section 14)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 14 #121 | "Approximately 17 sites" / "exactly 17 sites" `// TODO libc++ 16` | `grep -rEn '// TODO libc\\+\\+ 16' src/` (expect 17) |
| 14 #122 | "60+ sites" of `nix::fun<>` | `grep -rEn '\\bnix::fun<\|\\bfun<' src/ \| wc -l` (approx) |
| 14 #124 | "two `SharedSync` users" | `grep -rEn 'SharedSync<' src/` (expect 2 in non-decl files) |
| 14 #125 | "226 `boost::format`/`boost/format`/`HintFmt` matches" | `grep -rE '\\b(boost::format\|boost/format\|HintFmt)\\b' src/ \| wc -l` (expect ~226) |
| 14 #125 | "zero `std::format` uses" | `grep -rEn '\\bstd::format\\b' src/` (expect 0) |
| 14 #128 | "Both call sites pass `true`" — `getIntArg` | `grep -rEn 'getIntArg<' src/nix` (expect 2) |
| 14 #130 | "zero in-tree callers" of `append` shim | `grep -rEn '\\bappend +\\(' src/` filtered for the shim — manual |

### Globals/settings (section 15)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 15 keystone | "21-22 settings structs" via N4 | `grep -rEn '(class\|struct) +\w+ *: *(public\s+)?(virtual\s+)?Config\b' src/` (expect 21-22) |
| 15 #134 | "Zero `dynamic_cast<LocalSettings*>` sites anywhere in the tree" | `grep -rEn 'dynamic_cast<LocalSettings\*>' src/` (expect 0) |
| 15 #137 | "13 TUs (15 production register sites + 1 test-only site = 16 total)" | `grep -rEn 'GlobalConfig::Register +' src/` (expect 16) |
| 15 #138 | "Twelve+ mutation sites flip `settings.readOnlyMode = true;`" | `grep -rEn 'settings\\.readOnlyMode *= *true' src/` (expect 12+) |
| 15 #138 | "two C-API readers" | `grep -rEn 'settings\\.readOnlyMode' src/libstore-c src/libexpr-c` (expect 2 reader-side hits) |
| 15 #140 | "one site" — `impureOutputHash` (definition only) | `grep -rEn 'impureOutputHash\\b' src/` (expect 1) |
| 15 #143 | "Eight reassignment sites for `logger`" (7 production + 1 test) | `grep -rEn '\\blogger *= *(make\|std::move\|nullptr)' src/` (expect 8) |
| 15 #144 | "19 separate `Config`-derived globals" (legacy claim — N4 corrects to 21-22) | same as 15 keystone |
| 15 #145 | "73 call sites in 35 files" — `experimentalFeatureSettings` | `grep -rEn 'experimentalFeatureSettings\\.(require\|isEnabled)\\b' src/ \| wc -l` (expect 73) |
| 15 #145 | "41 sites in 24 files" — `.require` alone | `grep -rEn 'experimentalFeatureSettings\\.require\\b' src/ \| wc -l` (expect 41) |
| 15 #146 | "14 distinct names in `src/`" — `_NIX_TEST_*` / `_NIX_FORCE_*` | `grep -rEoh '_NIX_(TEST\|FORCE)_[A-Z0-9_]+' src/ \| sort -u \| wc -l` (expect 14) |
| 15 #146 | "20 tree-wide" — same vars including `tests/` | `grep -rEoh '_NIX_(TEST\|FORCE)_[A-Z0-9_]+' src/ tests/ \| sort -u \| wc -l` (expect 20) |
| 15 #146 | "`_NIX_FORCE_HTTP` ×3" sites | `grep -rEn '_NIX_FORCE_HTTP\\b' src/` (expect 3) |

### libstore/build audit (section 16)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 16 #150 | "six caches" — Worker per-goal-kind weak-pointer maps | manual — enumerate in `src/libstore/include/nix/store/build/worker.hh` |
| 16 #154 | "590-line monolith ... five nested lambdas" — `registerOutputs` | `awk '/registerOutputs *\\(\\)/{flag=1} flag{count++} /^}/{if(flag){print count;exit}}' src/libstore/unix/build/derivation-builder.cc` (line count not allowed in docs but valid in script) |

### Cross-cutting (section 17)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 17 #161 | "two `windows/` subdirectories" (libstore + libutil) | `find src -type d -name windows` (expect 2) |
| 17 #162 | "Three Windows-only public headers" | `ls src/libutil/windows/include/nix/util/windows-*.hh` (expect 3) |
| 17 #162 | "consumed exactly four times outside `windows/`" | `grep -rEln 'windows-(async-pipe\|environment\|known-folders)\\.hh' src/ \| grep -v '/windows/'` (expect 4) |
| 17 #164 | "~19 (12 in `file-descriptor.hh`, 7 in `file-system.hh`)" `#ifdef _WIN32` switches | `grep -cE '^#(if(n?def\|)\|elif).*\\b_WIN32\\b' src/libutil/include/nix/util/file-descriptor.hh src/libutil/include/nix/util/file-system.hh` |
| 17 #165 | "duplicated between `derivations.hh` and `parsed-derivations.hh`" — `DerivationOutputs` | `grep -rEn 'typedef +std::map<std::string, DerivationOutput> +DerivationOutputs' src/libstore/include` (expect 2) |
| 17 #166 | "9 Source/Sink, 21 Store, 12 EvalState" forward-decl unique headers | three commands: `grep -rln 'struct +Sou?rce *;' src/libutil/include src/libstore/include \| sort -u \| wc -l` (Source); `grep -rln 'class +Store *;' src/libstore/include src/libexpr/include src/libutil/include \| sort -u \| wc -l` (Store); `grep -rln 'class +EvalState *;' src/libexpr/include src/libcmd/include \| sort -u \| wc -l` (EvalState) — expect 9, 21, 12 |
| 17 #166 | "13/22/12 lines including duplicates" | `grep -rEn '\\b(struct +Source\|struct +Sink) *;' src/`, etc. — line counts |
| 17 #167 | "10 in libutil" buffer-size literals | `grep -rEn '\\b(65536\|64\\*1024\|32\\*1024\|128\\*1024)\\b' src/libutil \| wc -l` (expect 10) |
| 17 #167 | "19 tree-wide" buffer-size literals | `grep -rEn '\\b(65536\|64\\*1024\|32\\*1024\|128\\*1024)\\b' src/ \| wc -l` (expect 19) |
| 17 #168 | enumerated cache schema versions | `grep -rEn '(binary-cache-v\|eval-cache-v\|fetcher-cache-v\|tarball-cache-v\|nixSchemaVersion\|expectedJsonVersionDerivation)' src/` |
| 17 #169 | "16 friend declarations" (10 `Expr*` with one duplicate, 3 `prim_*`, `Value`, `ListBuilder`) | `grep -cE '^[[:space:]]*friend\\b' src/libexpr/include/nix/expr/eval.hh` (expect 16) |
| 17 #169 | "10 `Expr*` lines" | `grep -cE '^[[:space:]]*friend +struct +Expr\\w+;' src/libexpr/include/nix/expr/eval.hh` (expect 10) |
| 17 #170 | "Worker is `friend` of three concrete `Goal` subclasses (plus the inner `Waker`)" | `grep -rEn '^[[:space:]]*friend +class +Worker\\b' src/libstore` (expect 4 — 3 goals + Waker) |
| 17 #172 | "64-65 sites" — `unreachable()` tree-wide | `grep -rEn '\\bunreachable\\(\\)' src/ \| wc -l` (expect 64-65) |
| 17 #173 | "10 total pragma sites (8 -Wswitch-enum, 1 -Woverloaded-virtual, 1 -Wimplicit-fallthrough)" | `grep -rEn '#pragma +GCC +diagnostic +ignored' src/ \| wc -l` (expect 10); plus per-warning subcounts |
| 17 #173 | "lexer.l has 1" pragma (catalog correction from "twice") | `grep -cE '#pragma +GCC +diagnostic +ignored' src/libexpr/lexer.l` (expect 1) |
| 17 #174 | enumerated `HAVE_*` probes | `grep -rEoh 'HAVE_[A-Z0-9_]+' src/ \| sort -u` (catalog cites a list; verify subset) |
| 17 #175 | "5 friend lines across 3 levels" in `Bindings` | `grep -cE '^[[:space:]]*friend\\b' src/libexpr/include/nix/expr/attr-set.hh` (expect 5) |

### Daemon protocol audit (section 18)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 18 #176 | "37 case labels (35 distinct bodies after collapsing)" | `grep -cE '^[[:space:]]*case +WorkerProto::Op::' src/libstore/daemon.cc` (expect 37) |
| 18 #176 | enumerated case-label names | `grep -nE '^[[:space:]]*case +WorkerProto::Op::' src/libstore/daemon.cc` |
| 18 #177 | "~25 of the 31 `performOp` arms" share startWork/stopWork shape | manual — dispatch shape inspection |
| 18 #178 | "11 throw-shaped trust-check sites plus 1 censor-clamp" | `grep -nE '!trusted\|trusted\\b\\)\\.(throw\|isOverridden)' src/libstore/daemon.cc` (manual cross-check) |
| 18 #179 | "six sites" — version-gating in `performOp` | `grep -nE 'protoVersion.*\\{1,(21\|22\|23\|25\|27\|29\|37)\}' src/libstore/daemon.cc` |
| 18 #182 | "~25 times" — `RemoteStore::*` opcode methods | manual — enumerate methods in `src/libstore/remote-store.cc` |
| 18 #183 | "Five obsolete opcodes" — AddTextToStore/QueryDeriver/QueryDerivationOutputs/QueryDerivationOutputNames/SyncWithGC | `grep -rEn '\\b(AddTextToStore\|QueryDeriver\|QueryDerivationOutputs\|QueryDerivationOutputNames\|SyncWithGC)\\b' src/libstore/include/nix/store/worker-protocol.hh` |
| 18 #183 | "MINIMUM_PROTOCOL_VERSION ... currently `(1 << 8 | 18)`" | `grep -nE 'MINIMUM_PROTOCOL_VERSION' src/libstore/include/nix/store/worker-protocol.hh` |
| 18 #184 | "four-layer call stack" (Connection/ConnectionHandle/getConnection/processStderr) | manual — verify in `src/libstore/include/nix/store/remote-store-connection.hh` |

### libexpr eval-core: fetcher/lookup/JSON (section 19)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 19 #189 | "four orthogonal bool flags" in `FetchTreeParams` | `grep -nE 'struct +FetchTreeParams' src/libexpr/primops/fetchTree.cc` then read body |
| 19 #195 | "only one `lookupPathHooks` entry registered in-tree (`flake`)" | `grep -rEn 'lookupPathHooks\\[' src/libcmd src/libflake` (expect 1) |

### libexpr eval-core: cache/attr-set/profiler (section 20)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 20 #201 | "no `PRAGMA user_version` anywhere in `eval-cache.cc`" | `grep -nE 'PRAGMA +user_version' src/libexpr/eval-cache.cc` (expect 0) |
| 20 #203 | "All ten 'set*' methods" wrap body in `doSQLite` | `grep -nE '\\bset(Attrs\|String\|Bool\|Int\|ListOfStrings\|Placeholder\|Missing\|Misc\|Failed)\\b' src/libexpr/eval-cache.cc` (expect 10 method bodies) |
| 20 #205 | "13 `Counter` members" (6 in `EvalMemory::Statistics` + 7 in `EvalState`) | manual — enumerate `Counter` members in `src/libexpr/include/nix/expr/eval.hh` and `eval-memory.hh` |
| 20 #206 | "`maxLayers = 8`" magic constant | `grep -nE 'static +constexpr +unsigned +maxLayers *= *8\\b' src/libexpr/include/nix/expr/attr-set.hh` |
| 20 #207 | "`chunkSize = 65536` and `MaxChunks = numeric_limits<uint32_t>::max() / 65536`" | `grep -nE 'chunkSize *= *65536\\|MaxChunks *=' src/libexpr/include` |
| 20 #209 | "23 settings on `EvalSettings`" partitioned 7+2+4+1+9 | manual — count `Setting<` lines in `src/libexpr/include/nix/expr/eval-settings.hh` (expect 23) |

### libexpr eval-core: EvalState/Value (section 21)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 21 #210 | "20 members" claimed scattered across `primops.cc`/`paths.cc` (catalog corrects: 13 in `eval.cc`, 4 in `primops.cc`, 3 in `paths.cc`) | manual — locate definitions |
| 21 #211 | "10 force* overloads" (corrected from claim of 16) | `grep -nE '\\b(forceInt\|forceFloat\|forceBool\|forceFunction\|forceString\|forceStringNoCtx\|forceAttrs\|forceList\|evalBool\|evalAttrs)\\b' src/libexpr/eval.cc src/libexpr/include/nix/expr/eval.hh` |
| 21 #213 | enumeration of consumers of `vNull`/`vTrue`/`vFalse`/`vEmptyList` | `grep -rEn '&Value::v(Null\|True\|False\|EmptyList)\\b' src/` |
| 21 #216 | "no namespace-scope `static_assert(sizeof(Value) == 16)`" | `grep -rEn 'static_assert *\\( *sizeof\\(Value\\) *== *16' src/libexpr/include` (expect 0 at namespace scope) |
| 21 #217 | "16 friend declarations" — same as #169 | already covered |
| 21 #217 | "duplicate `friend struct ExprVar`" | `grep -cE '^[[:space:]]*friend +struct +ExprVar *;' src/libexpr/include/nix/expr/eval.hh` (expect 2 — known duplicate) |

### Cross-shard patterns (section 22)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 22 N1 | enumeration of `concurrent_flat_map` and `Sync<map>` sites | `grep -rEn 'concurrent_flat_map<' src/`; `grep -rEn 'Sync<std::(unordered_)?map<' src/` |
| 22 N3 | "Eight static-registration registries" | `grep -rEn '\\bRegister(PrimOp\|Command\|LegacyCommand\|StoreImplementation\|BuiltinBuilder)\\b\|registerInputScheme\\(' src/` (count distinct types — expect 8) |
| 22 N4 | "21-22 settings structs" | already covered |
| 22 N6 | enumerated Source/Sink wrapper types | `grep -rEn 'struct +(Tee\|Length\|Lambda\|Sized\|EnsureRead\|Chain\|Hash\|HashedSink\|RefScan\|PathRefScan\|Rewriting\|HashModulo\|Null\|String\|LogSink\|BuildLog)' src/libutil src/libstore` |
| 22 N7 | enumerated SourceAccessor factories | `grep -rEn '\\bmake[A-Z]\\w*Accessor\\b' src/libutil src/libstore src/libfetchers` |
| 22 N11 | "7-8 places" — iterate-attrset by name | manual — count loop sites in `prim_*` bodies |
| 22 N14 | "12 `MaintainCount` + 2 plain = 14 counters" | `grep -nE 'MaintainCount<uint64_t>\\|nrLocalBuilds\\|nrSubstitutions' src/libstore/include/nix/store/build/worker.hh` |
| 22 N16 | "44 invocations" of `VERSIONED_CHARACTERIZATION_TEST` (≈176 emitted tests) | `grep -rEn 'VERSIONED_CHARACTERIZATION_TEST\\b' src/libstore-tests \| wc -l` (expect 44) |
| 22 N17 | "three leaked `Sync<T*>` sites" — `_fileTransfer`, `windowSize`, `_interruptCallbacks` | `grep -rEn 'static +auto +\\* +const +_\\w+ *= *new +Sync\\b' src/` (expect 3 ish) |
| 22 N18 | "30+ env vars" — production `getEnv` calls | `grep -rEoh 'getEnv\\("[^"]+"\\)' src/ \| sort -u \| wc -l` |
| 22 N20 | "101 `case n*:` lines" | `grep -rEn '^[[:space:]]*case +nValueType::n\\w+:\\|^[[:space:]]*case +n\\w+:' src/libexpr \| wc -l` (expect ~101) |
| 22 N20 | "11+ dispatch sites" | manual |
| 22 N21 | enumerated counter shapes | manual |
| 22 N22 | "9 macro families that emit struct declarations" | manual — enumerate in `src/libutil/include`, `src/libexpr/include`, `src/libstore/include` |
| 22 N23 | "37 cases" / "~25 methods" — same as #176, #182 |  already covered |

### C-API (section 23)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 23 N24 | "14 single-member structs" — opaque wrappers | manual — enumerate in `*_internal.{h,hh}` |
| 23 N29 | "270+ uses" of `NIXC_CATCH_ERRS*` family | `grep -rEn 'NIXC_CATCH_ERRS' src/ \| wc -l` (expect 270+) |

### Test-infrastructure mirrors (section 24)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 24 N28 | "7 sites" — `_NIX_TEST_UNIT_DATA` plumbing in `meson.build` | `grep -rEn '_NIX_TEST_UNIT_DATA' src/*-tests/meson.build` (expect 7) |
| 24 N41 | "25+ fixtures" — `unitTestData = getUnitTestData() / ...` | `grep -rEn 'unitTestData *= *getUnitTestData\\(\\)' src/ \| wc -l` (expect 25+) |

### Dispatch tables and schemas (section 25)

| Source | Claim | Re-derivation |
| ------ | ----- | ------------- |
| 25 N27 | "13+ instances" — `*Impl : *` factory pattern | `grep -rEn 'struct +\\w+Impl +: +' src/ \| wc -l` (approx, then manual cull) |
| 25 N30 | "~25 sites" — `if (json) {` in modern CLI | `grep -rEn 'if +\\(json\\) +\\{' src/nix \| wc -l` (expect ~25) |
| 25 N33 | "16 friend declarations in `eval.hh`" | already covered |
| 25 N34 | enumerated cache invalidation idioms | manual |
| 25 N35 | "7-9 X-macros for 'list of names'" | manual — enumerate in `src/libutil/include`, `src/libexpr/include` |
| 25 N36 | "14 `make*Sink` factories" | `grep -rEn '\\bmake[A-Z]\\w*Sink\\b' src/libutil src/libstore \| wc -l` |
| 25 N37 | enumerated magic file headers | manual |
| 25 N38 | "9+ `extern template` instantiation lists" | `grep -rEn 'extern +template\\b' src/ \| wc -l` |
| 25 N40 | "4 schemes" with URL-query handling drift | manual — `path`, `git`, `mercurial`, `github` |
| 25 N42 | "five times" — atomic ID counter pattern | enumerated; manual |
| 25 N43 | "Six+ subsystems" with hand-rolled `std::thread` | `grep -rEn '\\bstd::thread +\\w+' src/libutil src/libstore` (manual cull for "owned member") |
| 25 N44 | enumerated `BaseSetting<T>` specialisation sites | `grep -rEn 'BaseSetting<\\w+>::(parse\|to_string\|trait\|appendOrSet)' src/` |

### Consolidation log "Verified counts" block

The consolidation log reproduces ten counts in a "Verified counts" block
that the script must check verbatim:

| Log claim | Re-derivation |
| --------- | ------------- |
| "GlobalConfig::Register sites: 16" | `grep -rEn 'GlobalConfig::Register +' src/ \| wc -l` |
| "case WorkerProto::Op:: labels: 37" | `grep -cE '^[[:space:]]*case +WorkerProto::Op::' src/libstore/daemon.cc` |
| "friend declarations in eval.hh: 16" | `grep -cE '^[[:space:]]*friend\\b' src/libexpr/include/nix/expr/eval.hh` |
| "friend class Worker: 4 sites" | `grep -rEn '^[[:space:]]*friend +class +Worker\\b' src/libstore/include \| wc -l` |
| "forward-decl: 9 Source/Sink, 21 Store, 12 EvalState" | three greps as above |
| "unreachable() sites: 64-65 tree-wide" | `grep -rEn '\\bunreachable\\(\\)' src/ \| wc -l` |
| "#pragma GCC diagnostic ignored: 10 total" | `grep -rEn '#pragma +GCC +diagnostic +ignored' src/ \| wc -l` |
| "_NIX_(TEST\|FORCE)_* distinct names in src/: 14" | `grep -rEoh '_NIX_(TEST\|FORCE)_[A-Z0-9_]+' src/ \| sort -u \| wc -l` |
| "// TODO libc++ 16: 17" | `grep -rEn '// TODO libc\\+\\+ 16' src/ \| wc -l` |
| "impureOutputHash: 1 site" | `grep -rEn 'impureOutputHash\\b' src/ \| wc -l` |

## verify-catalog-counts.sh design

### Purpose

A single script that enumerates the count claims above, runs each
verifying command against the current `src/`, compares the result
against the recorded claim, and exits non-zero if any drift is found.

### Inputs

- The catalog files under `doc/inventory/candidates/` and the
  consolidation log under `doc/inventory/review/`.
- A sidecar manifest (see "Catalog format recommendation" below) that
  records each claim's expected value plus the verifying shell
  command.
- The current source tree at `$REPO_ROOT/src/`.

### Algorithm sketch

1. Read the manifest into an array of `(id, expected, command,
   tolerance, methodology_note)` records.
2. For each record:
   a. Run `command` in a subshell with `cd $REPO_ROOT`. Capture stdout
   as the actual count.
   b. If `expected` is a single integer: compare equal.
   c. If `expected` is a range `"low-high"`: compare actual is within
   `[low, high]`.
   d. If `expected` is `"approx"`: warn, do not fail.
   e. If `expected` includes a methodology note ("`src/`-only" vs
   "tree-wide"), the command must match that scope; the script
   should not silently switch scopes.
3. Accumulate drifts; print a summary table sorted by claim ID.
4. Exit non-zero if any non-`approx` claim drifted.

### Implementation choice

Bash is sufficient. Python is overkill for what is essentially a
loop of `grep | wc -l` invocations and integer compares. A bash
script can drive a TOML or pipe-delimited manifest with awk/jq.
Recommended structure:

```
verify-catalog-counts.sh
  driver — reads manifest, runs each row, accumulates drift list
  manifest format — pipe-delimited or TOML (see below)
  helpers:
    run_count <command> -> integer
    compare <id> <expected> <actual> <tolerance>
    report_drift <id> <expected> <actual>
```

### Exit codes

- 0: every claim verified.
- 1: at least one claim drifted; script printed a summary.
- 2: script error (manifest unreadable, grep failed, etc.).

### Tolerance handling

Several catalog claims cite tolerance bands:

- "21-22 settings structs"
- "64-65 unreachable() sites"
- "~25 sites" (approximate)
- "60+ sites" (lower bound)

The manifest must encode the tolerance shape (`exact`, `range
low..high`, `min N`, `approx`) so the script doesn't false-positive
on a count that was always a range.

### CI integration

The script should produce machine-readable output (JSON or TAP) for
CI consumption in addition to a human-readable summary. A small
`--format=json` flag covers both audiences.

## Catalog format recommendation

Two approaches were considered:

### Option A: in-prose sentinel markers

Embed an HTML comment after each prose count:

```
"37 case labels" <!-- count: 37 cmd: grep -c '^case WorkerProto::Op::' src/libstore/daemon.cc -->
```

Pros:
- Single source of truth: prose and machine-readable claim are
  colocated.
- Editors only have to update one place; the marker is right next to
  the prose.

Cons:
- Markdown comments are fragile under line-wrapping; a reflow can
  split the comment across lines.
- The verifier has to parse markdown, identify each marker, and
  extract the embedded shell command. Shell commands inside HTML
  comments are awkward to escape (especially angle brackets, pipes,
  and backticks).
- Updates to the script's command-quoting conventions require
  re-editing every marker in every file.
- The catalog already has 80+ count claims spread across 25 files;
  retrofitting markers everywhere is mechanical but invasive.

### Option B: sidecar manifest

A single `doc/inventory/counts.toml` (or `counts.yml`) that maps each
claim ID to its verifying command, expected value, and tolerance:

```toml
[claims.137]
file = "doc/inventory/candidates/15-globals-settings.md"
prose = "15 production register sites + 1 test-only site = 16 total"
expected = 16
tolerance = "exact"
command = "grep -rEn 'GlobalConfig::Register +' src/ | wc -l"
methodology = "src-only, includes tests"

[claims.169]
file = "doc/inventory/candidates/17-cross-cutting.md"
prose = "16 friend declarations"
expected = 16
tolerance = "exact"
command = "grep -cE '^[[:space:]]*friend\\b' src/libexpr/include/nix/expr/eval.hh"
```

Pros:
- The manifest is the script's input directly — no markdown parsing.
- Shell commands live in TOML strings with normal quoting rules.
- One file to inspect, diff, and review.
- Tolerances and methodology notes have first-class fields.
- Adding new claims is one TOML record, not a markdown edit plus a
  marker convention.

Cons:
- The manifest can drift from prose. If a body says "15 sites" and
  the manifest says `expected = 16`, the verifier will pass against
  the manifest and the prose will be wrong.
- Two-place edit cost: when a count drifts and the catalog body is
  updated, the manifest must update in lockstep.

### Recommendation: sidecar manifest (Option B)

Rationale:

1. **Drift symmetry.** Both options have a drift risk. Option A
   risks marker syntax getting mangled under formatting. Option B
   risks manifest-vs-prose drift. The latter is *visible* under code
   review (the diff shows both the body edit and the manifest edit
   side-by-side); the former is invisible until the verifier breaks.

2. **Ergonomics.** The manifest format is what the script reads
   directly. Option A requires a markdown parser plus a comment
   extractor. The catalog has 80+ claims; the parser has to handle
   each one robustly. TOML is parsed by 200 lines of any
   competent library and has zero quoting ambiguity for shell
   commands.

3. **Discoverability.** A manifest is the canonical "list of
   things this script checks". Option A scatters that list across 25
   files; producing a complete inventory requires running a parser.

4. **Tolerance bands.** Several claims have legitimate ranges
   (21-22, 64-65). Option A's marker syntax has to encode ranges in
   prose-readable comments; Option B has a `tolerance = "21-22"`
   field directly.

**Mitigating manifest-vs-prose drift:** the verifier itself can
optionally *include the prose snippet* (already a `prose =` field
above), and a second linting pass can confirm the prose snippet
appears in the cited file. A failing snippet check is a clear
signal that the prose was edited without updating the manifest.
That secondary check is cheap and addresses Option B's main
weakness.

**Concretely:** create
`doc/inventory/counts.toml` populated from the inventory table in
this document. Add a top-level `[meta]` section noting the format
version. Reference the manifest from the catalog README's "Related"
section.

## Verification cadence

Three cadence options were considered:

### A. On every PR that touches `src/`

Pros: drifts caught at introduction.

Cons: noisy. The vast majority of PRs that touch `src/` do not
affect counts the catalog tracks; running the verifier per PR adds
CI time for no signal. PRs that *do* drift counts (e.g. landing a
new daemon opcode) will fail with a confusing error pointing at a
documentation file the PR author didn't edit.

### B. Weekly cron / nightly CI

Pros: low overhead, catches drift before it accumulates. CI has a
weekly slot with no perf cost.

Cons: drift is only detected after the fact. By the time the cron
fires, the offending PR is already merged.

### C. Manually before any catalog-driven refactor PR is opened

Pros: the verification happens at the moment the catalog is
*consumed* — exactly when the consumer cares whether the counts are
current.

Cons: easy to forget. If the cataloger doesn't run the verifier,
the refactor PR proceeds against stale counts.

### Recommendation: B + C

- **B (weekly cron)** is the structural backstop. A weekly CI job
  runs `verify-catalog-counts.sh`; on drift, it opens (or updates) a
  GitHub issue tagged `catalog-drift` listing each drifted claim.
  Cost: one CI run per week, ~30 seconds of grep.

- **C (manual before refactor PR)** is the workflow expectation.
  The catalog README (or a `CONTRIBUTING.md`-style note) says: "Before
  opening a PR that cites a catalog count, run
  `./scripts/verify-catalog-counts.sh` and reconcile any drifts."

**Explicitly *not* A:** running on every src-touching PR is wrong
because the verifier's failure mode (count drifted) is decoupled
from the PR's actual change. A PR that adds a new daemon opcode
shouldn't fail catalog verification — the catalog should *update*
as part of that PR. Making catalog-up-to-date a precondition for
src-changes inverts the dependency: the catalog is downstream of
the source.

The weekly cron + drift-issue pattern is the right granularity:
drifts are detected within a week, but no single src-PR is blocked
on catalog state.

## Counts that can't be automated

Several catalog claims involve qualitative judgement and cannot be
re-derived purely by grep. The script should *not* attempt to verify
these; they are flagged for human review on a slower cadence.

- **#172: "8 of 64 `unreachable()` sites are reachable from runtime
  input."** The total (64-65) is countable; the "reachable from
  runtime input" subset is a manual classification that requires
  reading each site and tracing data flow. Same applies to "the
  latent-crash variant in `runDebugRepl`" identification.

- **#178: "11 throw-shaped trust-check sites plus 1 censor-clamp."**
  The classification (which sites are trust-checks vs general error
  paths) is a reading judgement; a literal grep on `!trusted` will
  miss the censor-clamp, the implicit checks via `info.ultimate =
  false`, and similar shape variants.

- **#177: "~25 of the 31 `performOp` arms ... share the
  startWork/stopWork shape."** The "share the shape" judgement is
  interpretive — the verifier can count case labels (37) but not
  classify which fit the idiom.

- **#36: "five parallel value renderers."** Counting renderers
  requires recognising which functions implement the
  per-`nValueType` switch pattern. A grep can find candidates but
  judgement is needed to classify each as "renderer" or not.

- **#28: "four overrides ... follow a pattern of base-then-platform-
  specific suffix."** Pattern-matching overrides against a "base
  then suffix" shape is interpretive.

- **#21: "four-times-cut-and-pasted shape" of `Setting<T>`
  specialisations.** Counting how cleanly the cut-and-paste is — vs
  legitimate variation per type — needs a reading.

- **#65: "Almost every `prim_*` opens with `state.forceValue`/
  `forceAttrs`/...".** "Almost every" is a hand-wavy classification.
  A grep can count `prim_*` definitions and the helpers used; the
  "almost every" judgement is the catalog body's, not the script's.

- **#181: trusted-only allowlist contents in `ClientSettings::apply`.**
  The allowlist literals are countable; whether an entry "should" be
  on the list is a security judgement.

- **N1, N3, N9, N12, N17, N19 etc. — meta-architectural
  observations.** These name patterns the catalog wants us to
  recognise; the verifier can count *instances* of the pattern but
  cannot *recognise* the pattern itself.

The cadence for these qualitative claims should be: re-validate
during major refactor passes (every 6-12 months, or before any
section consolidation). A note in the catalog README under
"Manual-verification claims" should list them.

## Drift-handling process

When `verify-catalog-counts.sh` reports drift, the response depends
on what drifted.

### Drift in a count

Example: catalog body says "16 friend declarations"; verifier
reports 17.

**Action:** open a PR that updates *both* the catalog body and the
manifest entry. The PR description must:

1. Cite the upstream commit (or commits) that introduced the new
   site.
2. Update the prose count in the body.
3. Update the `expected =` field in `counts.toml`.
4. If the new count crosses a verdict threshold (e.g. a
   `PARTIALLY VALID` count that originally cited "11 sites" now
   reaches 15+), mention that the verdict may need re-review and
   tag the candidate's section maintainer.

The PR should *not* bundle other unrelated changes — count fixes are
their own discrete commits per CLAUDE.md's "Don't bundle unrelated
cleanup" rule.

### Drift in a verdict

Example: a candidate previously verdict-tabled as `VALID` is now
fully resolved upstream (the call sites were deleted; the helper
landed; the duplication is gone).

**Action:** re-review the candidate end-to-end. The original
verifying inspection may be invalidated. Specifically:

1. Read the body's "Validation:" paragraph end-to-end.
2. Confirm the cited mechanism is still in source (or note its
   removal).
3. Update the verdict-table row and the body opener.
4. If the verdict moves to `OBSOLETE`, mark it but do *not* delete
   the entry — the catalog convention is to mark obsolete rather
   than remove (see the "Verdict legend" in the README).

### Drift in a methodology note

Example: a count was scoped to `src/` and the `tests/` tree has
since absorbed sites that should be counted, or vice versa.

**Action:** decide which methodology the catalog wants to track,
update the manifest's `methodology` field, and update the body
prose to match.

### Drift in a deferred follow-up

Example: an E1-E8 follow-up has surfaced new evidence that
invalidates a candidate.

**Action:** flag in the consolidation log and update both the body
and the verdict table.

### Process expectation

Drift PRs are small (1-3 files touched typically) and should land
quickly. They are *not* candidates for code review beyond
"verifier output matches body". Reviewer focus is on:

- Did the PR author cite the upstream commit that caused the drift?
- Does the manifest change match the body change?
- If the verdict moved, was the re-review documented?

## Open questions

1. **Should the manifest live under `doc/inventory/` or under
   `scripts/`?** Putting it next to the catalog files is more
   discoverable; putting it next to the script is more conventional.
   Recommendation: `doc/inventory/counts.toml` (with the script
   under `scripts/verify-catalog-counts.sh`) since the manifest is
   *catalog metadata*, not script implementation.

2. **What is the policy for *new* catalog claims that are added
   without manifest entries?** Two options: (a) require every
   numeric claim in the catalog to have a manifest entry (enforced
   by a CI lint that scans bodies for digit-shaped tokens); (b)
   accept that some claims are not worth automating. Recommendation:
   start with (b); add (a) only if drift becomes a recurring issue.

3. **Should the consolidation log be under verifier scope, or
   only the candidate files?** The log's "Verified counts" block at
   the end is a useful target. Recommendation: include the log;
   it has only ten claims and they overlap with claims already in
   candidate bodies.

4. **How does the verifier handle counts that are intentional
   ranges (e.g. "64-65")?** The manifest's `tolerance = "range
   low..high"` covers it, but the verifier needs a clear policy on
   when a count crosses the range boundary (e.g. drifts to 66). The
   recommendation: any drift outside the recorded range is a
   normal drift requiring a body+manifest update — the range was
   a representation of methodology uncertainty, not a "count is
   allowed to drift within this band over time" license.

5. **How does the verifier handle file-renames upstream?** A
   command like `grep -cE 'pattern' src/libstore/daemon.cc` breaks
   silently if the file moves. Two mitigations: (a) the script
   detects an empty / missing-file result distinctly from "0
   matches" and treats it as an error; (b) before reporting drift,
   the script does a `find src -name $basename` sanity check.
   Recommendation: implement (a); add (b) only if file-renames
   become common.

6. **What about counts derived from `meson.build`?** N28's
   `_NIX_TEST_UNIT_DATA` plumbing lives in build files, not C++
   source. The script must include `src/*-tests/meson.build` in
   scope; trivially handled but worth noting.

7. **Should the verifier produce a "counts changed" diff for human
   review even when within tolerance?** A weekly cron that says
   "all 80 claims pass" hides slow drift inside ranges. A
   verbose mode (`--show-changes`) printing actual-vs-expected for
   every claim would let a human spot a trend. Recommendation:
   add `--show-changes`; default mode is summary-only.

8. **How does the verifier handle the `boost::format` vs
   `std::format` count from #125 ("226 / 0")?** That count is
   *intended* to drift over time as the migration lands. The
   manifest entry needs a different shape: instead of `expected =
   226`, it should be `expected = "decreasing from 226"` with a
   semantic of "the verifier should warn but not fail when this
   number decreases". Recommendation: add a `tolerance =
   "monotonic-decrease"` (or `monotonic-increase`) shape for
   migration-tracking counts and document them as
   "migration-progress" rather than "drift" semantically.
