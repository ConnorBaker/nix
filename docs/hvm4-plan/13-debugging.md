# Debugging and Troubleshooting

This section provides guidance for debugging the HVM4 backend during development.

## Common Issues and Solutions

### 1. Incorrect Laziness Behavior

**Symptom:** Values are being forced when they shouldn't be, causing unexpected errors.

**Diagnosis:**
```cpp
void HVM4Runtime::force(Term term) {
    std::cerr << "Forcing term at " << term_val(term) << std::endl;
    // ... force logic
}
```

**Common Causes:**
- List length computation forcing elements
- Attribute key sorting forcing values
- String context merging forcing string content

**Solution:** Ensure constructors are being used correctly:
- `#Lst{length, spine}` - length is computed from spine structure, not by forcing elements
- `#Atr{key, value}` - key is a strict NUM, value is a lazy thunk

### 2. Wrong Constructor Tags

**Symptom:** Pattern matching fails or produces wrong results.

**Diagnosis:**
```cpp
std::cerr << "Tag: " << term_ext(term) << " expected: " << EXPECTED_TAG << std::endl;
```

**Solution:** Verify base-64 encoding of constructor names matches the encoding table.

### 3. Stack Overflow in Recursion

**Symptom:** Deep recursive evaluation crashes.

**Diagnosis:**
```cpp
thread_local int recursionDepth = 0;
struct RecursionGuard {
    RecursionGuard() { ++recursionDepth; if (recursionDepth > 10000) abort(); }
    ~RecursionGuard() { --recursionDepth; }
};
```

**Solution:**
- Ensure Y-combinator is correctly applied
- Check for unintended infinite loops in cyclic rec
- Consider fuel-based evaluation for debugging

### 4. Context Loss in Strings

**Symptom:** String context is empty when it should contain path references.

**Diagnosis:**
```cpp
void debugContext(Term strTerm, HVM4Runtime& runtime) {
    Term ctx = stringContext(strTerm, runtime);
    if (term_ext(ctx) == NO_CONTEXT) {
        std::cerr << "No context" << std::endl;
    } else {
        std::cerr << "Has context elements" << std::endl;
    }
}
```

**Solution:** Ensure context merging is performed during:
- String concatenation
- String interpolation
- Path-to-string coercion

### 5. Attribute Lookup Returning Wrong Values

**Symptom:** `attrs.name` returns wrong value or fails when it shouldn't.

**Common Causes:**
- Symbol IDs not matching (different SymbolTable instances)
- Layers not being searched in correct order
- Binary search bounds incorrect

## Debug Logging

Enable verbose logging:
```bash
export NIX_HVM4_DEBUG=1
```

Or compile with debug flags:
```cpp
#ifdef HVM4_DEBUG
#define HVM4_LOG(msg) std::cerr << "[HVM4] " << msg << std::endl
#else
#define HVM4_LOG(msg)
#endif
```

## Testing Strategies

### Unit Test Template
```cpp
TEST_F(HVM4BackendTest, DebugSpecificIssue) {
    // 1. Minimal reproduction
    auto v = eval("minimal expression", true);

    // 2. Step-by-step verification
    ASSERT_EQ(v.type(), expectedType);

    // 3. Force nested values
    if (v.type() == nAttrs) {
        for (auto& [name, attr] : *v.attrs()) {
            state.forceValue(*attr.value, noPos);
        }
    }
}
```

### Comparison Testing
```cpp
TEST_F(HVM4BackendTest, CompareWithStandardEvaluator) {
    std::string expr = "complex expression";
    auto stdResult = evalStandard(expr);
    auto hvm4Result = eval(expr, true);
    ASSERT_TRUE(valuesEqual(stdResult, hvm4Result));
}
```

## Debugging Patterns

### Binary Search on Test Cases
When a test fails, use binary search to find minimal reproduction:
```cpp
// Instead of testing the whole expression at once
auto v1 = eval("first half", true);  // Does this work?
auto v2 = eval("second half", true); // Or this?
// Continue subdividing
```

### Verify Constructor Encoding
```cpp
void verifyEncodings() {
    ASSERT_EQ(encodeConstructor("#Nil"), 166118);
    ASSERT_EQ(encodeConstructor("#Con"), 121448);
}
```

## Performance Profiling

### HVM4 Statistics
```cpp
struct HVM4Stats {
    size_t heapAllocations = 0;
    size_t reductions = 0;
    size_t maxStackDepth = 0;

    void report() {
        std::cerr << "Heap: " << heapAllocations << std::endl;
        std::cerr << "Reductions: " << reductions << std::endl;
        std::cerr << "Max stack: " << maxStackDepth << std::endl;
    }
};
```

### Benchmarking
```cpp
TEST_F(HVM4BackendTest, BenchmarkLargeEval) {
    auto start = std::chrono::high_resolution_clock::now();
    auto v = eval("large expression", true);
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cerr << "Evaluation time: " << ms.count() << "ms" << std::endl;
}
```
