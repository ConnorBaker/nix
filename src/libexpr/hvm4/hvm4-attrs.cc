/**
 * Attribute Set Encoding Implementation (Wrapped Sorted Lists)
 *
 * Implements attribute set construction and inspection functions for HVM4.
 *
 * Attribute sets are encoded as wrapped sorted lists:
 *   attrs = #Ats{spine}
 *   spine = #Nil{} | #Con{#Atr{key_id, value}, tail}
 *   #Atr{key_id, value} - Single attribute node
 *
 * The #Ats{} wrapper enables type identification during result extraction.
 * The `//` operator merges two spines (O(n+m)) with overlay precedence.
 */

#include "nix/expr/hvm4/hvm4-attrs.hh"
#include <algorithm>
#include <map>

namespace nix::hvm4 {

bool isAttrsSet(Term term) {
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag == HVM4Runtime::TAG_C01) {
        uint32_t name = HVM4Runtime::termExt(term);
        return name == CTR_ATS;
    }
    return false;
}

Term wrapAttrsSpine(Term spine, HVM4Runtime& runtime) {
    // Create #Ats{spine}
    Term args[1] = {spine};
    return runtime.termNewCtr(CTR_ATS, 1, args);
}

Term getAttrsSpine(Term attrs, const HVM4Runtime& runtime) {
    if (!isAttrsSet(attrs)) {
        return makeNil(const_cast<HVM4Runtime&>(runtime));  // Error case: return empty
    }
    uint32_t loc = HVM4Runtime::termVal(attrs);
    return runtime.load(loc);  // spine is first (only) field
}

Term makeEmptyAttrs(HVM4Runtime& runtime) {
    // Empty attrs is #Ats{#Nil{}}
    Term spine = makeNil(runtime);
    return wrapAttrsSpine(spine, runtime);
}

Term makeAttrNode(uint32_t symbolId, Term value, HVM4Runtime& runtime) {
    // Create #Atr{symbol_id, value}
    Term keyTerm = HVM4Runtime::termNewNum(symbolId);
    Term args[2] = {keyTerm, value};
    return runtime.termNewCtr(CTR_ATR, 2, args);
}

// Internal helper: build spine from pairs (without wrapping)
static Term buildSpineFromPairs(std::vector<std::pair<uint32_t, Term>>& attrs, HVM4Runtime& runtime) {
    if (attrs.empty()) {
        return makeNil(runtime);
    }

    // Sort by symbol ID for deterministic ordering
    std::sort(attrs.begin(), attrs.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // Build sorted list from back to front
    Term spine = makeNil(runtime);
    for (auto it = attrs.rbegin(); it != attrs.rend(); ++it) {
        Term attrNode = makeAttrNode(it->first, it->second, runtime);
        spine = makeCons(attrNode, spine, runtime);
    }

    return spine;
}

Term buildAttrsFromPairs(std::vector<std::pair<uint32_t, Term>>& attrs, HVM4Runtime& runtime) {
    Term spine = buildSpineFromPairs(attrs, runtime);
    return wrapAttrsSpine(spine, runtime);
}

bool isAttrNode(Term term) {
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag == HVM4Runtime::TAG_C02) {
        uint32_t name = HVM4Runtime::termExt(term);
        return name == CTR_ATR;
    }
    return false;
}

bool isAttrSpine(Term term) {
    // An attr spine is either Nil or Cons
    return isNil(term) || isCons(term);
}

uint32_t getAttrKey(Term term, const HVM4Runtime& runtime) {
    if (!isAttrNode(term)) {
        return 0;  // Error case
    }

    uint32_t loc = HVM4Runtime::termVal(term);
    Term keyTerm = runtime.load(loc);  // key_id is first field
    return HVM4Runtime::termVal(keyTerm);
}

Term getAttrValue(Term term, const HVM4Runtime& runtime) {
    if (!isAttrNode(term)) {
        return HVM4Runtime::termNewEra();  // Error case
    }

    uint32_t loc = HVM4Runtime::termVal(term);
    return runtime.load(loc + 1);  // value is second field
}

// Internal helper: merge two spines (without wrapping)
static Term mergeSpines(Term baseSpine, Term overlaySpine, HVM4Runtime& runtime) {
    // Merge two sorted spines, overlay takes precedence for duplicate keys
    // This is O(n+m) where n and m are the sizes of the two spines
    // Values are shared - we don't copy them, just reference them in the new spine

    // If overlay is empty, return base
    if (isNil(overlaySpine)) {
        return baseSpine;
    }

    // If base is empty, return overlay
    if (isNil(baseSpine)) {
        return overlaySpine;
    }

    // Collect all attributes into a map (overlay overrides base)
    std::map<uint32_t, Term> merged;

    // First add base entries
    Term current = baseSpine;
    while (isCons(current)) {
        Term attrNode = getConsHead(current, runtime);
        uint32_t key = getAttrKey(attrNode, runtime);
        Term value = getAttrValue(attrNode, runtime);
        merged[key] = value;  // Will be overwritten by overlay if same key
        current = getConsTail(current, runtime);
    }

    // Then add overlay entries (overrides base)
    current = overlaySpine;
    while (isCons(current)) {
        Term attrNode = getConsHead(current, runtime);
        uint32_t key = getAttrKey(attrNode, runtime);
        Term value = getAttrValue(attrNode, runtime);
        merged[key] = value;  // Override any base entry
        current = getConsTail(current, runtime);
    }

    // Build new sorted spine from the merged map
    std::vector<std::pair<uint32_t, Term>> pairs;
    pairs.reserve(merged.size());
    for (const auto& [key, value] : merged) {
        pairs.push_back({key, value});
    }

    return buildSpineFromPairs(pairs, runtime);
}

Term mergeAttrs(Term base, Term overlay, HVM4Runtime& runtime) {
    // Unwrap both attribute sets
    Term baseSpine = getAttrsSpine(base, runtime);
    Term overlaySpine = getAttrsSpine(overlay, runtime);

    // Merge the spines
    Term mergedSpine = mergeSpines(baseSpine, overlaySpine, runtime);

    // Wrap and return
    return wrapAttrsSpine(mergedSpine, runtime);
}

size_t countAttrs(Term attrs, const HVM4Runtime& runtime) {
    // Unwrap to get spine
    Term spine = getAttrsSpine(attrs, runtime);

    size_t count = 0;
    Term current = spine;
    while (isCons(current)) {
        count++;
        current = getConsTail(current, runtime);
    }
    return count;
}

}  // namespace nix::hvm4
