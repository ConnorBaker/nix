# Error Handling Strategy

Status (2025-12-28): Structured error propagation is not implemented. The backend
uses ERA as a placeholder for missing attributes and failed asserts, and many
error conditions fall back to the standard evaluator. The sections below describe
the planned error model, not the current behavior.

## Error Encoding (planned)

All errors use the `#Err{message}` constructor:

```hvm4
// Error representation
#Err{message}  // message is a #Str{chars, #NoC{}}

// Error creation helper
@error = λmsg. #Err{@make_string(msg)}
```

## Error Categories

### 1. Attribute Access Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Missing attribute | `{ a = 1; }.b` | Immediate throw | Return `#Err{"missing attribute"}` |
| Select on non-attrset | `42.a` | Immediate throw | Return `#Err{"not an attrset"}` |
| hasAttr on non-attrset | `42 ? a` | Immediate throw | Return `#Err{"not an attrset"}` |
| Update with non-attrset | `42 // {}` | Immediate throw | Return `#Err{"not an attrset"}` |

**Key semantics**: Error only occurs when the attribute is *accessed*, not when the attrset is constructed:
```nix
{ a = 1; b = throw "x"; }.a  # Returns 1, no error
{ a = 1; b = throw "x"; }.b  # Throws "x"
```

### 2. List Operation Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| head/tail on empty | `builtins.head []` | Immediate throw | Return `#Err{"empty list"}` |
| elemAt out of bounds | `builtins.elemAt [1] 5` | Immediate throw | Return `#Err{"index out of bounds"}` |
| elemAt negative index | `builtins.elemAt [1] (-1)` | Immediate throw | Return `#Err{"negative index"}` |

**Key semantics**: List length is strict but elements are lazy:
```nix
builtins.length [1, throw "x", 3]  # Returns 3, no error
builtins.elemAt [1, throw "x", 3] 0  # Returns 1, no error
builtins.elemAt [1, throw "x", 3] 1  # Throws "x"
```

### 3. String Operation Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Concat non-string | `"a" + 42` | Immediate throw | Return `#Err{"expected string"}` |
| Negative substring start | `builtins.substring (-1) 5 s` | Immediate throw | Return `#Err{"negative start"}` |
| Invalid interpolation | `"${throw \"x\"}"` | Immediate throw | Error during construction |

**Key semantics**: Strings are fully strict; interpolation errors occur during construction.

### 4. Arithmetic Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Division by zero | `1 / 0` | Immediate throw | Return `#Err{"division by zero"}` |
| Type mismatch | `1 + "a"` | Immediate throw | Return `#Err{"expected number"}` |
| Overflow (64-bit) | Large result | Wrap around | Use BigInt encoding |

### 5. Function Application Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Wrong argument type | Pattern mismatch | Immediate throw | Return `#Err{"pattern mismatch"}` |
| Missing required attr | `({a}: a) {}` | Immediate throw | Return `#Err{"missing required: a"}` |
| Unexpected attr | `({a}: a) {a=1; b=2;}` | Immediate throw if no `...` | Return `#Err{"unexpected attr: b"}` |
| Call non-function | `42 1` | Immediate throw | Return `#Err{"not a function"}` |

## Error Propagation Strategy

### Error Values (CHOSEN)

Use error values that propagate through computation:

```hvm4
// Error-aware operations
@add = λa.λb. λ{
  #Err: λmsg. #Err{msg}   // Propagate error from a
  _: λ{
    #Err: λmsg. #Err{msg} // Propagate error from b
    _: a + b              // Actual addition
  }(b)
}(a)
```

### Error Extraction at Boundary

When extracting results from HVM4 back to C++:

```cpp
Value ResultExtractor::extractToNix(Term term) {
    uint32_t tag = term_ext(term);

    if (tag == ERR_CONSTRUCTOR) {
        uint32_t loc = term_val(term);
        Term msgTerm = runtime.getHeapAt(loc);
        std::string msg = extractString(msgTerm);
        throw EvalError(msg);  // Convert to Nix exception
    }

    // Normal extraction...
}
```

## Error Handling Tests Summary

| Feature | Test Category | Example Test |
|---------|---------------|--------------|
| Attrs | Missing attr | `EXPECT_THROW(eval("{ a = 1; }.b"), EvalError)` |
| Attrs | Type mismatch | `EXPECT_THROW(eval("42.a"), EvalError)` |
| Lists | Empty access | `EXPECT_THROW(eval("builtins.head []"), EvalError)` |
| Lists | Index bounds | `EXPECT_THROW(eval("builtins.elemAt [1] 5"), EvalError)` |
| Strings | Type mismatch | `EXPECT_THROW(eval("\"a\" + 42"), EvalError)` |
| Strings | Bad substring | `EXPECT_THROW(eval("builtins.substring (-1) 1 \"x\""), EvalError)` |
| Arithmetic | Div by zero | `EXPECT_THROW(eval("1 / 0"), EvalError)` |
| Functions | Missing arg | `EXPECT_THROW(eval("({a}: a) {}"), EvalError)` |
