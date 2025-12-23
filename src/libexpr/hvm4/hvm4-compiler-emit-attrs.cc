/**
 * HVM4 Compiler Attribute Set Emitters
 *
 * Contains emitters for attribute set expressions:
 * - emitAttrs, emitRecAttrs: Attribute set construction
 * - emitSelect: Attribute selection
 * - emitAttrLookup, emitSpineSearch: Runtime attribute lookup helpers
 * - emitOpHasAttr, emitOpHasAttrInternal, emitSpineHasAttr: Has-attribute checks
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"
#include "nix/expr/hvm4/hvm4-attrs.hh"

namespace nix::hvm4 {

// ============================================================================
// Attribute Set Construction
// ============================================================================

Term HVM4Compiler::emitAttrs(const ExprAttrs& e, CompileContext& ctx) {
    // Handle recursive attr sets with emitRecAttrs
    if (e.recursive) {
        return emitRecAttrs(e, ctx);
    }

    // Build attribute set from static attributes

    if (!e.attrs || e.attrs->empty()) {
        return makeEmptyAttrs(ctx.runtime());
    }

    // If there are inherit-from expressions, set up the inherit-from context
    bool hasInheritFrom = e.inheritFromExprs && !e.inheritFromExprs->empty();

    // Store entries info for DUP wrapping later
    struct InheritFromInfo {
        Term compiledExpr;
        uint32_t useCount;
        uint32_t dupLabel;
        uint32_t dupLoc;
    };
    std::vector<InheritFromInfo> inheritFromInfo;

    if (hasInheritFrom) {
        // Count how many times each inherit-from expression is used
        std::vector<uint32_t> useCounts(e.inheritFromExprs->size(), 0);
        for (const auto& [name, def] : *e.attrs) {
            if (def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom) {
                // def.e is ExprSelect(ExprInheritFrom(displ), attrName)
                auto* sel = dynamic_cast<const ExprSelect*>(def.e);
                if (sel) {
                    auto* inheritFrom = dynamic_cast<const ExprInheritFrom*>(sel->e);
                    if (inheritFrom && inheritFrom->displ < useCounts.size()) {
                        useCounts[inheritFrom->displ]++;
                    }
                }
            }
        }

        // Pre-compile inherit-from expressions and set up context
        InheritFromContext inheritCtx;
        inheritCtx.entries.reserve(e.inheritFromExprs->size());
        inheritFromInfo.reserve(e.inheritFromExprs->size());

        for (size_t i = 0; i < e.inheritFromExprs->size(); i++) {
            InheritFromEntry entry;
            entry.compiledExpr = emit(*(*e.inheritFromExprs)[i], ctx);
            entry.useCount = useCounts[i];
            entry.useIndex = 0;

            // Allocate DUP resources if needed
            if (entry.useCount > 1) {
                uint32_t numDups = entry.useCount - 1;
                entry.dupLabel = ctx.freshLabels(numDups);
                entry.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
            } else {
                entry.dupLabel = 0;
                entry.dupLoc = 0;
            }

            // Store info for DUP wrapping
            inheritFromInfo.push_back({
                entry.compiledExpr,
                entry.useCount,
                entry.dupLabel,
                entry.dupLoc
            });

            inheritCtx.entries.push_back(entry);
        }
        ctx.pushInheritFrom(std::move(inheritCtx));
    }

    // Collect attributes as (symbol_id, value) pairs
    std::vector<std::pair<uint32_t, Term>> attrs;
    attrs.reserve(e.attrs->size());

    for (const auto& [name, def] : *e.attrs) {
        // Get the symbol ID for the key
        uint32_t symbolId = name.getId();
        // Compile the value expression
        // For InheritedFrom, def.e is ExprSelect(ExprInheritFrom, attrName)
        Term value = emit(*def.e, ctx);
        attrs.push_back({symbolId, value});
    }

    // Pop inherit-from context
    if (hasInheritFrom) {
        ctx.popInheritFrom();
    }

    Term result = buildAttrsFromPairs(attrs, ctx.runtime());

    // Wrap with DUP chains for multi-use inherit-from expressions
    // Build from innermost to outermost
    for (int i = static_cast<int>(inheritFromInfo.size()) - 1; i >= 0; i--) {
        const auto& info = inheritFromInfo[i];
        if (info.useCount > 1) {
            uint32_t numDups = info.useCount - 1;

            // Build DUP chain from innermost to outermost
            for (int32_t j = static_cast<int32_t>(numDups) - 1; j >= 0; j--) {
                uint32_t dupLocForJ = info.dupLoc + 2 * j;
                Term val;
                if (j == 0) {
                    // First (outermost) DUP duplicates the original compiled expr
                    val = info.compiledExpr;
                } else {
                    // Subsequent DUPs duplicate the CO1 of the previous DUP
                    val = HVM4Runtime::termNewCo1(info.dupLabel + j - 1, info.dupLoc + 2 * (j - 1));
                }
                result = ctx.runtime().termNewDupAt(info.dupLabel + j, dupLocForJ, val, result);
            }
        }
    }

    return result;
}

Term HVM4Compiler::emitRecAttrs(const ExprAttrs& e, CompileContext& ctx) {
    // Emit recursive attribute set (acyclic case only)
    // Strategy: Sort bindings topologically and emit as nested lets,
    // then build the attribute set.

    if (!e.attrs || e.attrs->empty()) {
        return makeEmptyAttrs(ctx.runtime());
    }

    // If there are inherit-from expressions, pre-compile them and set up context
    // Using the same pattern as emitAttrs for DUP handling
    if (e.inheritFromExprs && !e.inheritFromExprs->empty()) {
        // Count how many times each inherit-from expression is used
        std::vector<uint32_t> useCounts(e.inheritFromExprs->size(), 0);
        for (const auto& [name, def] : *e.attrs) {
            if (def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom) {
                auto* sel = dynamic_cast<const ExprSelect*>(def.e);
                if (sel) {
                    auto* inheritFrom = dynamic_cast<const ExprInheritFrom*>(sel->e);
                    if (inheritFrom && inheritFrom->displ < useCounts.size()) {
                        useCounts[inheritFrom->displ]++;
                    }
                }
            }
        }

        InheritFromContext inheritCtx;
        inheritCtx.entries.reserve(e.inheritFromExprs->size());
        for (size_t i = 0; i < e.inheritFromExprs->size(); i++) {
            InheritFromEntry entry;
            entry.compiledExpr = emit(*(*e.inheritFromExprs)[i], ctx);
            entry.useCount = useCounts[i];
            entry.useIndex = 0;

            if (entry.useCount > 1) {
                uint32_t numDups = entry.useCount - 1;
                entry.dupLabel = ctx.freshLabels(numDups);
                entry.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
            } else {
                entry.dupLabel = 0;
                entry.dupLoc = 0;
            }

            inheritCtx.entries.push_back(entry);
        }
        ctx.pushInheritFrom(std::move(inheritCtx));
    }

    // Collect binding names
    std::set<Symbol> bindingNames;
    for (const auto& [name, def] : *e.attrs) {
        bindingNames.insert(name);
    }

    // Build dependency graph
    std::map<Symbol, std::set<Symbol>> deps;
    for (const auto& [name, def] : *e.attrs) {
        deps[name] = std::set<Symbol>();
        collectDependencies(*def.e, bindingNames, deps[name]);
    }

    // Topological sort
    auto sortedOpt = topologicalSort(deps);
    if (!sortedOpt) {
        throw HVM4Error("Cyclic dependencies in rec expression not yet supported");
    }
    const auto& sorted = *sortedOpt;

    // Now emit as nested lets in topological order
    // rec { a = 1; b = a + 1; c = b + 1; }
    // becomes: let a = 1; in let b = a + 1; in let c = b + 1; in { a = a; b = b; c = c; }

    // Record the starting binding count
    size_t startBinding = ctx.getBindings().size();

    // Pre-allocate lambda slots for all bindings
    std::map<Symbol, uint32_t> lamLocs;
    for (const auto& name : sorted) {
        lamLocs[name] = ctx.runtime().allocateLamSlot();
    }

    // Push all bindings into context
    for (const auto& name : sorted) {
        ctx.pushBinding(name, lamLocs[name]);
    }

    // Count usages in all expressions and the final attr set references
    for (const auto& [name, def] : *e.attrs) {
        countUsages(*def.e, ctx);
    }
    // Each binding is used at least once in the final attr set
    for (const auto& name : sorted) {
        auto* binding = ctx.lookup(name);
        if (binding) {
            binding->useCount++;
        }
    }

    // Set up DUPs for multi-use bindings
    for (size_t i = 0; i < sorted.size(); i++) {
        auto& binding = ctx.getBindings()[startBinding + i];
        if (binding.useCount > 1) {
            uint32_t numDups = binding.useCount - 1;
            binding.dupLabel = ctx.freshLabels(numDups);
            binding.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
            binding.dupIndex = 0;
        }
    }

    // Build the final attribute set - sort by symbol ID for sorted list
    std::vector<std::pair<Symbol, Term>> attrPairs;
    for (const auto& name : sorted) {
        auto* binding = ctx.lookup(name);
        Term valueRef;
        if (binding->useCount > 1) {
            uint32_t idx = binding->dupIndex++;
            uint32_t numDups = binding->useCount - 1;
            if (idx < numDups) {
                uint32_t dupLoc = binding->dupLoc + 2 * idx;
                valueRef = HVM4Runtime::termNewCo0(binding->dupLabel + idx, dupLoc);
            } else {
                uint32_t dupLoc = binding->dupLoc + 2 * (numDups - 1);
                valueRef = HVM4Runtime::termNewCo1(binding->dupLabel + numDups - 1, dupLoc);
            }
        } else {
            valueRef = HVM4Runtime::termNewVar(lamLocs[name]);
        }
        attrPairs.emplace_back(name, valueRef);
    }

    // Sort by symbol ID
    std::sort(attrPairs.begin(), attrPairs.end(), [](const auto& a, const auto& b) {
        return a.first.getId() < b.first.getId();
    });

    // Build the sorted list for the attr set
    Term sortedList = makeNil(ctx.runtime());
    for (int i = attrPairs.size() - 1; i >= 0; i--) {
        uint32_t symbolId = attrPairs[i].first.getId();
        Term attrNode = makeAttrNode(symbolId, attrPairs[i].second, ctx.runtime());
        sortedList = makeCons(attrNode, sortedList, ctx.runtime());
    }
    // Wrap the spine with #Ats{} for type identification
    Term body = wrapAttrsSpine(sortedList, ctx.runtime());

    // Wrap with DUPs for multi-use bindings
    bool needsDup = false;
    for (size_t i = startBinding; i < ctx.getBindings().size(); i++) {
        if (ctx.getBindings()[i].useCount > 1) {
            needsDup = true;
            break;
        }
    }
    if (needsDup) {
        body = wrapWithDups(body, ctx, startBinding);
    }

    // Build from inside out: wrap body with lambda for each binding (reverse order)
    for (int i = sorted.size() - 1; i >= 0; i--) {
        const Symbol& name = sorted[i];
        ctx.popBinding();

        // Finalize lambda
        body = ctx.runtime().finalizeLam(lamLocs[name], body);

        // Emit the binding value
        const auto& def = e.attrs->find(name)->second;
        Term value = emit(*def.e, ctx);

        // Apply lambda to value
        body = ctx.runtime().termNewApp(body, value);
    }

    // Pop inherit-from context if we pushed one
    if (e.inheritFromExprs && !e.inheritFromExprs->empty()) {
        ctx.popInheritFrom();
    }

    return body;
}

// ============================================================================
// Attribute Selection
// ============================================================================

Term HVM4Compiler::emitSelect(const ExprSelect& e, CompileContext& ctx) {
    // Compile the expression being selected from
    Term attrs = emit(*e.e, ctx);

    if (!e.def) {
        // No default: chain attribute lookups (returns ERA on failure)
        Term result = attrs;
        for (size_t i = 0; i < e.nAttrPath; i++) {
            const AttrName& attrName = e.attrPathStart[i];
            uint32_t symbolId = attrName.symbol.getId();
            result = emitAttrLookup(result, symbolId, ctx);
        }
        return result;
    }

    // Has default: use Maybe-wrapped lookups
    // Strategy: wrap result in #Som{value} or #Non{}, then extract or default
    //
    // For x.a.b or default:
    // 1. Start with #Som{x}
    // 2. For each step, if #Som{attrs}, lookup and wrap in #Som or #Non
    // 3. At end, MAT(#Som, extract, default)

    Term defaultVal = emit(*e.def, ctx);

    // Start with #Som{attrs}
    Term maybeTerm = ctx.runtime().termNewCtr(CTR_SOM, 1, &attrs);

    // For each path element, do a maybe-chain lookup
    for (size_t i = 0; i < e.nAttrPath; i++) {
        const AttrName& attrName = e.attrPathStart[i];
        uint32_t symbolId = attrName.symbol.getId();
        maybeTerm = emitMaybeAttrLookup(maybeTerm, symbolId, ctx);
    }

    // Extract from #Som or return default
    // MAT(CTR_SOM, λvalue. value, λ_. default) maybeTerm
    uint32_t valueLamLoc = ctx.runtime().allocateLamSlot();
    Term valueVar = HVM4Runtime::termNewVar(valueLamLoc);
    Term extractLam = ctx.runtime().finalizeLam(valueLamLoc, valueVar);

    Term defaultLam = ctx.runtime().termNewLam(defaultVal);

    Term mat = ctx.runtime().termNewMat(CTR_SOM, extractLam, defaultLam);
    return ctx.runtime().termNewApp(mat, maybeTerm);
}

Term HVM4Compiler::emitAttrLookup(Term attrs, uint32_t symbolId, CompileContext& ctx) {
    // Emit code to lookup an attribute in an attribute set
    // This generates a recursive search through the sorted list
    //
    // Attrs are wrapped: #Ats{spine}
    // We need to MAT on CTR_ATS to extract the spine, then search it.
    //
    // Structure:
    //   MAT(CTR_ATS, λspine. searchSpine(spine, symbolId), error) attrs

    Term keyTerm = HVM4Runtime::termNewNum(symbolId);

    // Allocate lambda slot for the extracted spine
    uint32_t spineLamLoc = ctx.runtime().allocateLamSlot();
    Term spineVar = HVM4Runtime::termNewVar(spineLamLoc);

    // Build the spine search code
    Term searchCode = emitSpineSearch(spineVar, keyTerm, ctx);

    // Wrap in lambda
    Term searchLam = ctx.runtime().finalizeLam(spineLamLoc, searchCode);
    Term error = HVM4Runtime::termNewEra();  // Error/missing attribute

    // MAT on CTR_ATS to extract spine and search it
    Term mat = ctx.runtime().termNewMat(CTR_ATS, searchLam, error);
    return ctx.runtime().termNewApp(mat, attrs);
}

Term HVM4Compiler::emitSpineSearch(Term spine, Term targetKey, CompileContext& ctx) {
    // Search a cons-list spine for an attribute with matching key
    // Returns the value if found, ERA if not found
    //
    // We use self-application for recursion (like Y-combinator):
    // searchFactory = λself. λlist.
    //   MAT(CTR_CON,
    //       λhead. λtail.
    //           MAT(CTR_ATR,
    //               λkey. λval. if key == targetKey then val else (self self tail),
    //               λx. ERA
    //           ) head,
    //       λx. ERA
    //   ) list
    // Then: (searchFactory searchFactory) spine

    Term notFound = HVM4Runtime::termNewEra();

    // === Allocate all lambda slots upfront ===
    uint32_t selfLamLoc = ctx.runtime().allocateLamSlot();   // λself
    uint32_t listLamLoc = ctx.runtime().allocateLamSlot();   // λlist
    uint32_t headLamLoc = ctx.runtime().allocateLamSlot();   // λhead
    uint32_t tailLamLoc = ctx.runtime().allocateLamSlot();   // λtail
    uint32_t keyLamLoc = ctx.runtime().allocateLamSlot();    // λkey
    uint32_t valLamLoc = ctx.runtime().allocateLamSlot();    // λval

    // === Create variable references ===
    Term selfVar = HVM4Runtime::termNewVar(selfLamLoc);
    Term listVar = HVM4Runtime::termNewVar(listLamLoc);
    Term headVar = HVM4Runtime::termNewVar(headLamLoc);
    Term tailVar = HVM4Runtime::termNewVar(tailLamLoc);
    Term keyVar = HVM4Runtime::termNewVar(keyLamLoc);
    Term valVar = HVM4Runtime::termNewVar(valLamLoc);

    // === Build the recursive call: self self tail ===
    // We need to DUP self since it's used twice (self self)
    uint32_t selfDupLabel = ctx.freshLabel();
    uint32_t selfDupLoc = ctx.allocate(2);
    Term selfRef0 = HVM4Runtime::termNewCo0(selfDupLabel, selfDupLoc);
    Term selfRef1 = HVM4Runtime::termNewCo1(selfDupLabel, selfDupLoc);
    Term selfSelfInner = ctx.runtime().termNewApp(selfRef0, selfRef1);  // self self
    Term recursiveCall = ctx.runtime().termNewApp(selfSelfInner, tailVar);  // self self tail

    // === Build the attr handler (innermost) ===
    // λkey. λval. if (key == targetKey) then val else (self self tail)
    Term keyMatch = ctx.runtime().termNewEql(keyVar, targetKey);
    Term returnValLam = ctx.runtime().termNewLam(valVar);
    // MAT(0, recursiveCall, λ_. val) keyMatch
    // If keyMatch == 0 (false), return recursiveCall (continue searching tail)
    // If keyMatch != 0 (true), return val
    Term condMat = ctx.runtime().termNewMat(0, recursiveCall, returnValLam);
    Term conditionalResult = ctx.runtime().termNewApp(condMat, keyMatch);

    // Build curried lambda: λkey. λval. conditionalResult
    Term valLam = ctx.runtime().finalizeLam(valLamLoc, conditionalResult);
    Term keyLam = ctx.runtime().finalizeLam(keyLamLoc, valLam);

    // === Build MAT for #Atr{key, val} ===
    Term attrIfNotMatch = ctx.runtime().termNewLam(notFound);
    Term attrMat = ctx.runtime().termNewMat(CTR_ATR, keyLam, attrIfNotMatch);

    // === Build the cons handler ===
    // λhead. λtail. MAT(ATR, ...) head
    Term checkHead = ctx.runtime().termNewApp(attrMat, headVar);

    // Build curried lambda: λhead. λtail. checkHead
    Term tailLam = ctx.runtime().finalizeLam(tailLamLoc, checkHead);
    Term headLam = ctx.runtime().finalizeLam(headLamLoc, tailLam);

    // === Build the nil handler ===
    Term nilIfNotMatch = ctx.runtime().termNewLam(notFound);

    // === Build MAT for #Con{head, tail} ===
    Term consMat = ctx.runtime().termNewMat(CTR_CON, headLam, nilIfNotMatch);

    // === Build: λlist. consMat list ===
    Term listBody = ctx.runtime().termNewApp(consMat, listVar);
    Term listLam = ctx.runtime().finalizeLam(listLamLoc, listBody);

    // === Build: λself. listLam ===
    // We need to wrap with DUP for self variable
    Term dupBody = ctx.runtime().termNewDupAt(selfDupLabel, selfDupLoc, selfVar, listLam);
    Term selfLam = ctx.runtime().finalizeLam(selfLamLoc, dupBody);

    // === Apply: selfLam selfLam spine ===
    // DUP selfLam for self-application
    uint32_t outerDupLabel = ctx.freshLabel();
    uint32_t outerDupLoc = ctx.allocate(2);
    Term outerRef0 = HVM4Runtime::termNewCo0(outerDupLabel, outerDupLoc);
    Term outerRef1 = HVM4Runtime::termNewCo1(outerDupLabel, outerDupLoc);
    Term selfSelfOuter = ctx.runtime().termNewApp(outerRef0, outerRef1);
    Term searchSpine = ctx.runtime().termNewApp(selfSelfOuter, spine);

    // Wrap with DUP for selfLam
    return ctx.runtime().termNewDupAt(outerDupLabel, outerDupLoc, selfLam, searchSpine);
}

// ============================================================================
// Maybe Attribute Lookup (for select-or-default)
// ============================================================================

Term HVM4Compiler::emitMaybeAttrLookup(Term maybeTerm, uint32_t symbolId, CompileContext& ctx) {
    // Chain a lookup on a Maybe-wrapped value
    // Input: #Som{attrs} or #Non{}
    // Output: #Som{value} (if found) or #Non{} (if not found or input was #Non)
    //
    // Logic:
    // MAT(CTR_SOM,
    //     λattrs. lookupMaybe(attrs, symbolId),  // If #Som, do lookup
    //     λ_. #Non{}                              // If #Non, propagate
    // ) maybeTerm

    Term keyTerm = HVM4Runtime::termNewNum(symbolId);

    // Build the #Non{} result for propagation
    Term nonResult = ctx.runtime().termNewCtr(CTR_NON, 0, nullptr);

    // Allocate lambda slot for extracted attrs
    uint32_t attrsLamLoc = ctx.runtime().allocateLamSlot();
    Term attrsVar = HVM4Runtime::termNewVar(attrsLamLoc);

    // Unwrap attrs (#Ats{spine}) and search with maybe result
    // MAT(CTR_ATS, λspine. searchMaybe(spine, key), #Non{}) attrs
    uint32_t spineLamLoc = ctx.runtime().allocateLamSlot();
    Term spineVar = HVM4Runtime::termNewVar(spineLamLoc);

    Term searchCode = emitSpineSearchMaybe(spineVar, keyTerm, ctx);
    Term searchLam = ctx.runtime().finalizeLam(spineLamLoc, searchCode);

    Term unwrapMat = ctx.runtime().termNewMat(CTR_ATS, searchLam, nonResult);
    Term lookupResult = ctx.runtime().termNewApp(unwrapMat, attrsVar);

    Term someLam = ctx.runtime().finalizeLam(attrsLamLoc, lookupResult);
    Term noneLam = ctx.runtime().termNewLam(nonResult);

    Term mat = ctx.runtime().termNewMat(CTR_SOM, someLam, noneLam);
    return ctx.runtime().termNewApp(mat, maybeTerm);
}

Term HVM4Compiler::emitSpineSearchMaybe(Term spine, Term targetKey, CompileContext& ctx) {
    // Search a cons-list spine for an attribute with matching key
    // Returns #Som{value} if found, #Non{} if not found
    //
    // Similar to emitSpineSearch but wraps result in Maybe constructors

    Term nonResult = ctx.runtime().termNewCtr(CTR_NON, 0, nullptr);

    // === Allocate all lambda slots upfront ===
    uint32_t selfLamLoc = ctx.runtime().allocateLamSlot();   // λself
    uint32_t listLamLoc = ctx.runtime().allocateLamSlot();   // λlist
    uint32_t headLamLoc = ctx.runtime().allocateLamSlot();   // λhead
    uint32_t tailLamLoc = ctx.runtime().allocateLamSlot();   // λtail
    uint32_t keyLamLoc = ctx.runtime().allocateLamSlot();    // λkey
    uint32_t valLamLoc = ctx.runtime().allocateLamSlot();    // λval

    // === Create variable references ===
    Term selfVar = HVM4Runtime::termNewVar(selfLamLoc);
    Term listVar = HVM4Runtime::termNewVar(listLamLoc);
    Term headVar = HVM4Runtime::termNewVar(headLamLoc);
    Term tailVar = HVM4Runtime::termNewVar(tailLamLoc);
    Term keyVar = HVM4Runtime::termNewVar(keyLamLoc);
    Term valVar = HVM4Runtime::termNewVar(valLamLoc);

    // === Build the recursive call: self self tail ===
    uint32_t selfDupLabel = ctx.freshLabel();
    uint32_t selfDupLoc = ctx.allocate(2);
    Term selfRef0 = HVM4Runtime::termNewCo0(selfDupLabel, selfDupLoc);
    Term selfRef1 = HVM4Runtime::termNewCo1(selfDupLabel, selfDupLoc);
    Term selfSelfInner = ctx.runtime().termNewApp(selfRef0, selfRef1);
    Term recursiveCall = ctx.runtime().termNewApp(selfSelfInner, tailVar);

    // === Build the attr handler (innermost) ===
    // λkey. λval. if (key == targetKey) then #Som{val} else (self self tail)
    Term keyMatch = ctx.runtime().termNewEql(keyVar, targetKey);

    // Create #Som{val}
    Term somVal = ctx.runtime().termNewCtr(CTR_SOM, 1, &valVar);
    Term returnSomLam = ctx.runtime().termNewLam(somVal);

    // MAT(0, recursiveCall, λ_. #Som{val}) keyMatch
    Term condMat = ctx.runtime().termNewMat(0, recursiveCall, returnSomLam);
    Term conditionalResult = ctx.runtime().termNewApp(condMat, keyMatch);

    // Build curried lambda: λkey. λval. conditionalResult
    Term valLam = ctx.runtime().finalizeLam(valLamLoc, conditionalResult);
    Term keyLam = ctx.runtime().finalizeLam(keyLamLoc, valLam);

    // === Build MAT for #Atr{key, val} ===
    Term attrIfNotMatch = ctx.runtime().termNewLam(nonResult);
    Term attrMat = ctx.runtime().termNewMat(CTR_ATR, keyLam, attrIfNotMatch);

    // === Build the cons handler ===
    Term checkHead = ctx.runtime().termNewApp(attrMat, headVar);
    Term tailLam = ctx.runtime().finalizeLam(tailLamLoc, checkHead);
    Term headLam = ctx.runtime().finalizeLam(headLamLoc, tailLam);

    // === Build the nil handler ===
    Term nilIfNotMatch = ctx.runtime().termNewLam(nonResult);

    // === Build MAT for #Con{head, tail} ===
    Term consMat = ctx.runtime().termNewMat(CTR_CON, headLam, nilIfNotMatch);

    // === Build: λlist. consMat list ===
    Term listBody = ctx.runtime().termNewApp(consMat, listVar);
    Term listLam = ctx.runtime().finalizeLam(listLamLoc, listBody);

    // === Build: λself. listLam ===
    Term dupBody = ctx.runtime().termNewDupAt(selfDupLabel, selfDupLoc, selfVar, listLam);
    Term selfLam = ctx.runtime().finalizeLam(selfLamLoc, dupBody);

    // === Apply: selfLam selfLam spine ===
    uint32_t outerDupLabel = ctx.freshLabel();
    uint32_t outerDupLoc = ctx.allocate(2);
    Term outerRef0 = HVM4Runtime::termNewCo0(outerDupLabel, outerDupLoc);
    Term outerRef1 = HVM4Runtime::termNewCo1(outerDupLabel, outerDupLoc);
    Term selfSelfOuter = ctx.runtime().termNewApp(outerRef0, outerRef1);
    Term searchSpine = ctx.runtime().termNewApp(selfSelfOuter, spine);

    return ctx.runtime().termNewDupAt(outerDupLabel, outerDupLoc, selfLam, searchSpine);
}

// ============================================================================
// Has-Attribute Check
// ============================================================================

Term HVM4Compiler::emitOpHasAttr(const ExprOpHasAttr& e, CompileContext& ctx) {
    // Compile has-attr check: attrs ? attr
    // Returns 1 (true) if attribute exists, 0 (false) otherwise
    //
    // Attrs are wrapped: #Ats{spine}
    // Structure: MAT(CTR_ATS, λspine. searchSpine(spine, key), 0) attrs

    Term attrs = emit(*e.e, ctx);

    // Get the attribute name's symbol ID
    const AttrName& attrName = e.attrPath[0];
    uint32_t symbolId = attrName.symbol.getId();
    Term keyTerm = HVM4Runtime::termNewNum(symbolId);

    // Allocate lambda slot for the extracted spine
    uint32_t spineLamLoc = ctx.runtime().allocateLamSlot();
    Term spineVar = HVM4Runtime::termNewVar(spineLamLoc);

    // Build the spine check code
    Term checkCode = emitSpineHasAttr(spineVar, keyTerm, ctx);

    // Wrap in lambda
    Term checkLam = ctx.runtime().finalizeLam(spineLamLoc, checkCode);
    Term zero = HVM4Runtime::termNewNum(0);  // Not an attrs -> false

    // MAT on CTR_ATS to extract spine and check it
    Term mat = ctx.runtime().termNewMat(CTR_ATS, checkLam, zero);
    return ctx.runtime().termNewApp(mat, attrs);
}

// Helper: check if attrs has the given symbolId (returns 1/0)
Term HVM4Compiler::emitOpHasAttrInternal(Term attrs, uint32_t symbolId, CompileContext& ctx) {
    // Attrs are wrapped: #Ats{spine}
    // Structure: MAT(CTR_ATS, λspine. searchSpine(spine, key), 0) attrs

    Term keyTerm = HVM4Runtime::termNewNum(symbolId);

    // Allocate lambda slot for the extracted spine
    uint32_t spineLamLoc = ctx.runtime().allocateLamSlot();
    Term spineVar = HVM4Runtime::termNewVar(spineLamLoc);

    // Build the spine check code
    Term checkCode = emitSpineHasAttr(spineVar, keyTerm, ctx);

    // Wrap in lambda
    Term checkLam = ctx.runtime().finalizeLam(spineLamLoc, checkCode);
    Term zero = HVM4Runtime::termNewNum(0);  // Not an attrs -> false

    // MAT on CTR_ATS to extract spine and check it
    Term mat = ctx.runtime().termNewMat(CTR_ATS, checkLam, zero);
    return ctx.runtime().termNewApp(mat, attrs);
}

Term HVM4Compiler::emitSpineHasAttr(Term spine, Term targetKey, CompileContext& ctx) {
    // Check if an attribute exists in a cons-list spine
    // Returns 1 if found, 0 if not
    //
    // Similar to emitSpineSearch but returns 1/0 instead of value/ERA
    //
    // searchFactory = λself. λlist.
    //   MAT(CTR_CON,
    //       λhead. λtail.
    //           MAT(CTR_ATR,
    //               λkey. λval. if key == targetKey then 1 else (self self tail),
    //               λx. 0
    //           ) head,
    //       λx. 0
    //   ) list
    // Then: (searchFactory searchFactory) spine

    Term zero = HVM4Runtime::termNewNum(0);  // Not found
    Term one = HVM4Runtime::termNewNum(1);   // Found

    // === Allocate all lambda slots upfront ===
    uint32_t selfLamLoc = ctx.runtime().allocateLamSlot();   // λself
    uint32_t listLamLoc = ctx.runtime().allocateLamSlot();   // λlist
    uint32_t headLamLoc = ctx.runtime().allocateLamSlot();   // λhead
    uint32_t tailLamLoc = ctx.runtime().allocateLamSlot();   // λtail
    uint32_t keyLamLoc = ctx.runtime().allocateLamSlot();    // λkey
    uint32_t valLamLoc = ctx.runtime().allocateLamSlot();    // λval

    // === Create variable references ===
    Term selfVar = HVM4Runtime::termNewVar(selfLamLoc);
    Term listVar = HVM4Runtime::termNewVar(listLamLoc);
    Term headVar = HVM4Runtime::termNewVar(headLamLoc);
    Term tailVar = HVM4Runtime::termNewVar(tailLamLoc);
    Term keyVar = HVM4Runtime::termNewVar(keyLamLoc);

    // === Build the recursive call: self self tail ===
    uint32_t selfDupLabel = ctx.freshLabel();
    uint32_t selfDupLoc = ctx.allocate(2);
    Term selfRef0 = HVM4Runtime::termNewCo0(selfDupLabel, selfDupLoc);
    Term selfRef1 = HVM4Runtime::termNewCo1(selfDupLabel, selfDupLoc);
    Term selfSelfInner = ctx.runtime().termNewApp(selfRef0, selfRef1);
    Term recursiveCall = ctx.runtime().termNewApp(selfSelfInner, tailVar);

    // === Build the attr handler (innermost) ===
    // λkey. λval. if (key == targetKey) then 1 else (self self tail)
    Term keyMatch = ctx.runtime().termNewEql(keyVar, targetKey);
    Term returnOneLam = ctx.runtime().termNewLam(one);
    // MAT(0, recursiveCall, λ_. 1) keyMatch
    Term condMat = ctx.runtime().termNewMat(0, recursiveCall, returnOneLam);
    Term conditionalResult = ctx.runtime().termNewApp(condMat, keyMatch);

    // Build curried lambda: λkey. λval. conditionalResult
    Term valLam = ctx.runtime().finalizeLam(valLamLoc, conditionalResult);
    Term keyLam = ctx.runtime().finalizeLam(keyLamLoc, valLam);

    // === Build MAT for #Atr{key, val} ===
    Term attrIfNotMatch = ctx.runtime().termNewLam(zero);
    Term attrMat = ctx.runtime().termNewMat(CTR_ATR, keyLam, attrIfNotMatch);

    // === Build the cons handler ===
    Term checkHead = ctx.runtime().termNewApp(attrMat, headVar);
    Term tailLam = ctx.runtime().finalizeLam(tailLamLoc, checkHead);
    Term headLam = ctx.runtime().finalizeLam(headLamLoc, tailLam);

    // === Build the nil handler ===
    Term nilIfNotMatch = ctx.runtime().termNewLam(zero);

    // === Build MAT for #Con{head, tail} ===
    Term consMat = ctx.runtime().termNewMat(CTR_CON, headLam, nilIfNotMatch);

    // === Build: λlist. consMat list ===
    Term listBody = ctx.runtime().termNewApp(consMat, listVar);
    Term listLam = ctx.runtime().finalizeLam(listLamLoc, listBody);

    // === Build: λself. listLam ===
    Term dupBody = ctx.runtime().termNewDupAt(selfDupLabel, selfDupLoc, selfVar, listLam);
    Term selfLam = ctx.runtime().finalizeLam(selfLamLoc, dupBody);

    // === Apply: selfLam selfLam spine ===
    uint32_t outerDupLabel = ctx.freshLabel();
    uint32_t outerDupLoc = ctx.allocate(2);
    Term outerRef0 = HVM4Runtime::termNewCo0(outerDupLabel, outerDupLoc);
    Term outerRef1 = HVM4Runtime::termNewCo1(outerDupLabel, outerDupLoc);
    Term selfSelfOuter = ctx.runtime().termNewApp(outerRef0, outerRef1);
    Term searchSpine = ctx.runtime().termNewApp(selfSelfOuter, spine);

    return ctx.runtime().termNewDupAt(outerDupLabel, outerDupLoc, selfLam, searchSpine);
}

}  // namespace nix::hvm4
