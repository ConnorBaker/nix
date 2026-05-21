# Inventory — Shard 12: libexpr parse/print

Topic: libexpr parser/lexer/AST and printers (lexer.l, parser.y, parser-state, nixexpr AST, print, print-ambiguous, get-drvs, paths, search-path, json-to-value, value-to-json, value-to-xml, repl-exit-status).

## File: src/libexpr/lexer.l

### Namespaces
- `nix` — opened with `namespace nix { ... }` to host the file-local helpers `unescapeStr` and `requireExperimentalFeature`, and (after `%%`) to define `setReplBindingsMode`.
- `nix::lexer::internal` — pulled in via `using namespace` at file scope so that `initLoc` / `adjustLoc` are reachable from the macros.

### Classes / structs / enums
None defined here directly; types come from `nixexpr.hh` / `parser-state.hh` / the Bison-generated header.

### Functions
- `unescapeStr(char * s, size_t length, std::function<Pos()> && pos) -> StringToken` — kind: static helper. Path: src/libexpr/lexer.l. Purpose: Decode `\n`/`\r`/`\t`/`\\X` escapes, normalise `\r` and `\r\n` into `\n`, and produce a `StringToken` view over the rewritten buffer; calls `forceNoNullByte` when an unescaped NUL appears.
- `requireExperimentalFeature(const ExperimentalFeature & feature, const Pos & pos)` — kind: static helper. Path: src/libexpr/lexer.l. Purpose: Throw a `ParseError` at `pos` unless `experimentalFeatureSettings.isEnabled(feature)`; gates the `<|`/`|>` pipe operators.
- `setReplBindingsMode(yyscan_t scanner)` — kind: free function in `nix`. Path: src/libexpr/lexer.l (post-`%%` block). Purpose: Push the lexer onto `REPL_BINDINGS_MODE`; the following matched rule emits the synthetic `REPL_BINDINGS` token before normal lexing resumes.

### Type aliases
- `using enum nix::Parser::token::token_kind_type` — flat-imports the token enum so the lexer rules can `return IF;` etc.
- `using YYSTYPE = nix::Parser::value_type` — Bison value-union alias.
- `using YYLTYPE = nix::Parser::location_type` — alias for `ParserLocation`.

### Macros / globals
- `CUR_POS` — expands to `state->at(*yylloc)`; the current `PosIdx` for diagnostics.
- `YY_USER_INIT` — expands to `initLoc(yylloc)`.
- `YY_USER_ACTION` — expands to `adjustLoc(yyscanner, yylloc, yytext, yyleng);` after each rule match.
- `PUSH_STATE(state)` — expands to `yy_push_state(state, yyscanner)`.
- `POP_STATE()` — expands to `yy_pop_state(yyscanner)`.

### Grammar / tokens (lexer rules)

#### Flex options
- `%option 8bit reentrant bison-bridge bison-locations align noyywrap never-interactive stack nodefault nounput noyy_top_state extra-type="::nix::LexerState *"`.

#### Start conditions
- `DEFAULT` (`%s`, inclusive) — default state.
- `STRING` (`%x`) — inside `"..."`.
- `IND_STRING` (`%x`) — inside `''...''`.
- `INPATH` (`%x`) — inside path body (no trailing slash).
- `INPATH_SLASH` (`%x`) — inside path body just after a `/` (forbids ending here).
- `PATH_START` (`%x`) — pre-anchor for paths beginning with interpolation (`PATH_SEG${...}` or `~/${...}`).
- `REPL_BINDINGS_MODE` (`%x`) — synthetic-token mode used by `setReplBindingsMode` so the parser sees a leading `REPL_BINDINGS`.

#### Named regex definitions
- `ANY  = .|\n`
- `ID   = [a-zA-Z_][a-zA-Z0-9_'-]*`
- `INT  = [0-9]+`
- `FLOAT = (([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?`
- `PATH_CHAR = [a-zA-Z0-9._\-+]`
- `PATH = {PATH_CHAR}*(/{PATH_CHAR}+)+/?`
- `PATH_SEG = {PATH_CHAR}*/`
- `HPATH = ~(/{PATH_CHAR}+)+/?`
- `HPATH_START = ~/`
- `SPATH = <{PATH_CHAR}+(/{PATH_CHAR}+)*>`
- `URI = [a-zA-Z][a-zA-Z0-9+\-.]*:[a-zA-Z0-9%/?:@&=+$,\-_.!~*']+`

#### Tokens emitted (non-keyword/value)
- `REPL_BINDINGS` — synthetic token returned from the `<REPL_BINDINGS_MODE>` rule (after `yyless(0)` and `unstash`).
- Keywords: `IF`, `THEN`, `ELSE`, `ASSERT`, `WITH`, `LET`, `IN_KW` (for `in`), `REC`, `INHERIT`, `OR_KW` (for `or`).
- Punctuation: `ELLIPSIS` (`...`).
- Operators: `EQ` (`==`), `NEQ` (`!=`), `LEQ` (`<=`), `GEQ` (`>=`), `AND` (`&&`), `OR` (`||`), `IMPL` (`->`), `UPDATE` (`//`), `CONCAT` (`++`).
- Pipe operators (gated by `Xp::PipeOperators`): `PIPE_FROM` (`<|`), `PIPE_INTO` (`|>`).
- `ID` — matched `{ID}`, value emplaced as `StringToken{yytext, yyleng}`.
- `INT_LIT` — matched `{INT}`, parsed via `string2Int<int64_t>`; throws `ParseError` on overflow; value emplaced as `NixInt`.
- `FLOAT_LIT` — matched `{FLOAT}`, parsed via `strtod`; throws `ParseError` if `errno != 0`; value emplaced as `NixFloat`.
- `DOLLAR_CURLY` (`${`) — also matched in `<STRING>`, `<IND_STRING>`, and `<INPATH,INPATH_SLASH>`; pushes `DEFAULT`.
- `'{'` — matched at top level; pushes `DEFAULT`.
- `'}'` — pops state unless current state is `INITIAL` (the bottom-of-stack marker).
- `'"'` — matched at top level (pushes `STRING`) and at `<STRING>` close (pops state).
- `IND_STRING_OPEN` — matched on `''(\ *\n)?`; pushes `IND_STRING`.
- `IND_STRING_CLOSE` — matched on `''` inside `IND_STRING`; pops state.
- `STR` — finished segment of double-quoted string content, fed through `unescapeStr`; also re-used by the `<INPATH,INPATH_SLASH>` path-body interpolation rule (which emits `STR` for the body fragment).
- `IND_STR` — segment of indented-string content, matched as bulk `([^\$\']|\$[^\{\']|\'[^\'\$])+`, plus the special cases `''$` / `$` (literal `$`), `'''` (literal `''`), `''\<c>` (un-escaped via `unescapeStr`), and a lone `'`.
- `PATH` — matched `{PATH}` directly, or via `<PATH_START>{PATH_SEG}` (after rewinding from a `PATH_SEG${`).
- `HPATH` — matched `{HPATH}` directly, or via `<PATH_START>{HPATH_START}`.
- `PATH_END` — synthetic terminator emitted by the `<INPATH>{ANY}` / `<INPATH><<EOF>>` rule when a non-path char follows; rewinds via `yyless(0)` + `unstash`.
- `SPATH` — matched `{SPATH}`.
- `URI` — matched `{URI}`.
- `EOF` — emitted from `<STRING>\$|\\|\$\\` when only those occur before EOF, leaving the parser to fail with the exact location.
- Single-character tokens — produced by the final `{ANY}` rule via `return (unsigned char) yytext[0];`.

#### Scanner actions
- Doc comments: `\/\*\*[^/*]([^*]|\*+[^*/])*\*+\/` resets `LexerState::docCommentDistance` to 0 and stashes the location into `lastDocCommentLoc`.
- Whitespace `[ \t\r\n]+`, `#`-line comments, and long comments `\/\*([^*]|\*+[^*/])*\*+\/` decrement `docCommentDistance` so they don't invalidate the buffered doc comment.
- Path interpolation: `<PATH_START>{ANY}` and `<PATH_START><<EOF>>` are unreachable in practice but exist to satisfy `%option nodefault`; they call `unreachable()`.
- Final `}` rule pops state only when `YYSTATE != INITIAL` (the `INITIAL` bottom-of-stack marker is never popped to avoid empty-stack exceptions on top-level `}`).
- `<INPATH_SLASH>{ANY}` / `<INPATH_SLASH><<EOF>>` throw `ParseError("path has a trailing slash")`.

### Embedded helpers (post-`%%` block)
- `static_assert(std::is_same_v<yyscan_t, void *>)` — confirms that the parser-side forward declaration matches Flex's actual definition.

---

## File: src/libexpr/lexer-helpers.cc

### Namespaces
- `nix::lexer::internal`.

### Functions
- `initLoc(Parser::location_type * loc)` — kind: free function. Path: src/libexpr/lexer-helpers.cc. Purpose: Reset `loc->beginOffset` and `loc->endOffset` to 0 at scanner start.
- `adjustLoc(yyscan_t yyscanner, Parser::location_type * loc, const char * s, size_t len)` — kind: free function. Path: src/libexpr/lexer-helpers.cc. Purpose: Per-token bookkeeping; calls `loc->stash()`, attaches the buffered doc comment via `positionToDocComment.emplace(...)` when `docCommentDistance == 1`, increments `docCommentDistance`, and slides `beginOffset = endOffset; endOffset += len`.

---

## File: src/libexpr/lexer-helpers.hh

### Namespaces
- `nix::lexer::internal`.

### Functions (declarations)
- `void initLoc(Parser::location_type * loc)` — see lexer-helpers.cc.
- `void adjustLoc(yyscan_t yyscanner, Parser::location_type * loc, const char * s, size_t len)` — see lexer-helpers.cc.

---

## File: src/libexpr/parser.y

### Namespaces
- `nix` — opened in `%code requires` (between `BISON_HEADER` guards) to declare typedefs and `parseExprFromBuf` / `parseReplBindingsFromBuf` / `setReplBindingsMode`; opened again in the post-`%%` block to define those entry points.
- `nix::parser` — Bison-generated `BisonParser` lives here (via `%define api.namespace { ::nix::parser }`).

### Classes / structs / enums (declared via `%code requires`)
- `nix::DocCommentMap` — kind: typedef. Path: src/libexpr/parser.y. Purpose: `boost::unordered_flat_map<PosIdx, DocComment, std::hash<PosIdx>>` mapping a code position to the doc comment immediately preceding it.

### Functions
- `parseExprFromBuf(char * text, size_t length, Pos::Origin origin, const SourcePath & basePath, Exprs & exprs, SymbolTable & symbols, const EvalSettings & settings, PosTable & positions, DocCommentMap & docComments, const ref<SourceAccessor> rootFS) -> Expr *` — kind: free function. Path: src/libexpr/parser.y. Purpose: Run lexer/parser on a heap-modifiable text buffer and return the parsed root `Expr *`.
- `parseReplBindingsFromBuf(char * text, size_t length, Pos::Origin origin, const SourcePath & basePath, Exprs & exprs, SymbolTable & symbols, const EvalSettings & settings, PosTable & positions, DocCommentMap & docComments, const ref<SourceAccessor> rootFS) -> ExprAttrs *` — kind: free function. Path: src/libexpr/parser.y. Purpose: Same as above but pre-pushes `REPL_BINDINGS_MODE` and asserts/`dynamic_cast`s the result to `ExprAttrs *`.
- `setReplBindingsMode(yyscan_t scanner)` — declared here in `%code requires`; defined in lexer.l.
- `setDocPosition(const LexerState & lexerState, ExprLambda * lambda, PosIdx start)` — kind: static helper. Path: src/libexpr/parser.y. Purpose: If `lexerState.positionToDocComment` has an entry for `start`, call `lambda->setDocComment` with it.
- `makeCall(Exprs & exprs, PosIdx pos, Expr * fn, Expr * arg) -> Expr *` — kind: static helper. Path: src/libexpr/parser.y. Purpose: Append `arg` to an existing `ExprCall` (left-associative apply) or wrap with a new `ExprCall`.
- `parser::BisonParser::error(const location_type & loc_, const std::string & error)` — kind: Bison-required override. Path: src/libexpr/parser.y. Purpose: Translate parser errors into `ParseError`; collapses location to `endOffset` when the message starts with "syntax error, unexpected end of file".

### Macros / globals
- `BISON_HEADER` — include guard for the `%code requires` block when imported from `parser-tab.hh`.
- `YY_DECL` — declares the reentrant `yylex(value_type *, location_type *, yyscan_t, ParserState *)` signature.
- `YYLLOC_DEFAULT(Current, Rhs, N)` — offset-only location merging macro.
- `CUR_POS` — `state->at(yylhs.location)` for parser actions.
- `SET_DOC_POS(lambda, pos)` — `setDocPosition(state->lexerState, lambda, state->at(pos))`.
- `typedef void * yyscan_t` — forward declaration to avoid pulling `lexer-tab.hh` into `parser-tab.hh`.

### Grammar / tokens

#### Bison directives
- `%skeleton "lalr1.cc"`, `%define api.location.type { ::nix::ParserLocation }`, `%define api.namespace { ::nix::parser }`, `%define api.parser.class { BisonParser }`, `%locations`, `%define parse.error detailed`, `%defines`, `%expect 0`, `%define api.value.type variant`.
- Parameters: `%parse-param { void * scanner }`, `%parse-param { nix::ParserState * state }`, `%lex-param { void * scanner }`, `%lex-param { nix::ParserState * state }`.

#### Typed nonterminals
- `Expr *`: `start`, `expr`, `expr_function`, `expr_if`, `expr_op`, `expr_select`, `expr_simple`, `expr_app`, `expr_pipe_from`, `expr_pipe_into`, `path_start`.
- `std::pmr::vector<Expr *>`: `list`.
- `ExprAttrs *`: `binds`, `binds1`.
- `FormalsBuilder`: `formals`, `formal_set`.
- `Formal`: `formal`.
- `std::vector<AttrName>`: `attrpath`.
- `std::vector<std::pair<AttrName, PosIdx>>`: `attrs`.
- `std::vector<std::pair<PosIdx, Expr *>>`: `string_parts_interpolated`.
- `std::vector<std::pair<PosIdx, std::variant<Expr *, StringToken>>>`: `ind_string_parts`.
- `ToBeStringyExpr`: `string_parts`, `string_attr`.
- `StringToken`: `attr`.

#### Tokens
- Value-carrying `StringToken`: `ID` ("identifier"), `STR` ("string"), `IND_STR` ("indented string"), `PATH` ("path"), `HPATH` ("'~/…' path"), `SPATH` ("'<…>' path"), `PATH_END` ("end of path"), `URI` ("URI").
- Value-carrying `NixInt`: `INT_LIT` ("integer"). Value-carrying `NixFloat`: `FLOAT_LIT` ("floating-point literal").
- Keyword tokens: `IF`, `THEN`, `ELSE`, `ASSERT`, `WITH`, `LET`, `IN_KW`, `REC`, `INHERIT`.
- Operator tokens: `EQ`, `NEQ`, `LEQ`, `GEQ`, `UPDATE`, `CONCAT`, `AND`, `OR`, `IMPL`, `OR_KW`, `PIPE_FROM`, `PIPE_INTO`.
- Punctuation tokens: `DOLLAR_CURLY`, `IND_STRING_OPEN`, `IND_STRING_CLOSE`, `ELLIPSIS`, `REPL_BINDINGS`.

#### Precedence / associativity (low to high)
- `%right IMPL`
- `%left OR`
- `%left AND`
- `%nonassoc EQ NEQ`
- `%nonassoc '<' '>' LEQ GEQ`
- `%right UPDATE`
- `%left NOT`
- `%left '+' '-'`
- `%left '*' '/'`
- `%right CONCAT`
- `%nonassoc '?'`
- `%nonassoc NEGATE`

#### Grammar rules
- `start` — top-level; `expr` (assigns `state->result = $1`) or `REPL_BINDINGS binds1` (assigns `state->result = $2`); both also `(void) yynerrs_;`.
- `expr` — `expr_function`.
- `expr_function`:
  - `ID ':' expr_function` — bare lambda; `state->exprs.add<ExprLambda>(CUR_POS, ..., $3)`; `SET_DOC_POS(me, @1)`.
  - `formal_set ':' expr_function` — formal-set lambda; calls `state->validateFormals` then `add<ExprLambda>(state->positions, state->exprs.alloc, CUR_POS, $formal_set, $body)`.
  - `formal_set '@' ID ':' expr_function` — `formal_set @ name :` lambda.
  - `ID '@' formal_set ':' expr_function` — `name @ formal_set :` lambda.
  - `ASSERT expr ';' expr_function` — `state->exprs.add<ExprAssert>(CUR_POS, $2, $4)`.
  - `WITH expr ';' expr_function` — `state->exprs.add<ExprWith>(CUR_POS, $2, $4)`.
  - `LET binds IN_KW expr_function` — checks `!$2->dynamicAttrs->empty()` and throws `ParseError`; otherwise `state->exprs.add<ExprLet>($2, $4)`.
  - `expr_if`.
- `expr_if`:
  - `IF expr THEN expr ELSE expr` — `state->exprs.add<ExprIf>(CUR_POS, $2, $4, $6)`.
  - `expr_pipe_from`, `expr_pipe_into`, `expr_op`.
- `expr_pipe_from`:
  - `expr_op PIPE_FROM expr_pipe_from` and `expr_op PIPE_FROM expr_op` — `makeCall(state->exprs, state->at(@2), $1, $3)`.
- `expr_pipe_into`:
  - `expr_pipe_into PIPE_INTO expr_op` and `expr_op PIPE_INTO expr_op` — `makeCall(state->exprs, state->at(@2), $3, $1)`.
- `expr_op`:
  - `'!' expr_op %prec NOT` — `ExprOpNot($2)`.
  - `'-' expr_op %prec NEGATE` — desugared as `ExprCall(CUR_POS, ExprVar(state->s.sub), {ExprInt(0), $2})`.
  - `expr_op EQ expr_op`, `expr_op NEQ expr_op` — `ExprOpEq` / `ExprOpNEq`.
  - `expr_op '<' expr_op` — `ExprCall(@2, ExprVar(state->s.lessThan), {$1, $3})`.
  - `expr_op LEQ expr_op` — `ExprOpNot(ExprCall(@2, ExprVar(state->s.lessThan), {$3, $1}))`.
  - `expr_op '>' expr_op` — `ExprCall(@2, ExprVar(state->s.lessThan), {$3, $1})`.
  - `expr_op GEQ expr_op` — `ExprOpNot(ExprCall(@2, ExprVar(state->s.lessThan), {$1, $3}))`.
  - `expr_op AND expr_op`, `expr_op OR expr_op`, `expr_op IMPL expr_op`, `expr_op UPDATE expr_op` — `ExprOpAnd` / `ExprOpOr` / `ExprOpImpl` / `ExprOpUpdate` (pos-aware ctor).
  - `expr_op '?' attrpath` — `ExprOpHasAttr(state->exprs.alloc, $1, $3)`.
  - `expr_op '+' expr_op` — `ExprConcatStrings(alloc, @2, false, {{@1, $1}, {@3, $3}})`.
  - `expr_op '-' expr_op` — `ExprCall(@2, ExprVar(state->s.sub), {$1, $3})`.
  - `expr_op '*' expr_op` — `ExprCall(@2, ExprVar(state->s.mul), {$1, $3})`.
  - `expr_op '/' expr_op` — `ExprCall(@2, ExprVar(state->s.div), {$1, $3})`.
  - `expr_op CONCAT expr_op` — `ExprOpConcatLists(@2, $1, $3)`.
  - `expr_app`.
- `expr_app`:
  - `expr_app expr_select` — `makeCall(state->exprs, CUR_POS, $1, $2); $2->warnIfCursedOr(...)`.
  - `expr_select` — `$$ = $1; $$->resetCursedOr();`.
- `expr_select`:
  - `expr_simple '.' attrpath` — `ExprSelect(alloc, CUR_POS, $1, $3, nullptr)`.
  - `expr_simple '.' attrpath OR_KW expr_select` — `ExprSelect(alloc, CUR_POS, $1, $3, $5)`; calls `warnIfCursedOr` on `$5`.
  - `expr_simple OR_KW` — backwards-compat 'cursed or' fallback; `ExprCall(CUR_POS, $1, {ExprVar(CUR_POS, state->s.or_)}, state->positions.add(state->origin, @$.endOffset))`.
  - `expr_simple`.
- `expr_simple`:
  - `ID` — if literal text equals `"__curPos"`, builds `ExprPos(CUR_POS)`; otherwise `ExprVar(CUR_POS, state->symbols.create($1))`.
  - `INT_LIT` — `ExprInt($1)`.
  - `FLOAT_LIT` — `ExprFloat($1)`.
  - `'"' string_parts '"'` — `$2.toExpr(state->exprs)`.
  - `IND_STRING_OPEN ind_string_parts IND_STRING_CLOSE` — `state->stripIndentation(CUR_POS, $2)`.
  - `path_start PATH_END` — pure path literal.
  - `path_start string_parts_interpolated PATH_END` — inserts `{state->at(@1), $1}` at front and builds `ExprConcatStrings(alloc, CUR_POS, false, $2)`.
  - `SPATH` — desugars `<…>` into `ExprCall(findFile, [nixPath, ExprString(path)])` where `path` is the inner text.
  - `URI` — diagnoses `lintUrlLiterals` and emits `ExprString(alloc, $1)`.
  - `'(' expr ')'` — passthrough.
  - `LET '{' binds '}'` — `let { … }` desugar; sets `recursive = true` and rewrites to `(rec {...}).body` via `ExprSelect(alloc, noPos, $3, state->s.body)`.
  - `REC '{' binds '}'` — sets `recursive = true; pos = CUR_POS; $$ = $3`.
  - `'{' binds1 '}'` — sets `pos = CUR_POS; $$ = $2`.
  - `'{' '}'` — `ExprAttrs(CUR_POS)`.
  - `'[' list ']'` — `ExprList(alloc, $2)`.
- `string_parts`:
  - `STR` — `ToBeStringyExpr{$1}`.
  - `string_parts_interpolated` — wraps as `ExprConcatStrings(alloc, CUR_POS, true, $1)`.
  - empty — `ToBeStringyExpr{std::string_view()}`.
- `string_parts_interpolated`:
  - `string_parts_interpolated STR` — appends `(state->at(@2), ExprString(alloc, $2))`.
  - `string_parts_interpolated DOLLAR_CURLY expr '}'` — appends `(state->at(@2), $3)`.
  - `DOLLAR_CURLY expr '}'` — initial element only.
  - `STR DOLLAR_CURLY expr '}'` — emits `ExprString` and the interpolated `expr`.
- `path_start`:
  - `PATH` — handles absolute paths (against `rootFS`) and relative paths (against `state->basePath`); diagnoses `lintAbsolutePathLiterals` and `lintShortPathLiterals`; preserves a trailing `/` when present.
  - `HPATH` — throws under `pureEval`; diagnoses `lintAbsolutePathLiterals`; resolves against `getHome()` and uses `rootFS`.
- `ind_string_parts`:
  - `ind_string_parts IND_STR` — appends `(state->at(@2), $2)` (the variant's `StringToken` arm).
  - `ind_string_parts DOLLAR_CURLY expr '}'` — appends `(state->at(@2), $3)`.
  - empty.
- `binds`:
  - `binds1`.
  - empty — fresh `state->exprs.add<ExprAttrs>()`.
- `binds1`:
  - `binds1 attrpath '=' expr ';'` — `state->addAttr(...)`.
  - `binds INHERIT attrs ';'` — fills `attrs->attrs` with `ExprAttrs::AttrDef::Kind::Inherited` entries; calls `state->dupAttr` on conflict.
  - `binds INHERIT '(' expr ')' attrs ';'` — populates `inheritFromExprs` and emits `ExprSelect(alloc, iPos, ExprInheritFrom(...), i.symbol)` `Kind::InheritedFrom` entries; calls `state->dupAttr` on conflict.
  - `attrpath '=' expr ';'` — first binding, allocates a fresh `ExprAttrs` then `state->addAttr(...)`.
- `attrs`:
  - `attrs attr` — appends `(symbols.create($2), state->at(@2))`.
  - `attrs string_attr` — visits the variant; appends as a regular symbol or throws `ParseError("dynamic attributes not allowed in inherit")`.
  - empty.
- `attrpath`:
  - `attrpath '.' attr` — appends `AttrName(symbols.create($3))`.
  - `attrpath '.' string_attr` — visits the variant; appends as `AttrName(symbol)` or `AttrName(expr)`.
  - `attr` — first element via `AttrName(symbols.create($1))`.
  - `string_attr` — first element via `AttrName(symbol)` or `AttrName(expr)` (visit branches).
- `attr`:
  - `ID`.
  - `OR_KW` — yields `StringToken{"or", 2}`.
- `string_attr`:
  - `'"' string_parts '"'` — `$$ = std::move($2)`.
  - `DOLLAR_CURLY expr '}'` — `$$ = ToBeStringyExpr{$2}`.
- `list`:
  - `list expr_select` — appends element and calls `$2->warnIfCursedOr(...)`.
  - empty.
- `formal_set`:
  - `'{' formals ',' ELLIPSIS '}'` — `$$ = std::move($formals); $$.ellipsis = true;`.
  - `'{' ELLIPSIS '}'` — fresh builder with `ellipsis = true`.
  - `'{' formals ',' '}'` and `'{' formals '}'` — `$$ = std::move($formals); $$.ellipsis = false;`.
  - `'{' '}'` — fresh builder with `ellipsis = false`.
- `formals`:
  - `formals ',' formal` — appends.
  - `formal` — first element.
- `formal`:
  - `ID` — `Formal{CUR_POS, symbols.create($1), nullptr}`.
  - `ID '?' expr` — `Formal{CUR_POS, symbols.create($1), $3}`.

---

## File: src/libexpr/parser-scanner-decls.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `class Parser : public parser::BisonParser` — kind: class. Path: src/libexpr/parser-scanner-decls.hh. Purpose: Thin alias inheriting Bison-generated parser ctors via `using BisonParser::BisonParser`.

### Type aliases (top-level, only when `BISON_HEADER` is not defined)
- `using YYSTYPE = nix::parser::BisonParser::value_type` — Bison value alias for non-Bison TUs.
- `using YYLTYPE = nix::parser::BisonParser::location_type` — Bison location alias for non-Bison TUs.

### Other
- Includes `lexer-tab.hh` (with `// IWYU pragma: export`) when not building inside the Bison-generated header.

---

## File: src/libexpr/include/nix/expr/parser-state.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct StringToken` — kind: struct. Path: src/libexpr/include/nix/expr/parser-state.hh. Purpose: Lightweight non-owning view over a chunk of the parser input buffer; deliberately uses raw `char *` + `size_t` rather than `std::string_view` to avoid implicit deletion of the special members.
  - members: `const char * p`, `size_t l`, `bool hasIndentation`.
  - method: `operator std::string_view() const` — returns `{p, l}`.
- `struct ParserLocation` — kind: struct. Path: src/libexpr/include/nix/expr/parser-state.hh. Purpose: Bison location storing only `beginOffset`/`endOffset`, with stash fields for `yyless(0)`-style rewinding.
  - members: `int beginOffset`, `int endOffset`, `int stashedBeginOffset`, `int stashedEndOffset`.
  - methods: `void stash()`, `void unstash()`.
- `class ToBeStringyExpr` — kind: class. Path: src/libexpr/include/nix/expr/parser-state.hh. Purpose: Variant-backed deferred string parse (`monostate`, `string_view`, or `Expr *`) so the parser can avoid allocating an `ExprString` until/unless one is needed.
  - private alias: `using Raw = std::variant<std::monostate, std::string_view, Expr *>`.
  - private member: `Raw raw`.
  - ctors: default; from `std::string_view`; from `Expr *` (asserts non-null).
  - methods: `template<class F> void visit(const F & f)` — dispatches `string_view`/`Expr *`/`ExprString -> string_view`; `Expr * toExpr(Exprs & exprs)` — materialises an `ExprString` or returns the existing `Expr *`.
- `struct LexerState` — kind: struct. Path: src/libexpr/include/nix/expr/parser-state.hh. Purpose: Bookkeeping shared between lexer rules (doc-comment buffering, position table reference, current origin).
  - members: `int docCommentDistance = std::numeric_limits<int>::max()`, `ParserLocation lastDocCommentLoc`, `DocCommentMap & positionToDocComment`, `PosTable & positions`, `PosTable::Origin origin`.
  - method: `PosIdx at(const ParserLocation & loc)` (defined inline at end of file as `positions.add(origin, loc.beginOffset)`).
- `struct ParserState` — kind: struct. Path: src/libexpr/include/nix/expr/parser-state.hh. Purpose: Per-parse working state; ties Bison actions to the lexer state, expression arena, symbol/position tables, and parsing settings; holds the produced `result`.
  - members: `const LexerState & lexerState`, `Exprs & exprs`, `SymbolTable & symbols`, `PosTable & positions`, `Expr * result`, `SourcePath basePath`, `PosTable::Origin origin`, `const ref<SourceAccessor> rootFS`, `static constexpr Expr::AstSymbols s = StaticEvalSymbols::create().exprSymbols`, `const EvalSettings & settings`.
  - methods (all defined inline in this header): `dupAttr(const AttrSelectionPath & attrPath, const PosIdx pos, const PosIdx prevPos)`; `dupAttr(Symbol attr, const PosIdx pos, const PosIdx prevPos)`; `addAttr(ExprAttrs * attrs, AttrSelectionPath && attrPath, const ParserLocation & loc, Expr * e, const ParserLocation & exprLoc)`; `addAttr(ExprAttrs * attrs, AttrSelectionPath & attrPath, const Symbol & symbol, ExprAttrs::AttrDef && def)`; `validateFormals(FormalsBuilder & formals, PosIdx pos = noPos, Symbol arg = {})`; `Expr * stripIndentation(const PosIdx pos, std::span<std::pair<PosIdx, std::variant<Expr *, StringToken>>> es)`; `PosIdx at(const ParserLocation & loc)`.

### Functions
- All non-trivial methods of `ParserState` are defined inline here (`dupAttr`, both `addAttr` overloads, `validateFormals`, `stripIndentation`, `at`).
- `LexerState::at` — inline definition (`positions.add(origin, loc.beginOffset)`).

---

## File: src/libexpr/nixexpr.cc

### Namespaces
- `nix`.

### Globals
- `Counter Expr::nrExprs` — definition for the static counter declared in `nixexpr.hh`.
- `ExprBlackHole eBlackHole` — sentinel value used to mark thunks under evaluation.

### Functions (free)
- `operator<<(std::ostream & str, const SymbolStr & symbol) -> std::ostream &` — kind: stream insertion operator. Path: src/libexpr/nixexpr.cc. Purpose: FIXME-flagged convenience that delegates to `printIdentifier`.
- `showAttrSelectionPath(const SymbolTable & symbols, std::span<const AttrName> attrPath) -> std::string` — kind: free function. Path: src/libexpr/nixexpr.cc. Purpose: Render an attribute selection path using `${…}` interpolation for dynamic components and a `.` separator.

### Member function definitions
For each AST node `Expr*` subclass, this file defines `show` and `bindVars` overrides (and a few extras):
- `Expr::show` — base implementation calls `unreachable()`.
- `Expr::bindVars` — base implementation calls `unreachable()`.
- `Expr::setName` — empty default (`void Expr::setName(Symbol)`).
- `ExprInt::show` (writes `v.integer()`); `ExprInt::bindVars` (only registers debug repl env).
- `ExprFloat::show` (writes `v.fpoint()`); `ExprFloat::bindVars` (debug repl only).
- `ExprString::show` (calls `printLiteralString(str, v.string_view())`); `ExprString::bindVars` (debug repl only).
- `ExprPath::show` (writes `v.pathStrView()`); `ExprPath::bindVars` (debug repl only).
- `ExprVar::show` (writes `symbols[name]`); `ExprVar::bindVars` resolves `level`/`displ` against the static env, falling back to the nearest enclosing `with` and throwing `UndefinedVarError` if none.
- `ExprInheritFrom::bindVars` — minimal: only registers debug repl env mapping (the level/displ are pre-baked).
- `ExprSelect::show`, `ExprSelect::bindVars`.
- `ExprOpHasAttr::show`, `ExprOpHasAttr::bindVars`.
- `ExprAttrs::showBindings` — emits `inherit`, `inherit (expr)` (keyed by source `Displacement`), plain attrs, and `"${…}" =` dynamic attrs in deterministic order.
- `ExprAttrs::show` (prefixes `rec ` when `recursive`), `ExprAttrs::bindVars` (calls `moveDataToAllocator`, then walks attrs and dynamic attrs with the appropriate env via `chooseByKind`).
- `ExprAttrs::bindInheritSources` — builds an `inner` `StaticEnv` of size 0 (intentionally) and calls `bindVars` on each `inheritFromExprs` entry against the outer env.
- `ExprAttrs::moveDataToAllocator` — copies `attrs`, `dynamicAttrs`, and `inheritFromExprs` into the bump allocator.
- `ExprList::show`, `ExprList::bindVars`.
- `ExprLambda::show`, `ExprLambda::bindVars` (builds new env from formals/arg, sorts, then binds defaults and body).
- `ExprLambda::setName` (sets `name`, propagates to `body`).
- `ExprLambda::showNamePos(const EvalState &) const` — returns `"<id> at <pos>"`.
- `ExprLambda::setDocComment` — RFC-145 innermost-wins propagation; only assigns when `!this->docComment`, then propagates to `body`.
- `ExprCall::show`, `ExprCall::bindVars` (calls `moveDataToAllocator` first), `ExprCall::moveDataToAllocator`, `ExprCall::resetCursedOr`, `ExprCall::warnIfCursedOr`.
- `ExprLet::show`, `ExprLet::bindVars` (calls `attrs->moveDataToAllocator`, builds new env, calls `bindInheritSources`, walks attrs, then binds body).
- `ExprWith::show`, `ExprWith::bindVars` — also computes `parentWith` and `prevWith`.
- `ExprIf::show`, `ExprIf::bindVars`.
- `ExprAssert::show`, `ExprAssert::bindVars`.
- `ExprOpNot::show`, `ExprOpNot::bindVars`.
- `ExprConcatStrings::show`, `ExprConcatStrings::bindVars`.
- `ExprPos::show` (writes `__curPos`), `ExprPos::bindVars` (debug repl only).
- `SymbolTable::totalSize` — sum of all symbol-string lengths (defined in this TU).
- `DocComment::getInnerText(const PosTable &)` — extracts the inner body text from the source range, stripping `/**`/`*/` and dedenting via `stripIndentation`.

---

## File: src/libexpr/include/nix/expr/nixexpr.hh

### Namespaces
- `nix`.

### Forward declarations
- `class EvalState`, `class PosTable`, `struct Env`, `struct ExprWith`, `struct StaticEnv`, `struct Value`.

### Classes / structs / enums

#### Structural / utility types
- `struct DocComment` — kind: struct. Path: src/libexpr/include/nix/expr/nixexpr.hh. Purpose: RFC-145 doc-comment span; convertible to `bool` (true iff `begin` is set), exposes `getInnerText`.
  - members: `PosIdx begin`, `PosIdx end`.
  - methods: `operator bool() const`, `std::string getInnerText(const PosTable &) const`.
- `struct AttrName` — kind: struct. Path: src/libexpr/include/nix/expr/nixexpr.hh. Purpose: One element of an attribute path; either a static `Symbol` or a dynamic `Expr *`.
  - members: `Symbol symbol`, `Expr * expr = nullptr`.
  - ctors: `AttrName(Symbol s)`, `AttrName(Expr * e)`.
- `static_assert(std::is_trivially_copy_constructible_v<AttrName>)` — invariant assertion.

#### Type aliases / typedefs
- `typedef std::vector<AttrName> AttrSelectionPath`.
- `using UpdateQueue = SmallTemporaryValueVector<conservativeStackReservation>` — accumulator buffer for `ExprOpUpdate::evalForUpdate`.
- `typedef uint32_t Level`.
- `typedef uint32_t Displacement`.

#### Macros
- `COMMON_METHODS` — emits `show`, `eval`, `bindVars` overrides for every `Expr*` subclass.
- `MakeBinOpMembers(name, s)` — emits `pos`, `e1`, `e2`, two ctors, `show`, `bindVars`, `eval` decl, and `getPos` for binary operator AST nodes.
- `MakeBinOp(name, s)` — declares `struct name : Expr { MakeBinOpMembers(name, s) }`.

#### AST root
- `struct Expr` — kind: struct (abstract by convention). Path: src/libexpr/include/nix/expr/nixexpr.hh. Purpose: Base class of every AST node; default implementations for `show`, `bindVars` (both `unreachable()`), `setName` (empty), and inline empty stubs for `setDocComment`, `getPos` (returns `noPos`), `resetCursedOr`, `warnIfCursedOr`. Other virtual methods (`eval`, `maybeThunk`, `evalForUpdate`) are declared here and defined out of line; subclasses are expected to override `eval`. Embedded helpers: `AstSymbols`, `nrExprs` counter.
  - nested `struct AstSymbols { Symbol sub, lessThan, mul, div, or_, findFile, nixPath, body; }` — pre-resolved symbol handles for desugaring.
  - static: `Counter nrExprs`.
  - ctor: `Expr()` increments `nrExprs`.
  - virtual dtor: `virtual ~Expr() {};`.
  - virtual methods: `virtual void show(const SymbolTable &, std::ostream &) const`; `virtual void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &)`; `virtual void eval(EvalState &, Env &, Value &)` (declared, defined elsewhere); `virtual Value * maybeThunk(EvalState &, Env &)`; `virtual void evalForUpdate(EvalState &, Env &, UpdateQueue &, std::string_view errorCtx)`; `virtual void setName(Symbol)`; `virtual void setDocComment(DocComment) {}`; `virtual PosIdx getPos() const { return noPos; }`; `virtual void resetCursedOr() {}`; `virtual void warnIfCursedOr(const SymbolTable &, const PosTable &) {}`.

#### AST leaf / primitive nodes
- `struct ExprInt : Expr` — integer literal. Member: `Value v`. Ctors: `ExprInt(NixInt n)`, `ExprInt(NixInt::Inner n)`. Overrides: `Value * maybeThunk(...)`, plus `COMMON_METHODS` (`show`, `eval`, `bindVars`).
- `struct ExprFloat : Expr` — float literal. Member: `Value v`. Ctor: `ExprFloat(NixFloat nf)`. Overrides: `maybeThunk`, `COMMON_METHODS`.
- `struct ExprString : Expr` — string literal. Member: `Value v`. Ctors: `ExprString(const StringData & s)`, `ExprString(std::pmr::polymorphic_allocator<char> & alloc, std::string_view sv)`. Overrides: `maybeThunk`, `COMMON_METHODS`.
- `struct ExprPath : Expr` — path literal. Members: `ref<SourceAccessor> accessor`, `Value v`. Ctor: `ExprPath(std::pmr::polymorphic_allocator<char> & alloc, ref<SourceAccessor> accessor, std::string_view sv)`. Overrides: `maybeThunk`, `COMMON_METHODS`.
- `struct ExprVar : Expr` — variable reference. Members: `PosIdx pos`, `Symbol name`, `ExprWith * fromWith = nullptr`, `Level level = 0`, `Displacement displ = 0`. Ctors: `ExprVar(Symbol name)`, `ExprVar(const PosIdx & pos, Symbol name)`. Overrides: `maybeThunk`, `getPos`, `COMMON_METHODS`.
- `struct ExprInheritFrom : ExprVar` — inherit-from anchor. Ctor: `ExprInheritFrom(PosIdx pos, Displacement displ)` initialises the underlying `ExprVar` with `name = {}`, sets `level = 0`, `displ = displ`, `fromWith = nullptr`. Override: `bindVars` (only registers debug repl env).
- `struct ExprSelect : Expr` — `e.attrPath [or def]`. Members: `PosIdx pos`, `uint32_t nAttrPath`, `Expr * e`, `Expr * def`, `AttrName * attrPathStart`. Ctors: `(allocator, pos, e, std::span<const AttrName> attrPath, Expr * def)` (allocates and copies the path) and `(allocator, pos, e, Symbol name)` (single-element path). Methods: `PosIdx getPos() const override`; `std::span<const AttrName> getAttrPath() const`; `Symbol evalExceptFinalSelect(EvalState &, Env &, Value & attrs)`. Plus `COMMON_METHODS`.
- `struct ExprOpHasAttr : Expr` — `e ? attrPath`. Members: `Expr * e`, `std::span<AttrName> attrPath`. Ctor: `ExprOpHasAttr(std::pmr::polymorphic_allocator<char> & alloc, Expr * e, std::span<AttrName> attrPath)` (allocates and copies). Override: `getPos` (returns `e->getPos()`), `COMMON_METHODS`.

#### AST aggregate nodes
- `struct ExprAttrs : Expr` — `{ ... }` / `rec { ... }` attrset.
  - members: `bool recursive`, `PosIdx pos`, `std::optional<AttrDefs> attrs`, `std::unique_ptr<std::pmr::vector<Expr *>> inheritFromExprs`, `std::optional<DynamicAttrDefs> dynamicAttrs`.
  - nested `struct AttrDef`:
    - nested `enum class Kind { Plain, Inherited, InheritedFrom }`.
    - members: `Kind kind`, `Expr * e`, `PosIdx pos`, `Displacement displ = 0`.
    - ctors: `AttrDef(Expr * e, const PosIdx & pos, Kind kind = Kind::Plain)`, default `AttrDef()`.
    - method: `template<typename T> const T & chooseByKind(const T & plain, const T & inherited, const T & inheritedFrom) const`.
  - typedef: `typedef std::pmr::map<Symbol, AttrDef> AttrDefs`.
  - nested `struct DynamicAttrDef`:
    - members: `Expr * nameExpr`, `Expr * valueExpr`, `PosIdx pos`.
    - ctor: `DynamicAttrDef(Expr * nameExpr, Expr * valueExpr, const PosIdx & pos)`.
  - typedef: `typedef std::pmr::vector<DynamicAttrDef> DynamicAttrDefs`.
  - ctors: `ExprAttrs(const PosIdx & pos)` (sets `recursive = false`, initialises `attrs` and `dynamicAttrs` empty), default `ExprAttrs()` (same but no pos).
  - methods/overrides: `PosIdx getPos() const override`; `COMMON_METHODS`; `std::shared_ptr<const StaticEnv> bindInheritSources(EvalState &, const std::shared_ptr<const StaticEnv> &)`; `Env * buildInheritFromEnv(EvalState &, Env & up)`; `void showBindings(const SymbolTable &, std::ostream &) const`; `void moveDataToAllocator(std::pmr::polymorphic_allocator<char> &)`.
- `struct ExprList : Expr` — `[ ... ]`. Member: `std::span<Expr *> elems`. Ctor: `ExprList(std::pmr::polymorphic_allocator<char> & alloc, std::span<Expr *> exprs)`. Overrides: `COMMON_METHODS`, `Value * maybeThunk(...) override`, `PosIdx getPos() const override` (returns `elems.front()->getPos()` or `noPos`).

#### Lambdas / formals
- `struct Formal` — function-formal entry. Members: `PosIdx pos`, `Symbol name`, `Expr * def`.
- `struct FormalsBuilder` — pre-finalisation builder. Typedef: `typedef std::vector<Formal> Formals_`. Members: `Formals_ formals` (sorted-by-`(name, pos)` invariant); `bool ellipsis`. Method: `bool has(Symbol arg) const`.
- `struct Formals` — finalised formals view. Members: `std::span<Formal> formals`, `bool ellipsis`. Ctor: `Formals(std::span<Formal>, bool ellipsis)`. Methods: `bool has(Symbol arg) const`; `std::vector<Formal> lexicographicOrder(const SymbolTable &) const`.
- `struct ExprLambda : Expr` — lambda node.
  - public members: `PosIdx pos`, `Symbol name`, `Symbol arg`.
  - private members: `bool hasFormals`, `bool ellipsis`, `uint16_t nFormals`, `Formal * formalsStart`.
  - public members (later): `Expr * body`, `DocComment docComment`.
  - method: `std::optional<Formals> getFormals() const`.
  - ctors: `(const PosTable &, std::pmr::polymorphic_allocator<char> &, PosIdx pos, Symbol arg, const FormalsBuilder & formals, Expr * body)` (asserts `formals.formals.size() <= nFormals_max`), `(PosIdx pos, Symbol arg, Expr * body)` (no formals), `(const PosTable &, std::pmr::polymorphic_allocator<char> &, PosIdx pos, const FormalsBuilder &, Expr * body)` (delegating with anonymous arg).
  - overrides: `void setName(Symbol) override`; `PosIdx getPos() const override`; `virtual void setDocComment(DocComment) override`; `COMMON_METHODS`.
  - method: `std::string showNamePos(const EvalState &) const`.

#### Calls / control flow
- `struct ExprCall : Expr` — `fn arg ...`. Members: `Expr * fun`, `std::optional<std::pmr::vector<Expr *>> args`, `PosIdx pos`, `std::optional<PosIdx> cursedOrEndPos`. Ctors: `(const PosIdx & pos, Expr * fun, std::pmr::vector<Expr *> && args)` (sets `cursedOrEndPos = {}`), `(const PosIdx & pos, Expr * fun, std::pmr::vector<Expr *> && args, PosIdx && cursedOrEndPos)`. Overrides: `getPos`, `virtual void resetCursedOr() override`, `virtual void warnIfCursedOr(const SymbolTable &, const PosTable &) override`, `COMMON_METHODS`. Method: `void moveDataToAllocator(std::pmr::polymorphic_allocator<char> &)`.
- `struct ExprLet : Expr` — `let … in body`. Members: `ExprAttrs * attrs`, `Expr * body`. Ctor: `ExprLet(ExprAttrs * attrs, Expr * body)`. Overrides: `COMMON_METHODS`.
- `struct ExprWith : Expr` — `with attrs; body`. Members: `PosIdx pos`, `uint32_t prevWith`, `Expr * attrs`, `Expr * body`, `ExprWith * parentWith`. Ctor: `(const PosIdx & pos, Expr * attrs, Expr * body)`. Overrides: `getPos`, `COMMON_METHODS`.
- `struct ExprIf : Expr` — conditional. Members: `PosIdx pos`, `Expr * cond`, `Expr * then`, `Expr * else_`. Ctor: `(const PosIdx & pos, Expr * cond, Expr * then, Expr * else_)`. Overrides: `getPos`, `COMMON_METHODS`.
- `struct ExprAssert : Expr` — `assert cond; body`. Members: `PosIdx pos`, `Expr * cond`, `Expr * body`. Ctor: `(const PosIdx & pos, Expr * cond, Expr * body)`. Overrides: `getPos`, `COMMON_METHODS`.

#### Operator nodes
- `struct ExprOpNot : Expr` — `!e`. Member: `Expr * e`. Ctor: `ExprOpNot(Expr * e)`. Override: `getPos` (returns `e->getPos()`), `COMMON_METHODS`.
- `struct ExprOpEq : Expr` — `==`, declared via `MakeBinOp(ExprOpEq, "==")`.
- `struct ExprOpNEq : Expr` — `!=`, via `MakeBinOp`.
- `struct ExprOpAnd : Expr` — `&&`, via `MakeBinOp`.
- `struct ExprOpOr : Expr` — `||`, via `MakeBinOp`.
- `struct ExprOpImpl : Expr` — `->`, via `MakeBinOp`.
- `struct ExprOpConcatLists : Expr` — `++`, via `MakeBinOp`.
- `struct ExprOpUpdate : Expr` — `//`. Uses `MakeBinOpMembers`. Adds private overload `void eval(EvalState & state, Value & v, Value & v1, Value & v2)` for the merge fast path, private `void evalForUpdate(EvalState &, Env &, UpdateQueue & q)` (no `errorCtx`), and public `virtual void evalForUpdate(EvalState &, Env &, UpdateQueue &, std::string_view errorCtx) override` to participate in batched updates.

#### Strings / position
- `struct ExprConcatStrings : Expr` — concatenation of pieces. Members: `PosIdx pos`, `bool forceString`, `std::span<std::pair<PosIdx, Expr *>> es`. Ctors: `(allocator, pos, forceString, std::span<std::pair<PosIdx, Expr *>> es)` and `(allocator, pos, forceString, std::initializer_list<std::pair<PosIdx, Expr *>> es)`. Overrides: `getPos`, `COMMON_METHODS`.
- `struct ExprPos : Expr` — `__curPos`. Member: `PosIdx pos`. Ctor: `ExprPos(const PosIdx & pos)`. Overrides: `getPos`, `COMMON_METHODS`.

#### Sentinel / arena / static env
- `struct ExprBlackHole : Expr` — black-hole sentinel. Methods (inline): `void show(const SymbolTable &, std::ostream &) const override {}`, `void eval(EvalState &, Env &, Value &) override` (declared, defined in eval.cc), `void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}`. Static: `[[noreturn]] static void throwInfiniteRecursionError(EvalState &, Value &)`.
- `extern ExprBlackHole eBlackHole` — global declaration (definition in nixexpr.cc).
- `class Exprs` — arena owning AST allocations.
  - private members: `std::pmr::synchronized_pool_resource fallbackResource`, `BumpMemoryResource buffer{BumpMemoryResource::defaultReserveSize, &fallbackResource}`.
  - public member: `std::pmr::polymorphic_allocator<char> alloc{&buffer}`.
  - templates: generic `template<class C> [[gnu::always_inline]] C * add(auto &&... args)` (calls `alloc.new_object<C>`); explicit overloads constrained by `requires(std::same_as<C, ExprCall>)` for the two `ExprCall` ctors; explicit overloads constrained by `requires(std::same_as<C, ExprConcatStrings>)` for the two `ExprConcatStrings` ctors (span and initializer_list).
- `struct StaticEnv` — compile-time scope frame. Members: `ExprWith * isWith`, `std::shared_ptr<const StaticEnv> up`, `Vars vars` (typedef `std::vector<std::pair<Symbol, Displacement>>`). Ctor: `StaticEnv(ExprWith * isWith, std::shared_ptr<const StaticEnv> up, size_t expectedSize = 0)` (reserves `vars`). Methods: `void sort()` (stable sort by symbol), `void deduplicate()` (collapses consecutive duplicates), `Vars::const_iterator find(Symbol name) const`.

### Functions (free)
- `std::string showAttrSelectionPath(const SymbolTable &, std::span<const AttrName>)` — declared here, defined in nixexpr.cc.

---

## File: src/libexpr/print.cc

### Namespaces
- `nix`.

### Type aliases (file-local)
- `typedef std::pair<std::string, Value *> AttrPair`.
- `typedef std::set<const void *> ValuesSeen` — duplicate-value tracker.
- `typedef std::vector<std::pair<std::string, Value *>> AttrVec`.

### Classes / structs / enums
- `struct ImportantFirstAttrNameCmp` — kind: struct (functor). Path: src/libexpr/print.cc. Purpose: Comparator that orders an `AttrPair` so that "important" attr names (e.g. `type`, `_type`) sort first when truncating output.
  - method: `bool operator()(const AttrPair & lhs, const AttrPair & rhs) const`.
- `class Printer` — kind: class (file-local). Path: src/libexpr/print.cc. Purpose: Stateful Nix-value pretty-printer used by `printValue` / `ValuePrinter`; handles depth limits, ANSI colours, repeat detection, indentation, derivation summarisation, and per-value-type rendering helpers.
  - private members: `std::ostream & output`, `EvalState & state`, `PrintOptions options`, `NixStringContext * context`, `std::optional<ValuesSeen> seen`, `size_t totalAttrsPrinted = 0`, `size_t totalListItemsPrinted = 0`, `std::string indent`.
  - private methods: `void increaseIndent()`, `void decreaseIndent()`, `void printSpace(bool prettyPrint)`, `void printRepeated()`, `void printNullptr()`, `void printElided(unsigned int, std::string_view single, std::string_view plural)` (member overload delegating to `::nix::printElided`), `void printInt(Value &)`, `void printFloat(Value &)`, `void printBool(Value &)`, `void printString(Value &)`, `void printPath(Value &)`, `void printNull()`, `void printDerivation(Value &)`, `bool shouldPrettyPrintAttrs(AttrVec &)` (forces single-item items), `void printAttrs(Value &, size_t depth)`, `bool shouldPrettyPrintList(std::span<Value * const>)` (forces single-item items), `void printList(Value &, size_t depth)`, `void printFunction(Value &)` (lambda/primop/primopapp), `void printThunk(Value &)` (incl. blackhole and app cases), `void printFailed()`, `void printExternal(Value &)`, `void printUnknown()`, `void printError_(Error &)`, `void print(Value & v, size_t depth)` (depth check, force, dispatch by `v.type()`).
  - public ctor: `Printer(std::ostream &, EvalState &, PrintOptions, NixStringContext *)`.
  - public method: `void print(Value & v)` — resets counters/indent, optionally enables `seen`, declares a local `ValuesSeen seen;` (note: the locally declared `seen` shadows nothing observable since the member `seen` is what the inner methods use), and invokes `print(v, 0)`.

### Functions
- `printElided(std::ostream & output, unsigned int value, const std::string_view single, const std::string_view plural, bool ansiColors)` — kind: free function. Path: src/libexpr/print.cc. Purpose: Render the `«N elided»` placeholder for truncated attrs/list items/strings, with optional ANSI faint formatting.
- `printLiteralString(std::ostream &, const std::string_view, size_t maxLength, bool ansiColors) -> std::ostream &` — kind: free function (overload). Path: src/libexpr/print.cc. Purpose: Render a string as a Nix double-quoted literal, escaping `\n`/`\r`/`\t`/`\"`/`\\` and `${`, applying optional ANSI magenta, truncating with `printElided` when over `maxLength`.
- `printLiteralString(std::ostream &, const std::string_view) -> std::ostream &` — kind: free function (overload). Path: src/libexpr/print.cc. Purpose: Convenience for the unrestricted, non-ANSI case (delegates with `numeric_limits<size_t>::max()` and `false`).
- `printLiteralBool(std::ostream &, bool) -> std::ostream &` — kind: free function. Path: src/libexpr/print.cc. Purpose: Print `true` or `false`.
- `isReservedKeyword(const std::string_view) -> bool` — kind: free function. Path: src/libexpr/print.cc. Purpose: Returns true for `if|then|else|assert|with|let|in|rec|inherit`. Implemented via a static `boost::unordered_flat_set<std::string_view>`.
- `printIdentifier(std::ostream &, std::string_view) -> std::ostream &` — kind: free function. Path: src/libexpr/print.cc. Purpose: Print as identifier, falling through to `printLiteralString` when empty/reserved/non-conformant; checks first char is `[a-zA-Z_]`, rest is `[a-zA-Z0-9_'-]`.
- `isVarName(std::string_view) -> bool` — kind: file-local helper. Path: src/libexpr/print.cc. Purpose: Predicate equivalent to `printIdentifier`'s acceptance test (no leading digit/dash/quote, no reserved keyword, body is `[a-zA-Z0-9_'-]`).
- `printAttributeName(std::ostream &, std::string_view) -> std::ostream &` — kind: free function. Path: src/libexpr/print.cc. Purpose: Print as identifier when `isVarName`, otherwise fall through to `printLiteralString`.
- `isImportantAttrName(const std::string &) -> bool` — kind: file-local helper. Path: src/libexpr/print.cc. Purpose: True for `"type"` or `"_type"` (used by `ImportantFirstAttrNameCmp`).
- `printValue(EvalState &, std::ostream &, Value &, PrintOptions, NixStringContext *)` — kind: free function. Path: src/libexpr/print.cc. Purpose: Public entry point; instantiates a `Printer` and calls `print(v)`.
- `operator<<(std::ostream &, const ValuePrinter &) -> std::ostream &` — kind: free function. Path: src/libexpr/print.cc. Purpose: Allow streaming a `ValuePrinter` directly; calls `printValue(...)` with the captured fields.
- `template<> HintFmt & HintFmt::operator%(const ValuePrinter & value)` — kind: explicit specialisation. Path: src/libexpr/print.cc. Purpose: Inserts a `ValuePrinter` argument without applying magenta colouring (since `ValuePrinter` does its own ANSI handling).

---

## File: src/libexpr/include/nix/expr/print.hh

### Namespaces
- `nix`.

### Forward declarations
- `class EvalState`, `struct Value`.

### Classes / structs / enums
- `class ValuePrinter` — kind: class. Path: src/libexpr/include/nix/expr/print.hh. Purpose: Partially-applied form of `printValue` so callers can write `out << ValuePrinter{...}` without allocating an intermediate string.
  - private members: `EvalState & state`, `Value & value`, `PrintOptions options`, `NixStringContext * context`.
  - friend: `std::ostream & operator<<(std::ostream &, const ValuePrinter &)`.
  - ctor: `ValuePrinter(EvalState & state, Value & value, PrintOptions options = PrintOptions{}, NixStringContext * context = nullptr)`.

### Functions (declarations)
- `std::ostream & printLiteralString(std::ostream &, std::string_view)` — declared here.
- `inline std::ostream & printLiteralString(std::ostream &, const char * s)` — inline overload converting to `string_view`.
- `inline std::ostream & printLiteralString(std::ostream &, const std::string & s)` — inline overload.
- `std::ostream & printLiteralBool(std::ostream &, bool)` — declared.
- `std::ostream & printAttributeName(std::ostream &, std::string_view)` — declared.
- `bool isReservedKeyword(const std::string_view)` — declared.
- `std::ostream & printIdentifier(std::ostream &, std::string_view)` — declared (FIXME-flagged as ambiguous).
- `void printValue(EvalState &, std::ostream &, Value &, PrintOptions options = PrintOptions{}, NixStringContext * context = nullptr)` — declared.
- `std::ostream & operator<<(std::ostream &, const ValuePrinter &)` — declared.
- `template<> HintFmt & HintFmt::operator%(const ValuePrinter & value)` — explicit specialisation declaration.

---

## File: src/libexpr/include/nix/expr/print-options.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum class ErrorPrintBehavior { Print, Throw, ThrowTopLevel }` — kind: scoped enum. Path: src/libexpr/include/nix/expr/print-options.hh. Purpose: How a printer should treat errors raised by individual values: render them inline (`Print`), propagate (`Throw`), or only propagate at the top level (`ThrowTopLevel`).
- `struct PrintOptions` — kind: struct. Path: src/libexpr/include/nix/expr/print-options.hh. Purpose: Tunable knobs for `printValue`/`Printer`.
  - members: `bool ansiColors = false`, `bool force = false`, `bool derivationPaths = false`, `bool trackRepeated = true`, `size_t maxDepth = std::numeric_limits<size_t>::max()`, `size_t maxAttrs = std::numeric_limits<size_t>::max()`, `size_t maxListItems = std::numeric_limits<size_t>::max()`, `size_t maxStringLength = std::numeric_limits<size_t>::max()`, `size_t prettyIndent = 0`, `ErrorPrintBehavior errors = ErrorPrintBehavior::Print`.
  - method: `inline bool shouldPrettyPrint()` — true iff `prettyIndent > 0`.

### Macros / globals
- `static constexpr PrintOptions errorPrintOptions` — preset for printing potentially-large values inside error messages: `ansiColors = true`, `maxDepth = 10`, `maxAttrs = 10`, `maxListItems = 10`, `maxStringLength = 1024`.

---

## File: src/libexpr/print-ambiguous.cc

### Namespaces
- `nix`.

### Functions
- `printAmbiguous(EvalState & state, Value & v, std::ostream & str, std::set<const void *> * seen, NixStringContext * context, size_t depth)` — kind: free function. Path: src/libexpr/print-ambiguous.cc. Purpose: Recursive renderer matching the historical `nix-instantiate --eval`/`nix-env`-manifest format. Calls `checkInterrupt`, throws `StackOverflowError` past `state.settings.maxCallDepth`, dispatches on `v.type()`: emits literals via `printLiteralBool`/`printLiteralString` etc., renders attrs in `lexicographicOrder` separated by `; `, lists separated by spaces, replaces non-blackhole `nThunk` with `<CODE>`, blackhole with `«potential infinite recursion»`, `nFailed` with `<CODE>`, and lambdas/primops/primopapps with `<LAMBDA>`/`<PRIMOP>`/`<PRIMOP-APP>`. Cycles in attrs/lists print `«repeated»`. The `default` arm calls `printError(...)` then `unreachable()`.

---

## File: src/libexpr/include/nix/expr/print-ambiguous.hh

### Namespaces
- `nix`.

### Forward declarations
- `class EvalState` (the file also pulls in `value.hh` and `symbol-table.hh` via includes).

### Functions (declarations)
- `void printAmbiguous(EvalState & state, Value & v, std::ostream & str, std::set<const void *> * seen, NixStringContext * context = nullptr, size_t depth = 0)` — see print-ambiguous.cc.

---

## File: src/libexpr/include/nix/expr/repl-exit-status.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum class ReplExitStatus { QuitAll, Continue }` — kind: scoped enum. Path: src/libexpr/include/nix/expr/repl-exit-status.hh. Purpose: Distinguishes user-initiated `:quit` (terminate program/debugger via `QuitAll`) from `:continue` (resume program execution after a debugger break).

---

## File: src/libexpr/get-drvs.cc

### Namespaces
- `nix`.

### Type aliases (file-local)
- `typedef std::set<const Bindings *> Done` — cache of already-considered attrsets used during recursive traversal.

### Functions
- `PackageInfo::PackageInfo(EvalState & state, std::string attrPath, const Bindings * attrs)` — kind: ctor. Path: src/libexpr/get-drvs.cc. Purpose: Construct from an evaluated attrset; only sets `state`, `attrs`, and `attrPath`.
- `PackageInfo::PackageInfo(EvalState & state, ref<Store> store, const std::string & drvPathWithOutputs)` — kind: ctor. Path: src/libexpr/get-drvs.cc. Purpose: Construct from a `drvPath!output` selector; reads the derivation back from the store; throws if more than one output is selected or the named output is missing; sets `drvPath`, `name`, `outputName`, `outPath`.
- `PackageInfo::queryName() const -> std::string` — kind: member function. Path: src/libexpr/get-drvs.cc. Purpose: Lazily reads `name` from `attrs->get(state->s.name)`; throws `TypeError` if missing.
- `PackageInfo::querySystem() const -> std::string` — kind: member function. Purpose: Lazily reads `system` (defaults to `"unknown"`).
- `PackageInfo::queryDrvPath() const -> std::optional<StorePath>` — kind: member function. Purpose: Lazily reads and validates `drvPath` via `coerceToStorePath` and `requireDerivation`.
- `PackageInfo::requireDrvPath() const -> StorePath` — kind: member function. Purpose: Calls `queryDrvPath`; throws if not present.
- `PackageInfo::queryOutPath() const -> StorePath` — kind: member function. Purpose: Lazily reads `outPath`; throws if missing.
- `PackageInfo::queryOutputs(bool withPaths, bool onlyOutputsToInstall) -> Outputs` — kind: member function. Purpose: Reads the `outputs` attr (defaults to `[ "out" ]`); optionally restricts to `outputSpecified` or `meta.outputsToInstall`.
- `PackageInfo::queryOutputName() const -> std::string` — kind: member function. Purpose: Lazily reads `outputName` (default `""`).
- `PackageInfo::getMeta() -> const Bindings *` — kind: member function (private). Purpose: Lazily fetches and forces `meta`; returns nullptr when not present or no attrs.
- `PackageInfo::queryMetaNames() -> StringSet` — kind: member function. Purpose: Returns the set of meta keys.
- `PackageInfo::checkMeta(Value & v) -> bool` — kind: member function (private). Purpose: Recursively validates that meta value is one of int/bool/string/float, list/attrs of those, and contains no `outPath`; uses `addCallDepth` for stack guard.
- `PackageInfo::queryMeta(const std::string & name) -> Value *` — kind: member function. Purpose: Returns a meta value if present and `checkMeta` passes; otherwise nullptr.
- `PackageInfo::queryMetaString(const std::string & name) -> std::string` — kind: member function. Purpose: Returns the string of `queryMeta(name)` or `""`.
- `PackageInfo::queryMetaInt(const std::string & name, NixInt def) -> NixInt` — kind: member function. Purpose: Returns int meta or coerces string-as-int (back-compat); falls back to `def`.
- `PackageInfo::queryMetaFloat(const std::string & name, NixFloat def) -> NixFloat` — kind: member function. Purpose: Same shape as `queryMetaInt` but for floats.
- `PackageInfo::queryMetaBool(const std::string & name, bool def) -> bool` — kind: member function. Purpose: Returns bool meta or coerces `"true"`/`"false"` strings; falls back to `def`.
- `PackageInfo::setMeta(const std::string & name, Value * v) -> void` — kind: member function. Purpose: Builds a new `Bindings` containing the existing meta minus `name`, plus `(name, v)` if `v` is non-null.
- `getDerivation(EvalState & state, Value & v, const std::string & attrPath, PackageInfos & drvs, Done & done, bool ignoreAssertionFailures) -> bool` — kind: static helper. Path: src/libexpr/get-drvs.cc. Purpose: Forces `v`, returns `true` (the caller may keep recursing) when not a derivation; otherwise builds a `PackageInfo` and pushes it onto `drvs` (deduped via `done`). Returns `false` when not derivation duplicate or once a derivation has been recorded. Catches `AssertionError` when `ignoreAssertionFailures`.
- `getDerivation(EvalState & state, Value & v, bool ignoreAssertionFailures) -> std::optional<PackageInfo>` — kind: free function. Path: src/libexpr/get-drvs.cc. Purpose: Public entry point for "is this a single derivation?"; allocates an empty `Done` and `PackageInfos`, returns the front if exactly one was recorded.
- `addToPath(const std::string & s1, std::string_view s2) -> std::string` — kind: static helper. Purpose: Returns `s2` if `s1` is empty, else `s1 + "." + s2`.
- `isAttrPathComponent(std::string_view symbol) -> bool` — kind: static helper. Purpose: Validates that the symbol matches `[A-Za-z_][A-Za-z0-9-_+]*` (acceptable as a `nix-env -qa` attribute path component).
- `getDerivations(EvalState & state, Value & vIn, const std::string & pathPrefix, Bindings & autoArgs, PackageInfos & drvs, Done & done, bool ignoreAssertionFailures) -> void` — kind: static helper (recursive). Path: src/libexpr/get-drvs.cc. Purpose: Applies `autoCallFunction(autoArgs, vIn, v)`; classifies via `getDerivation`; for attrsets, walks `lexicographicOrder` honouring `_combineChannels` and `recurseForDerivations`; for lists, recurses on each element; otherwise throws `TypeError`.
- `getDerivations(EvalState & state, Value & v, const std::string & pathPrefix, Bindings & autoArgs, PackageInfos & drvs, bool ignoreAssertionFailures) -> void` — kind: free function. Path: src/libexpr/get-drvs.cc. Purpose: Public entry point that allocates an empty `Done` and delegates to the inner overload.

---

## File: src/libexpr/include/nix/expr/get-drvs.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct PackageInfo` — kind: struct. Path: src/libexpr/include/nix/expr/get-drvs.hh. Purpose: A "parsed" Nix derivation/package summary that lazily caches derived attributes.
  - public typedef: `Outputs = std::map<std::string, std::optional<StorePath>, std::less<>>`.
  - private members: `EvalState * state`; mutable `std::string name`, `std::string system`, `std::optional<std::optional<StorePath>> drvPath`, `std::optional<StorePath> outPath`, `std::string outputName`; `Outputs outputs`; `bool failed = false`; `const Bindings * attrs = nullptr`, `const Bindings * meta = nullptr`.
  - public member: `std::string attrPath`.
  - private methods: `const Bindings * getMeta()`, `bool checkMeta(Value &)`.
  - public methods: ctors `PackageInfo(EvalState &)` (state-only), `PackageInfo(EvalState &, std::string attrPath, const Bindings *)`, `PackageInfo(EvalState &, ref<Store>, const std::string & drvPathWithOutputs)`; `queryName`, `querySystem`, `queryDrvPath`, `requireDrvPath`, `queryOutPath`, `queryOutputName`, `queryOutputs(bool withPaths = true, bool onlyOutputsToInstall = false)`, `queryMetaNames`, `queryMeta`, `queryMetaString`, `queryMetaInt`, `queryMetaFloat`, `queryMetaBool`, `setMeta`; inline setters `setName(const std::string &)`, `setDrvPath(StorePath)`, `setOutPath(StorePath)`, `setFailed()`, `hasFailed()`.

### Type aliases
- `typedef std::list<PackageInfo, traceable_allocator<PackageInfo>> PackageInfos`.

### Functions (declarations)
- `std::optional<PackageInfo> getDerivation(EvalState & state, Value & v, bool ignoreAssertionFailures)` — declared.
- `void getDerivations(EvalState & state, Value & v, const std::string & pathPrefix, Bindings & autoArgs, PackageInfos & drvs, bool ignoreAssertionFailures)` — declared.

---

## File: src/libexpr/paths.cc

### Namespaces
- `nix`.

### Functions
- `EvalState::rootPath(CanonPath path) -> SourcePath` — kind: member function. Path: src/libexpr/paths.cc. Purpose: Wrap a canonical path with the eval state's root accessor (`{rootFS, std::move(path)}`).
- `EvalState::rootPath(std::string_view path) -> SourcePath` — kind: member function (overload). Path: src/libexpr/paths.cc. Purpose: Same as above for a `string_view`, going through `absPath` (FIXME: touches the OS cwd, marked for relocation out of `EvalState`).
- `EvalState::storePath(const StorePath & path) -> SourcePath` — kind: member function. Path: src/libexpr/paths.cc. Purpose: Build a `SourcePath` rooted at `rootFS` for the printed store path.
- `EvalState::ensureLazyPathCopied(const StorePath & path) -> void` — kind: member function. Path: src/libexpr/paths.cc. Purpose: When not read-only, force-copies any virtual mounted accessor for `path` into the real store via `fetchToStore` and panics on hash mismatch (sanity check after a `dryRun` mount).
- `EvalState::ensureLazyPathsCopied(const NixStringContext & context) -> void` — kind: member function. Path: src/libexpr/paths.cc. Purpose: Iterate the context, calling `ensureLazyPathCopied` for each `Opaque` element.
- `EvalState::mountInput(fetchers::Input & input, const fetchers::Input & originalInput, ref<SourceAccessor> accessor) -> StorePath` — kind: member function. Path: src/libexpr/paths.cc. Purpose: Compute a virtual store mount for a fetched input via `fetchToStore2(... DryRun ...)`, allowlist the path, mount it, set the `narHash` attr on `input.attrs`, and validate against `originalInput.getNarHash()`.

---

## File: src/libexpr/search-path.cc

### Namespaces
- `nix`.

### Functions
- `LookupPath::Prefix::suffixIfPotentialMatch(std::string_view path) const -> std::optional<std::string_view>` — kind: member function. Path: src/libexpr/search-path.cc. Purpose: If the prefix matches `path` (and a separator follows when both prefix and path are non-empty), return the path remainder (with leading separator skipped); otherwise nullopt.
- `LookupPath::Elem::parse(std::string_view rawElem) -> LookupPath::Elem` — kind: static member function. Path: src/libexpr/search-path.cc. Purpose: Split `prefix=path` (or just `path`) into a `LookupPath::Elem`.
- `LookupPath::parse(const Strings & rawElems) -> LookupPath` — kind: static member function. Path: src/libexpr/search-path.cc. Purpose: Parse a list of raw search-path entries by delegating to `Elem::parse`.

---

## File: src/libexpr/include/nix/expr/search-path.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct LookupPath` — kind: struct. Path: src/libexpr/include/nix/expr/search-path.hh. Purpose: Ordered list of path lookup rules used by `< >` syntax / `builtins.findFile`.
  - forward decls: nested `struct Elem`, `struct Prefix`, `struct Path` (fully defined later in the file).
  - member: `std::list<LookupPath::Elem> elements`.
  - method: `static LookupPath parse(const Strings & rawElems)`.
- `struct LookupPath::Prefix` — kind: nested struct. Purpose: First half of an `Elem` (path prefix to match).
  - member: `std::string s`.
  - macro: `GENERATE_CMP(LookupPath::Prefix, me->s)`.
  - method: `std::optional<std::string_view> suffixIfPotentialMatch(std::string_view path) const`.
- `struct LookupPath::Path` — kind: nested struct. Purpose: Second half of an `Elem` (path or URL to resolve into).
  - member: `std::string s`.
  - macro: `GENERATE_CMP(LookupPath::Path, me->s)`.
- `struct LookupPath::Elem` — kind: nested struct. Purpose: A single search-path rule.
  - members: `Prefix prefix`, `Path path`.
  - macro: `GENERATE_CMP(LookupPath::Elem, me->prefix, me->path)`.
  - method: `static LookupPath::Elem parse(std::string_view rawElem)`.

---

## File: src/libexpr/json-to-value.cc

### Namespaces
- `nix`.

### Type aliases (file-local)
- `using json = nlohmann::json` — file-local convenience.

### Classes / structs / enums (all file-local; nested unless noted)
- `class JSONSax : nlohmann::json_sax<json>` — kind: class. Path: src/libexpr/json-to-value.cc. Purpose: SAX-style adapter that builds Nix `Value`s from streamed nlohmann events; owns a stack of `JSONState` continuations via `std::unique_ptr`.
  - nested `class JSONState` — base "current container" state.
    - protected members: `std::unique_ptr<JSONState> parent`, `RootValue v`.
    - public ctors: `explicit JSONState(std::unique_ptr<JSONState> && p)`, `explicit JSONState(Value * v)`; deleted `JSONState(JSONState & p)`.
    - public methods: `virtual std::unique_ptr<JSONState> resolve(EvalState &)` (default throws `logic_error`); `Value & value(EvalState & state)` (lazy-allocates `v`); `virtual ~JSONState()`; `virtual void add()` (default empty).
  - nested `class JSONObjectState : public JSONState`:
    - inherits ctors via `using JSONState::JSONState`.
    - private member: `ValueMap attrs`.
    - private overrides: `std::unique_ptr<JSONState> resolve(EvalState &) override` (builds bindings via `state.buildBindings`, attaches via `mkAttrs`, returns the parent), `void add() override` (clears `v`).
    - public method: `void key(string_t & name, EvalState & state)` (calls `forceNoNullByte`, inserts into `attrs`).
  - nested `class JSONListState : public JSONState`:
    - private member: `ValueVector values`.
    - private overrides: `std::unique_ptr<JSONState> resolve(EvalState &) override` (calls `state.buildList`, attaches via `mkList`), `void add() override` (pushes `*v`, clears `v`).
    - public ctor: `JSONListState(std::unique_ptr<JSONState> && p, std::size_t reserve)` — calls base ctor and reserves capacity.
  - private members: `EvalState & state`, `std::unique_ptr<JSONState> rs`.
  - public ctor: `JSONSax(EvalState & state, Value & v)` — initialises `rs(new JSONState(&v))`.
  - SAX overrides: `bool null() override`; `bool boolean(bool val) override`; `bool number_integer(number_integer_t) override`; `bool number_unsigned(number_unsigned_t) override` (throws `Error` on overflow of `NixInt::Inner`); `bool number_float(number_float_t, const string_t &) override`; `bool string(string_t & val) override` (calls `forceNoNullByte`, calls `mkString(val, state.mem)`); `bool binary(binary_t &) override` (guarded by `NLOHMANN_JSON_VERSION_MAJOR >= 3 && NLOHMANN_JSON_VERSION_MINOR >= 8`; asserts unreachable); `bool start_object(std::size_t len) override`; `bool key(string_t & name) override`; `bool end_object() override`; `bool end_array() override` (delegates to `end_object`); `bool start_array(size_t len) override` (clamps reserve at 128 when `len == numeric_limits<size_t>::max()`); `bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception & ex) override` (throws `JSONParseError`).

### Functions
- `parseJSON(EvalState & state, const std::string_view & s_, Value & v) -> void` — kind: free function. Path: src/libexpr/json-to-value.cc. Purpose: Run `nlohmann::json::sax_parse` through a `JSONSax` instance and throw `JSONParseError("Invalid JSON Value")` on failure.

---

## File: src/libexpr/include/nix/expr/json-to-value.hh

### Namespaces
- `nix`.

### Forward declarations
- `class EvalState`, `struct Value`.

### Classes / structs / enums
- `MakeError(JSONParseError, Error)` — declares an `Error` subclass via the `MakeError` macro.

### Functions (declarations)
- `void parseJSON(EvalState & state, const std::string_view & s, Value & v)` — declared.

---

## File: src/libexpr/value-to-json.cc

### Namespaces
- `nix`.

### Type aliases (file-local)
- `using json = nlohmann::json`.

### Functions
- `printValueAsJSON(EvalState & state, bool strict, Value & v, const PosIdx pos, NixStringContext & context, bool copyToStore) -> json` — kind: free function. Path: src/libexpr/value-to-json.cc. Purpose: Recursively convert a Nix value to an `nlohmann::json` tree; calls `checkInterrupt`, `addCallDepth(pos)`, optionally `forceValue` when `strict`. Dispatches on `v.type()`: `nInt -> .integer().value`, `nBool -> .boolean()`, `nString -> copyContext + .string_view()`, `nPath -> printStorePath(copyPathToStore(...))` or `path.abs()` based on `copyToStore`, `nNull -> default-init`, `nAttrs -> tryAttrsToString`/`outPath` shortcut/recursive expansion in `lexicographicOrder`, `nList -> recursion with index trace`, `nExternal -> ExternalValueBase::printValueAsJSON`, `nFloat -> .fpoint()`. Throws `TypeError` for `nThunk`/`nFailed`/`nFunction`.
- `printValueAsJSON(EvalState & state, bool strict, Value & v, const PosIdx pos, std::ostream & str, NixStringContext & context, bool copyToStore) -> void` — kind: free function (overload). Path: src/libexpr/value-to-json.cc. Purpose: Streams the converted JSON, wrapping `nlohmann::json::exception` as `JSONSerializationError`.
- `ExternalValueBase::printValueAsJSON(EvalState & state, bool strict, NixStringContext & context, bool copyToStore) const -> json` — kind: virtual member function. Path: src/libexpr/value-to-json.cc. Purpose: Default implementation throws `TypeError("cannot convert %1% to JSON", showType())`; concrete external values override.

---

## File: src/libexpr/include/nix/expr/value-to-json.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `MakeError(JSONSerializationError, Error)` — declares an `Error` subclass.

### Functions (declarations)
- `nlohmann::json printValueAsJSON(EvalState & state, bool strict, Value & v, const PosIdx pos, NixStringContext & context, bool copyToStore = true)` — declared.
- `void printValueAsJSON(EvalState & state, bool strict, Value & v, const PosIdx pos, std::ostream & str, NixStringContext & context, bool copyToStore = true)` — declared.

---

## File: src/libexpr/value-to-xml.cc

### Namespaces
- `nix`.

### Functions
- `singletonAttrs(const std::string & name, std::string_view value) -> XMLAttrs` — kind: static helper. Path: src/libexpr/value-to-xml.cc. Purpose: Build an `XMLAttrs` map containing a single `name=value` pair.
- `printValueAsXML(EvalState & state, bool strict, bool location, Value & v, XMLWriter & doc, NixStringContext & context, StringSet & drvsSeen, const PosIdx pos) -> void` — kind: static recursive helper. Path: src/libexpr/value-to-xml.cc. Purpose: Render `v` into the XML writer using the `nix-instantiate --xml` schema. Calls `checkInterrupt`, `addCallDepth(pos)`, optionally `forceValue`. Emits `<int>`, `<bool>`, `<string>`, `<path>`, `<null>`; for derivations emits `<derivation drvPath outPath>` with attrs, deduping via `drvsSeen` (writes `<repeated/>` on dup); for non-derivation attrsets, `<attrs>` containing nested `<attr name=...>` via `showAttrs`; `<list>` for lists; `<function>` with `<attrspat>` (with optional `name` and `ellipsis="1"`) or `<varpat name>`, falling back to `<unevaluated/>` for primops; `<float>`; and `<unevaluated/>` for `nThunk` and `nFailed`.
- `posToXML(EvalState &, XMLAttrs & xmlAttrs, const Pos & pos) -> void` — kind: static helper. Path: src/libexpr/value-to-xml.cc. Purpose: Populate `path`/`line`/`column` attributes from a `Pos` (when `pos.origin` is a `SourcePath`).
- `showAttrs(EvalState &, bool strict, bool location, const Bindings & attrs, XMLWriter & doc, NixStringContext & context, StringSet & drvsSeen) -> void` — kind: static helper. Path: src/libexpr/value-to-xml.cc. Purpose: Emit each attr in `lexicographicOrder` as `<attr name="...">...</attr>`, optionally including position attributes when `location`.
- `ExternalValueBase::printValueAsXML(EvalState &, bool strict, bool location, XMLWriter & doc, NixStringContext &, StringSet & drvsSeen, const PosIdx pos) const -> void` — kind: virtual member function. Path: src/libexpr/value-to-xml.cc. Purpose: Default implementation emits `<unevaluated/>`; concrete external values override.
- `printValueAsXML(EvalState & state, bool strict, bool location, Value & v, std::ostream & out, NixStringContext & context, const PosIdx pos) -> void` — kind: free function. Path: src/libexpr/value-to-xml.cc. Purpose: Wraps the output in `<expr>...</expr>` and delegates to the static recursion with a fresh `drvsSeen` set.

---

## File: src/libexpr/include/nix/expr/value-to-xml.hh

### Namespaces
- `nix`.

### Functions (declarations)
- `void printValueAsXML(EvalState & state, bool strict, bool location, Value & v, std::ostream & out, NixStringContext & context, const PosIdx pos)` — declared.

---

## Cross-file observations

- Five distinct value-renderers live in this shard, each with its own `nValueType` `switch`: `Printer::print` in `print.cc`, `printAmbiguous` in `print-ambiguous.cc`, `printValueAsJSON` in `value-to-json.cc`, the static `printValueAsXML` in `value-to-xml.cc`, plus the AST-side `Expr::show` (and its subclass overrides) in `nixexpr.cc`. They duplicate similar control flow:
  - call `state.forceValue` (or check `force` option, or are gated by a `strict` flag),
  - check `state.settings.maxCallDepth` / call `addCallDepth(...)` and throw `StackOverflowError`,
  - recurse with a per-printer `seen`/`drvsSeen`/`Done` set,
  - emit a special-case rendering for derivations (testing `state.s.drvPath` / `state.isDerivation`),
  - dispatch on `v.type()` enum values.
  This suggests a shared visitor scaffold (depth check, force, recurse-with-seen-set, derivation special-casing) could be factored out so that `Printer`, `printAmbiguous`, `printValueAsJSON`, `printValueAsXML` only override the per-type rendering.

- `Printer::shouldPrettyPrintAttrs` and `Printer::shouldPrettyPrintList` are nearly identical; the only difference is whether the input is `AttrVec` or `std::span<Value * const>`. Templating the helper, or factoring a small `forceForDecision(item)` utility, would dedupe them.

- `printAmbiguous` and `Printer::print` each independently check `nThunk`/`nFailed`, special-case `nFailed` to "thunk"/"<CODE>" output, and re-check black-hole status to print the "potential infinite recursion" message; the ambiguous renderer additionally has its own `<CODE>`/`<LAMBDA>`/`<PRIMOP>` placeholders. The black-hole/thunk handling is a candidate for a single helper.

- The `seen-set` pattern recurs four times: `print.cc` (`ValuesSeen = std::set<const void *>`), `print-ambiguous.cc` (a passed-in `std::set<const void *>`), `get-drvs.cc` (`Done = std::set<const Bindings *>`), `value-to-xml.cc` (`StringSet drvsSeen` keyed on derivation path). Three of those track `Bindings *`/`Value *` identity; only XML uses the `drvPath` string. A tiny `SeenSet` abstraction could unify the first three.

- Path resolution rules sit in two places that look related but aren't shared: `path_start` in `parser.y` (handles absolute, relative, and `~/` paths with their lint diagnoses) and `EvalState::rootPath`/`storePath` in `paths.cc`. The parser rule constructs an `ExprPath` directly with the right accessor, but the relative-path computation (`CanonPath(literal, basePath.path).abs()`) has the same shape as `EvalState::rootPath(string_view)` modulo the source of `basePath`.

- Two parallel containers exist for formals: `FormalsBuilder` (`std::vector<Formal>` + ellipsis, used during parsing) and `Formals` (`std::span<Formal>` + ellipsis, used post-allocation). Both implement `has(Symbol)` independently with the same lower-bound predicate; consolidating to one (e.g. `Formals` over a non-owning span and a builder-only vector wrapper) would remove duplicate code, especially as `ExprLambda` already converts between them by copying the formals into the bump arena.

- `ExprAttrs::AttrDef::chooseByKind<T>` and the duplicated `chooseByKind` style triple `(plain, inherited, inheritedFrom)` are mirrored manually in `bindVars` callers (`*attrs`, both `ExprAttrs::bindVars` and `ExprLet::bindVars`). The pattern is consistent enough that future additions to `Kind` would need updates in three places.

- `parseExprFromBuf` and `parseReplBindingsFromBuf` are byte-identical except for one extra `setReplBindingsMode(scanner)` call after `yy_scan_buffer` and the final `dynamic_cast<ExprAttrs *>` plus the two assertions. Those two functions could share a helper.

- The `MakeBinOp` macro dance in `nixexpr.hh` produces six near-identical AST node types (`ExprOpEq`, `ExprOpNEq`, `ExprOpAnd`, `ExprOpOr`, `ExprOpImpl`, `ExprOpConcatLists`) plus the partial `MakeBinOpMembers` reuse for `ExprOpUpdate`. A CRTP base or a templated `BinOp<EvalFn>` would be a more conventional alternative if/when these need to grow members.

- `printIdentifier` in `print.cc` and the file-local `isVarName` in the same TU share near-identical character-class checks (the public function falls back to `printLiteralString` when the predicate fails). Both could converge on a single classifier (e.g. `isVarNameTail` / `isVarNameHead`).

- `printLiteralString(stream, sv, maxLength, ansiColors)` is the only impl; the public `printLiteralString(stream, sv)` thin-wraps it. The same pattern (default-arg overload in header, full-arg impl in cc) recurs in `printValueAsJSON`, `printValueAsXML`, and `printAmbiguous`. They are consistent and not duplicated, but worth noting.

- Three independent `MakeError(...)` declarations live in this shard (`JSONParseError` in `json-to-value.hh`, `JSONSerializationError` in `value-to-json.hh`; `ParseError` is defined elsewhere). They follow a clear convention: the file producing the error owns the type. No duplication issue, but worth mentioning in case there is a desire for a shared `ExprError` umbrella.
