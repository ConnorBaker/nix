# 3. Strings

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

Nix strings have:
- Arbitrary length content
- **String context** tracking store path dependencies
- Interpolation (`"${expr}"`)
- Path conversion with context tracking

## Nix Implementation Details

```cpp
struct StringWithContext {
    const StringData* str;      // The string content
    const Context* context;     // Store path dependencies (may be null)
};

// Context elements: Opaque (path), DrvDeep (drv+closure), Built (drv output)
```

## Option A: List of Character Codes

Simple encoding as list of integers.

```hvm4
// String = List of Char (32-bit Unicode codepoints)
// Already supported by HVM4 syntax: "hello" → cons list

@str_length = @length  // Reuse list length
@str_concat = @concat
@substring = λstart.λlen.λstr. @take(len, @drop(start, str))
@str_eq = λs1.λs2. @list_eq(s1, s2)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Memory | 64 bits per character (inefficient) |
| Operations | O(n) for most |
| Unicode | Full support |
| Implementation | Trivial |

**Best for:** Prototyping, small strings

## Option B: Chunked String (Rope)

Tree of string chunks for efficient operations.

```hvm4
// Rope = #Leaf{chars} | #Node{left, right, total_len}
// chars is small list of characters

@rope_concat = λr1.λr2. #Node{r1, r2, @rope_len(r1) + @rope_len(r2)}
@rope_substring = λstart.λlen.λrope. ...  // O(log n) split operations
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| concat | O(1) or O(log n) |
| substring | O(log n) |
| Complexity | Medium |

## Option C: Packed Chunks

Pack multiple characters into 32-bit words.

```hvm4
// Pack 4 ASCII chars or 2 UTF-16 code units per NUM
// String = #Str{encoding, chunks} where chunks is list of packed NUMs

// Requires bit manipulation
@pack_ascii = λc1.λc2.λc3.λc4. c1 + (c2 << 8) + (c3 << 16) + (c4 << 24)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Memory | 4x more efficient for ASCII |
| Operations | More complex |
| Unicode | Needs variable encoding |

## String Context Handling

**Critical**: String context must be threaded through all string operations.

```hvm4
// StringWithContext = #Str{chars, context}
// Context = #NoC{} | #Ctx{elements}
// CtxElem = #Opq{path} | #Drv{drvPath} | #Blt{drvPath, output}

@str_concat = λs1.λs2.
  #Str{@char_concat(s1.chars, s2.chars), @ctx_merge(s1.context, s2.context)}

@ctx_merge = λc1.λc2. λ{
  #NoC: c2
  #Ctx: λelems1. λ{
    #NoC: c1
    #Ctx: λelems2. #Ctx{@set_union(elems1, elems2)}
  }(c2)
}(c1)
```

**Key Insight**: Context is a **set** - use same encoding as attribute set keys.

## CHOSEN: HVM4 Native #Chr List + Context Wrapper (Option A with Context)

**Rationale:**
- HVM4 already represents strings as `#Con{#Chr{n}, ...}` natively
- Wrap with context to preserve Nix's store path tracking
- Context is a sorted set (reuse same encoding as attribute set keys)
- String content is strict in Nix (not lazy), so char list is natural
- Memory cost (64 bits per char) acceptable for most Nix strings

**Encoding:**
```hvm4
// StringWithContext = #Str{chars, context}
// chars = list of #Chr{codepoint}
// context = #NoC{} | #Ctx{elements}
// elements = sorted list of context element hashes

// Example: "hello" with no context
#Str{#Con{#Chr{104}, #Con{#Chr{101}, #Con{#Chr{108},
     #Con{#Chr{108}, #Con{#Chr{111}, #Nil{}}}}}}, #NoC{}}

// Example: "${drv}" with context
#Str{chars, #Ctx{#Con{hash_of_drv, #Nil{}}}}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-string.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-string.hh`

#### Step 1: Define Encoding Constants and API

```cpp
// hvm4-string.hh
namespace nix::hvm4 {

// Constructor names (base-64 encoded)
constexpr uint32_t STRING_WRAPPER = /* encode "#Str" */;
constexpr uint32_t CHAR_NODE = /* encode "#Chr" */;   // HVM4 native
constexpr uint32_t CONTEXT_NONE = /* encode "#NoC" */;
constexpr uint32_t CONTEXT_SET = /* encode "#Ctx" */;

// Create string with no context
Term makeString(std::string_view content, HVM4Runtime& runtime);

// Create string with context
Term makeStringWithContext(std::string_view content,
                           const NixStringContext& context,
                           HVM4Runtime& runtime);

// Create empty string
Term makeEmptyString(HVM4Runtime& runtime);

// Concatenate two strings (merges contexts)
Term concatStrings(Term s1, Term s2, HVM4Runtime& runtime);

// Get string length (character count)
uint32_t stringLength(Term strTerm, const HVM4Runtime& runtime);

// Extract chars list from string wrapper
Term stringChars(Term strTerm, const HVM4Runtime& runtime);

// Extract context from string wrapper
Term stringContext(Term strTerm, const HVM4Runtime& runtime);

// Check if term is our string encoding
bool isNixString(Term term);

}  // namespace nix::hvm4
```

#### Step 2: Implement String Construction

```cpp
// hvm4-string.cc
Term makeChar(uint32_t codepoint, HVM4Runtime& runtime) {
    Term args[1] = { runtime.makeNum(codepoint) };
    return runtime.makeCtr(CHAR_NODE, 1, args);
}

Term makeCharList(std::string_view content, HVM4Runtime& runtime) {
    // Build from back to front (cons list)
    Term list = runtime.makeCtr(LIST_NIL, 0, nullptr);

    // Handle UTF-8 → codepoints
    auto it = content.rbegin();
    while (it != content.rend()) {
        // For now, assume ASCII (1 byte = 1 codepoint)
        // TODO: Full UTF-8 decoding
        uint32_t codepoint = static_cast<uint8_t>(*it);
        Term chr = makeChar(codepoint, runtime);
        list = makeCons(chr, list, runtime);
        ++it;
    }
    return list;
}

Term makeNoContext(HVM4Runtime& runtime) {
    return runtime.makeCtr(CONTEXT_NONE, 0, nullptr);
}

Term makeContextSet(Term elements, HVM4Runtime& runtime) {
    Term args[1] = { elements };
    return runtime.makeCtr(CONTEXT_SET, 1, args);
}

Term makeString(std::string_view content, HVM4Runtime& runtime) {
    Term chars = makeCharList(content, runtime);
    Term ctx = makeNoContext(runtime);
    Term args[2] = { chars, ctx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}

Term makeStringWithContext(std::string_view content,
                           const NixStringContext& nixCtx,
                           HVM4Runtime& runtime) {
    Term chars = makeCharList(content, runtime);
    Term ctx = encodeContext(nixCtx, runtime);
    Term args[2] = { chars, ctx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}
```

#### Step 3: Implement Context Encoding

```cpp
// Context is a set of context elements, encoded as sorted list of hashes
Term encodeContext(const NixStringContext& nixCtx, HVM4Runtime& runtime) {
    if (nixCtx.empty()) {
        return makeNoContext(runtime);
    }

    // Collect and sort context element hashes
    std::vector<uint32_t> hashes;
    for (const auto& elem : nixCtx) {
        // Hash the context element for compact representation
        // Store full elements in a side table if needed for extraction
        uint32_t hash = hashContextElement(elem);
        hashes.push_back(hash);
    }
    std::sort(hashes.begin(), hashes.end());

    // Build sorted list
    Term list = runtime.makeCtr(LIST_NIL, 0, nullptr);
    for (int i = hashes.size() - 1; i >= 0; i--) {
        Term hashTerm = runtime.makeNum(hashes[i]);
        list = makeCons(hashTerm, list, runtime);
    }

    return makeContextSet(list, runtime);
}

// Merge two contexts (set union)
Term mergeContext(Term ctx1, Term ctx2, HVM4Runtime& runtime) {
    // If either is NoContext, return the other
    if (term_ext(ctx1) == CONTEXT_NONE) return ctx2;
    if (term_ext(ctx2) == CONTEXT_NONE) return ctx1;

    // Both have context - merge sorted lists
    Term list1 = getContextElements(ctx1, runtime);
    Term list2 = getContextElements(ctx2, runtime);
    Term merged = mergeSortedLists(list1, list2, runtime);

    return makeContextSet(merged, runtime);
}
```

#### Step 4: Add ExprString Support

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprString*>(&expr)) {
    return true;  // String literals always compile
}

// In emit():
if (auto* e = dynamic_cast<const ExprString*>(&expr)) {
    return emitString(*e, ctx);
}

Term HVM4Compiler::emitString(const ExprString& e, CompileContext& ctx) {
    // ExprString contains the string content
    std::string_view content = e.s;
    return makeString(content, ctx.runtime());
}
```

#### Step 5: Update ExprConcatStrings for String Interpolation

```cpp
// In canCompileWithScope:
if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
    for (auto& [pos, subExpr] : *e->es) {
        if (!canCompileWithScope(*subExpr, scope)) return false;
    }
    return true;
}

// Emitter:
Term HVM4Compiler::emitConcatStrings(const ExprConcatStrings& e,
                                      CompileContext& ctx) {
    if (e.es->empty()) {
        return makeEmptyString(ctx.runtime());
    }

    // Compile first element
    Term result = emit(*(*e.es)[0].second, ctx);

    // If forceString, ensure it's converted to string
    if (e.forceString) {
        result = emitCoerceToString(result, ctx);
    }

    // Concatenate remaining elements
    for (size_t i = 1; i < e.es->size(); i++) {
        Term elem = emit(*(*e.es)[i].second, ctx);
        if (e.forceString) {
            elem = emitCoerceToString(elem, ctx);
        }
        result = concatStrings(result, elem, ctx.runtime());
    }

    return result;
}

// String concatenation with context merge
Term concatStrings(Term s1, Term s2, HVM4Runtime& runtime) {
    // Extract parts
    Term chars1 = stringChars(s1, runtime);
    Term ctx1 = stringContext(s1, runtime);
    Term chars2 = stringChars(s2, runtime);
    Term ctx2 = stringContext(s2, runtime);

    // Concat char lists (append chars2 to chars1)
    Term mergedChars = appendLists(chars1, chars2, runtime);

    // Merge contexts
    Term mergedCtx = mergeContext(ctx1, ctx2, runtime);

    // Build result
    Term args[2] = { mergedChars, mergedCtx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}
```

#### Step 6: Implement String Operations

```cpp
// String length - count chars in list
uint32_t stringLength(Term strTerm, const HVM4Runtime& runtime) {
    Term chars = stringChars(strTerm, runtime);
    uint32_t count = 0;

    Term current = chars;
    while (term_ext(current) == LIST_CONS) {
        count++;
        uint32_t loc = term_val(current);
        current = runtime.getHeapAt(loc + 1);  // tail
    }
    return count;
}

// Substring operation
Term substringOp(Term str, uint32_t start, uint32_t len,
                 HVM4Runtime& runtime) {
    Term chars = stringChars(str, runtime);
    Term ctx = stringContext(str, runtime);  // Context preserved

    // Skip 'start' characters
    Term current = chars;
    for (uint32_t i = 0; i < start && term_ext(current) == LIST_CONS; i++) {
        uint32_t loc = term_val(current);
        current = runtime.getHeapAt(loc + 1);
    }

    // Take 'len' characters
    Term resultChars = takeN(current, len, runtime);

    Term args[2] = { resultChars, ctx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}

// String equality (for comparisons)
Term stringEqual(Term s1, Term s2, HVM4Runtime& runtime) {
    Term chars1 = stringChars(s1, runtime);
    Term chars2 = stringChars(s2, runtime);

    // Compare character by character
    return listEqual(chars1, chars2, runtime);
}
```

#### Step 7: Implement Result Extraction

```cpp
// In hvm4-result.cc:
void ResultExtractor::extractString(Term term, Value& result) {
    Term chars = stringChars(term, runtime);
    Term ctx = stringContext(term, runtime);

    // Extract characters to std::string
    std::string content;
    Term current = chars;
    while (term_ext(current) == LIST_CONS) {
        uint32_t loc = term_val(current);
        Term chrTerm = runtime.getHeapAt(loc);

        // #Chr{codepoint}
        uint32_t chrLoc = term_val(chrTerm);
        Term cpTerm = runtime.getHeapAt(chrLoc);
        uint32_t codepoint = term_val(cpTerm);

        // For now, assume ASCII
        content.push_back(static_cast<char>(codepoint));

        current = runtime.getHeapAt(loc + 1);  // tail
    }

    // Extract context
    NixStringContext nixCtx;
    if (term_ext(ctx) == CONTEXT_SET) {
        uint32_t loc = term_val(ctx);
        Term elements = runtime.getHeapAt(loc);
        decodeContextElements(elements, nixCtx, runtime);
    }

    // Create Nix string value
    if (nixCtx.empty()) {
        result.mkString(content);
    } else {
        result.mkStringWithContext(content, nixCtx);
    }
}
```

#### Step 8: Add Comprehensive Tests

```cpp
// In hvm4.cc test file
TEST_F(HVM4BackendTest, StringEmpty) {
    auto v = eval("\"\"", true);
    ASSERT_EQ(v.type(), nString);
    ASSERT_EQ(v.string_view(), "");
}

TEST_F(HVM4BackendTest, StringSimple) {
    auto v = eval("\"hello\"", true);
    ASSERT_EQ(v.type(), nString);
    ASSERT_EQ(v.string_view(), "hello");
}

TEST_F(HVM4BackendTest, StringWithSpaces) {
    auto v = eval("\"hello world\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, StringInterpolation) {
    auto v = eval("let x = \"world\"; in \"hello ${x}\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, StringIntInterpolation) {
    auto v = eval("let n = 42; in \"number: ${toString n}\"", true);
    // Requires toString primop
    ASSERT_EQ(v.string_view(), "number: 42");
}

TEST_F(HVM4BackendTest, StringConcat) {
    auto v = eval("\"hello\" + \" \" + \"world\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, StringInLet) {
    auto v = eval("let s = \"test\"; in s", true);
    ASSERT_EQ(v.string_view(), "test");
}

TEST_F(HVM4BackendTest, StringNoContext) {
    auto v = eval("\"plain string\"", true);
    ASSERT_TRUE(v.context().empty());
}

TEST_F(HVM4BackendTest, StringMultiline) {
    auto v = eval("''\n  line1\n  line2\n''", true);
    ASSERT_EQ(v.string_view(), "line1\nline2\n");
}

TEST_F(HVM4BackendTest, StringEscape) {
    auto v = eval("\"hello\\nworld\"", true);
    ASSERT_EQ(v.string_view(), "hello\nworld");
}

TEST_F(HVM4BackendTest, StringLength) {
    // Requires stringLength primop
    auto v = eval("builtins.stringLength \"hello\"", true);
    ASSERT_EQ(v.integer().value, 5);
}

// === Escape Sequences ===

TEST_F(HVM4BackendTest, StringEscapeTab) {
    auto v = eval("\"a\\tb\"", true);
    ASSERT_EQ(v.string_view(), "a\tb");
}

TEST_F(HVM4BackendTest, StringEscapeCarriageReturn) {
    auto v = eval("\"a\\rb\"", true);
    ASSERT_EQ(v.string_view(), "a\rb");
}

TEST_F(HVM4BackendTest, StringEscapeBackslash) {
    auto v = eval("\"a\\\\b\"", true);
    ASSERT_EQ(v.string_view(), "a\\b");
}

TEST_F(HVM4BackendTest, StringEscapeDollar) {
    auto v = eval("\"a\\${b}\"", true);  // Escaped interpolation
    ASSERT_EQ(v.string_view(), "a${b}");
}

TEST_F(HVM4BackendTest, StringEscapeQuote) {
    auto v = eval("\"a\\\"b\"", true);
    ASSERT_EQ(v.string_view(), "a\"b");
}

// === Unicode ===

TEST_F(HVM4BackendTest, StringUnicodeBasic) {
    auto v = eval("\"héllo wörld\"", true);
    ASSERT_EQ(v.string_view(), "héllo wörld");
}

TEST_F(HVM4BackendTest, StringUnicodeEmoji) {
    auto v = eval("\"hello 🌍\"", true);
    ASSERT_EQ(v.string_view(), "hello 🌍");
}

TEST_F(HVM4BackendTest, StringUnicodeCJK) {
    auto v = eval("\"你好世界\"", true);
    ASSERT_EQ(v.string_view(), "你好世界");
}

TEST_F(HVM4BackendTest, StringLengthUnicode) {
    // Note: Nix counts bytes, not codepoints
    auto v = eval("builtins.stringLength \"héllo\"", true);
    // "héllo" is 6 bytes in UTF-8 (é is 2 bytes)
    ASSERT_EQ(v.integer().value, 6);
}

// === Multiline Strings ===

TEST_F(HVM4BackendTest, StringMultilineIndent) {
    auto v = eval("''\n    line1\n    line2\n  ''", true);
    // Indentation stripping
    ASSERT_EQ(v.string_view(), "  line1\n  line2\n");
}

TEST_F(HVM4BackendTest, StringMultilineEscape) {
    auto v = eval("''\\n''", true);  // Literal \n in multiline
    ASSERT_EQ(v.string_view(), "\\n");
}

TEST_F(HVM4BackendTest, StringMultilineInterpolation) {
    auto v = eval("let x = \"test\"; in ''\n  ${x}\n''", true);
    ASSERT_EQ(v.string_view(), "test\n");
}

// === String Primops ===

TEST_F(HVM4BackendTest, StringSubstring) {
    auto v = eval("builtins.substring 0 5 \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "hello");
}

TEST_F(HVM4BackendTest, StringSubstringMiddle) {
    auto v = eval("builtins.substring 6 5 \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "world");
}

TEST_F(HVM4BackendTest, StringSubstringBeyondEnd) {
    // Nix clamps to string length
    auto v = eval("builtins.substring 6 100 \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "world");
}

TEST_F(HVM4BackendTest, StringReplaceStrings) {
    auto v = eval("builtins.replaceStrings [\"o\"] [\"0\"] \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "hell0 w0rld");
}

TEST_F(HVM4BackendTest, StringSplit) {
    auto v = eval("builtins.split \",\" \"a,b,c\"", true);
    ASSERT_EQ(v.listSize(), 5);  // ["a", [","], "b", [","], "c"]
}

TEST_F(HVM4BackendTest, StringMatch) {
    auto v = eval("builtins.match \"a(.)c\" \"abc\"", true);
    ASSERT_EQ(v.listSize(), 1);  // ["b"]
}

TEST_F(HVM4BackendTest, StringMatchNoMatch) {
    auto v = eval("builtins.match \"xyz\" \"abc\"", true);
    ASSERT_EQ(v.type(), nNull);  // null when no match
}

TEST_F(HVM4BackendTest, StringHashString) {
    auto v = eval("builtins.hashString \"sha256\" \"hello\"", true);
    // SHA256 of "hello"
    ASSERT_EQ(v.string_view(),
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

// === Context Handling ===

TEST_F(HVM4BackendTest, StringContextMerge) {
    // Context should merge when concatenating
    // Note: This test requires derivation support to create context
    // For now, test that context is preserved through operations
    auto v = eval("\"${./test}\"", true);
    ASSERT_FALSE(v.context().empty());  // Path adds context
}

TEST_F(HVM4BackendTest, StringContextPreserved) {
    // Context preserved through substring
    auto v = eval("builtins.substring 0 3 \"${./test}\"", true);
    ASSERT_FALSE(v.context().empty());
}

// === Error Handling ===

TEST_F(HVM4BackendTest, StringConcatNonString) {
    // Concatenating non-coercible type should throw
    EXPECT_THROW(eval("\"hello\" + 42", true), EvalError);
}

TEST_F(HVM4BackendTest, StringSubstringNegativeStart) {
    // Negative start should throw
    EXPECT_THROW(eval("builtins.substring (-1) 5 \"hello\"", true), EvalError);
}

// === Strictness ===

TEST_F(HVM4BackendTest, StringInterpolationStrict) {
    // String interpolation forces the interpolated expression
    EXPECT_THROW(eval("\"${throw \\\"error\\\"}\"", true), EvalError);
}

TEST_F(HVM4BackendTest, StringContentStrict) {
    // Unlike lists, string content is always strict
    // This should throw during construction
    EXPECT_THROW(eval("\"hello ${throw \\\"x\\\"}\"", true), EvalError);
}
```

---
