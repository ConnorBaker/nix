#pragma once

/**
 * HVM4 Result Extraction
 *
 * Converts evaluated HVM4 terms back to Nix Values.
 *
 * This module handles the conversion of HVM4 normal form terms
 * into Nix Value structures that can be used by the rest of the
 * Nix evaluator.
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include "nix/expr/hvm4/hvm4-string.hh"
#include "nix/expr/hvm4/hvm4-path.hh"
#include "nix/expr/value.hh"
#include "nix/expr/symbol-table.hh"

namespace nix {
    class EvalState;
}

namespace nix::hvm4 {

/**
 * Exception thrown when result extraction fails.
 */
class ExtractionError : public HVM4Error {
public:
    using HVM4Error::HVM4Error;
};

/**
 * Result extractor class.
 *
 * Converts HVM4 terms (after evaluation to normal form) back to
 * Nix Values.
 */
class ResultExtractor {
public:
    /**
     * Create a result extractor.
     *
     * @param state The Nix evaluation state (for memory allocation)
     * @param runtime The HVM4 runtime (for heap access)
     * @param stringTable The string table for string lookups
     * @param accessorRegistry The accessor registry for path extraction
     */
    ResultExtractor(EvalState& state, HVM4Runtime& runtime, const StringTable& stringTable, const AccessorRegistry& accessorRegistry);

    /**
     * Extract an HVM4 term to a Nix Value.
     *
     * The term should already be in strong normal form (fully evaluated).
     *
     * @param term The HVM4 term to extract
     * @param result The Nix Value to populate
     * @throws ExtractionError if the term cannot be converted
     */
    void extract(Term term, Value& result);

    /**
     * Check if a term can be extracted to a Nix Value.
     *
     * Currently supported:
     * - NUM (32-bit integers, interpreted as signed)
     * - C02 with BIGINT_POS/NEG tags (64-bit integers)
     * - ERA (null)
     * - C01/C02 with CTR_STR (strings)
     * - C02 with CTR_LST (lists)
     * - C01 with CTR_ABS (base attribute sets)
     * - C02 with CTR_ALY (layered attribute sets)
     * - C02 with CTR_PTH (paths)
     *
     * Not supported:
     * - LAM (unapplied functions)
     * - Other constructors
     */
    bool canExtract(Term term) const;

private:
    EvalState& state_;
    HVM4Runtime& runtime_;
    const StringTable& stringTable_;
    const AccessorRegistry& accessorRegistry_;

    // Type-specific extractors
    void extractNum(Term term, Value& result);
    void extractBigInt(Term term, Value& result);
    void extractFloat(Term term, Value& result);
    void extractBool(Term term, Value& result);
    void extractString(Term term, Value& result);
    void extractList(Term term, Value& result);
    void extractAttrs(Term term, Value& result);
    void extractPath(Term term, Value& result);
};

}  // namespace nix::hvm4
