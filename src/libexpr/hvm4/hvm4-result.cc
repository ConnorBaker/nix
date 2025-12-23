/**
 * HVM4 Result Extraction Implementation
 *
 * Converts HVM4 normal form terms back to Nix Values. This module handles
 * the final step of HVM4 evaluation: extracting the computed result and
 * converting it to a Nix-compatible Value structure.
 *
 * Supported result types:
 * - NUM (32-bit integers, interpreted as signed)
 * - BigInt constructors (#Pos{lo, hi} and #Neg{lo, hi}) for 64-bit integers
 * - ERA (erasure, mapped to null)
 *
 * @see hvm4-bigint.cc for integer encoding/decoding details
 */

#include "nix/expr/hvm4/hvm4-result.hh"
#include "nix/expr/hvm4/hvm4-attrs.hh"
#include "nix/expr/hvm4/hvm4-bigint.hh"
#include "nix/expr/hvm4/hvm4-list.hh"
#include "nix/expr/hvm4/hvm4-path.hh"
#include "nix/expr/hvm4/hvm4-string.hh"
#include "nix/expr/eval.hh"

#include <bit>

namespace nix::hvm4 {

ResultExtractor::ResultExtractor(EvalState& state, HVM4Runtime& runtime, const StringTable& stringTable, const AccessorRegistry& accessorRegistry)
    : state_(state)
    , runtime_(runtime)
    , stringTable_(stringTable)
    , accessorRegistry_(accessorRegistry)
{}

void ResultExtractor::extract(Term term, Value& result) {
    uint8_t tag = HVM4Runtime::termTag(term);

    switch (tag) {
        case HVM4Runtime::TAG_NUM:
            extractNum(term, result);
            break;

        case HVM4Runtime::TAG_C00: {
            // Arity-0 constructor - check if it's our null encoding
            uint32_t name = HVM4Runtime::termExt(term);
            if (name == NIX_NULL) {
                result.mkNull();
            } else {
                throw ExtractionError("Unknown arity-0 constructor in HVM4 result");
            }
            break;
        }

        case HVM4Runtime::TAG_C01: {
            // Arity-1 constructor - check if it's a string, attrs, or int-to-string
            uint32_t name = HVM4Runtime::termExt(term);
            if (name == CTR_STR || name == CTR_SNUM) {
                // Both simple strings and int-to-string are handled by extractString
                extractString(term, result);
            } else if (name == CTR_ATS) {
                extractAttrs(term, result);
            } else {
                throw ExtractionError("Unknown arity-1 constructor in HVM4 result");
            }
            break;
        }

        case HVM4Runtime::TAG_C02: {
            // Could be BigInt, float, list, path, string concat, or other constructor
            uint32_t name = HVM4Runtime::termExt(term);
            if (name == BIGINT_POS || name == BIGINT_NEG) {
                extractBigInt(term, result);
            } else if (name == NIX_FLT) {
                extractFloat(term, result);
            } else if (name == CTR_LST) {
                extractList(term, result);
            } else if (name == CTR_PTH) {
                extractPath(term, result);
            } else if (name == CTR_SCAT) {
                // String concatenation - handled by extractString
                extractString(term, result);
            } else {
                throw ExtractionError("Unknown constructor in HVM4 result");
            }
            break;
        }

        case HVM4Runtime::TAG_ERA:
            // ERA also represents null/void (for backwards compatibility)
            result.mkNull();
            break;

        case HVM4Runtime::TAG_LAM:
            throw ExtractionError(
                "Cannot extract lambda from HVM4 - functions must be fully applied");

        case HVM4Runtime::TAG_APP:
            throw ExtractionError(
                "Cannot extract unapplied function - expression did not reduce to normal form");

        case HVM4Runtime::TAG_VAR:
            throw ExtractionError(
                "Cannot extract free variable - expression did not reduce to normal form");

        default:
            // Check for other constructor arities
            if (tag >= HVM4Runtime::TAG_C00 && tag <= HVM4Runtime::TAG_C00 + 16) {
                throw ExtractionError(
                    "Constructor values not yet supported for extraction");
            }
            throw ExtractionError("Unsupported HVM4 term type for extraction");
    }
}

bool ResultExtractor::canExtract(Term term) const {
    uint8_t tag = HVM4Runtime::termTag(term);

    switch (tag) {
        case HVM4Runtime::TAG_NUM:
            return true;

        case HVM4Runtime::TAG_C00: {
            // Arity-0 constructor - check if it's null
            uint32_t name = HVM4Runtime::termExt(term);
            return name == NIX_NULL;
        }

        case HVM4Runtime::TAG_C01: {
            // Arity-1 constructor - check if it's a string, attrs, or int-to-string
            uint32_t name = HVM4Runtime::termExt(term);
            return name == CTR_STR || name == CTR_ATS || name == CTR_SNUM;
        }

        case HVM4Runtime::TAG_C02: {
            uint32_t name = HVM4Runtime::termExt(term);
            return name == BIGINT_POS || name == BIGINT_NEG || name == NIX_FLT || name == CTR_LST || name == CTR_PTH || name == CTR_SCAT;
        }

        case HVM4Runtime::TAG_ERA:
            return true;

        default:
            return false;
    }
}

void ResultExtractor::extractNum(Term term, Value& result) {
    // NUM stores 32-bit value, interpret as signed
    uint32_t bits = HVM4Runtime::termVal(term);
    int32_t signedVal = static_cast<int32_t>(bits);
    result.mkInt(static_cast<NixInt::Inner>(signedVal));
}

void ResultExtractor::extractBigInt(Term term, Value& result) {
    auto decoded = decodeInt64(term, runtime_);
    if (!decoded) {
        throw ExtractionError("Invalid BigInt encoding in HVM4 result");
    }
    result.mkInt(*decoded);
}

void ResultExtractor::extractFloat(Term term, Value& result) {
    auto decoded = decodeFloat(term, runtime_);
    if (!decoded) {
        throw ExtractionError("Invalid Float encoding in HVM4 result");
    }
    result.mkFloat(*decoded);
}

void ResultExtractor::extractBool(Term term, Value& result) {
    // Booleans are represented as NUM with 0 (false) or non-zero (true)
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag != HVM4Runtime::TAG_NUM) {
        throw ExtractionError("Expected boolean (NUM) in HVM4 result");
    }
    uint32_t val = HVM4Runtime::termVal(term);
    result.mkBool(val != 0);
}

void ResultExtractor::extractList(Term term, Value& result) {
    // Lists are encoded as #Lst{length, spine}
    if (!isList(term)) {
        throw ExtractionError("Expected list in HVM4 result");
    }

    uint32_t length = getListLength(term, runtime_);
    Term spine = getListSpine(term, runtime_);

    // Create the Nix list with the correct size
    auto list = state_.buildList(length);

    // Walk the spine and extract each element
    Term current = spine;
    for (uint32_t i = 0; i < length; i++) {
        if (!isCons(current)) {
            throw ExtractionError("Malformed list spine in HVM4 result");
        }

        // Get the head element and evaluate it to normal form
        Term head = getConsHead(current, runtime_);
        head = runtime_.evaluateSNF(head);

        // Allocate a new value and extract into it
        Value* elemValue = state_.allocValue();
        extract(head, *elemValue);
        list[i] = elemValue;

        // Move to the tail
        current = getConsTail(current, runtime_);
    }

    // Verify we've consumed the entire spine
    if (!isNil(current)) {
        throw ExtractionError("List spine longer than expected length");
    }

    // Finalize the list
    result.mkList(list);
}

void ResultExtractor::extractString(Term term, Value& result) {
    // Strings can be:
    // - #Str{string_id} - simple string literal
    // - #SCat{left, right} - string concatenation
    // - #SNum{value} - integer-to-string conversion
    //
    // Use extractStringContent to handle all cases uniformly

    try {
        std::string content = extractStringContent(term, stringTable_, runtime_);
        result.mkString(content, state_.mem);
    } catch (const std::runtime_error& e) {
        throw ExtractionError(std::string("Error extracting string: ") + e.what());
    }
}

void ResultExtractor::extractAttrs(Term term, Value& result) {
    // Attribute sets are encoded as #Ats{spine}
    // With the simplified encoding, there's only one wrapper type

    if (!isAttrsSet(term)) {
        throw ExtractionError("Expected attribute set in HVM4 result");
    }

    // Get the sorted list spine from #Ats{spine}
    Term spine = getAttrsSpine(term, runtime_);

    // Count attributes first
    size_t count = 0;
    Term current = spine;
    while (isCons(current)) {
        count++;
        current = getConsTail(current, runtime_);
    }

    // Create the Nix bindings
    auto bindings = state_.buildBindings(count);

    // Walk the spine and extract each attribute
    current = spine;
    while (isCons(current)) {
        Term attrNode = getConsHead(current, runtime_);
        if (!isAttrNode(attrNode)) {
            throw ExtractionError("Malformed attribute node in HVM4 result");
        }

        uint32_t symbolId = getAttrKey(attrNode, runtime_);
        Term valueTerm = getAttrValue(attrNode, runtime_);

        // Evaluate the value term to normal form
        valueTerm = runtime_.evaluateSNF(valueTerm);

        // Create symbol from ID using bit_cast
        // Symbol is just a wrapper around uint32_t with private constructor
        Symbol sym = std::bit_cast<Symbol>(symbolId);

        // Allocate and extract value
        Value* attrValue = state_.allocValue();
        extract(valueTerm, *attrValue);

        // Add to bindings
        bindings.insert(sym, attrValue);

        current = getConsTail(current, runtime_);
    }

    // Verify we reached the end of the spine
    if (!isNil(current)) {
        throw ExtractionError("Attribute spine not terminated with Nil");
    }

    // Finalize the attribute set
    result.mkAttrs(bindings);
}

void ResultExtractor::extractPath(Term term, Value& result) {
    // Paths are encoded as #Pth{accessor_id, path_string_id}
    if (!isPath(term)) {
        throw ExtractionError("Expected path in HVM4 result");
    }

    // Get the accessor ID and look up the accessor
    uint32_t accessorId = getPathAccessorId(term, runtime_);
    SourceAccessor* accessor = accessorRegistry_.getAccessor(accessorId);
    if (!accessor) {
        throw ExtractionError("Invalid accessor ID in HVM4 path result");
    }

    // Get the path string ID and look up the path string
    uint32_t pathStringId = getPathStringId(term, runtime_);
    if (!stringTable_.valid(pathStringId)) {
        throw ExtractionError("Invalid path string ID in HVM4 path result");
    }

    std::string_view pathStr = stringTable_.get(pathStringId);

    // Create the path value
    // Value::mkPath takes (SourceAccessor*, StringData)
    result.mkPath(accessor, StringData::make(state_.mem, pathStr));
}

}  // namespace nix::hvm4
