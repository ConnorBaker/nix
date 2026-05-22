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

64. **`Formals` and `FormalsBuilder` independently implement `has(Symbol)`.** Two parallel containers exist for function formals: `FormalsBuilder` (`std::vector<Formal>` + ellipsis, used during parsing) and `Formals` (`std::span<Formal>` + ellipsis, used post-allocation). Both implement `has(Symbol)` independently with the same lower-bound predicate.
    - ../verified/12-libexpr-parse.md
    - **Validation:** VALID. Effort: trivial.

65. **`primop_*` argument-validation boilerplate is heavily duplicated.** Almost every `prim_*` opens with `state.forceValue`/`forceAttrs`/`forceList`/`forceString`/`forceStringNoCtx`/`forceBool`/`forceInt`/`forceFloat`, each with a hand-written "while evaluating the Nth argument passed to builtins.<name>" message. Same shape ~hundreds of times. A `validateArg(state, n, primop_name, type)` helper would shrink the binary.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Compounds with #211 (the `force*` family itself). Effort: medium.

66. **`prim_isNull` … `prim_isPath` (8 type-predicate primops, plus `prim_isAttrs`/`prim_isList`/`prim_isFunction`).** All identical except for the enum-tag they compare against. A registration macro or table-driven approach would remove the boilerplate.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Effort: small.

67. **Numeric primops (`__add`/`__sub`/`__mul`/`__div`).** Each one is the same template instantiated four times: forceValue both args, dispatch on `nFloat` else int, check overflow with `valueChecked()`, raise `EvalError` with a slightly different verb.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Effort: small.

68. **`prim_ceil` and `prim_floor` are byte-for-byte twins** (only `ceil(value)` vs `floor(value)` differs). The precision-loss/overflow blocks and the GitHub issue link are identical.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Compounds with #112 (the four-times-cited issue link). Effort: trivial.
