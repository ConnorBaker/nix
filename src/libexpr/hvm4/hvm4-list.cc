/**
 * List Encoding Implementation
 *
 * Implements list construction and inspection functions for HVM4.
 *
 * Lists are encoded as #Lst{length, spine} where the spine is a
 * standard cons-list structure:
 *   spine = #Nil{} | #Con{head, tail}
 *
 * The cached length in #Lst{} enables O(1) length operations,
 * which is critical for Nix's lazy evaluation semantics.
 */

#include "nix/expr/hvm4/hvm4-list.hh"

namespace nix::hvm4 {

Term makeNil(HVM4Runtime& runtime) {
    // Create empty spine marker: #Nil{} (arity-0 constructor)
    return runtime.termNewCtr(CTR_NIL, 0, nullptr);
}

Term makeCons(Term head, Term tail, HVM4Runtime& runtime) {
    // Create cons cell: #Con{head, tail} (arity-2 constructor)
    Term args[2] = {head, tail};
    return runtime.termNewCtr(CTR_CON, 2, args);
}

Term makeList(uint32_t length, Term spine, HVM4Runtime& runtime) {
    // Create list wrapper: #Lst{length, spine} (arity-2 constructor)
    // Length is stored as a NUM term for consistency
    Term lengthTerm = HVM4Runtime::termNewNum(length);
    Term args[2] = {lengthTerm, spine};
    return runtime.termNewCtr(CTR_LST, 2, args);
}

Term buildListFromElements(const std::vector<Term>& elements, HVM4Runtime& runtime) {
    // Build spine from back to front
    Term spine = makeNil(runtime);

    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
        spine = makeCons(*it, spine, runtime);
    }

    return makeList(static_cast<uint32_t>(elements.size()), spine, runtime);
}

bool isNil(Term term) {
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag == HVM4Runtime::TAG_C00) {
        uint32_t name = HVM4Runtime::termExt(term);
        return name == CTR_NIL;
    }
    return false;
}

bool isCons(Term term) {
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag == HVM4Runtime::TAG_C02) {
        uint32_t name = HVM4Runtime::termExt(term);
        return name == CTR_CON;
    }
    return false;
}

bool isList(Term term) {
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag == HVM4Runtime::TAG_C02) {
        uint32_t name = HVM4Runtime::termExt(term);
        return name == CTR_LST;
    }
    return false;
}

bool isEmptyList(Term term, const HVM4Runtime& runtime) {
    if (!isList(term)) return false;

    uint32_t loc = HVM4Runtime::termVal(term);
    Term lengthTerm = runtime.load(loc);

    if (HVM4Runtime::termTag(lengthTerm) != HVM4Runtime::TAG_NUM) {
        return false;
    }

    return HVM4Runtime::termVal(lengthTerm) == 0;
}

uint32_t getListLength(Term term, const HVM4Runtime& runtime) {
    if (!isList(term)) return 0;

    uint32_t loc = HVM4Runtime::termVal(term);
    Term lengthTerm = runtime.load(loc);

    if (HVM4Runtime::termTag(lengthTerm) != HVM4Runtime::TAG_NUM) {
        return 0;
    }

    return HVM4Runtime::termVal(lengthTerm);
}

Term getListSpine(Term term, const HVM4Runtime& runtime) {
    if (!isList(term)) {
        return HVM4Runtime::termNewEra();  // Error case
    }

    uint32_t loc = HVM4Runtime::termVal(term);
    return runtime.load(loc + 1);  // Spine is second field
}

Term getConsHead(Term term, const HVM4Runtime& runtime) {
    if (!isCons(term)) {
        return HVM4Runtime::termNewEra();  // Error case
    }

    uint32_t loc = HVM4Runtime::termVal(term);
    return runtime.load(loc);  // Head is first field
}

Term getConsTail(Term term, const HVM4Runtime& runtime) {
    if (!isCons(term)) {
        return HVM4Runtime::termNewEra();  // Error case
    }

    uint32_t loc = HVM4Runtime::termVal(term);
    return runtime.load(loc + 1);  // Tail is second field
}

Term concatLists(Term list1, Term list2, HVM4Runtime& runtime) {
    // Get lengths and spines from both lists
    uint32_t len1 = getListLength(list1, runtime);
    uint32_t len2 = getListLength(list2, runtime);
    Term spine1 = getListSpine(list1, runtime);
    Term spine2 = getListSpine(list2, runtime);

    // Optimization: if first list is empty, return second list
    if (len1 == 0) {
        return list2;
    }

    // Optimization: if second list is empty, return first list
    if (len2 == 0) {
        return list1;
    }

    // Walk spine1 to collect elements, then append spine2
    // We need to rebuild spine1 with spine2 as the tail of the last cons
    // Elements are shared (not copied) - only cons cells are rebuilt

    // Collect elements from spine1
    std::vector<Term> elements1;
    elements1.reserve(len1);
    Term current = spine1;
    while (isCons(current)) {
        Term head = getConsHead(current, runtime);
        elements1.push_back(head);
        current = getConsTail(current, runtime);
    }

    // Build new spine: elements from spine1 followed by spine2
    Term newSpine = spine2;  // Start with spine2 as the tail
    for (auto it = elements1.rbegin(); it != elements1.rend(); ++it) {
        newSpine = makeCons(*it, newSpine, runtime);
    }

    return makeList(len1 + len2, newSpine, runtime);
}

}  // namespace nix::hvm4
