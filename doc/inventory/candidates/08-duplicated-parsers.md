# Duplicated parsers / regexes

Candidates 61-68. All eight VALID, with one file-path correction (#61).

| # | Verdict | Effort |
| - | ------- | ------ |
| 61 | VALID | trivial |
| 62 | VALID | medium |
| 63 | VALID | small |
| 64 | VALID | trivial |
| 65 | VALID | medium |
| 66 | VALID | small |
| 67 | VALID | small |
| 68 | VALID | trivial |

---

61. **Two regex stash points for git-style refs/revisions.** `url-parts.hh` declares `refRegexS`/`revRegexS`/`refAndOrRevRegex` with `extern std::regex refRegex`/`revRegex` defined in `url.cc`; `git.cc::parseLsRemoteLine` defines its own anonymous `std::regex line_regex`. The `line_regex` matches a different shape (whole ls-remote line) so a direct merge isn't possible, but a shared tokeniser would help.
    - ../verified/02-libutil-data.md
    - **Validation:** VALID. **Correction:** `line_regex` is in `src/libutil/git.cc`, not `src/libfetchers/git.cc`. Effort: trivial.

62. **URL vs flakeref vs url-name parsers all walk similar URL shapes.** `tryParseScpStyle` (URL), `parseFlakeRef`/`fromParsedURL`/`parsePathFlakeRefWithFragment`/`parseFlakeIdRef` (flakeref), `getNameFromURL` (url-name), per-`InputScheme` URL handling (fetchers). Five places probe `github|gitlab|sourcehut`-style schemes and the `<owner>/<repo>` path layout.
    - ../verified/02-libutil-data.md, ../verified/14-libfetchers.md, ../verified/15-libflake-libmain.md
    - **Validation:** VALID. Effort: medium.

63. **Path resolution rules sit in two parallel places in libexpr.** `path_start` in `parser.y` (handles absolute, relative, and `~/` paths with their lint diagnoses) and `EvalState::rootPath`/`storePath` in `paths.cc`. The relative-path computation `CanonPath(literal, basePath.path).abs()` has the same shape as `EvalState::rootPath(string_view)`.
    - ../verified/12-libexpr-parse.md
    - **Validation:** VALID. Effort: small.
    - **Branch:** SKIPPED (Rule 6 — same-shape ≠ same-problem). The parser's relative-path branch resolves against `basePath.path` (the directory of the .nix file being parsed); `EvalState::rootPath(string_view)` resolves against cwd via `absPath()`. The two have no behavioural overlap, only structural similarity at the surface. The parser also lacks access to `EvalState` (only `ParserState`), so direct reuse would require an `EvalState` parameter or a free-function refactor that the candidate body does not sketch. No clean shared abstraction exists without changing the semantics of one or both call sites.

64. **`Formals` and `FormalsBuilder` independently implement `has(Symbol)`.** Two parallel containers exist for function formals: `FormalsBuilder` (`std::vector<Formal>` + ellipsis, used during parsing) and `Formals` (`std::span<Formal>` + ellipsis, used post-allocation). Both implement `has(Symbol)` independently with the same lower-bound predicate.
    - ../verified/12-libexpr-parse.md
    - **Validation:** VALID. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libexpr` (free `formalsHas(span<const Formal>, Symbol)` inline next to `Formal`)

65. **`primop_*` argument-validation boilerplate is heavily duplicated.** Almost every `prim_*` opens with `state.forceValue`/`forceAttrs`/`forceList`/`forceString`/`forceStringNoCtx`/`forceBool`/`forceInt`/`forceFloat`, each with a hand-written "while evaluating the Nth argument passed to builtins.<name>" message. A `validateArg(state, n, primop_name, type)` helper would shrink the binary.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. **Reach corrected (per B2):** `grep -rn '"while evaluating the .* argument passed to builtins\."' src/libexpr/` returns **79 canonical-template sites**, plus ~135 additional `state.forceX(...)` call sites in primops/`flake-primops.cc` that use bespoke trace contexts which the canonical helper cannot mechanically migrate (e.g. `prim_genericClosure` uses an empty `errorCtx` plus a custom outer try/catch — `validateArg` would actively get in the way). The original "~hundreds of times" framing is broadly right; the migratable subset is 79. Type-predicate primops (`prim_isNull`...`prim_isPath`) bypass `forceX` entirely — they're #66's territory, not #65's. **Cross-layer relationship:** sits one layer above #211 — #65 migrates the per-primop trace context; #211 consolidates the inner `force*` shells themselves. **Merge with #211 was rejected (per B2):** different risk profiles (#211 internal-only; #65 migrates ~79 user-visible error-trace strings), different consumer scopes (#211's `forceTyped<T>` benefits non-primop callers in `Expr::eval`/`attr-path.cc`/`print.cc`/`flake.cc`; #65's `validateArg<T>` is primop-specific). **Sequencing:** #211 first (~1 week, internal); #65 second (~4-6 weeks, mostly test-golden-file audit). **See also:** B2, #66, #211. Effort: medium.

66. **`prim_isNull` … `prim_isPath` (8 type-predicate primops, plus `prim_isAttrs`/`prim_isList`/`prim_isFunction`).** All identical except for the enum-tag they compare against. A registration macro or table-driven approach would remove the boilerplate.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libexpr` (`makeTypeCheck(ValueType)` factory drives nine `RegisterPrimOp`s)

67. **Numeric primops (`__add`/`__sub`/`__mul`/`__div`).** Each one is the same template instantiated four times: forceValue both args, dispatch on `nFloat` else int, check overflow with `valueChecked()`, raise `EvalError` with a slightly different verb.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libexpr` (`primNumeric<NumOp>` template parameterised on a `NumOp` enum tag; division retains its eager forceFloat-of-the-divisor + zero-check sequencing as a `if constexpr` branch. Error wording preserved byte-for-byte, including the pre-existing "first of the multiplication" phrasing in the float branch -- bundling a wording fix into a refactor commit was deliberately deferred.)

68. **`prim_ceil` and `prim_floor` are byte-for-byte twins** (only `ceil(value)` vs `floor(value)` differs). The precision-loss/overflow blocks and the GitHub issue link are identical.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Compounds with #112 (the four-times-cited issue link). Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libexpr` (`makeRoundingPrimOp(NixFloat (*)(NixFloat), const char *)` factory; both registrations preserved)
