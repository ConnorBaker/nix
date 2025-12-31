# HVM4 Evaluator Backend for Nix

> Note: This is the source document. The consolidated, split plan lives under `docs/hvm4-plan` starting at [00-overview.md](./00-overview.md).

## Research Notes and Implementation Plan

---

# Part 1: Codebase Research

## 1. Nix Evaluator Architecture

### 1.1 AST Representation (`src/libexpr/include/nix/expr/nixexpr.hh`)

The Nix parser produces an AST with these expression types:

| Expression | Description | Lines |
|------------|-------------|-------|
| `ExprInt` | Integer literals (64-bit) | 152-168 |
| `ExprFloat` | Float literals (double) | 170-181 |
| `ExprString` | String literals with context | 183-200 |
| `ExprPath` | Path literals | 202-225 |
| `ExprVar` | Variable references | 227-261 |
| `ExprSelect` | Attribute selection (`a.b.c`) | 263-323 |
| `ExprOpHasAttr` | Has attribute (`a ? b`) | 325-355 |
| `ExprAttrs` | Attribute sets `{ ... }` and `rec { ... }` | 357-442 |
| `ExprList` | Lists `[ ... ]` | 444-480 |
| `ExprLambda` | Functions `x: body` or `{ args }: body` | 514-591 |
| `ExprCall` | Function application | 593-628 |
| `ExprLet` | Let expressions | 630-638 |
| `ExprWith` | With expressions | 640-657 |
| `ExprIf` | Conditionals | 659-675 |
| `ExprAssert` | Assert expressions | 677-692 |
| `ExprOpNot` | Boolean not | 694-706 |
| `ExprConcatStrings` | String interpolation **AND addition (+)** | 762-795 |
| Binary ops | `ExprOpEq`, `ExprOpNEq`, `ExprOpAnd`, `ExprOpOr`, `ExprOpImpl`, `ExprOpUpdate`, `ExprOpConcatLists` | 708-760 |

**IMPORTANT DISCOVERY**: There is NO `ExprOpAdd`, `ExprOpSub`, `ExprOpMul`, `ExprOpDiv`!
- Addition (`+`) is handled by `ExprConcatStrings` (checks if operands are numbers vs strings)
- Subtraction, multiplication, division are primops: `__sub`, `__mul`, `__div`

### 1.2 Exact Expression Structures

#### ExprInt (Lines 152-168)
```cpp
struct ExprInt : Expr {
    Value v;  // Contains the integer value

    ExprInt(NixInt n) { v.mkInt(n); }
    ExprInt(NixInt::Inner n) { v.mkInt(n); }  // NixInt::Inner = int64_t
};

// Access pattern:
int64_t value = exprInt.v.integer().value;  // .integer() returns NixInt (Checked<int64_t>)
```

#### ExprFloat (Lines 170-181)
```cpp
struct ExprFloat : Expr {
    Value v;
    ExprFloat(NixFloat nf) { v.mkFloat(nf); }  // NixFloat = double
};

// Access pattern:
double value = exprFloat.v.fpoint();
```

#### ExprVar (Lines 227-261)
```cpp
typedef uint32_t Level;
typedef uint32_t Displacement;

struct ExprVar : Expr {
    PosIdx pos;
    Symbol name;                    // Variable name (interned string ID)
    ExprWith * fromWith = nullptr;  // nullptr if not from 'with'
    Level level = 0;                // Env frames to walk up
    Displacement displ = 0;         // Index into env.values[]
};

// Variable lookup (eval.cc:889-920):
// 1. Walk up `level` environments via env->up
// 2. Return env->values[displ]
```

#### ExprLambda (Lines 514-591)
```cpp
struct ExprLambda : Expr {
    PosIdx pos;
    Symbol name;              // Optional function name (for debugging)
    Symbol arg;               // Simple argument name for `x: ...`

private:
    bool hasFormals;          // Has pattern matching `{...}: ...`
    bool ellipsis;            // Has `...` in formals
    uint16_t nFormals;
    Formal * formalsStart;

public:
    Expr * body;

    std::optional<Formals> getFormals() const;  // Returns Formals if hasFormals
};

struct Formal {
    PosIdx pos;
    Symbol name;
    Expr * def;  // Default value, nullptr if none
};

struct Formals {
    std::span<Formal> formals;
    bool ellipsis;
};
```

#### ExprCall (Lines 593-628)
```cpp
struct ExprCall : Expr {
    Expr * fun;
    std::optional<std::pmr::vector<Expr *>> args;  // Never null
    PosIdx pos;
};
```

#### ExprLet (Lines 630-638)
```cpp
struct ExprLet : Expr {
    ExprAttrs * attrs;  // The bindings
    Expr * body;
};

// attrs->attrs is std::pmr::map<Symbol, AttrDef>
// AttrDef has: Kind kind, Expr * e, PosIdx pos, Displacement displ
```

#### ExprIf (Lines 659-675)
```cpp
struct ExprIf : Expr {
    PosIdx pos;
    Expr *cond, *then, *else_;
};
```

#### Binary Operations (Lines 708-760)
```cpp
// Created via MakeBinOp macro
struct ExprOpEq : Expr { PosIdx pos; Expr *e1, *e2; };
struct ExprOpNEq : Expr { PosIdx pos; Expr *e1, *e2; };
struct ExprOpAnd : Expr { PosIdx pos; Expr *e1, *e2; };
struct ExprOpOr : Expr { PosIdx pos; Expr *e1, *e2; };
struct ExprOpImpl : Expr { PosIdx pos; Expr *e1, *e2; };
struct ExprOpConcatLists : Expr { PosIdx pos; Expr *e1, *e2; };

struct ExprOpNot : Expr { Expr * e; };
```

#### ExprConcatStrings (Lines 762-795) - **Handles + operator!**
```cpp
struct ExprConcatStrings : Expr {
    PosIdx pos;
    bool forceString;  // true for string interpolation, false for + operator
    std::pmr::vector<std::pair<PosIdx, Expr *>> * es;  // Expressions to concat
};

// When forceString=false and es has 2 numeric elements, performs addition
// See eval.cc:2060-2120 for the logic
```

### 1.3 Value Representation (`src/libexpr/include/nix/expr/value.hh`)

```cpp
typedef enum {
    nThunk,    // Unevaluated expression
    nInt,      // 64-bit integer
    nFloat,    // Double precision float
    nBool,     // Boolean
    nString,   // String with context
    nPath,     // Path value
    nNull,     // Null value
    nAttrs,    // Attribute set
    nList,     // List
    nFunction, // Lambda or primop
    nExternal, // External value type
} ValueType;

// NixInt type (from checked-arithmetic.hh)
using NixInt = checked::Checked<int64_t>;

// Checked<T> has:
//   T value;                           // The actual value
//   Result operator+(Checked<T>) const; // Returns Result with overflow detection
//   Result::valueChecked() -> std::optional<T>  // nullopt if overflow
```

### 1.4 Symbol System (`src/libexpr/include/nix/expr/symbol-table.hh`)

```cpp
class Symbol {
    uint32_t id;  // Internal ID, 0 = empty/null
public:
    uint32_t getId() const noexcept { return id; }
    explicit operator bool() const noexcept { return id > 0; }
};

// To get string from Symbol:
SymbolTable & symbols;
Symbol sym;
std::string_view name = symbols[sym];  // SymbolStr converts to string_view
```

### 1.5 Environment Structure (`src/libexpr/include/nix/expr/eval.hh`:173-177)

```cpp
struct Env {
    Env * up;           // Parent environment
    Value * values[0];  // Flexible array of values
};

// Memory allocation:
Env & env = state.mem.allocEnv(size);  // Allocates Env + size Value* slots
```

### 1.6 Expression Traversal

**No visitor pattern exists.** Use `dynamic_cast`:

```cpp
void compile(const Expr& expr) {
    if (auto* e = dynamic_cast<const ExprInt*>(&expr)) { ... }
    if (auto* e = dynamic_cast<const ExprFloat*>(&expr)) { ... }
    if (auto* e = dynamic_cast<const ExprVar*>(&expr)) { ... }
    // etc.
}
```

---

## 2. HVM4 Architecture

### 2.1 Core Concepts

HVM4 is a runtime for the **Interaction Calculus**, extending lambda calculus with:
- **Duplications** (`!x&L=v; t`): One value in two locations
- **Superpositions** (`&L{a,b}`): Two values in one location

This enables **optimal lazy evaluation** - work is never duplicated, even inside lambdas.

### 2.2 Global State (`hvm4/clang/hvm4.c`:145-169)

```c
// Capacities
#define HEAP_CAP (1ULL << 32)  // 4GB heap (4 billion terms)
#define BOOK_CAP (1ULL << 24)  // 16M definitions
#define WNF_CAP  (1ULL << 32)  // 4GB evaluation stack

// Types
typedef uint64_t Term;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

// Global state (must be initialized before use)
static Term *HEAP;      // Main heap array
static u64   ALLOC = 1; // Next free heap slot (starts at 1, 0 reserved)
static u32  *BOOK;      // Function definitions: name_id -> heap_location
static Term *STACK;     // Evaluation stack
static u32   S_POS = 1; // Stack position
static u64   ITRS  = 0; // Interaction counter
static char **TABLE;    // Name table: id -> string
```

**Update (2025-12-20+):** Upstream HVM4 now uses per-thread heap slices and WNF
stacks (`HEAP_NEXT/HEAP_END`, `WNF_BANKS`, `WNF_ITRS_BANKS`) instead of a single
global `ALLOC/STACK/ITRS`. See `hvm4/clang/hvm4.c` and `hvm4/clang/wnf/stack_init.c`.

### 2.3 Term Bit Layout (64-bit)

```
| SUB (1) | TAG (7) | EXT (24) | VAL (32) |
  bit 63   bits 56-62  bits 32-55  bits 0-31
```

```c
// Bit positions and masks
#define SUB_SHIFT 63
#define TAG_SHIFT 56
#define EXT_SHIFT 32
#define VAL_SHIFT 0

#define TAG_MASK 0x7F
#define EXT_MASK 0xFFFFFF
#define VAL_MASK 0xFFFFFFFF

// Accessors
fn u8  term_tag(Term t) { return (t >> TAG_SHIFT) & TAG_MASK; }
fn u32 term_ext(Term t) { return (t >> EXT_SHIFT) & EXT_MASK; }
fn u32 term_val(Term t) { return (t >> VAL_SHIFT) & VAL_MASK; }
fn u8  term_sub(Term t) { return (t >> SUB_SHIFT) & 0x1; }  // Substitution flag
```

### 2.4 Tag Constants

```c
#define APP  0   // Application: HEAP[val] = fun, HEAP[val+1] = arg
#define VAR  1   // Variable: val = de Bruijn index
#define LAM  2   // Lambda: ext = depth, HEAP[val] = body
#define CO0  3   // First dup projection
#define CO1  4   // Second dup projection
#define SUP  5   // Superposition: HEAP[val] = left, HEAP[val+1] = right
#define DUP  6   // Duplication: ext = label, HEAP[val] = value, HEAP[val+1] = body
#define REF  8   // Reference to definition: ext = name_id
#define ERA 11   // Erasure (unused value)
#define NUM 30   // Number: val = 32-bit unsigned value
#define OP2 33   // Binary op: ext = operator, HEAP[val] = left, HEAP[val+1] = right

// Constructors: C00-C16 for arities 0-16
#define C00 13   // Constructor arity 0
#define C01 14   // Constructor arity 1
#define C02 15   // Constructor arity 2 (used for BigInt)
// ...
#define C16 29   // Constructor arity 16
```

### 2.5 Complete term_new_* API

```c
// Base constructor
fn Term term_new(u8 sub, u8 tag, u32 ext, u32 val);

// Variables and References
fn Term term_new_var(u32 loc);           // VAR with de Bruijn index in val
fn Term term_new_ref(u32 nam);           // REF to definition
fn Term term_new_nam(u32 nam);           // Stuck variable (for printing)

// Lambda and Application
fn Term term_new_lam(Term bod);          // Allocates 1 slot, stores body
fn Term term_new_lam_at(u32 loc, Term bod);  // Uses pre-allocated location
fn Term term_new_app(Term fun, Term arg);    // Allocates 2 slots
fn Term term_new_app_at(u32 loc, Term fun, Term arg);

// Duplication and Superposition
fn Term term_new_dup(u32 lab, Term val, Term bod);  // Allocates 2 slots
fn Term term_new_dup_at(u32 loc, u32 lab, Term val, Term bod);
fn Term term_new_sup(u32 lab, Term tm0, Term tm1);  // Allocates 2 slots
fn Term term_new_sup_at(u32 loc, u32 lab, Term tm0, Term tm1);
fn Term term_new_co0(u32 lab, u32 loc);  // First projection of dup
fn Term term_new_co1(u32 lab, u32 loc);  // Second projection of dup

// Numbers and Operations
fn Term term_new_num(u32 n);             // NUM with value in val field
fn Term term_new_op2(u32 opr, Term x, Term y);  // Binary operation

// Operators for op2 (ext field)
#define OP_ADD  0   // +
#define OP_SUB  1   // -
#define OP_MUL  2   // *
#define OP_DIV  3   // /
#define OP_MOD  4   // %
#define OP_EQ   5   // ==
#define OP_NE   6   // !=
#define OP_LT   7   // <
#define OP_GT   8   // >
#define OP_LTE  9   // <=
#define OP_GTE 10   // >=

// Pattern Matching
fn Term term_new_swi(u32 num, Term f, Term g);  // Switch on numbers
fn Term term_new_ctr(u32 nam, u32 ari, Term *args);  // Constructor
fn Term term_new_ctr_at(u32 loc, u32 nam, u32 ari, Term *args);

// Boolean operations
fn Term term_new_eql(Term a, Term b);    // Equality test
fn Term term_new_and(Term a, Term b);    // Short-circuit AND
fn Term term_new_or(Term a, Term b);     // Short-circuit OR

// Other
fn Term term_new_era(void);              // Erasure
```

### 2.6 De Bruijn Index Calculation

From `hvm4/clang/parse/bind_lookup.c`:

```c
// Formula: idx = current_depth - 1 - binding_depth
// Example: λx. λy. x  at depth 2
//   x was bound at depth 0
//   idx = 2 - 1 - 0 = 1

// In HVM4:
// - LAM stores current depth in EXT field
// - VAR stores de Bruijn index in VAL field
```

### 2.7 Auto-Dup Algorithm (`hvm4/clang/parse/auto_dup.c`)

When a variable is used N times, need N-1 DUP nodes:

```c
fn Term parse_auto_dup(Term body, u32 idx, u32 uses, u8 tgt, u32 ext) {
  if (uses <= 1) return body;

  u32 n = uses - 1;              // N uses → N-1 dups
  u32 lab = PARSE_FRESH_LAB;
  PARSE_FRESH_LAB += n;

  // Replace variable references in body with CO0/CO1 chain
  // Walk body and replace each (tgt, idx) with:
  //   i < n: CO0(lab+i, adjusted_idx)
  //   i == n: CO1(lab+n-1, adjusted_idx)

  // Build dup chain from outside in:
  // !d0&=x; !d1&=d0₁; ... body
  Term result = body;
  for (int i = n - 1; i >= 0; i--) {
    Term v = (i == 0)
      ? term_new(0, tgt, ext, idx)           // Original var
      : term_new(0, CO1, lab + i - 1, 0);    // Previous dup's second copy
    u64 loc = heap_alloc(2);
    HEAP[loc + 0] = v;
    HEAP[loc + 1] = result;
    result = term_new(0, DUP, lab + i, loc);
  }
  return result;
}
```

**Example**: `λx. (x x)` becomes:
```
λx. !d0&= x; (d0₀ d0₁)

// Memory layout:
// loc=A: [VAR(0), APP at B]     <- DUP node
// loc=B: [CO0(lab,A), CO1(lab,A)]  <- APP node
```

### 2.8 Heap Operations

```c
// Allocation (bump allocator)
fn u64 heap_alloc(u64 size) {
  u64 at = ALLOC;
  ALLOC += size;
  return at;
}

// Substitution (sets SUB bit to mark slot as containing substitution)
fn void heap_subst_var(u32 loc, Term val) {
  HEAP[loc] = term_mark(val);  // Sets bit 63
}

fn Term term_mark(Term t)   { return t | ((u64)1 << 63); }
fn Term term_unmark(Term t) { return t & ~((u64)1 << 63); }
```

### 2.9 Evaluation

```c
// Weak normal form (head normal form)
__attribute__((hot)) fn Term wnf(Term term);

// Strong normal form (work-stealing traversal over heap graph)
fn Term eval_normalize(Term term) {
  // 1) Reduce root to WNF
  // 2) Traverse reachable heap nodes via WSQ
  // 3) Use Uset to avoid revisiting locations
  // 4) If thread_count > 1, workers steal tasks
}
```

### 2.10 Runtime Initialization Pattern

From `hvm4/clang/main.c`:

```c
// 1. Configure threads (default: 1)
thread_set_count(1);
wnf_set_tid(0);

// 2. Allocate memory
BOOK  = calloc(BOOK_CAP, sizeof(u32));
HEAP  = calloc(HEAP_CAP, sizeof(Term));
TABLE = calloc(BOOK_CAP, sizeof(char*));
heap_init_slices();

// 3. Evaluate
Term result = eval_normalize(term);
```

---

# Part 2: Semantic Differences

## Critical Differences

| Aspect | Nix | HVM4 | Impact |
|--------|-----|------|--------|
| **Variable usage** | Unlimited | Affine (single use) | Must auto-insert DUP nodes |
| **Integers** | 64-bit signed | 32-bit unsigned | Need BigInt encoding |
| **Floats** | Double precision | Not supported | Cannot support floats |
| **Strings** | Native + context | List of chars | Complex encoding needed |
| **Attribute sets** | Central type | Not native | Must encode as constructors |
| **Laziness** | Explicit thunks | Optimal built-in | Good match |
| **I/O** | import, derivation | Pure computation | Must handle at boundary |
| **GC** | Boehm GC | No GC | Potential memory issues |
| **Addition** | ExprConcatStrings | OP2 with OP_ADD | Special handling needed |

## Detailed Analysis

### Variable Usage (Critical)

Nix allows:
```nix
let x = expensive; in x + x + x
```

HVM4 requires explicit duplication. The compiler must:
1. **Two-pass compilation**: First pass counts variable uses, second pass emits code
2. Insert DUP nodes for multi-use variables using auto-dup pattern

```hvm4
// Compiled form for 3 uses of x:
!d0 &0= expensive;
!d1 &1= d0₁;
(d0₀ + d1₀ + d1₁)
```

### Integer Representation (Critical)

Nix uses 64-bit signed integers (`checked::Checked<int64_t>`).
HVM4 only has 32-bit unsigned.

**Solution**: BigInt encoding as constructors:
```hvm4
#Pos{lo, hi}  // Positive: value = (hi << 32) | lo
#Neg{lo, hi}  // Negative: value = -((hi << 32) | lo)
#Sml{n}       // Small optimization: fits in 31 bits signed
```

For values fitting in 31 bits (signed), use native `NUM` for performance.

### Addition Operator (Important!)

In Nix, `+` is NOT a binary operator expression!
It's handled by `ExprConcatStrings` with `forceString=false`.

```cpp
// eval.cc:2060-2120 logic:
if (firstType == nInt) {
    // All elements must be numbers, do addition
    NixInt n = 0;
    for (auto & elem : *es) {
        n = n + state.forceInt(*elem.second, ...);
    }
    v.mkInt(n);
}
```

**Compiler must detect**: `ExprConcatStrings` with numeric operands → `term_new_op2(OP_ADD, ...)`

---

# Part 3: Implementation Plan

## Scope: Minimal Prototype

**Supported:**
- Integer literals (with BigInt encoding)
- Lambdas and application
- Let bindings (non-recursive initially)
- If-then-else
- Arithmetic: `+` (via ExprConcatStrings detection)
- Comparisons: `==`, `!=`
- Boolean: `&&`, `||`, `!`

**Not Supported:**
- Attribute sets
- Lists
- Strings
- Paths
- Recursive let/rec
- With expressions
- Imports
- `-`, `*`, `/` (these are primops, need primop compilation)

## File Structure

```
src/libexpr/hvm4/
├── hvm4-backend.hh        # Backend interface
├── hvm4-backend.cc        # Backend implementation
├── hvm4-compiler.hh       # AST -> HVM4 compiler
├── hvm4-compiler.cc       # Compiler implementation
├── hvm4-runtime.hh        # HVM4 runtime wrapper
├── hvm4-runtime.cc        # Runtime (includes hvm4.c)
├── hvm4-bigint.hh         # BigInt encoding
├── hvm4-bigint.cc         # BigInt implementation
├── hvm4-result.hh         # Result extraction
└── hvm4-result.cc         # Result implementation
```

## Implementation Steps

### Step 1: Runtime Wrapper (`hvm4-runtime.hh/cc`)

```cpp
// hvm4-runtime.hh
#pragma once
#include <cstdint>
#include <cstddef>

namespace nix::hvm4 {

// Forward declare HVM4 types
using Term = uint64_t;

class HVM4Runtime {
public:
    HVM4Runtime();
    ~HVM4Runtime();

    void initialize(size_t heapSize = 1ULL << 28);  // 256MB default
    void reset();  // Reset heap for new evaluation

    Term evaluateWNF(Term term);
    Term evaluateSNF(Term term);

    uint64_t getInteractionCount() const;
    uint64_t getAllocatedBytes() const;

    // Expose heap for term construction
    Term* getHeap() { return heap; }
    uint64_t allocate(uint64_t size);

private:
    Term* heap = nullptr;
    uint32_t* book = nullptr;
    Term* stack = nullptr;
    char** table = nullptr;
    bool initialized = false;
};

}  // namespace nix::hvm4
```

```cpp
// hvm4-runtime.cc
#include "hvm4-runtime.hh"

// Include HVM4 as C code
extern "C" {
// Undefine any conflicting macros
#undef HEAP
#undef BOOK
#undef STACK
#undef TABLE
#undef ALLOC

// Include HVM4 single-file runtime
#include "hvm4/clang/hvm4.c"
}

namespace nix::hvm4 {

HVM4Runtime::HVM4Runtime() {}

HVM4Runtime::~HVM4Runtime() {
    if (heap) free(heap);
    if (book) free(book);
    if (stack) free(stack);
    if (table) free(table);
}

void HVM4Runtime::initialize(size_t heapSize) {
    if (initialized) return;

    // Allocate HVM4 globals
    heap = (Term*)calloc(HEAP_CAP, sizeof(Term));
    book = (uint32_t*)calloc(BOOK_CAP, sizeof(uint32_t));
    stack = (Term*)calloc(WNF_CAP, sizeof(Term));
    table = (char**)calloc(BOOK_CAP, sizeof(char*));

    if (!heap || !book || !stack || !table) {
        throw std::bad_alloc();
    }

    // Set HVM4 globals
    HEAP = heap;
    BOOK = book;
    STACK = stack;
    TABLE = table;

    // Initialize counters
    ALLOC = 1;  // Slot 0 reserved
    S_POS = 1;
    ITRS = 0;

    initialized = true;
}

void HVM4Runtime::reset() {
    if (!initialized) return;
    ALLOC = 1;
    S_POS = 1;
    ITRS = 0;
    memset(heap, 0, HEAP_CAP * sizeof(Term));
}

Term HVM4Runtime::evaluateWNF(Term term) {
    return wnf(term);
}

Term HVM4Runtime::evaluateSNF(Term term) {
    return snf(term, 0);
}

uint64_t HVM4Runtime::getInteractionCount() const {
    return ITRS;
}

uint64_t HVM4Runtime::allocate(uint64_t size) {
    return heap_alloc(size);
}

}  // namespace nix::hvm4
```

### Step 2: BigInt Encoding (`hvm4-bigint.hh/cc`)

```cpp
// hvm4-bigint.hh
#pragma once
#include <cstdint>
#include <optional>

namespace nix::hvm4 {

using Term = uint64_t;

// Constructor tags (encoded as 24-bit values)
// Using simple numeric IDs since we control the encoding
constexpr uint32_t BIGINT_POS = 1;  // #Pos{lo, hi}
constexpr uint32_t BIGINT_NEG = 2;  // #Neg{lo, hi}

// Check if value fits in signed 31-bit (safe for HVM4 NUM)
inline bool fitsInSmallInt(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

// Encode 64-bit signed integer as HVM4 term
Term encodeInt64(int64_t value, uint64_t (*alloc)(uint64_t));

// Decode HVM4 term to 64-bit signed integer
std::optional<int64_t> decodeInt64(Term term, Term* heap);

// Check if term is a BigInt constructor
bool isBigInt(Term term);

}  // namespace nix::hvm4
```

```cpp
// hvm4-bigint.cc
#include "hvm4-bigint.hh"

extern "C" {
#include "hvm4/clang/hvm4.c"  // For term_new_*, term_tag, etc.
}

namespace nix::hvm4 {

Term encodeInt64(int64_t value, uint64_t (*alloc)(uint64_t)) {
    // Small integer optimization
    if (fitsInSmallInt(value)) {
        // Use native NUM for small values
        // Note: NUM is unsigned 32-bit, we store signed as bit pattern
        return term_new_num(static_cast<uint32_t>(static_cast<int32_t>(value)));
    }

    // BigInt encoding for large values
    uint64_t absVal = (value < 0) ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);
    uint32_t lo = static_cast<uint32_t>(absVal & 0xFFFFFFFF);
    uint32_t hi = static_cast<uint32_t>((absVal >> 32) & 0xFFFFFFFF);

    uint32_t tag = (value >= 0) ? BIGINT_POS : BIGINT_NEG;

    // Allocate constructor with 2 fields
    Term args[2] = { term_new_num(lo), term_new_num(hi) };
    return term_new_ctr(tag, 2, args);
}

std::optional<int64_t> decodeInt64(Term term, Term* heap) {
    uint8_t tag = term_tag(term);

    if (tag == NUM) {
        // Small integer - interpret as signed
        uint32_t bits = term_val(term);
        return static_cast<int64_t>(static_cast<int32_t>(bits));
    }

    if (tag == C02) {  // Constructor with arity 2
        uint32_t name = term_ext(term);
        uint32_t loc = term_val(term);

        Term loTerm = heap[loc];
        Term hiTerm = heap[loc + 1];

        if (term_tag(loTerm) != NUM || term_tag(hiTerm) != NUM) {
            return std::nullopt;  // Invalid BigInt
        }

        uint32_t lo = term_val(loTerm);
        uint32_t hi = term_val(hiTerm);
        uint64_t absVal = (static_cast<uint64_t>(hi) << 32) | lo;

        if (name == BIGINT_POS) {
            return static_cast<int64_t>(absVal);
        } else if (name == BIGINT_NEG) {
            return -static_cast<int64_t>(absVal);
        }
    }

    return std::nullopt;
}

bool isBigInt(Term term) {
    uint8_t tag = term_tag(term);
    if (tag == NUM) return true;
    if (tag == C02) {
        uint32_t name = term_ext(term);
        return name == BIGINT_POS || name == BIGINT_NEG;
    }
    return false;
}

}  // namespace nix::hvm4
```

### Step 3: Compiler (`hvm4-compiler.hh/cc`)

```cpp
// hvm4-compiler.hh
#pragma once
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "hvm4-runtime.hh"
#include <vector>
#include <optional>

namespace nix::hvm4 {

// Variable usage tracking for auto-dup
struct VarBinding {
    Symbol name;
    uint32_t depth;         // Binding depth
    uint32_t useCount = 0;  // Times referenced
    uint32_t dupLabel = 0;  // Label for DUP chain (assigned if useCount > 1)
};

// Compilation context
class CompileContext {
public:
    explicit CompileContext(HVM4Runtime& runtime, const SymbolTable& symbols);

    // Binding management
    void pushBinding(Symbol name);
    void popBinding();
    std::optional<VarBinding*> lookup(Symbol name);

    // Depth tracking
    uint32_t currentDepth() const { return depth; }

    // Fresh label generation
    uint32_t freshLabel();

    // Memory allocation
    uint64_t allocate(uint64_t size);
    Term* heap() { return runtime.getHeap(); }

private:
    HVM4Runtime& runtime;
    const SymbolTable& symbols;
    std::vector<VarBinding> bindings;
    uint32_t depth = 0;
    uint32_t labelCounter = 0;
};

// The compiler
class HVM4Compiler {
public:
    HVM4Compiler(HVM4Runtime& runtime, const SymbolTable& symbols);

    // Main entry point - compiles expression to HVM4 term
    // Uses two-pass: first counts usages, second emits code with dups
    Term compile(const Expr& expr);

    // Check if expression can be compiled
    bool canCompile(const Expr& expr) const;

private:
    HVM4Runtime& runtime;
    const SymbolTable& symbols;

    // First pass: count variable usages
    void countUsages(const Expr& expr, CompileContext& ctx);

    // Second pass: emit code with auto-dup
    Term emit(const Expr& expr, CompileContext& ctx);

    // Expression-specific emitters
    Term emitInt(const ExprInt& e, CompileContext& ctx);
    Term emitVar(const ExprVar& e, CompileContext& ctx);
    Term emitLambda(const ExprLambda& e, CompileContext& ctx);
    Term emitCall(const ExprCall& e, CompileContext& ctx);
    Term emitIf(const ExprIf& e, CompileContext& ctx);
    Term emitLet(const ExprLet& e, CompileContext& ctx);
    Term emitOpNot(const ExprOpNot& e, CompileContext& ctx);
    Term emitOpAnd(const ExprOpAnd& e, CompileContext& ctx);
    Term emitOpOr(const ExprOpOr& e, CompileContext& ctx);
    Term emitOpEq(const ExprOpEq& e, CompileContext& ctx);
    Term emitOpNEq(const ExprOpNEq& e, CompileContext& ctx);
    Term emitConcatStrings(const ExprConcatStrings& e, CompileContext& ctx);

    // Auto-dup insertion
    Term wrapWithDups(Term body, const std::vector<VarBinding>& bindings, CompileContext& ctx);
};

}  // namespace nix::hvm4
```

```cpp
// hvm4-compiler.cc (key parts)
#include "hvm4-compiler.hh"
#include "hvm4-bigint.hh"

extern "C" {
#include "hvm4/clang/hvm4.c"
}

namespace nix::hvm4 {

// ... CompileContext implementation ...

Term HVM4Compiler::compile(const Expr& expr) {
    CompileContext ctx(runtime, symbols);

    // First pass: count variable usages
    countUsages(expr, ctx);

    // Reset context for second pass
    ctx = CompileContext(runtime, symbols);

    // Second pass: emit code
    return emit(expr, ctx);
}

void HVM4Compiler::countUsages(const Expr& expr, CompileContext& ctx) {
    if (auto* e = dynamic_cast<const ExprInt*>(&expr)) {
        // No variables
    }
    else if (auto* e = dynamic_cast<const ExprVar*>(&expr)) {
        auto binding = ctx.lookup(e->name);
        if (binding) {
            (*binding)->useCount++;
        }
    }
    else if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
        ctx.pushBinding(e->arg);
        countUsages(*e->body, ctx);
        ctx.popBinding();
    }
    else if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
        countUsages(*e->fun, ctx);
        for (auto* arg : *e->args) {
            countUsages(*arg, ctx);
        }
    }
    else if (auto* e = dynamic_cast<const ExprIf*>(&expr)) {
        countUsages(*e->cond, ctx);
        countUsages(*e->then, ctx);
        countUsages(*e->else_, ctx);
    }
    else if (auto* e = dynamic_cast<const ExprLet*>(&expr)) {
        // Count usages in binding expressions
        for (auto& [name, def] : *e->attrs->attrs) {
            countUsages(*def.e, ctx);
        }
        // Push bindings for body
        for (auto& [name, def] : *e->attrs->attrs) {
            ctx.pushBinding(name);
        }
        countUsages(*e->body, ctx);
        for (size_t i = 0; i < e->attrs->attrs->size(); i++) {
            ctx.popBinding();
        }
    }
    // ... other expression types ...
}

Term HVM4Compiler::emit(const Expr& expr, CompileContext& ctx) {
    if (auto* e = dynamic_cast<const ExprInt*>(&expr)) {
        return emitInt(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprVar*>(&expr)) {
        return emitVar(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
        return emitLambda(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
        return emitCall(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprIf*>(&expr)) {
        return emitIf(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprLet*>(&expr)) {
        return emitLet(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprOpNot*>(&expr)) {
        return emitOpNot(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprOpAnd*>(&expr)) {
        return emitOpAnd(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprOpOr*>(&expr)) {
        return emitOpOr(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprOpEq*>(&expr)) {
        return emitOpEq(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprOpNEq*>(&expr)) {
        return emitOpNEq(*e, ctx);
    }
    if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
        return emitConcatStrings(*e, ctx);
    }

    throw Error("Unsupported expression type for HVM4 backend");
}

Term HVM4Compiler::emitInt(const ExprInt& e, CompileContext& ctx) {
    int64_t value = e.v.integer().value;
    return encodeInt64(value, [&ctx](uint64_t size) { return ctx.allocate(size); });
}

Term HVM4Compiler::emitVar(const ExprVar& e, CompileContext& ctx) {
    auto binding = ctx.lookup(e.name);
    if (!binding) {
        throw Error("Undefined variable in HVM4 compilation: %s", symbols[e.name]);
    }

    // Calculate de Bruijn index
    uint32_t idx = ctx.currentDepth() - 1 - (*binding)->depth;

    // If multi-use, this will be replaced by CO0/CO1 during dup insertion
    return term_new_var(idx);
}

Term HVM4Compiler::emitLambda(const ExprLambda& e, CompileContext& ctx) {
    // For now, only support simple lambdas (x: body), not ({...}: body)
    if (e.getFormals()) {
        throw Error("Pattern-matching lambdas not yet supported by HVM4 backend");
    }

    ctx.pushBinding(e.arg);
    Term body = emit(*e.body, ctx);

    // Insert dups if argument is used multiple times
    auto& argBinding = ctx.bindings.back();
    if (argBinding.useCount > 1) {
        body = wrapWithDups(body, {argBinding}, ctx);
    }

    ctx.popBinding();

    return term_new_lam(body);
}

Term HVM4Compiler::emitCall(const ExprCall& e, CompileContext& ctx) {
    Term fun = emit(*e.fun, ctx);

    for (auto* arg : *e.args) {
        Term argTerm = emit(*arg, ctx);
        fun = term_new_app(fun, argTerm);
    }

    return fun;
}

Term HVM4Compiler::emitIf(const ExprIf& e, CompileContext& ctx) {
    Term cond = emit(*e.cond, ctx);
    Term thenBranch = emit(*e.then, ctx);
    Term elseBranch = emit(*e.else_, ctx);

    // HVM4 switch: ~cond { 0: else; _: then }
    // false=0, true=non-zero
    return term_new_swi(0, elseBranch, term_new_lam(thenBranch));
    // Then apply to condition
    // Actually need: (switch then else) cond
    // Hmm, need to check HVM4 switch semantics...
}

Term HVM4Compiler::emitConcatStrings(const ExprConcatStrings& e, CompileContext& ctx) {
    // Check if this is addition (forceString=false, all numeric)
    if (!e.forceString && e.es->size() == 2) {
        // Assume it's addition for now (proper type checking would need evaluation)
        Term left = emit(*(*e.es)[0].second, ctx);
        Term right = emit(*(*e.es)[1].second, ctx);
        return term_new_op2(OP_ADD, left, right);
    }

    throw Error("String concatenation not yet supported by HVM4 backend");
}

Term HVM4Compiler::wrapWithDups(Term body, const std::vector<VarBinding>& bindings,
                                 CompileContext& ctx) {
    for (const auto& binding : bindings) {
        if (binding.useCount <= 1) continue;

        uint32_t n = binding.useCount - 1;  // N uses → N-1 dups
        uint32_t baseLab = ctx.freshLabel();

        // Build dup chain: !d0&=x; !d1&=d0₁; ... body
        Term result = body;
        uint32_t idx = ctx.currentDepth() - 1 - binding.depth;

        for (int i = n - 1; i >= 0; i--) {
            Term v = (i == 0)
                ? term_new_var(idx)
                : term_new_co1(baseLab + i - 1, 0);  // Previous dup's CO1

            result = term_new_dup(baseLab + i, v, result);
        }

        body = result;
    }
    return body;
}

}  // namespace nix::hvm4
```

### Step 4: Result Extraction (`hvm4-result.hh/cc`)

```cpp
// hvm4-result.hh
#pragma once
#include "nix/expr/value.hh"
#include "hvm4-runtime.hh"

namespace nix::hvm4 {

class ResultExtractor {
public:
    ResultExtractor(EvalState& state, HVM4Runtime& runtime);

    // Extract HVM4 term to Nix Value
    void extract(Term term, Value& result);

private:
    EvalState& state;
    HVM4Runtime& runtime;

    void extractNum(Term term, Value& result);
    void extractBigInt(Term term, Value& result);
};

}  // namespace nix::hvm4
```

```cpp
// hvm4-result.cc
#include "hvm4-result.hh"
#include "hvm4-bigint.hh"

namespace nix::hvm4 {

ResultExtractor::ResultExtractor(EvalState& state, HVM4Runtime& runtime)
    : state(state), runtime(runtime) {}

void ResultExtractor::extract(Term term, Value& result) {
    // First evaluate to normal form
    term = runtime.evaluateSNF(term);

    uint8_t tag = term_tag(term);

    switch (tag) {
        case NUM:
            extractNum(term, result);
            break;

        case C02:  // Constructor arity 2 (BigInt)
            if (isBigInt(term)) {
                extractBigInt(term, result);
            } else {
                throw Error("Unknown constructor in HVM4 result");
            }
            break;

        case LAM:
            throw Error("Cannot extract lambda value from HVM4 - functions must be fully applied");

        case ERA:
            result.mkNull();
            break;

        default:
            throw Error("Unsupported HVM4 result term type: tag=%d", tag);
    }
}

void ResultExtractor::extractNum(Term term, Value& result) {
    // Interpret as signed 32-bit
    uint32_t bits = term_val(term);
    int32_t signedVal = static_cast<int32_t>(bits);
    result.mkInt(signedVal);
}

void ResultExtractor::extractBigInt(Term term, Value& result) {
    auto decoded = decodeInt64(term, runtime.getHeap());
    if (!decoded) {
        throw Error("Invalid BigInt encoding in HVM4 result");
    }
    result.mkInt(*decoded);
}

}  // namespace nix::hvm4
```

### Step 5: Backend Integration (`hvm4-backend.hh/cc`)

```cpp
// hvm4-backend.hh
#pragma once
#include "nix/expr/eval.hh"
#include "hvm4-runtime.hh"
#include "hvm4-compiler.hh"
#include "hvm4-result.hh"

namespace nix::hvm4 {

class HVM4Backend {
public:
    explicit HVM4Backend(EvalState& state);

    // Check if expression can be evaluated by HVM4
    bool canEvaluate(const Expr& expr) const;

    // Try to evaluate expression using HVM4
    // Returns true if successful, false to fall back to standard evaluator
    bool tryEvaluate(Expr* expr, Env& env, Value& result);

private:
    EvalState& state;
    HVM4Runtime runtime;
    std::unique_ptr<HVM4Compiler> compiler;
    std::unique_ptr<ResultExtractor> extractor;

    bool initialized = false;
    void ensureInitialized();
};

}  // namespace nix::hvm4
```

Integration point in `eval-inline.hh`:

```cpp
// In EvalState::forceValue():

#ifdef NIX_USE_HVM4
if (evalSettings.useHvm4Backend && hvm4Backend) {
    if (hvm4Backend->canEvaluate(*expr)) {
        if (hvm4Backend->tryEvaluate(expr, *env, v)) {
            return;  // Successfully evaluated with HVM4
        }
        // Fall through to standard evaluation
    }
}
#endif
```

### Step 6: Build System

**`src/libexpr/meson.options`** (create or add to existing):
```meson
option(
  'hvm4-backend',
  type : 'feature',
  value : 'disabled',
  description : 'Enable experimental HVM4 evaluator backend',
)
```

**`src/libexpr/meson.build`** additions:
```meson
# Near the top, after deps setup
hvm4_enabled = get_option('hvm4-backend').enabled()

# Conditional source compilation
if hvm4_enabled
  # HVM4 is C code
  add_languages('c', native: false)

  hvm4_sources = files(
    'hvm4/hvm4-backend.cc',
    'hvm4/hvm4-compiler.cc',
    'hvm4/hvm4-runtime.cc',
    'hvm4/hvm4-bigint.cc',
    'hvm4/hvm4-result.cc',
  )
  sources += hvm4_sources

  # Include path for hvm4.c
  hvm4_inc = include_directories('../../hvm4/clang')
  include_dirs += hvm4_inc
endif

# Set config define
configdata_pub.set('NIX_USE_HVM4', hvm4_enabled.to_int())
```

### Step 7: Testing

**Unit tests** in `src/libexpr-tests/hvm4/`:

```cpp
// test-hvm4-bigint.cc
#include <gtest/gtest.h>
#include "hvm4/hvm4-bigint.hh"

namespace nix::hvm4 {

TEST(HVM4BigInt, SmallPositive) {
    // Test small positive integers use native NUM
    EXPECT_TRUE(fitsInSmallInt(0));
    EXPECT_TRUE(fitsInSmallInt(42));
    EXPECT_TRUE(fitsInSmallInt(INT32_MAX));
}

TEST(HVM4BigInt, SmallNegative) {
    EXPECT_TRUE(fitsInSmallInt(-1));
    EXPECT_TRUE(fitsInSmallInt(INT32_MIN));
}

TEST(HVM4BigInt, LargeValues) {
    EXPECT_FALSE(fitsInSmallInt(INT64_MAX));
    EXPECT_FALSE(fitsInSmallInt(INT64_MIN));
    EXPECT_FALSE(fitsInSmallInt(static_cast<int64_t>(INT32_MAX) + 1));
}

// ... roundtrip tests ...

}  // namespace nix::hvm4
```

```cpp
// test-hvm4-compiler.cc
#include <gtest/gtest.h>
#include "nix/expr/tests/libexpr.hh"
#include "hvm4/hvm4-backend.hh"

namespace nix::hvm4 {

class HVM4CompilerTest : public nix::LibExprTest {};

TEST_F(HVM4CompilerTest, CompileInteger) {
    auto v = eval("42", true);
    ASSERT_EQ(v.type(), nInt);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4CompilerTest, CompileAddition) {
    auto v = eval("1 + 2", true);
    ASSERT_EQ(v.type(), nInt);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4CompilerTest, CompileLambdaApplication) {
    auto v = eval("(x: x + 1) 5", true);
    ASSERT_EQ(v.type(), nInt);
    ASSERT_EQ(v.integer().value, 6);
}

TEST_F(HVM4CompilerTest, CompileMultiUseVariable) {
    auto v = eval("let x = 5; in x + x", true);
    ASSERT_EQ(v.type(), nInt);
    ASSERT_EQ(v.integer().value, 10);
}

}  // namespace nix::hvm4
```

---

## Critical Files Reference

**Nix:**
- `src/libexpr/include/nix/expr/nixexpr.hh` - Expr type hierarchy (exact structures documented above)
- `src/libexpr/include/nix/expr/value.hh` - Value representation, NixInt = Checked<int64_t>
- `src/libexpr/include/nix/expr/eval-inline.hh` - forceValue() integration point
- `src/libexpr/include/nix/expr/symbol-table.hh` - Symbol type (uint32_t ID)
- `src/libexpr/eval.cc` - ExprConcatStrings::eval handles addition (lines 2060-2120)
- `src/libexpr/meson.build` - Build configuration

**HVM4:**
- `hvm4/clang/hvm4.c` - Complete single-file runtime (includes all .c files)
- `hvm4/clang/term/new/*.c` - All term_new_* functions
- `hvm4/clang/parse/auto_dup.c` - Auto-duplication algorithm
- `hvm4/clang/wnf/_.c` - WNF evaluator
- `hvm4/clang/eval/normalize.c` - SNF evaluator (parallel work-stealing)
- `hvm4/clang/cnf/_.c` - CNF readback step (collapser)
- `hvm4/clang/eval/collapse.c` - CNF readback traversal
- `hvm4/clang/data/wsq.c` - Work-stealing deque (normalize)
- `hvm4/clang/data/wspq.c` - Keyed work-stealing queue (collapse)
- `hvm4/clang/data/uset.c` - Visited set for normalize
- `hvm4/README.md` - Language reference

**Test Infrastructure:**
- `src/libexpr-tests/meson.build` - Test build configuration
- `src/libexpr-test-support/include/nix/expr/tests/libexpr.hh` - LibExprTest base class
- Framework: gtest + gmock + RapidCheck

---

---

# Part 4: Implementation Progress

## Current Status (2025-12-28)

This section reflects the current tree (see `src/libexpr/hvm4/STATUS.md`).

### Core Infrastructure
- Runtime wrapper, compiler, result extractor, and backend integration: **DONE**
- Build system integration (meson feature flag + headers): **DONE**
- HVM4 runtime integration now relies on upstream `eval_normalize` / `eval_collapse` (see Part 2 updates)

### Data Encodings (DONE)
- Integers (32-bit NUM) + 64-bit BigInt constructors
- Floats (encoding/decoding only)
- Strings (`#Str`, `#SCat`, `#SNum`)
- Lists (`#Lst{len, spine}`)
- Attrsets (`#Ats{spine}` + `#Atr{key,val}`)
- Paths (`#Pth{accessor_id, path_string_id}`)

### Implemented Language Features
- **Literals**: int, float, string, path, null, booleans
- **Arithmetic**: `+ - * /` (integer only), BigInt comparisons via MAT
- **Comparison**: `< <= > >= == !=` (integer / structural; float falls back)
- **Boolean**: `&& || !` and implication
- **Control**: if-then-else, assert
- **Bindings**: non-recursive let, acyclic `rec`, basic `with` (nested outer lookup still limited)
- **Functions**: lambdas, pattern lambdas, defaults, ellipsis, @-patterns, closures, currying
- **Lists**: literals, `++` (currently literal-literal only)
- **Strings**: literals, constant concat, basic interpolation (non-`toString`)
- **Attrs**: literals, select, `?`, `//`, inherit, inherit-from, acyclic `rec`

### Tests
- HVM4 test suite is extensive under `src/libexpr-tests/hvm4/`.
- Test counts change; see `src/libexpr/hvm4/STATUS.md` for current notes.

## Things to Go Back and Fix / Change

### Semantics / Correctness
- Preserve laziness when returning lists/attrs to Nix (avoid forcing all elements/values at extraction).
- Replace ERA-as-null with explicit `#Err{}` propagation and proper `EvalError` conversion.
- Implement full nested `with` chain lookup (outer scope fallback + correct DUP counting).
- Add cyclic `rec` support (SCC/Y-combinator) and allow `inherit-from` to see rec bindings.
- Support dynamic attributes and dynamic attribute paths.

### Feature Gaps
- Builtins: `head`, `tail`, `length`, `map`, `filter`, `foldl'`, `attrNames`, etc.
- Imports: static import resolution + memoization (reject/route dynamic imports to fallback).
- Derivations: pure record construction + post-eval handling.
- String context and path-to-string coercion.
- Float arithmetic/comparisons.
- BigInt arithmetic beyond 32-bit OP2.

### Runtime / Interop
- Validate wrapper behavior against recent upstream HVM4 changes (per-thread heaps, eval_normalize).
- Decide on thread-count configuration and deterministic output strategy.

### Performance / Limits
- Division by zero handling.
- `++` support for non-literal list operands.
  
## References

- `src/libexpr/hvm4/STATUS.md` - source of truth for current implementation status
- `src/libexpr/hvm4/` - HVM4 backend implementation (compiler/runtime/extractor)
- `src/libexpr-tests/hvm4/` - HVM4 test suite
- `hvm4/clang/` - vendored HVM4 runtime (eval_normalize, eval_collapse, wnf)
