/**
 * HVM4 Runtime Implementation
 *
 * This file provides the HVM4 (Higher Virtual Machine 4) runtime for Nix.
 * It wraps the vendored HVM4 C implementation from hvm4/clang/ with a
 * C++ interface suitable for integration with the Nix evaluator.
 *
 * Architecture:
 * 1. VENDORED C RUNTIME - Included from hvm4/clang/hvm4.c
 *    - Term representation and constructors
 *    - Interaction rule implementations
 *    - WNF (Weak Normal Form) evaluator
 *    - SNF (Strong Normal Form) evaluator
 *
 * 2. C++ WRAPPER CLASS - HVM4Runtime
 *    - Memory management (heap, stack, book)
 *    - Global state synchronization via setGlobals()
 *    - High-level term construction API
 *
 * The vendored C runtime uses static global variables (HEAP, BOOK, STACK, etc.)
 * for performance. The C++ wrapper manages these through setGlobals() calls
 * before each evaluation.
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"

#include <cstdlib>
#include <cstring>

// ============================================================================
// PART 1: VENDORED HVM4 C RUNTIME
// ============================================================================
//
// Include the vendored HVM4 implementation from hvm4/clang/.
// We disable the parser and printer since we don't need them for Nix integration.
// ============================================================================

extern "C" {

// Disable parser, printer, and collapse - we only need the evaluator
// Collapse uses 'template' as parameter name which is a C++ keyword
#define HVM4_NO_PARSER 1
#define HVM4_NO_PRINT 1
#define HVM4_NO_COLLAPSE 1

// Include the vendored HVM4 runtime
// Path is relative to src/libexpr/hvm4/
#include "../../../hvm4/clang/hvm4.c"

}  // extern "C"


// ============================================================================
// PART 2: C++ WRAPPER CLASS
// ============================================================================
//
// The HVM4Runtime class provides a C++ interface to the vendored C runtime.
// It manages memory allocation, global state, and provides type-safe term
// construction methods.
// ============================================================================

namespace nix::hvm4 {

// ---------------------------------------------------------------------------
// 2.1 Constructor, Destructor, and Memory Management
// ---------------------------------------------------------------------------

HVM4Runtime::HVM4Runtime(size_t heapSize, size_t stackSize)
    : heapCapacity_(heapSize), stackCapacity_(stackSize)
{
    // Allocate memory
    heap_ = static_cast<Term*>(calloc(heapCapacity_, sizeof(Term)));
    book_ = static_cast<uint32_t*>(calloc(BOOK_CAP, sizeof(uint32_t)));
    stack_ = static_cast<Term*>(calloc(stackCapacity_, sizeof(Term)));

    if (!heap_ || !book_ || !stack_) {
        cleanup();
        throw HVM4Error("Failed to allocate HVM4 runtime memory");
    }

    // Initialize counters
    allocPos_ = 1;  // Slot 0 is reserved
    stackPos_ = 1;
    interactions_ = 0;

    initialized_ = true;
}

// Destructor
HVM4Runtime::~HVM4Runtime() {
    cleanup();
}

// Move constructor
HVM4Runtime::HVM4Runtime(HVM4Runtime&& other) noexcept
    : heap_(other.heap_)
    , book_(other.book_)
    , stack_(other.stack_)
    , heapCapacity_(other.heapCapacity_)
    , stackCapacity_(other.stackCapacity_)
    , allocPos_(other.allocPos_)
    , stackPos_(other.stackPos_)
    , interactions_(other.interactions_)
    , initialized_(other.initialized_)
{
    other.heap_ = nullptr;
    other.book_ = nullptr;
    other.stack_ = nullptr;
    other.initialized_ = false;
}

// Move assignment
HVM4Runtime& HVM4Runtime::operator=(HVM4Runtime&& other) noexcept {
    if (this != &other) {
        cleanup();

        heap_ = other.heap_;
        book_ = other.book_;
        stack_ = other.stack_;
        heapCapacity_ = other.heapCapacity_;
        stackCapacity_ = other.stackCapacity_;
        allocPos_ = other.allocPos_;
        stackPos_ = other.stackPos_;
        interactions_ = other.interactions_;
        initialized_ = other.initialized_;

        other.heap_ = nullptr;
        other.book_ = nullptr;
        other.stack_ = nullptr;
        other.initialized_ = false;
    }
    return *this;
}

void HVM4Runtime::cleanup() {
    if (heap_) { free(heap_); heap_ = nullptr; }
    if (book_) { free(book_); book_ = nullptr; }
    if (stack_) { free(stack_); stack_ = nullptr; }
    initialized_ = false;
}

void HVM4Runtime::setGlobals() {
    // Set the global variables used by the vendored HVM4 C code
    HEAP = heap_;
    BOOK = book_;
    STACK = stack_;
    ALLOC = allocPos_;
    S_POS = stackPos_;
    ITRS = interactions_;
    DEBUG = 0;  // Disable debug output
}

void HVM4Runtime::reset() {
    if (!initialized_) return;

    // Clear heap
    memset(heap_, 0, heapCapacity_ * sizeof(Term));

    // Reset counters
    allocPos_ = 1;
    stackPos_ = 1;
    interactions_ = 0;
}

// ---------------------------------------------------------------------------
// 2.2 Evaluation Methods
// ---------------------------------------------------------------------------

Term HVM4Runtime::evaluateWNF(Term term) {
    if (!initialized_) {
        throw HVM4Error("HVM4 runtime not initialized");
    }

    setGlobals();
    Term result = wnf(term);  // Use vendored wnf() function

    // Save updated state
    allocPos_ = ALLOC;
    stackPos_ = S_POS;
    interactions_ = ITRS;

    return result;
}

Term HVM4Runtime::evaluateSNF(Term term) {
    if (!initialized_) {
        throw HVM4Error("HVM4 runtime not initialized");
    }

    setGlobals();
    Term result = snf(term, 0);  // Use vendored snf() function

    // Save updated state
    allocPos_ = ALLOC;
    stackPos_ = S_POS;
    interactions_ = ITRS;

    return result;
}

uint64_t HVM4Runtime::getInteractionCount() const {
    return interactions_;
}

uint64_t HVM4Runtime::getAllocatedBytes() const {
    return allocPos_ * sizeof(Term);
}

uint64_t HVM4Runtime::allocate(uint64_t size) {
    if (allocPos_ + size > heapCapacity_) {
        throw HVM4Error("HVM4 heap overflow");
    }
    uint64_t loc = allocPos_;
    allocPos_ += size;
    return loc;
}

void HVM4Runtime::store(uint64_t loc, Term term) {
    if (loc >= heapCapacity_) {
        throw HVM4Error("HVM4 heap access out of bounds");
    }
    heap_[loc] = term;
}

Term HVM4Runtime::load(uint64_t loc) const {
    if (loc >= heapCapacity_) {
        throw HVM4Error("HVM4 heap access out of bounds");
    }
    return heap_[loc];
}

// ---------------------------------------------------------------------------
// 2.3 Term Construction API
// ---------------------------------------------------------------------------
//
// These methods provide a type-safe C++ interface for constructing HVM4 terms.
// They use the vendored term_new_* functions where appropriate.
// ---------------------------------------------------------------------------

Term HVM4Runtime::termNew(uint8_t sub, uint8_t tag, uint32_t ext, uint32_t val) {
    return term_new(sub, tag, ext, val);  // Use vendored term_new
}

Term HVM4Runtime::termNewVar(uint32_t idx) {
    return term_new_var(idx);  // Use vendored term_new_var
}

Term HVM4Runtime::termNewNum(uint32_t n) {
    return term_new_num(n);  // Use vendored term_new_num
}

Term HVM4Runtime::termNewLam(Term body) {
    uint64_t loc = allocate(1);
    heap_[loc] = body;
    return term_new(0, LAM, 0, static_cast<uint32_t>(loc));
}

uint32_t HVM4Runtime::allocateLamSlot() {
    uint64_t loc = allocate(1);
    heap_[loc] = 0;  // Placeholder, will be set by finalizeLam
    return static_cast<uint32_t>(loc);
}

Term HVM4Runtime::finalizeLam(uint32_t lamLoc, Term body) {
    heap_[lamLoc] = body;
    return term_new(0, LAM, 0, lamLoc);
}

Term HVM4Runtime::termNewApp(Term fun, Term arg) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = fun;
    heap_[loc + 1] = arg;
    return term_new(0, APP, 0, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewOp2(uint32_t opr, Term x, Term y) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = x;
    heap_[loc + 1] = y;
    return term_new(0, OP2, opr, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewSup(uint32_t lab, Term a, Term b) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = a;
    heap_[loc + 1] = b;
    return term_new(0, SUP, lab, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewDup(uint32_t lab, Term val, Term body) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = val;
    heap_[loc + 1] = body;
    return term_new(0, DUP, lab, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewDupAt(uint32_t lab, uint32_t loc, Term val, Term body) {
    // Create DUP at pre-allocated location
    heap_[loc + 0] = val;
    heap_[loc + 1] = body;
    return term_new(0, DUP, lab, loc);
}

Term HVM4Runtime::termNewCo0(uint32_t lab, uint32_t loc) {
    return term_new(0, CO0, lab, loc);
}

Term HVM4Runtime::termNewCo1(uint32_t lab, uint32_t loc) {
    return term_new(0, CO1, lab, loc);
}

Term HVM4Runtime::termNewEra() {
    return term_new_era();  // Use vendored term_new_era
}

Term HVM4Runtime::termNewCtr(uint32_t name, uint32_t arity, const Term* args) {
    if (arity > 16) {
        throw HVM4Error("Constructor arity too large");
    }
    uint64_t loc = allocate(arity);
    for (uint32_t i = 0; i < arity; i++) {
        heap_[loc + i] = args[i];
    }
    uint8_t tag = C00 + arity;
    return term_new(0, tag, name, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewMat(uint32_t tagVal, Term ifMatch, Term ifNotMatch) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = ifMatch;
    heap_[loc + 1] = ifNotMatch;
    return term_new(0, MAT, tagVal, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewAnd(Term a, Term b) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = a;
    heap_[loc + 1] = b;
    return term_new(0, AND, 0, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewOr(Term a, Term b) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = a;
    heap_[loc + 1] = b;
    return term_new(0, OR, 0, static_cast<uint32_t>(loc));
}

Term HVM4Runtime::termNewEql(Term a, Term b) {
    uint64_t loc = allocate(2);
    heap_[loc + 0] = a;
    heap_[loc + 1] = b;
    return term_new(0, EQL, 0, static_cast<uint32_t>(loc));
}

// ---------------------------------------------------------------------------
// 2.4 Term Inspection API
// ---------------------------------------------------------------------------

uint8_t HVM4Runtime::termTag(Term term) {
    return term_tag(term);  // Use vendored term_tag
}

uint32_t HVM4Runtime::termExt(Term term) {
    return term_ext(term);  // Use vendored term_ext
}

uint32_t HVM4Runtime::termVal(Term term) {
    return term_val(term);  // Use vendored term_val
}

bool HVM4Runtime::termSub(Term term) {
    return term_sub(term);  // Use vendored term_sub
}

}  // namespace nix::hvm4
