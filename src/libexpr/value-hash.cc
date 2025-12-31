#include "nix/expr/value-hash.hh"

#include "nix/expr/attr-set.hh"
#include "nix/expr/env-hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-hash.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace nix {

namespace {

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
 * Type tags for value hashing.
 * Each value type gets a unique tag to prevent hash collisions between types.
 */
enum class ValueTypeTag : uint8_t {
    Int = 0x01,
    Float = 0x02,
    Bool = 0x03,
    Null = 0x04,
    String = 0x05,
    Path = 0x06,
    Attrs = 0x07,
    List = 0x08,
    Lambda = 0x09,
    Thunk = 0x0A,
    External = 0x0B,
    PrimOp = 0x0C,
    PrimOpApp = 0x0D,
};

/**
 * Check if a value is already in the ancestors stack (cycle detection).
 * Returns the depth from the top of the stack if found, or -1 if not found.
 */
int findInAncestors(const Value * v, const std::vector<const Value *> & ancestors)
{
    // Search from the back (most recent) to find the shortest cycle
    for (size_t i = ancestors.size(); i > 0; --i) {
        if (ancestors[i - 1] == v) {
            return static_cast<int>(ancestors.size() - i);
        }
    }
    return -1;
}

/**
 * RAII helper to push/pop a value on the ancestors stack.
 */
class AncestorGuard
{
    std::vector<const Value *> & ancestors;

public:
    AncestorGuard(std::vector<const Value *> & ancestors, const Value * v)
        : ancestors(ancestors)
    {
        ancestors.push_back(v);
    }

    ~AncestorGuard()
    {
        ancestors.pop_back();
    }
};

/**
 * Hash an integer value.
 * Integer is encoded in little-endian for cross-machine stability.
 */
ContentHash hashInt(NixInt n)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Int);
    sink({reinterpret_cast<const char *>(&tag), 1});
    uint64_t val = toLittleEndian64(static_cast<uint64_t>(n.value));
    sink({reinterpret_cast<const char *>(&val), sizeof(val)});
    return ContentHash{sink.finish().hash};
}

/**
 * Canonicalize a float value for hashing.
 *
 * IEEE 754 floats have multiple bit patterns that are semantically equivalent:
 * - NaN: Many different bit patterns (quiet NaN, signaling NaN, payloads)
 * - Signed zero: +0.0 and -0.0 compare equal but have different bits
 *
 * This function normalizes these cases to ensure equivalent values hash identically.
 *
 * @param f The float to canonicalize
 * @return Canonicalized float value
 */
inline NixFloat canonicalizeFloat(NixFloat f)
{
    // Canonicalize NaN to a single quiet NaN representation
    // std::isnan is the portable way to detect all NaN variants
    if (std::isnan(f)) {
        return std::numeric_limits<NixFloat>::quiet_NaN();
    }

    // Canonicalize -0.0 to +0.0
    // -0.0 == +0.0 is true, but they have different bit patterns
    if (f == 0.0) {
        return 0.0;  // Always positive zero
    }

    return f;
}

/**
 * Hash a float value using its bit representation.
 * Float bits are encoded in little-endian for cross-machine stability.
 *
 * The float is canonicalized first to ensure equivalent values hash identically:
 * - All NaN variants → single quiet NaN
 * - -0.0 → +0.0
 */
ContentHash hashFloat(NixFloat f)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Float);
    sink({reinterpret_cast<const char *>(&tag), 1});

    // Canonicalize before hashing to ensure equivalent values hash identically
    NixFloat canonical = canonicalizeFloat(f);

    // Use bit representation for deterministic hashing of floats
    uint64_t bits;
    static_assert(sizeof(canonical) == sizeof(bits));
    std::memcpy(&bits, &canonical, sizeof(bits));
    bits = toLittleEndian64(bits);
    sink({reinterpret_cast<const char *>(&bits), sizeof(bits)});
    return ContentHash{sink.finish().hash};
}

/**
 * Hash a boolean value.
 */
ContentHash hashBool(bool b)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Bool);
    sink({reinterpret_cast<const char *>(&tag), 1});
    uint8_t val = b ? 1 : 0;
    sink({reinterpret_cast<const char *>(&val), 1});
    return ContentHash{sink.finish().hash};
}

/**
 * Hash the null value.
 */
ContentHash hashNull()
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Null);
    sink({reinterpret_cast<const char *>(&tag), 1});
    return ContentHash{sink.finish().hash};
}

/**
 * Hash a string value with its context.
 * Context entries are sorted by string bytes for determinism.
 * All lengths are encoded in little-endian for cross-machine stability.
 */
ContentHash hashString(const Value & v)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::String);
    sink({reinterpret_cast<const char *>(&tag), 1});

    // Hash the string content
    std::string_view str = v.string_view();
    uint64_t len = toLittleEndian64(str.size());
    sink({reinterpret_cast<const char *>(&len), sizeof(len)});
    sink({str.data(), str.size()});

    // Hash the context (sorted by string bytes)
    const auto * ctx = v.context();
    if (ctx && ctx->size() > 0) {
        // Collect context strings
        std::vector<std::string_view> contextStrings;
        contextStrings.reserve(ctx->size());
        for (const auto * entry : *ctx) {
            contextStrings.push_back(entry->view());
        }

        // Sort by string bytes
        std::sort(contextStrings.begin(), contextStrings.end());

        // Hash sorted context
        uint64_t ctxSize = toLittleEndian64(contextStrings.size());
        sink({reinterpret_cast<const char *>(&ctxSize), sizeof(ctxSize)});
        for (const auto & ctxStr : contextStrings) {
            uint64_t ctxLen = toLittleEndian64(ctxStr.size());
            sink({reinterpret_cast<const char *>(&ctxLen), sizeof(ctxLen)});
            sink({ctxStr.data(), ctxStr.size()});
        }
    } else {
        uint64_t zero = 0;  // Already 0, no need to convert
        sink({reinterpret_cast<const char *>(&zero), sizeof(zero)});
    }

    return ContentHash{sink.finish().hash};
}

/**
 * Hash a path value using content-based fingerprinting.
 *
 * Uses the SourceAccessor to compute a hash that is stable across machines
 * (same content = same hash, regardless of absolute path).
 *
 * Strategy:
 * 1. Try getFingerprint() - returns accessor fingerprint + relative path if available
 * 2. Fall back to hashPath() - computes SHA256 of actual file/directory contents
 * 3. If both fail (e.g., accessor is null or path doesn't exist), use raw path string
 */
ContentHash hashPath(const Value & v)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Path);
    sink({reinterpret_cast<const char *>(&tag), 1});

    std::string_view pathStr = v.pathStrView();
    SourceAccessor * accessor = v.pathAccessor();

    if (accessor) {
        CanonPath canonPath(pathStr);

        // First, try getFingerprint() - fast if the accessor has a known fingerprint
        auto [fingerprintPath, maybeFingerprint] = accessor->getFingerprint(canonPath);
        if (maybeFingerprint) {
            // Use accessor fingerprint + relative path within accessor
            uint8_t marker = 0x01; // Fingerprint-based
            sink({reinterpret_cast<const char *>(&marker), 1});

            // Hash the fingerprint string
            uint64_t fpLen = toLittleEndian64(maybeFingerprint->size());
            sink({reinterpret_cast<const char *>(&fpLen), sizeof(fpLen)});
            sink({maybeFingerprint->data(), maybeFingerprint->size()});

            // Hash the relative path within the accessor
            std::string relPath{fingerprintPath.rel()};
            uint64_t relLen = toLittleEndian64(relPath.size());
            sink({reinterpret_cast<const char *>(&relLen), sizeof(relLen)});
            sink({relPath.data(), relPath.size()});

            return ContentHash{sink.finish().hash};
        }

        // Second, try hashPath() - computes content hash
        try {
            if (accessor->pathExists(canonPath)) {
                Hash contentHash = accessor->hashPath(canonPath);
                uint8_t marker = 0x02; // Content hash-based
                sink({reinterpret_cast<const char *>(&marker), 1});
                sink({reinterpret_cast<const char *>(contentHash.hash), contentHash.hashSize});
                return ContentHash{sink.finish().hash};
            }
        } catch (...) {
            // Path doesn't exist or can't be hashed - fall through to raw path
        }
    }

    // Fallback: use raw path string with a distinct marker
    // WARNING: This is NOT cross-machine stable!
    uint8_t marker = 0x00; // Raw path (not portable)
    sink({reinterpret_cast<const char *>(&marker), 1});
    uint64_t len = toLittleEndian64(pathStr.size());
    sink({reinterpret_cast<const char *>(&len), sizeof(len)});
    sink({pathStr.data(), pathStr.size()});

    return ContentHash{sink.finish().hash};
}

/**
 * Check if a Symbol is valid in the given SymbolTable.
 * A symbol is valid if its ID is non-zero and within bounds of the table.
 */
inline bool isSymbolValid(Symbol sym, const SymbolTable & symbols)
{
    uint32_t id = sym.getId();
    // Symbol IDs are 1-indexed; 0 means empty/invalid symbol
    // The ID must be in range [1, symbols.size()]
    return id > 0 && id <= symbols.size();
}

/**
 * Hash an attribute set value.
 * Attributes are sorted by name (string bytes) for determinism.
 * All counts/lengths are encoded in little-endian for cross-machine stability.
 *
 * If the attributes contain symbols from a different SymbolTable (e.g., when
 * values are shared across EvalStates in the C API), this returns a pointer-based
 * placeholder hash to avoid crashes.
 */
ContentHash hashAttrs(
    const Value & v, const SymbolTable & symbols, std::vector<const Value *> & ancestors, ValueHashCache * cache)
{
    const Bindings * attrs = v.attrs();

    // Check if all attribute symbols are valid in this symbol table.
    // This can fail when values are shared across different EvalStates (C API usage).
    for (const Attr & attr : *attrs) {
        if (!isSymbolValid(attr.name, symbols)) {
            // Symbol mismatch - use a pointer-based fallback hash.
            // This is non-portable but safe (just causes cache misses).
            HashSink sink(evalHashAlgo);
            uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Attrs);
            sink({reinterpret_cast<const char *>(&tag), 1});
            auto ptr = reinterpret_cast<uintptr_t>(attrs);
            uint64_t ptrLE = toLittleEndian64(static_cast<uint64_t>(ptr));
            sink({reinterpret_cast<const char *>(&ptrLE), sizeof(ptrLE)});
            return ContentHash{sink.finish().hash};
        }
    }

    // Get attributes in lexicographic order (sorted by name string bytes)
    std::vector<const Attr *> sortedAttrs = attrs->lexicographicOrder(symbols);

    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Attrs);
    sink({reinterpret_cast<const char *>(&tag), 1});

    // Hash number of attributes
    uint64_t numAttrs = toLittleEndian64(sortedAttrs.size());
    sink({reinterpret_cast<const char *>(&numAttrs), sizeof(numAttrs)});

    // Hash each (name, value) pair
    for (const Attr * attr : sortedAttrs) {
        // Hash attribute name as string bytes
        std::string_view name = symbols[attr->name];
        uint64_t nameLen = toLittleEndian64(name.size());
        sink({reinterpret_cast<const char *>(&nameLen), sizeof(nameLen)});
        sink({name.data(), name.size()});

        // Recursively hash the value
        ContentHash valueHash = computeValueContentHash(*attr->value, symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(valueHash.data()), valueHash.size()});
    }

    return ContentHash{sink.finish().hash};
}

/**
 * Hash a list value.
 * Elements are hashed in order.
 * Element count is encoded in little-endian for cross-machine stability.
 */
ContentHash hashList(const Value & v, const SymbolTable & symbols, std::vector<const Value *> & ancestors, ValueHashCache * cache)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::List);
    sink({reinterpret_cast<const char *>(&tag), 1});

    auto list = v.listView();

    // Hash number of elements
    uint64_t numElems = toLittleEndian64(list.size());
    sink({reinterpret_cast<const char *>(&numElems), sizeof(numElems)});

    // Hash each element in order
    for (size_t i = 0; i < list.size(); ++i) {
        ContentHash elemHash = computeValueContentHash(*list[i], symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(elemHash.data()), elemHash.size()});
    }

    return ContentHash{sink.finish().hash};
}

/**
 * Hash a lambda/function value.
 *
 * Hashes the expression (stable across evaluations) and uses content-based
 * hashing for the environment (stable across evaluations and machines).
 *
 * The env is hashed using computeEnvStructuralHash, which hashes the entire
 * parent chain content-based. This avoids pointer-based hashing issues where
 * GC pointer reuse could cause hash collisions.
 */
ContentHash hashLambda(
    const Value & v,
    const SymbolTable & symbols,
    std::vector<const Value *> & ancestors,
    ValueHashCache * cache)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Lambda);
    sink({reinterpret_cast<const char *>(&tag), 1});

    auto lam = v.lambda();

    // Hash the expression (stable across evaluations)
    if (lam.fun) {
        uint8_t hasExpr = 1;
        sink({reinterpret_cast<const char *>(&hasExpr), 1});
        ContentHash exprHash = hashExpr(lam.fun, symbols);
        sink({reinterpret_cast<const char *>(exprHash.data()), exprHash.size()});
    } else {
        uint8_t hasExpr = 0;
        sink({reinterpret_cast<const char *>(&hasExpr), 1});
    }

    // Hash the environment content (stable across evaluations and machines).
    // Using content-based env hashing avoids issues with GC pointer reuse.
    if (lam.env) {
        uint8_t hasEnv = 1;
        sink({reinterpret_cast<const char *>(&hasEnv), 1});
        StructuralHash envHash = computeEnvStructuralHash(*lam.env, lam.env->size, symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(envHash.data()), envHash.size()});
    } else {
        uint8_t hasEnv = 0;
        sink({reinterpret_cast<const char *>(&hasEnv), 1});
    }

    return ContentHash{sink.finish().hash};
}

/**
 * Hash a primop (builtin function) value.
 *
 * Primops are hashed by their name, which is stable across evaluations.
 * This is cross-evaluation stable since primop names are part of the Nix language.
 */
ContentHash hashPrimOp(const Value & v)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::PrimOp);
    sink({reinterpret_cast<const char *>(&tag), 1});

    auto * primOp = v.primOp();

    // Hash the primop name (stable across evaluations)
    // Use uint64_t for consistency with other string length encoding
    if (primOp && !primOp->name.empty()) {
        uint64_t nameLen = toLittleEndian64(primOp->name.size());
        sink({reinterpret_cast<const char *>(&nameLen), sizeof(nameLen)});
        sink({primOp->name.data(), primOp->name.size()});
    } else {
        uint64_t nameLen = 0;
        sink({reinterpret_cast<const char *>(&nameLen), sizeof(nameLen)});
    }

    return ContentHash{sink.finish().hash};
}

/**
 * Hash a thunk value.
 *
 * Thunks are unevaluated expressions. We hash the expression (stable) and
 * use content-based hashing for the environment (stable across evaluations).
 *
 * The env is hashed using computeEnvStructuralHash, which hashes the entire
 * parent chain content-based. This avoids pointer-based hashing issues where
 * GC pointer reuse could cause hash collisions.
 */
ContentHash hashThunk(
    const Value & v,
    const SymbolTable & symbols,
    std::vector<const Value *> & ancestors,
    ValueHashCache * cache)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Thunk);
    sink({reinterpret_cast<const char *>(&tag), 1});

    auto thunk = v.thunk();

    // Hash the expression (stable across evaluations)
    if (thunk.expr) {
        uint8_t hasExpr = 1;
        sink({reinterpret_cast<const char *>(&hasExpr), 1});
        ContentHash exprHash = hashExpr(thunk.expr, symbols);
        sink({reinterpret_cast<const char *>(exprHash.data()), exprHash.size()});
    } else {
        uint8_t hasExpr = 0;
        sink({reinterpret_cast<const char *>(&hasExpr), 1});
    }

    // Hash the environment content (stable across evaluations and machines).
    // Using content-based env hashing avoids issues with GC pointer reuse.
    if (thunk.env) {
        uint8_t hasEnv = 1;
        sink({reinterpret_cast<const char *>(&hasEnv), 1});
        StructuralHash envHash = computeEnvStructuralHash(*thunk.env, thunk.env->size, symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(envHash.data()), envHash.size()});
    } else {
        uint8_t hasEnv = 0;
        sink({reinterpret_cast<const char *>(&hasEnv), 1});
    }

    return ContentHash{sink.finish().hash};
}

/**
 * Hash an external value.
 *
 * External values are arbitrary C++ objects from plugins. Without a content
 * hash hook from the external value itself, we cannot compute a stable content hash.
 *
 * We hash:
 *   1. The type name (for basic type discrimination)
 *   2. The pointer address (unique within this evaluation)
 *
 * WARNING: This is NOT cross-evaluation stable. External values should either:
 *   - Provide a content hash hook (not yet implemented)
 *   - Be excluded from persistent caching
 */
ContentHash hashExternal(const Value & v)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::External);
    sink({reinterpret_cast<const char *>(&tag), 1});

    // Hash the type name
    std::string typeName = v.external()->showType();
    uint64_t len = toLittleEndian64(typeName.size());
    sink({reinterpret_cast<const char *>(&len), sizeof(len)});
    sink({typeName.data(), typeName.size()});

    // Hash the pointer address as a unique identifier (NOT cross-evaluation stable!)
    auto extPtr = reinterpret_cast<uintptr_t>(v.external());
    uint64_t extPtrLE = toLittleEndian64(static_cast<uint64_t>(extPtr));
    sink({reinterpret_cast<const char *>(&extPtrLE), sizeof(extPtrLE)});

    return ContentHash{sink.finish().hash};
}

} // anonymous namespace

/**
 * Hash a function application thunk (tApp).
 *
 * Function applications are lazy: f x creates a thunk that will call f with x.
 * We hash the left (function) and right (argument) value pointers.
 *
 * NOTE: This uses recursive content hashing for the values.
 * This is within-evaluation stable but NOT cross-evaluation stable (due to pointer hashing in children).
 *
 * Must be defined outside anonymous namespace since it calls computeValueContentHash.
 */
static ContentHash hashApp(const Value & v, const SymbolTable & symbols, std::vector<const Value *> & ancestors, ValueHashCache * cache)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::Thunk); // Use Thunk tag since Apps are a type of thunk
    sink({reinterpret_cast<const char *>(&tag), 1});

    // Distinguish from regular thunks with a sub-tag
    uint8_t subTag = 0x01; // App sub-tag
    sink({reinterpret_cast<const char *>(&subTag), 1});

    auto app = v.app();

    // Hash the left (function) value
    if (app.left) {
        uint8_t hasLeft = 1;
        sink({reinterpret_cast<const char *>(&hasLeft), 1});
        // Recursively hash the function value
        ContentHash leftHash = computeValueContentHash(*app.left, symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(leftHash.data()), leftHash.size()});
    } else {
        uint8_t hasLeft = 0;
        sink({reinterpret_cast<const char *>(&hasLeft), 1});
    }

    // Hash the right (argument) value
    if (app.right) {
        uint8_t hasRight = 1;
        sink({reinterpret_cast<const char *>(&hasRight), 1});
        // Recursively hash the argument value
        ContentHash rightHash = computeValueContentHash(*app.right, symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(rightHash.data()), rightHash.size()});
    } else {
        uint8_t hasRight = 0;
        sink({reinterpret_cast<const char *>(&hasRight), 1});
    }

    return ContentHash{sink.finish().hash};
}

/**
 * Hash a primop application (partially applied builtin) value.
 *
 * This hashes the original primop name plus all applied arguments.
 * Must be defined outside anonymous namespace since it calls computeValueContentHash.
 */
static ContentHash hashPrimOpApp(const Value & v, const SymbolTable & symbols, std::vector<const Value *> & ancestors, ValueHashCache * cache)
{
    HashSink sink(evalHashAlgo);
    uint8_t tag = static_cast<uint8_t>(ValueTypeTag::PrimOpApp);
    sink({reinterpret_cast<const char *>(&tag), 1});

    auto primOpApp = v.primOpApp();

    // Get the original primop
    // Use uint64_t for consistency with other string length encoding
    const PrimOp * primOp = v.primOpAppPrimOp();
    if (primOp && !primOp->name.empty()) {
        uint64_t nameLen = toLittleEndian64(primOp->name.size());
        sink({reinterpret_cast<const char *>(&nameLen), sizeof(nameLen)});
        sink({primOp->name.data(), primOp->name.size()});
    } else {
        uint64_t nameLen = 0;
        sink({reinterpret_cast<const char *>(&nameLen), sizeof(nameLen)});
    }

    // Hash the left and right values (the applied arguments)
    // Use the public API from the header (recursive call)
    if (primOpApp.left) {
        uint8_t hasLeft = 1;
        sink({reinterpret_cast<const char *>(&hasLeft), 1});
        ContentHash leftHash = computeValueContentHash(*primOpApp.left, symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(leftHash.data()), leftHash.size()});
    } else {
        uint8_t hasLeft = 0;
        sink({reinterpret_cast<const char *>(&hasLeft), 1});
    }

    if (primOpApp.right) {
        uint8_t hasRight = 1;
        sink({reinterpret_cast<const char *>(&hasRight), 1});
        ContentHash rightHash = computeValueContentHash(*primOpApp.right, symbols, ancestors, cache);
        sink({reinterpret_cast<const char *>(rightHash.data()), rightHash.size()});
    } else {
        uint8_t hasRight = 0;
        sink({reinterpret_cast<const char *>(&hasRight), 1});
    }

    return ContentHash{sink.finish().hash};
}

ContentHash computeValueContentHash(
    const Value & v, const SymbolTable & symbols, std::vector<const Value *> & ancestors, ValueHashCache * cache)
{
    // Check if value is in a valid, hashable state.
    // Skip uninitialized values and blackholes (values currently being forced).
    if (!v.isValid() || v.isBlackhole()) {
        return ContentHash::placeholder();
    }

    // Cache lookup: if we've already computed this value's hash, return it.
    // This is safe because:
    // - Forced values are immutable (content never changes)
    // - Thunk/lambda values hash their (expr, env) which is stable
    if (cache) {
        auto it = cache->find(&v);
        if (it != cache->end()) {
            return it->second;
        }
    }

    // Cycle detection: check if this value is already being hashed
    int depth = findInAncestors(&v, ancestors);
    if (depth >= 0) {
        return ContentHash::backRef(static_cast<size_t>(depth));
    }

    // Push this value onto the ancestors stack
    AncestorGuard guard(ancestors, &v);

    ContentHash result;

    // Hash based on value type
    switch (v.type()) {
    case nInt:
        result = hashInt(v.integer());
        break;

    case nFloat:
        result = hashFloat(v.fpoint());
        break;

    case nBool:
        result = hashBool(v.boolean());
        break;

    case nNull:
        result = hashNull();
        break;

    case nString:
        result = hashString(v);
        break;

    case nPath:
        result = hashPath(v);
        break;

    case nAttrs:
        result = hashAttrs(v, symbols, ancestors, cache);
        break;

    case nList:
        result = hashList(v, symbols, ancestors, cache);
        break;

    case nFunction:
        // nFunction covers tLambda, tPrimOp, and tPrimOpApp - dispatch accordingly
        if (v.isPrimOp()) {
            result = hashPrimOp(v);
        } else if (v.isPrimOpApp()) {
            result = hashPrimOpApp(v, symbols, ancestors, cache);
        } else {
            // tLambda - actual lambda
            result = hashLambda(v, symbols, ancestors, cache);
        }
        break;

    case nThunk:
        // nThunk covers both tThunk (actual thunk) and tApp (function application)
        // They have different storage types and must be handled separately
        if (v.isThunk()) {
            result = hashThunk(v, symbols, ancestors, cache);
        } else if (v.isApp()) {
            result = hashApp(v, symbols, ancestors, cache);
        } else {
            // Shouldn't happen, but return placeholder just in case
            result = ContentHash::placeholder();
        }
        break;

    case nExternal:
        result = hashExternal(v);
        break;

    default:
        // Should be unreachable, but return a placeholder just in case
        result = ContentHash::placeholder();
        break;
    }

    // Store in cache for future lookups
    if (cache) {
        cache->emplace(&v, result);
    }

    return result;
}

ContentHash computeValueContentHash(const Value & v, const SymbolTable & symbols, ValueHashCache * cache)
{
    std::vector<const Value *> ancestors;
    return computeValueContentHash(v, symbols, ancestors, cache);
}

namespace {

/**
 * Determine portability of a path hash by mirroring the actual hash logic.
 *
 * This MUST follow the exact same code path as hashPath() to ensure
 * the portability flag matches the actual hash computed. If hashPath()
 * would use a fingerprint or content hash, this returns Portable.
 * If it would fall back to raw path string, this returns NonPortable_RawPath.
 */
HashPortability getPathPortability(const Value & v)
{
    SourceAccessor * accessor = v.pathAccessor();
    if (!accessor) {
        return HashPortability::NonPortable_RawPath;
    }

    std::string_view pathStr = v.pathStrView();
    CanonPath canonPath(pathStr);

    // Mirror hashPath() logic exactly:

    // 1. Try getFingerprint() - same as hashPath()
    auto [fingerprintPath, maybeFingerprint] = accessor->getFingerprint(canonPath);
    if (maybeFingerprint) {
        return HashPortability::Portable;
    }

    // 2. Try hashPath() - must actually succeed, not just pathExists()
    // hashPath() can throw even if pathExists() returns true (e.g., permission denied)
    try {
        if (accessor->pathExists(canonPath)) {
            // Actually try to hash the path, same as hashPath() does
            accessor->hashPath(canonPath);
            return HashPortability::Portable;
        }
    } catch (...) {
        // hashPath() failed - will fall back to raw path
    }

    return HashPortability::NonPortable_RawPath;
}

/**
 * Recursively compute portability for a value and its children.
 */
HashPortability computeValuePortability(
    const Value & v,
    const SymbolTable & symbols,
    std::vector<const Value *> & ancestors)
{
    // Check if value is in a valid, hashable state.
    // Uninitialized values and blackholes can't be meaningfully assessed for portability.
    if (!v.isValid() || v.isBlackhole()) {
        return HashPortability::Portable; // Placeholders are portable
    }

    // Cycle detection
    for (const Value * ancestor : ancestors) {
        if (ancestor == &v) {
            return HashPortability::Portable; // Back-refs are portable
        }
    }

    ancestors.push_back(&v);

    HashPortability result = HashPortability::Portable;

    switch (v.type()) {
    case nInt:
    case nFloat:
    case nBool:
    case nNull:
    case nString:
        // These are always portable
        result = HashPortability::Portable;
        break;

    case nPath:
        result = getPathPortability(v);
        break;

    case nAttrs: {
        const Bindings * attrs = v.attrs();
        for (const Attr & attr : *attrs) {
            result = combinePortability(result, computeValuePortability(*attr.value, symbols, ancestors));
            if (!isPortable(result))
                break; // Early exit once non-portable
        }
        break;
    }

    case nList: {
        auto list = v.listView();
        for (size_t i = 0; i < list.size(); ++i) {
            result = combinePortability(result, computeValuePortability(*list[i], symbols, ancestors));
            if (!isPortable(result))
                break;
        }
        break;
    }

    case nFunction:
        // nFunction covers tLambda, tPrimOp, and tPrimOpApp
        if (v.isPrimOp()) {
            // PrimOp hashes by name only - portable
            result = HashPortability::Portable;
        } else if (v.isPrimOpApp()) {
            // PrimOpApp portability depends on the applied arguments
            auto primOpApp = v.primOpApp();
            if (primOpApp.left) {
                result = combinePortability(result, computeValuePortability(*primOpApp.left, symbols, ancestors));
            }
            if (primOpApp.right && isPortable(result)) {
                result = combinePortability(result, computeValuePortability(*primOpApp.right, symbols, ancestors));
            }
        } else {
            // Lambda hashes use env pointer fallback - non-portable
            result = HashPortability::NonPortable_Pointer;
        }
        break;

    case nThunk:
        // Thunk hashes use env pointer fallback - non-portable
        result = HashPortability::NonPortable_Pointer;
        break;

    case nExternal:
        // External hashes use pointer - non-portable
        result = HashPortability::NonPortable_Pointer;
        break;
    }

    ancestors.pop_back();
    return result;
}

} // anonymous namespace

ContentHashResult computeValueContentHashWithPortability(const Value & v, const SymbolTable & symbols)
{
    std::vector<const Value *> ancestors;
    ContentHash hash = computeValueContentHash(v, symbols, ancestors);

    ancestors.clear();
    HashPortability portability = computeValuePortability(v, symbols, ancestors);

    return ContentHashResult{hash, portability};
}

} // namespace nix
