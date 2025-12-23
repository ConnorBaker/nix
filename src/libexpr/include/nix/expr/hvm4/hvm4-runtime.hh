#pragma once

/**
 * HVM4 Runtime Wrapper for Nix
 *
 * This module provides a C++ wrapper around the HVM4 runtime,
 * managing memory allocation and evaluation of HVM4 terms.
 *
 * HVM4 is a runtime for the Interaction Calculus, an extension of
 * lambda calculus that provides optimal lazy evaluation through
 * explicit duplication (DUP) and superposition (SUP) nodes.
 *
 * Term Layout (64-bit):
 *   | SUB (1) | TAG (7) | EXT (24) | VAL (32) |
 *     bit 63   bits 56-62  bits 32-55  bits 0-31
 *
 * Key Term Types:
 *   - NUM (tag 30): Integer value in VAL field
 *   - VAR (tag 1):  Variable reference, VAL is heap location (NOT de Bruijn index)
 *   - LAM (tag 2):  Lambda, body stored at HEAP[VAL]
 *   - APP (tag 0):  Application, fun at HEAP[VAL], arg at HEAP[VAL+1]
 *   - OP2 (tag 33): Binary operation, opcode in EXT, operands at HEAP[VAL], HEAP[VAL+1]
 *   - DUP (tag 6):  Duplication node for affine variable handling
 *   - SUP (tag 5):  Superposition node
 *
 * Lambda Reduction Semantics:
 *   When reducing (LAM body) @ arg:
 *   1. Read body from HEAP[lam_loc] BEFORE substitution
 *   2. Substitute arg at lam_loc (marks with SUB bit)
 *   3. Return the original body term
 *
 * Creating Lambdas:
 *   To create a lambda whose body references the bound variable:
 *   1. Call allocateLamSlot() to reserve the heap location
 *   2. Build the body using VAR(lamLoc) for variable references
 *   3. Call finalizeLam(lamLoc, body) to complete the lambda
 */

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace nix::hvm4 {

// HVM4 term type (64-bit)
using Term = uint64_t;

// Forward declaration of the HVM4 state struct
struct HVM4State;

/**
 * Exception thrown when HVM4 evaluation fails.
 */
class HVM4Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * HVM4 Runtime wrapper class.
 *
 * Manages the HVM4 heap, book, stack, and provides methods
 * for evaluating terms.
 */
class HVM4Runtime {
public:
    /**
     * Create a new HVM4 runtime instance.
     *
     * @param heapSize Size of heap in terms (default 256MB worth)
     * @param stackSize Size of evaluation stack (default 1MB worth)
     */
    explicit HVM4Runtime(
        size_t heapSize = 1ULL << 26,   // 64M terms = 512MB
        size_t stackSize = 1ULL << 20   // 1M stack entries
    );

    ~HVM4Runtime();

    // Non-copyable
    HVM4Runtime(const HVM4Runtime&) = delete;
    HVM4Runtime& operator=(const HVM4Runtime&) = delete;

    // Movable
    HVM4Runtime(HVM4Runtime&& other) noexcept;
    HVM4Runtime& operator=(HVM4Runtime&& other) noexcept;

    /**
     * Reset the runtime state for a new evaluation.
     * Clears the heap and resets counters.
     */
    void reset();

    /**
     * Evaluate a term to weak head normal form.
     *
     * @param term The term to evaluate
     * @return The evaluated term in WHNF
     */
    Term evaluateWNF(Term term);

    /**
     * Evaluate a term to strong normal form (fully evaluate).
     *
     * @param term The term to evaluate
     * @return The fully evaluated term
     */
    Term evaluateSNF(Term term);

    /**
     * Get the number of reduction interactions performed.
     */
    uint64_t getInteractionCount() const;

    /**
     * Get the number of bytes allocated on the heap.
     */
    uint64_t getAllocatedBytes() const;

    /**
     * Get the total heap size in bytes.
     */
    size_t getHeapSize() const { return heapCapacity_ * sizeof(Term); }

    /**
     * Allocate space on the HVM4 heap.
     *
     * @param size Number of terms to allocate
     * @return Location of allocated space
     */
    uint64_t allocate(uint64_t size);

    /**
     * Get direct access to the heap for term construction.
     */
    Term* getHeap() { return heap_; }
    const Term* getHeap() const { return heap_; }

    /**
     * Store a term at a heap location.
     */
    void store(uint64_t loc, Term term);

    /**
     * Load a term from a heap location.
     */
    Term load(uint64_t loc) const;

    // ===== Term Construction API =====
    // These mirror the HVM4 term_new_* functions

    /**
     * Create a raw term with explicit fields.
     */
    static Term termNew(uint8_t sub, uint8_t tag, uint32_t ext, uint32_t val);

    /**
     * Create a variable term.
     * @param idx De Bruijn index
     */
    static Term termNewVar(uint32_t idx);

    /**
     * Create a number term.
     * @param n 32-bit unsigned value
     */
    static Term termNewNum(uint32_t n);

    /**
     * Create a lambda term.
     * @param body The lambda body
     */
    Term termNewLam(Term body);

    /**
     * Allocate a lambda slot and return its location.
     * Use this when you need to create a body that references
     * the lambda's binding (using VAR(lamLoc)).
     *
     * @return The heap location for this lambda
     */
    uint32_t allocateLamSlot();

    /**
     * Finalize a lambda by setting its body.
     * Call this after allocateLamSlot() and after constructing the body.
     *
     * @param lamLoc The location from allocateLamSlot()
     * @param body The lambda body (may contain VAR(lamLoc) references)
     * @return The lambda term
     */
    Term finalizeLam(uint32_t lamLoc, Term body);

    /**
     * Create an application term.
     * @param fun The function
     * @param arg The argument
     */
    Term termNewApp(Term fun, Term arg);

    /**
     * Create a binary operation term.
     * @param opr Operation code (OP_ADD, OP_SUB, etc.)
     * @param x Left operand
     * @param y Right operand
     */
    Term termNewOp2(uint32_t opr, Term x, Term y);

    /**
     * Create a superposition term.
     * @param lab Label
     * @param a First component
     * @param b Second component
     */
    Term termNewSup(uint32_t lab, Term a, Term b);

    /**
     * Create a duplication term.
     * @param lab Label
     * @param val Value being duplicated
     * @param body Continuation
     */
    Term termNewDup(uint32_t lab, Term val, Term body);

    /**
     * Create a duplication term at a pre-allocated location.
     * @param lab Label
     * @param loc Pre-allocated heap location (must have 2 slots)
     * @param val Value being duplicated
     * @param body Continuation
     */
    Term termNewDupAt(uint32_t lab, uint32_t loc, Term val, Term body);

    /**
     * Create a CO0 (first dup projection) term.
     */
    static Term termNewCo0(uint32_t lab, uint32_t loc);

    /**
     * Create a CO1 (second dup projection) term.
     */
    static Term termNewCo1(uint32_t lab, uint32_t loc);

    /**
     * Create an erasure term.
     */
    static Term termNewEra();

    /**
     * Create a constructor term.
     * @param name Constructor name/tag
     * @param arity Number of fields
     * @param args Array of field terms
     */
    Term termNewCtr(uint32_t name, uint32_t arity, const Term* args);

    /**
     * Create a match/switch term.
     * @param tag Tag to match
     * @param ifMatch Term if matched
     * @param ifNotMatch Term if not matched (applied to scrutinee)
     */
    Term termNewMat(uint32_t tag, Term ifMatch, Term ifNotMatch);

    /**
     * Create a short-circuit AND term.
     * @param a First operand
     * @param b Second operand (only evaluated if a is true)
     */
    Term termNewAnd(Term a, Term b);

    /**
     * Create a short-circuit OR term.
     * @param a First operand
     * @param b Second operand (only evaluated if a is false)
     */
    Term termNewOr(Term a, Term b);

    /**
     * Create a structural equality term.
     * Unlike OP_EQ which only works on NUM values, this handles
     * deep structural comparison of constructors and other terms.
     * Returns 1 if equal, 0 if not.
     * @param a First operand
     * @param b Second operand
     */
    Term termNewEql(Term a, Term b);

    // ===== Term Inspection API =====

    /**
     * Get the tag of a term.
     */
    static uint8_t termTag(Term term);

    /**
     * Get the ext field of a term.
     */
    static uint32_t termExt(Term term);

    /**
     * Get the val field of a term.
     */
    static uint32_t termVal(Term term);

    /**
     * Check if a term has the substitution bit set.
     */
    static bool termSub(Term term);

    // Tag constants
    static constexpr uint8_t TAG_APP = 0;
    static constexpr uint8_t TAG_VAR = 1;
    static constexpr uint8_t TAG_LAM = 2;
    static constexpr uint8_t TAG_CO0 = 3;
    static constexpr uint8_t TAG_CO1 = 4;
    static constexpr uint8_t TAG_SUP = 5;
    static constexpr uint8_t TAG_DUP = 6;
    static constexpr uint8_t TAG_REF = 8;
    static constexpr uint8_t TAG_ERA = 11;
    static constexpr uint8_t TAG_MAT = 12;
    static constexpr uint8_t TAG_C00 = 13;
    static constexpr uint8_t TAG_C01 = 14;
    static constexpr uint8_t TAG_C02 = 15;
    static constexpr uint8_t TAG_NUM = 30;
    static constexpr uint8_t TAG_OP2 = 33;
    static constexpr uint8_t TAG_EQL = 37;  // Structural equality
    static constexpr uint8_t TAG_AND = 38;  // Short-circuit boolean AND
    static constexpr uint8_t TAG_OR = 39;   // Short-circuit boolean OR

    // Operation codes (matching vendored hvm4/clang/hvm4.c)
    static constexpr uint32_t OP_ADD = 0;
    static constexpr uint32_t OP_SUB = 1;
    static constexpr uint32_t OP_MUL = 2;
    static constexpr uint32_t OP_DIV = 3;
    static constexpr uint32_t OP_MOD = 4;
    static constexpr uint32_t OP_AND = 5;
    static constexpr uint32_t OP_OR = 6;
    static constexpr uint32_t OP_XOR = 7;
    static constexpr uint32_t OP_LSH = 8;   // Left shift
    static constexpr uint32_t OP_RSH = 9;   // Right shift
    static constexpr uint32_t OP_NOT = 10;  // Bitwise NOT (unary, use as Op2(OP_NOT, 0, x))
    static constexpr uint32_t OP_EQ = 11;
    static constexpr uint32_t OP_NE = 12;
    static constexpr uint32_t OP_LT = 13;
    static constexpr uint32_t OP_LE = 14;
    static constexpr uint32_t OP_GT = 15;
    static constexpr uint32_t OP_GE = 16;
    // Aliases for compatibility
    static constexpr uint32_t OP_SHL = OP_LSH;
    static constexpr uint32_t OP_SHR = OP_RSH;

private:
    Term* heap_ = nullptr;
    uint32_t* book_ = nullptr;
    Term* stack_ = nullptr;

    size_t heapCapacity_ = 0;
    size_t stackCapacity_ = 0;

    uint64_t allocPos_ = 1;  // Current allocation position (0 is reserved)
    uint32_t stackPos_ = 1;  // Current stack position
    uint64_t interactions_ = 0;  // Interaction counter

    bool initialized_ = false;

    void cleanup();
    void setGlobals();
};

}  // namespace nix::hvm4
