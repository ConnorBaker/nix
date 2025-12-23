# 10. Other Primops

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

Nix has ~100+ primops. Key categories:

## Type Checking Primops

Type checking in HVM4 uses pattern matching on the actual data constructors:

```hvm4
// Types are distinguished by their constructors:
// - Integers: NUM (small) or #Pos/#Neg (BigInt)
// - Booleans: #Tru{} or #Fls{} (or 0/1 as NUM)
// - Strings: #Str{chars, context}
// - Lists: #Lst{length, spine}
// - Attrs: #ABs{list} or #ALy{overlay, base}
// - Paths: #Pth{accessor, path}
// - Functions: LAM

@isString = λv. λ{#Str: λ_.λ_. 1; _: 0}(v)
@isList = λv. λ{#Lst: λ_.λ_. 1; _: 0}(v)
@isAttrs = λv. λ{#ABs: λ_. 1; #ALy: λ_.λ_. 1; _: 0}(v)
@isPath = λv. λ{#Pth: λ_.λ_. 1; _: 0}(v)
// Note: isInt, isBool, isFunction require special handling
```

## List Primops

See Section 2. Most map naturally to HVM4.

```hvm4
@builtins_map = @map
@builtins_filter = @filter
@builtins_foldl = λf.λz.λlist. ...  // Strict fold
@builtins_length = @length
@builtins_head = @head
@builtins_tail = @tail
@builtins_elemAt = @elemAt
@builtins_concatLists = λlists. @foldl(@concat, #Nil{}, lists)
```

## Attribute Set Primops

```hvm4
@builtins_attrNames = λattrs. @map(λattr. attr.name, @to_list(attrs))
@builtins_attrValues = λattrs. @map(λattr. attr.value, @to_list(attrs))
@builtins_hasAttr = λname.λattrs. @is_some(@lookup(name, attrs))
@builtins_getAttr = λname.λattrs.
  @lookup(name, attrs) .or_else. @error("attribute not found")
@builtins_removeAttrs = λattrs.λnames. ...
@builtins_intersectAttrs = λa.λb. ...
```

## String Primops

```hvm4
@builtins_stringLength = @str_length
@builtins_substring = λstart.λlen.λs. @substring(start, len, s.string)
@builtins_toString = λv. @coerce_to_string(v)
@builtins_hashString = λtype.λs. ...  // Would need hash implementation
```

## Type Coercion Rules

Nix has specific type coercion rules that MUST be preserved in the HVM4 backend. These rules determine how values are converted between types implicitly and explicitly.

### String Coercion

The `toString` and string interpolation coercions follow these rules:

```hvm4
@coerce_to_string = λv. λ{
  // Already a string - return as-is (preserving context)
  #Str: λchars.λctx. #Str{chars, ctx}

  // Integer - convert to decimal string
  #Pos: λlo.λhi. @int_to_str(@bigint_to_int(#Pos{lo, hi}))
  #Neg: λlo.λhi. @str_concat(@make_string("-"), @int_to_str(@bigint_abs(#Neg{lo, hi})))

  // For small integers (NUM):
  // This case would be caught by @isSmallInt check

  // Boolean - "true" or "false"
  #Tru: @make_string("true")
  #Fls: @make_string("false")

  // Null - empty string
  #Nul: @make_string("")

  // Path - convert to store path (adds context!)
  #Pth: λacc.λpath. @coerce_path_to_string(#Pth{acc, path})

  // List - error (can't coerce list to string)
  #Lst: λ_.λ_. #Err{@make_string("cannot coerce list to string")}

  // Attrs - check for __toString or outPath
  #ABs: λlist. @coerce_attrs_to_string(#ABs{list})
  #ALy: λov.λbase. @coerce_attrs_to_string(#ALy{ov, base})

  // Function - error
  _: #Err{@make_string("cannot coerce this value to string")}
}(v)

// Special case for attrsets with __toString or outPath
@coerce_attrs_to_string = λattrs.
  @if (@hasAttr(attrs, "__toString"))
      (@coerce_to_string((@getAttr(attrs, "__toString"))(attrs)))
      (@if (@hasAttr(attrs, "outPath"))
           (@coerce_to_string(@getAttr(attrs, "outPath")))
           (#Err{@make_string("cannot coerce set to string")}))
```

### Coercion to Path

```hvm4
@coerce_to_path = λv. λ{
  // Already a path
  #Pth: λacc.λpath. #Pth{acc, path}

  // String - convert to path (absolute or relative)
  #Str: λchars.λctx.
    @if (@str_starts_with(chars, "/"))
        (#Pth{0, #Str{chars, #NoC{}}})  // Absolute path
        (#Err{@make_string("cannot coerce relative string to path")})

  // Other types - error
  _: #Err{@make_string("cannot coerce to path")}
}(v)
```

### Coercion to Integer

```hvm4
@coerce_to_int = λv. λ{
  // Already an integer (small or big)
  #Pos: λlo.λhi. #Pos{lo, hi}
  #Neg: λlo.λhi. #Neg{lo, hi}

  // For small integers, NUM passes through
  // (handled by isSmallInt check)

  // Other types - error
  _: #Err{@make_string("cannot coerce to integer")}
}(v)
```

### Context Preservation During Coercion

When coercing values to strings, context MUST be preserved and merged:

```hvm4
// Path to string adds store path to context
@coerce_path_to_string = λpath.
  let store_path = @copy_to_store(path.path);
  #Str{@path_chars(store_path), #Ctx{#Con{@hash_path(store_path), #Nil{}}}}

// Attrset with outPath inherits its context
@coerce_attrs_to_string = λattrs.
  let out = @getAttr(attrs, "outPath");
  // outPath's context is inherited
  @coerce_to_string(out)
```

### Coercion Tests

```cpp
// === String Coercion Tests ===

TEST_F(HVM4BackendTest, CoerceIntToString) {
    auto v = eval("toString 42", true);
    ASSERT_EQ(v.string_view(), "42");
}

TEST_F(HVM4BackendTest, CoerceNegativeIntToString) {
    auto v = eval("toString (-42)", true);
    ASSERT_EQ(v.string_view(), "-42");
}

TEST_F(HVM4BackendTest, CoerceBoolToString) {
    auto v = eval("toString true", true);
    ASSERT_EQ(v.string_view(), "true");

    v = eval("toString false", true);
    ASSERT_EQ(v.string_view(), "false");
}

TEST_F(HVM4BackendTest, CoerceNullToString) {
    auto v = eval("toString null", true);
    ASSERT_EQ(v.string_view(), "");
}

TEST_F(HVM4BackendTest, CoerceStringToString) {
    auto v = eval("toString \"hello\"", true);
    ASSERT_EQ(v.string_view(), "hello");
}

TEST_F(HVM4BackendTest, CoerceListToStringError) {
    EXPECT_THROW(eval("toString [1 2 3]", true), EvalError);
}

TEST_F(HVM4BackendTest, CoerceAttrsWithToString) {
    auto v = eval(R"(
        let x = { __toString = self: "custom"; };
        in toString x
    )", true);
    ASSERT_EQ(v.string_view(), "custom");
}

TEST_F(HVM4BackendTest, CoerceAttrsWithOutPath) {
    auto v = eval(R"(
        let x = { outPath = "/nix/store/abc123"; };
        in toString x
    )", true);
    ASSERT_EQ(v.string_view(), "/nix/store/abc123");
}

TEST_F(HVM4BackendTest, CoerceAttrsWithBothPreferToString) {
    // __toString takes precedence over outPath
    auto v = eval(R"(
        let x = {
            __toString = self: "toString";
            outPath = "/nix/store/abc";
        };
        in toString x
    )", true);
    ASSERT_EQ(v.string_view(), "toString");
}

TEST_F(HVM4BackendTest, CoerceFunctionToStringError) {
    EXPECT_THROW(eval("toString (x: x)", true), EvalError);
}

TEST_F(HVM4BackendTest, InterpolationCoercion) {
    auto v = eval("\"num: ${toString 42}\"", true);
    ASSERT_EQ(v.string_view(), "num: 42");
}

TEST_F(HVM4BackendTest, InterpolationDirectString) {
    auto v = eval("let s = \"world\"; in \"hello ${s}\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

// === BigInt Coercion Tests ===

TEST_F(HVM4BackendTest, CoerceBigIntToString) {
    auto v = eval("toString 9999999999", true);
    ASSERT_EQ(v.string_view(), "9999999999");
}

TEST_F(HVM4BackendTest, CoerceNegativeBigIntToString) {
    auto v = eval("toString (-9999999999)", true);
    ASSERT_EQ(v.string_view(), "-9999999999");
}

// === Deep Coercion Tests ===

TEST_F(HVM4BackendTest, DeepToString) {
    // Nested __toString calls
    auto v = eval(R"(
        let
            inner = { __toString = self: "inner"; };
            outer = { __toString = self: "outer-${toString inner}"; };
        in toString outer
    )", true);
    ASSERT_EQ(v.string_view(), "outer-inner");
}
```

## Context Propagation

String context tracks store path dependencies and MUST be correctly propagated through all string operations.

### Context Types

```hvm4
// Context element types:
// #Opq{path}       - Opaque context (general store path dependency)
// #Drv{drvPath}    - Derivation context (input derivation)
// #Blt{drv, output} - Built output context (derivation output)

// Context is a sorted set for efficient merging
// #NoC{}           - Empty context
// #Ctx{elements}   - Non-empty context (elements is sorted list)
```

### Context Merging Rules

```hvm4
@merge_context = λctx1.λctx2. λ{
  #NoC: ctx2  // Empty + anything = anything
  #Ctx: λelems1. λ{
    #NoC: #Ctx{elems1}  // Anything + empty = anything
    #Ctx: λelems2. #Ctx{@set_union(elems1, elems2)}
  }(ctx2)
}(ctx1)

// All string operations must merge contexts:
@str_concat = λs1.λs2.
  #Str{
    @char_concat(s1.chars, s2.chars),
    @merge_context(s1.context, s2.context)
  }
```

### Context Propagation Tests

```cpp
// === Context Propagation Tests ===

TEST_F(HVM4BackendTest, ContextPreservedInConcat) {
    // When concatenating strings with context, context is merged
    auto v = eval(R"(
        let
            p1 = builtins.placeholder "out";
            p2 = builtins.placeholder "dev";
        in p1 + p2
    )", true);
    // Result should have context from both placeholders
    auto ctx = v.context();
    ASSERT_GE(ctx.size(), 2);
}

TEST_F(HVM4BackendTest, ContextPreservedInInterpolation) {
    auto v = eval(R"(
        let p = builtins.placeholder "out";
        in "prefix-${p}-suffix"
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextMergeInComplexExpr) {
    auto v = eval(R"(
        let
            drv1 = derivation { name = "a"; system = "x"; builder = "/bin/sh"; };
            drv2 = derivation { name = "b"; system = "x"; builder = "/bin/sh"; };
        in "${drv1}" + "${drv2}"
    )", true);
    auto ctx = v.context();
    ASSERT_GE(ctx.size(), 2);
}

TEST_F(HVM4BackendTest, NoContextForLiteral) {
    auto v = eval("\"hello world\"", true);
    auto ctx = v.context();
    ASSERT_TRUE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedThroughLet) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            x = p;
            y = x;
        in y
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedInListToString) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            parts = [p "-" "suffix"];
        in builtins.concatStringsSep "" parts
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedInSubstring) {
    auto v = eval(R"(
        let p = builtins.placeholder "out";
        in builtins.substring 0 5 p
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedInReplaceStrings) {
    auto v = eval(R"(
        let p = builtins.placeholder "out";
        in builtins.replaceStrings ["x"] ["y"] p
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextFromDerivation) {
    auto v = eval(R"(
        let drv = derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        };
        in "${drv}"
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
    // Should contain derivation context
}

TEST_F(HVM4BackendTest, ContextUnsafeDiscardWorks) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            noCtx = builtins.unsafeDiscardStringContext p;
        in noCtx
    )", true);
    auto ctx = v.context();
    ASSERT_TRUE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextGetAndAdd) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            ctx = builtins.getContext p;
        in builtins.length (builtins.attrNames ctx)
    )", true);
    ASSERT_GE(v.integer().value, 1);
}
```

## Boolean Primops

```hvm4
@builtins_true = #Tru{}
@builtins_false = #Fls{}

// Short-circuit AND (implemented as conditional, not OP2)
@bool_and = λa.λb. λ{#Tru: b; #Fls: #Fls{}}(a)

// Short-circuit OR
@bool_or = λa.λb. λ{#Tru: #Tru{}; #Fls: b}(a)

// Negation
@bool_not = λa. λ{#Tru: #Fls{}; #Fls: #Tru{}}(a)

// Implication: a -> b = !a || b
@bool_impl = λa.λb. λ{#Tru: b; #Fls: #Tru{}}(a)
```

## Comparison Primops

```hvm4
// Deep equality (structural)
@builtins_eq = λa.λb.
  @typeOf(a) == @typeOf(b) .&.
  λ{
    #Str: @str_eq(a, b)
    #Lst: @list_eq(a, b)
    #ABs: @attrs_eq(a, b)
    // ... other types
    _: a == b  // Primitive comparison
  }(@typeOf(a))

// Note: Functions are not comparable (throw error)
@list_eq = λas.λbs.
  @length(as) == @length(bs) .&.
  @all(@zipWith(@builtins_eq, as, bs))
```

## Debugging Primops

```hvm4
// trace: prints first arg, returns second
@builtins_trace = λmsg.λval.
  // Would require effect-based I/O
  #Trace{msg, λ_. val}

// seq: force first arg, return second
@builtins_seq = λa.λb.
  // Force evaluation of a, then return b
  @force(a);
  b

// deepSeq: recursively force first arg
@builtins_deepSeq = λa.λb.
  @deepForce(a);
  b
```

## JSON/TOML Primops

```hvm4
// These would require effect-based parsing or pre-processing

// builtins.fromJSON requires parsing JSON → Nix values
// Map JSON types to Nix:
//   JSON object → Nix attrs
//   JSON array → Nix list
//   JSON string → Nix string
//   JSON number → Nix int (if no decimal) or float (if decimal)
//   JSON true/false → Nix true/false
//   JSON null → Nix null

// Option: Pre-parse during compilation if argument is literal
// Option: Effect-based parsing at runtime
```

## Primop Implementation Strategy

**Phase 1:** Implement essential primops inline
**Phase 2:** Create primop registry with HVM4 implementations

```hvm4
// Registry approach:
@primop = λname. λ{
  "add": @builtin_add
  "sub": @builtin_sub
  "map": @builtin_map
  // ...
  λ_. @error("unknown primop")
}(name)
```

## Primop Tests

```cpp
// === Type Checking Primops ===

TEST_F(HVM4BackendTest, IsInt) {
    auto v = eval("builtins.isInt 42", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isInt \"hello\"", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsString) {
    auto v = eval("builtins.isString \"hello\"", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isString 42", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsBool) {
    auto v = eval("builtins.isBool true", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isBool 1", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsList) {
    auto v = eval("builtins.isList [1 2 3]", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isList { }", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsAttrs) {
    auto v = eval("builtins.isAttrs { a = 1; }", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isAttrs [1 2 3]", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsPath) {
    auto v = eval("builtins.isPath ./.", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isPath \"/path\"", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsFunction) {
    auto v = eval("builtins.isFunction (x: x)", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isFunction 42", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, TypeOf) {
    auto v = eval("builtins.typeOf 42", true);
    ASSERT_EQ(v.string_view(), "int");

    v = eval("builtins.typeOf \"hello\"", true);
    ASSERT_EQ(v.string_view(), "string");

    v = eval("builtins.typeOf true", true);
    ASSERT_EQ(v.string_view(), "bool");

    v = eval("builtins.typeOf [1 2]", true);
    ASSERT_EQ(v.string_view(), "list");

    v = eval("builtins.typeOf { }", true);
    ASSERT_EQ(v.string_view(), "set");
}

// === Boolean Primops ===

TEST_F(HVM4BackendTest, BoolAnd) {
    auto v = eval("true && true", true);
    ASSERT_TRUE(v.boolean());

    v = eval("true && false", true);
    ASSERT_FALSE(v.boolean());

    v = eval("false && true", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, BoolOr) {
    auto v = eval("true || false", true);
    ASSERT_TRUE(v.boolean());

    v = eval("false || false", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, BoolNot) {
    auto v = eval("!true", true);
    ASSERT_FALSE(v.boolean());

    v = eval("!false", true);
    ASSERT_TRUE(v.boolean());
}

TEST_F(HVM4BackendTest, BoolImplication) {
    auto v = eval("true -> true", true);
    ASSERT_TRUE(v.boolean());

    v = eval("true -> false", true);
    ASSERT_FALSE(v.boolean());

    v = eval("false -> false", true);
    ASSERT_TRUE(v.boolean());  // Implication with false antecedent is true
}

TEST_F(HVM4BackendTest, BoolShortCircuit) {
    // Short-circuit: second arg not evaluated
    auto v = eval("false && (throw \"not evaluated\")", true);
    ASSERT_FALSE(v.boolean());

    v = eval("true || (throw \"not evaluated\")", true);
    ASSERT_TRUE(v.boolean());
}

// === Debugging Primops ===

TEST_F(HVM4BackendTest, Seq) {
    auto v = eval("builtins.seq 1 2", true);
    ASSERT_EQ(v.integer().value, 2);
}

TEST_F(HVM4BackendTest, SeqForces) {
    // seq should force first argument
    EXPECT_THROW(eval("builtins.seq (throw \"forced\") 2", true), EvalError);
}

TEST_F(HVM4BackendTest, DeepSeq) {
    auto v = eval("builtins.deepSeq { a = 1; b = 2; } 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, DeepSeqForces) {
    // deepSeq should force nested values
    EXPECT_THROW(
        eval("builtins.deepSeq { a = throw \"deep\"; } 42", true),
        EvalError
    );
}

// === Miscellaneous Primops ===

TEST_F(HVM4BackendTest, Null) {
    auto v = eval("null", true);
    ASSERT_EQ(v.type(), nNull);
}

TEST_F(HVM4BackendTest, IsNull) {
    auto v = eval("builtins.isNull null", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isNull 0", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, Throw) {
    EXPECT_THROW(eval("throw \"error message\"", true), EvalError);
}

TEST_F(HVM4BackendTest, TryEval) {
    auto v = eval("builtins.tryEval 42", true);
    ASSERT_EQ(v.type(), nAttrs);
    auto success = v.attrs()->get(state.symbols.create("success"));
    state.forceValue(*success->value, noPos);
    ASSERT_TRUE(success->value->boolean());
}

TEST_F(HVM4BackendTest, TryEvalCatch) {
    auto v = eval("builtins.tryEval (throw \"error\")", true);
    auto success = v.attrs()->get(state.symbols.create("success"));
    state.forceValue(*success->value, noPos);
    ASSERT_FALSE(success->value->boolean());
}

TEST_F(HVM4BackendTest, Assert) {
    auto v = eval("assert true; 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, AssertFails) {
    EXPECT_THROW(eval("assert false; 42", true), EvalError);
}

TEST_F(HVM4BackendTest, Abort) {
    EXPECT_THROW(eval("builtins.abort \"stopped\"", true), EvalError);
}
```

---
