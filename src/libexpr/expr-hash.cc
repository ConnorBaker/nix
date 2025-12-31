#include "nix/expr/expr-hash.hh"

#include "nix/expr/nixexpr.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace nix {

namespace {

/**
 * Type tags for each expression type.
 * These ensure different expression types produce different hashes
 * even if they have similar content.
 */
enum class ExprTag : uint8_t {
    Int = 1,
    Float = 2,
    String = 3,
    Path = 4,
    Var = 5,
    Select = 6,
    OpHasAttr = 7,
    Attrs = 8,
    List = 9,
    Lambda = 10,
    Call = 11,
    Let = 12,
    With = 13,
    If = 14,
    Assert = 15,
    OpNot = 16,
    OpEq = 17,
    OpNEq = 18,
    OpAnd = 19,
    OpOr = 20,
    OpImpl = 21,
    OpUpdate = 22,
    OpConcatLists = 23,
    ConcatStrings = 24,
    Pos = 25,
    BlackHole = 26,
    InheritFrom = 27,
};

/**
 * Convert a 32-bit integer to little-endian byte order.
 * This ensures consistent hashes across big-endian and little-endian machines.
 */
inline uint32_t toLittleEndian32(uint32_t v)
{
    if constexpr (std::endian::native == std::endian::big) {
        return ((v & 0xFF000000) >> 24) | ((v & 0x00FF0000) >> 8) | ((v & 0x0000FF00) << 8)
            | ((v & 0x000000FF) << 24);
    }
    return v;
}

/**
 * Convert a 64-bit integer to little-endian byte order.
 * This ensures consistent hashes across big-endian and little-endian machines.
 */
inline uint64_t toLittleEndian64(uint64_t v)
{
    if constexpr (std::endian::native == std::endian::big) {
        return ((v & 0xFF00000000000000ULL) >> 56) | ((v & 0x00FF000000000000ULL) >> 40)
            | ((v & 0x0000FF0000000000ULL) >> 24) | ((v & 0x000000FF00000000ULL) >> 8)
            | ((v & 0x00000000FF000000ULL) << 8) | ((v & 0x0000000000FF0000ULL) << 24)
            | ((v & 0x000000000000FF00ULL) << 40) | ((v & 0x00000000000000FFULL) << 56);
    }
    return v;
}

/**
 * Helper class for computing expression hashes with cycle detection.
 */
class ExprHasher
{
    const SymbolTable & symbols;
    std::vector<const Expr *> ancestors;
    ExprHashCache * cache;

    /**
     * Feed raw bytes into a HashSink.
     */
    static void feedBytes(HashSink & sink, const void * data, size_t size)
    {
        sink({reinterpret_cast<const char *>(data), size});
    }

    /**
     * Feed a type tag into the hash.
     */
    static void feedTag(HashSink & sink, ExprTag tag)
    {
        uint8_t t = static_cast<uint8_t>(tag);
        feedBytes(sink, &t, sizeof(t));
    }

    /**
     * Feed a string_view into the hash (length-prefixed for unambiguous parsing).
     * Length is encoded in little-endian for cross-machine stability.
     */
    static void feedString(HashSink & sink, std::string_view s)
    {
        uint64_t len = toLittleEndian64(s.size());
        feedBytes(sink, &len, sizeof(len));
        sink(s);
    }

    /**
     * Feed a Symbol's string bytes into the hash.
     */
    void feedSymbol(HashSink & sink, Symbol sym)
    {
        if (sym) {
            std::string_view sv = symbols[sym];
            feedString(sink, sv);
        } else {
            // Empty/null symbol
            feedString(sink, "");
        }
    }

    /**
     * Feed a ContentHash into a HashSink.
     */
    static void feedHash(HashSink & sink, const ContentHash & h)
    {
        feedBytes(sink, h.data(), h.size());
    }

    /**
     * Feed a uint32_t into the hash in little-endian format.
     */
    static void feedUInt32(HashSink & sink, uint32_t v)
    {
        uint32_t le = toLittleEndian32(v);
        feedBytes(sink, &le, sizeof(le));
    }

    /**
     * Feed a uint64_t into the hash in little-endian format.
     */
    static void feedUInt64(HashSink & sink, uint64_t v)
    {
        uint64_t le = toLittleEndian64(v);
        feedBytes(sink, &le, sizeof(le));
    }

    /**
     * Feed a bool into the hash.
     */
    static void feedBool(HashSink & sink, bool v)
    {
        uint8_t b = v ? 1 : 0;
        feedBytes(sink, &b, sizeof(b));
    }

    /**
     * Compute a content-based fingerprint for a path.
     *
     * This uses the SourceAccessor to compute a hash that is stable across
     * machines (same content = same hash, regardless of absolute path).
     *
     * Strategy:
     * 1. Try getFingerprint() - returns accessor fingerprint + relative path if available
     * 2. Fall back to hashPath() - computes SHA256 of actual file/directory contents
     * 3. If both fail (e.g., path doesn't exist), use the raw path string with a marker
     *
     * @param accessor The source accessor for the path
     * @param pathStr The path string within the accessor
     * @return A content-based fingerprint
     */
    static void feedPathFingerprint(HashSink & sink, SourceAccessor & accessor, std::string_view pathStr)
    {
        CanonPath canonPath(pathStr);

        // First, try getFingerprint() - this is fast if the accessor has a known fingerprint
        auto [fingerprintPath, maybeFingerprint] = accessor.getFingerprint(canonPath);
        if (maybeFingerprint) {
            // Use accessor fingerprint + relative path within accessor
            uint8_t marker = 0x01; // Fingerprint-based
            feedBytes(sink, &marker, 1);
            feedString(sink, *maybeFingerprint);
            feedString(sink, fingerprintPath.rel());
            return;
        }

        // Second, try hashPath() - computes content hash
        // This is slower but provides true content-addressability
        try {
            if (accessor.pathExists(canonPath)) {
                Hash contentHash = accessor.hashPath(canonPath);
                uint8_t marker = 0x02; // Content hash-based
                feedBytes(sink, &marker, 1);
                feedBytes(sink, contentHash.hash, contentHash.hashSize);
                return;
            }
        } catch (...) {
            // Path doesn't exist or can't be hashed - fall through to raw path
        }

        // Fallback: use raw path string with a distinct marker
        // WARNING: This is NOT cross-machine stable!
        uint8_t marker = 0x00; // Raw path (not portable)
        feedBytes(sink, &marker, 1);
        feedString(sink, pathStr);
    }

    /**
     * Check if an expression is already in the ancestor stack (cycle detection).
     * Returns the depth if found, or -1 if not found.
     */
    ssize_t findInAncestors(const Expr * e) const
    {
        for (size_t i = 0; i < ancestors.size(); ++i) {
            if (ancestors[i] == e) {
                return static_cast<ssize_t>(ancestors.size() - 1 - i);
            }
        }
        return -1;
    }

    /**
     * Hash an attribute path (for ExprSelect and ExprOpHasAttr).
     * Each element is either a Symbol or an Expr (for dynamic attrs).
     */
    void hashAttrPath(HashSink & sink, std::span<const AttrName> attrPath)
    {
        uint64_t size = attrPath.size();
        feedUInt64(sink, size);
        for (const auto & attr : attrPath) {
            if (attr.expr) {
                // Dynamic attribute - hash as expression
                feedBool(sink, true); // isDynamic
                ContentHash h = hashExprImpl(attr.expr);
                feedHash(sink, h);
            } else {
                // Static attribute - hash symbol string bytes
                feedBool(sink, false); // isDynamic
                feedSymbol(sink, attr.symbol);
            }
        }
    }

    /**
     * Compute the withDepth for an ExprVar.
     * This is the number of parentWith hops from fromWith to nullptr.
     */
    static uint32_t computeWithDepth(const ExprVar * var)
    {
        uint32_t depth = 0;
        ExprWith * w = var->fromWith;
        while (w) {
            depth++;
            w = w->parentWith;
        }
        return depth;
    }

    /**
     * Main recursive implementation.
     */
    ContentHash hashExprImpl(const Expr * e)
    {
        if (!e) {
            // Null expression - return placeholder
            return ContentHash::placeholder();
        }

        // Cache lookup: if we've already computed this expression's hash, return it.
        // Note: We only cache at the top level (ancestors.empty()) because:
        // - Sub-expressions during recursive hashing may have different cycle context
        // - The cache stores complete hashes, not partial results
        // However, for performance, we cache all completed hashes since expressions
        // are immutable and their hashes don't depend on the ancestor stack
        // (the ancestor stack is only for cycle detection within a single hash computation).
        if (cache) {
            auto it = cache->find(e);
            if (it != cache->end()) {
                return it->second;
            }
        }

        // Cycle detection: check if we're already hashing this expression
        ssize_t depth = findInAncestors(e);
        if (depth >= 0) {
            return ContentHash::backRef(static_cast<size_t>(depth));
        }

        // Push onto ancestor stack
        ancestors.push_back(e);

        HashSink sink(evalHashAlgo);
        ContentHash result;

        // Dispatch based on expression type using dynamic_cast
        // This is not the most efficient approach but is correct and maintainable

        if (auto * expr = dynamic_cast<const ExprInt *>(e)) {
            feedTag(sink, ExprTag::Int);
            // Convert to little-endian for cross-machine stability
            feedUInt64(sink, static_cast<uint64_t>(expr->v.integer().value));
        } else if (auto * expr = dynamic_cast<const ExprFloat *>(e)) {
            feedTag(sink, ExprTag::Float);
            // Canonicalize float before hashing to ensure equivalent values hash identically:
            // - All NaN variants → single quiet NaN
            // - -0.0 → +0.0
            double val = expr->v.fpoint();
            if (std::isnan(val)) {
                val = std::numeric_limits<double>::quiet_NaN();
            } else if (val == 0.0) {
                val = 0.0;  // Canonicalize -0.0 to +0.0
            }
            // Bit-cast float to uint64 and convert to little-endian for cross-machine stability
            uint64_t bits;
            static_assert(sizeof(val) == sizeof(bits));
            std::memcpy(&bits, &val, sizeof(bits));
            feedUInt64(sink, bits);
        } else if (auto * expr = dynamic_cast<const ExprString *>(e)) {
            feedTag(sink, ExprTag::String);
            // Hash string content, NOT context
            feedString(sink, expr->v.string_view());
        } else if (auto * expr = dynamic_cast<const ExprPath *>(e)) {
            feedTag(sink, ExprTag::Path);
            // Use content-based path fingerprinting for cross-machine stability
            feedPathFingerprint(sink, *expr->accessor, expr->v.pathStrView());
        } else if (auto * expr = dynamic_cast<const ExprInheritFrom *>(e)) {
            // ExprInheritFrom is a subclass of ExprVar, check it first
            feedTag(sink, ExprTag::InheritFrom);
            feedUInt32(sink, expr->level);
            feedUInt32(sink, expr->displ);
        } else if (auto * expr = dynamic_cast<const ExprVar *>(e)) {
            feedTag(sink, ExprTag::Var);
            feedBool(sink, expr->fromWith != nullptr);
            if (expr->fromWith) {
                // For with-bound variables, we MUST hash the variable name because:
                // - De Bruijn indices (level, displ) identify WHICH with scope to search
                // - The variable name identifies WHAT to look up in that scope
                // Without the name, `with {x=1;y=2;}; x` and `y` would hash identically!
                feedSymbol(sink, expr->name);
                uint32_t withDepth = computeWithDepth(expr);
                feedUInt32(sink, withDepth);
            } else {
                // For lexically-bound variables, De Bruijn indices are sufficient
                // since the position uniquely identifies the binding
                feedUInt32(sink, expr->level);
                feedUInt32(sink, expr->displ);
            }
        } else if (auto * expr = dynamic_cast<const ExprSelect *>(e)) {
            feedTag(sink, ExprTag::Select);
            // Hash base expression
            feedHash(sink, hashExprImpl(expr->e));
            // Hash attribute path
            hashAttrPath(sink, expr->getAttrPath());
            // Hash default expression if present
            feedBool(sink, expr->def != nullptr);
            if (expr->def) {
                feedHash(sink, hashExprImpl(expr->def));
            }
        } else if (auto * expr = dynamic_cast<const ExprOpHasAttr *>(e)) {
            feedTag(sink, ExprTag::OpHasAttr);
            // Hash base expression
            feedHash(sink, hashExprImpl(expr->e));
            // Hash attribute path
            hashAttrPath(sink, expr->attrPath);
        } else if (auto * expr = dynamic_cast<const ExprAttrs *>(e)) {
            feedTag(sink, ExprTag::Attrs);
            feedBool(sink, expr->recursive);

            // Sort attributes by their string name for deterministic ordering
            std::vector<std::pair<std::string_view, const ExprAttrs::AttrDef *>> sortedAttrs;
            if (expr->attrs) {
                for (const auto & [sym, def] : *expr->attrs) {
                    sortedAttrs.emplace_back(symbols[sym], &def);
                }
            }
            std::sort(sortedAttrs.begin(), sortedAttrs.end(), [](const auto & a, const auto & b) {
                return a.first < b.first;
            });

            // Hash sorted attributes
            feedUInt64(sink, sortedAttrs.size());
            for (const auto & [name, def] : sortedAttrs) {
                feedString(sink, name);
                feedHash(sink, hashExprImpl(def->e));
                // Also hash the kind (Plain, Inherited, InheritedFrom)
                uint8_t kind = static_cast<uint8_t>(def->kind);
                feedBytes(sink, &kind, sizeof(kind));
            }

            // Hash dynamic attributes (order matters here)
            if (expr->dynamicAttrs) {
                feedUInt64(sink, expr->dynamicAttrs->size());
                for (const auto & dyn : *expr->dynamicAttrs) {
                    feedHash(sink, hashExprImpl(dyn.nameExpr));
                    feedHash(sink, hashExprImpl(dyn.valueExpr));
                }
            } else {
                feedUInt64(sink, 0);
            }

            // Hash inheritFromExprs if present
            if (expr->inheritFromExprs) {
                feedUInt64(sink, expr->inheritFromExprs->size());
                for (const auto * ie : *expr->inheritFromExprs) {
                    feedHash(sink, hashExprImpl(ie));
                }
            } else {
                feedUInt64(sink, 0);
            }
        } else if (auto * expr = dynamic_cast<const ExprList *>(e)) {
            feedTag(sink, ExprTag::List);
            feedUInt64(sink, expr->elems.size());
            for (const auto * elem : expr->elems) {
                feedHash(sink, hashExprImpl(elem));
            }
        } else if (auto * expr = dynamic_cast<const ExprLambda *>(e)) {
            feedTag(sink, ExprTag::Lambda);
            // Hash formals structure (NOT names, since those are bound by position)
            if (auto formals = expr->getFormals()) {
                feedBool(sink, true); // hasFormals
                feedUInt64(sink, formals->formals.size());
                feedBool(sink, formals->ellipsis);
                // Sort formals by name string for deterministic ordering
                auto sorted = formals->lexicographicOrder(symbols);
                for (const auto & formal : sorted) {
                    feedSymbol(sink, formal.name);
                    feedBool(sink, formal.def != nullptr);
                    if (formal.def) {
                        feedHash(sink, hashExprImpl(formal.def));
                    }
                }
            } else {
                feedBool(sink, false); // hasFormals
            }
            // Hash whether there's a simple arg binding (like x: ...), but NOT the name
            // since x: x and y: y are alpha-equivalent.
            // The name is just a local binding, not part of the interface.
            feedBool(sink, static_cast<bool>(expr->arg));
            // Note: We intentionally do NOT hash expr->arg symbol name for alpha-equivalence
            // Hash body
            feedHash(sink, hashExprImpl(expr->body));
        } else if (auto * expr = dynamic_cast<const ExprCall *>(e)) {
            feedTag(sink, ExprTag::Call);
            feedHash(sink, hashExprImpl(expr->fun));
            if (expr->args) {
                feedUInt64(sink, expr->args->size());
                for (const auto * arg : *expr->args) {
                    feedHash(sink, hashExprImpl(arg));
                }
            } else {
                feedUInt64(sink, 0);
            }
        } else if (auto * expr = dynamic_cast<const ExprLet *>(e)) {
            feedTag(sink, ExprTag::Let);
            feedHash(sink, hashExprImpl(expr->attrs));
            feedHash(sink, hashExprImpl(expr->body));
        } else if (auto * expr = dynamic_cast<const ExprWith *>(e)) {
            feedTag(sink, ExprTag::With);
            feedHash(sink, hashExprImpl(expr->attrs));
            feedHash(sink, hashExprImpl(expr->body));
        } else if (auto * expr = dynamic_cast<const ExprIf *>(e)) {
            feedTag(sink, ExprTag::If);
            feedHash(sink, hashExprImpl(expr->cond));
            feedHash(sink, hashExprImpl(expr->then));
            feedHash(sink, hashExprImpl(expr->else_));
        } else if (auto * expr = dynamic_cast<const ExprAssert *>(e)) {
            feedTag(sink, ExprTag::Assert);
            feedHash(sink, hashExprImpl(expr->cond));
            feedHash(sink, hashExprImpl(expr->body));
        } else if (auto * expr = dynamic_cast<const ExprOpNot *>(e)) {
            feedTag(sink, ExprTag::OpNot);
            feedHash(sink, hashExprImpl(expr->e));
        } else if (auto * expr = dynamic_cast<const ExprOpEq *>(e)) {
            feedTag(sink, ExprTag::OpEq);
            feedHash(sink, hashExprImpl(expr->e1));
            feedHash(sink, hashExprImpl(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpNEq *>(e)) {
            feedTag(sink, ExprTag::OpNEq);
            feedHash(sink, hashExprImpl(expr->e1));
            feedHash(sink, hashExprImpl(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpAnd *>(e)) {
            feedTag(sink, ExprTag::OpAnd);
            feedHash(sink, hashExprImpl(expr->e1));
            feedHash(sink, hashExprImpl(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpOr *>(e)) {
            feedTag(sink, ExprTag::OpOr);
            feedHash(sink, hashExprImpl(expr->e1));
            feedHash(sink, hashExprImpl(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpImpl *>(e)) {
            feedTag(sink, ExprTag::OpImpl);
            feedHash(sink, hashExprImpl(expr->e1));
            feedHash(sink, hashExprImpl(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpUpdate *>(e)) {
            feedTag(sink, ExprTag::OpUpdate);
            feedHash(sink, hashExprImpl(expr->e1));
            feedHash(sink, hashExprImpl(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpConcatLists *>(e)) {
            feedTag(sink, ExprTag::OpConcatLists);
            feedHash(sink, hashExprImpl(expr->e1));
            feedHash(sink, hashExprImpl(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprConcatStrings *>(e)) {
            feedTag(sink, ExprTag::ConcatStrings);
            feedBool(sink, expr->forceString);
            feedUInt64(sink, expr->es.size());
            for (const auto & [pos, subExpr] : expr->es) {
                feedHash(sink, hashExprImpl(subExpr));
            }
        } else if (auto * expr = dynamic_cast<const ExprPos *>(e)) {
            // ExprPos represents __curPos - it evaluates to position info at the call site.
            // Different __curPos expressions at different locations MUST hash differently
            // since they return different values.
            //
            // We hash the PosIdx value, which is stable within the same parse session.
            // NOTE: This is NOT cross-evaluation stable because:
            //   1. PosIdx values depend on parse order
            //   2. The underlying file paths are machine-specific
            //
            // For cross-evaluation caching, __curPos usage should be tracked as
            // "position-dependent" and excluded from caching.
            feedTag(sink, ExprTag::Pos);
            PosIdx posIdx = expr->getPos();
            // Hash the position index - this distinguishes different __curPos call sites
            // within the same evaluation, even though it's not cross-evaluation stable.
            // Use PosIdx::hash() to get a hashable representation.
            size_t posHash = posIdx.hash();
            feedUInt64(sink, static_cast<uint64_t>(posHash));
        } else if (dynamic_cast<const ExprBlackHole *>(e)) {
            // Black hole - represents infinite recursion
            feedTag(sink, ExprTag::BlackHole);
        } else {
            // Unknown expression type (e.g., ExprParseFile which is private to eval.cc)
            // Hash by pointer address to make them unique within this evaluation.
            // This is NOT cross-evaluation stable, but prevents hash collisions.
            feedTag(sink, static_cast<ExprTag>(255));
            auto ptr = reinterpret_cast<uintptr_t>(e);
            feedUInt64(sink, static_cast<uint64_t>(ptr));
        }

        auto hashResult = sink.finish();
        result = ContentHash{hashResult.hash};

        // Pop from ancestor stack
        ancestors.pop_back();

        // Store in cache for future lookups
        if (cache) {
            cache->emplace(e, result);
        }

        return result;
    }

    /**
     * Compute portability for an expression tree.
     * Returns the least portable classification found.
     */
    HashPortability computeExprPortability(const Expr * e)
    {
        if (!e) {
            return HashPortability::Portable;
        }

        // Check for cycles - back-refs are portable
        if (findInAncestors(e) >= 0) {
            return HashPortability::Portable;
        }

        ancestors.push_back(e);
        HashPortability result = HashPortability::Portable;

        // Check each expression type for non-portable components
        if (dynamic_cast<const ExprPos *>(e)) {
            // ExprPos uses PosIdx::hash() which is session-local
            result = HashPortability::NonPortable_SessionLocal;
        } else if (auto * expr = dynamic_cast<const ExprPath *>(e)) {
            // Check if path would use raw fallback
            CanonPath canonPath(expr->v.pathStrView());
            auto [fingerprintPath, maybeFingerprint] = expr->accessor->getFingerprint(canonPath);
            if (maybeFingerprint) {
                result = HashPortability::Portable;
            } else {
                // Try hashPath to see if it would succeed
                try {
                    if (expr->accessor->pathExists(canonPath)) {
                        expr->accessor->hashPath(canonPath);
                        result = HashPortability::Portable;
                    } else {
                        result = HashPortability::NonPortable_RawPath;
                    }
                } catch (...) {
                    result = HashPortability::NonPortable_RawPath;
                }
            }
        } else if (auto * expr = dynamic_cast<const ExprSelect *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e));
            for (const auto & attr : expr->getAttrPath()) {
                if (attr.expr) {
                    result = combinePortability(result, computeExprPortability(attr.expr));
                }
            }
            if (expr->def) {
                result = combinePortability(result, computeExprPortability(expr->def));
            }
        } else if (auto * expr = dynamic_cast<const ExprOpHasAttr *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e));
            for (const auto & attr : expr->attrPath) {
                if (attr.expr) {
                    result = combinePortability(result, computeExprPortability(attr.expr));
                }
            }
        } else if (auto * expr = dynamic_cast<const ExprAttrs *>(e)) {
            if (expr->attrs) {
                for (const auto & [sym, def] : *expr->attrs) {
                    result = combinePortability(result, computeExprPortability(def.e));
                    if (!isPortable(result)) break;
                }
            }
            if (isPortable(result) && expr->dynamicAttrs) {
                for (const auto & dyn : *expr->dynamicAttrs) {
                    result = combinePortability(result, computeExprPortability(dyn.nameExpr));
                    result = combinePortability(result, computeExprPortability(dyn.valueExpr));
                    if (!isPortable(result)) break;
                }
            }
            if (isPortable(result) && expr->inheritFromExprs) {
                for (const auto * ie : *expr->inheritFromExprs) {
                    result = combinePortability(result, computeExprPortability(ie));
                    if (!isPortable(result)) break;
                }
            }
        } else if (auto * expr = dynamic_cast<const ExprList *>(e)) {
            for (const auto * elem : expr->elems) {
                result = combinePortability(result, computeExprPortability(elem));
                if (!isPortable(result)) break;
            }
        } else if (auto * expr = dynamic_cast<const ExprLambda *>(e)) {
            if (auto formals = expr->getFormals()) {
                for (const auto & formal : formals->formals) {
                    if (formal.def) {
                        result = combinePortability(result, computeExprPortability(formal.def));
                        if (!isPortable(result)) break;
                    }
                }
            }
            if (isPortable(result)) {
                result = combinePortability(result, computeExprPortability(expr->body));
            }
        } else if (auto * expr = dynamic_cast<const ExprCall *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->fun));
            if (isPortable(result) && expr->args) {
                for (const auto * arg : *expr->args) {
                    result = combinePortability(result, computeExprPortability(arg));
                    if (!isPortable(result)) break;
                }
            }
        } else if (auto * expr = dynamic_cast<const ExprLet *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->attrs));
            result = combinePortability(result, computeExprPortability(expr->body));
        } else if (auto * expr = dynamic_cast<const ExprWith *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->attrs));
            result = combinePortability(result, computeExprPortability(expr->body));
        } else if (auto * expr = dynamic_cast<const ExprIf *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->cond));
            result = combinePortability(result, computeExprPortability(expr->then));
            result = combinePortability(result, computeExprPortability(expr->else_));
        } else if (auto * expr = dynamic_cast<const ExprAssert *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->cond));
            result = combinePortability(result, computeExprPortability(expr->body));
        } else if (auto * expr = dynamic_cast<const ExprOpNot *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e));
        } else if (auto * expr = dynamic_cast<const ExprOpEq *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e1));
            result = combinePortability(result, computeExprPortability(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpNEq *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e1));
            result = combinePortability(result, computeExprPortability(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpAnd *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e1));
            result = combinePortability(result, computeExprPortability(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpOr *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e1));
            result = combinePortability(result, computeExprPortability(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpImpl *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e1));
            result = combinePortability(result, computeExprPortability(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpUpdate *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e1));
            result = combinePortability(result, computeExprPortability(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprOpConcatLists *>(e)) {
            result = combinePortability(result, computeExprPortability(expr->e1));
            result = combinePortability(result, computeExprPortability(expr->e2));
        } else if (auto * expr = dynamic_cast<const ExprConcatStrings *>(e)) {
            for (const auto & [pos, subExpr] : expr->es) {
                result = combinePortability(result, computeExprPortability(subExpr));
                if (!isPortable(result)) break;
            }
        } else if (dynamic_cast<const ExprInt *>(e) || dynamic_cast<const ExprFloat *>(e)
                   || dynamic_cast<const ExprString *>(e) || dynamic_cast<const ExprVar *>(e)
                   || dynamic_cast<const ExprInheritFrom *>(e) || dynamic_cast<const ExprBlackHole *>(e)) {
            // These are portable
            result = HashPortability::Portable;
        } else {
            // Unknown expression type - uses pointer hashing, so non-portable
            result = HashPortability::NonPortable_Pointer;
        }

        ancestors.pop_back();
        return result;
    }

public:
    explicit ExprHasher(const SymbolTable & symbols, ExprHashCache * cache = nullptr)
        : symbols(symbols)
        , cache(cache)
    {
    }

    ContentHash hash(const Expr * e)
    {
        return hashExprImpl(e);
    }

    ContentHashResult hashWithPortability(const Expr * e)
    {
        ContentHash h = hashExprImpl(e);
        ancestors.clear();
        HashPortability p = computeExprPortability(e);
        return ContentHashResult{h, p};
    }
};

} // anonymous namespace

ContentHash hashExpr(const Expr * e, const SymbolTable & symbols, ExprHashCache * cache)
{
    ExprHasher hasher(symbols, cache);
    return hasher.hash(e);
}

ContentHashResult hashExprWithPortability(const Expr * e, const SymbolTable & symbols)
{
    ExprHasher hasher(symbols);
    return hasher.hashWithPortability(e);
}

} // namespace nix
