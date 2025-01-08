#pragma once
///@file

#include <bit>
#include <cassert>
#include <cstdint>
#include <span>

#include "config-expr.hh"
#include "eval-gc.hh"
#include "source-accessor.hh"
#include "symbol-table.hh"
#include "value/context.hh"
#include "source-path.hh"
#include "print-options.hh"
#include "checked-arithmetic.hh"

#include <nlohmann/json_fwd.hpp>

namespace nix {

struct Value;
class BindingsBuilder;

static constexpr size_t NUM_BYTES_FOR_ADDRESSING = sizeof(uintptr_t);
static constexpr size_t NUM_BITS_FOR_ADDRESSING = NUM_BYTES_FOR_ADDRESSING * 8;

static_assert(std::endian::native == std::endian::little, "Only little endian supported");
static_assert(NUM_BITS_FOR_ADDRESSING == 64, "Only 64-bit supported");
static_assert(HAVE_BOEHMGC == 0, "GC not supported");

static constexpr size_t NUM_BITS_FOR_TYPE_TAG = 4u;
static constexpr size_t NUM_BITS_SHIFTED_FOR_TYPE_TAG = NUM_BITS_FOR_ADDRESSING - NUM_BITS_FOR_TYPE_TAG;

typedef enum {
    tUninitialized = 0,
    tInt = 1,
    tBool,
    tString,
    tPath,
    tNull,
    tAttrs,
    tList1,
    tListN,
    tThunk,
    tApp,
    tLambda,
    tPrimOp,
    tPrimOpApp,
    tExternal,
    tFloat
} InternalType;

inline uintptr_t setInternalTypeTag(uintptr_t ptrToTag, InternalType tag)
{
    return
        // Shift out the tag bits.
        ((ptrToTag << NUM_BITS_FOR_TYPE_TAG) >> NUM_BITS_FOR_TYPE_TAG)
        // Set the tag bits.
        | (static_cast<uintptr_t>(tag) << NUM_BITS_SHIFTED_FOR_TYPE_TAG);
}

inline uintptr_t removeInternalTypeTag(uintptr_t taggedPtr)
{
    return
        // Shift out the tag bits, converting to intptr_t to sign-extend when shifting back.
        static_cast<uintptr_t>(static_cast<intptr_t>(taggedPtr << NUM_BITS_FOR_TYPE_TAG) >> NUM_BITS_FOR_TYPE_TAG);
}

inline InternalType getInternalTypeTag(uintptr_t taggedPtr)
{
    return static_cast<InternalType>(taggedPtr >> NUM_BITS_SHIFTED_FOR_TYPE_TAG);
}

/**
 * This type abstracts over all actual value types in the language,
 * grouping together implementation details like tList*, different function
 * types, and types in non-normal form (so thunks and co.)
 */
typedef enum { nThunk, nInt, nFloat, nBool, nString, nPath, nNull, nAttrs, nList, nFunction, nExternal } ValueType;

class Bindings;
struct Env;
struct Expr;
struct ExprLambda;
struct ExprBlackHole;
struct PrimOp;
class Symbol;
class PosIdx;
struct Pos;
class StorePath;
class EvalState;
class XMLWriter;
class Printer;

using NixInt = checked::Checked<int64_t>;
using NixFloat = double;

/**
 * External values must descend from ExternalValueBase, so that
 * type-agnostic nix functions (e.g. showType) can be implemented
 */
class ExternalValueBase
{
    friend std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v);
    friend class Printer;
protected:
    /**
     * Print out the value
     */
    virtual std::ostream & print(std::ostream & str) const = 0;

public:
    /**
     * Return a simple string describing the type
     */
    virtual std::string showType() const = 0;

    /**
     * Return a string to be used in builtins.typeOf
     */
    virtual std::string typeOf() const = 0;

    /**
     * Coerce the value to a string. Defaults to uncoercable, i.e. throws an
     * error.
     */
    virtual std::string coerceToString(
        EvalState & state, const PosIdx & pos, NixStringContext & context, bool copyMore, bool copyToStore) const;

    /**
     * Compare to another value of the same type. Defaults to uncomparable,
     * i.e. always false.
     */
    virtual bool operator==(const ExternalValueBase & b) const noexcept;

    /**
     * Print the value as JSON. Defaults to unconvertable, i.e. throws an error
     */
    virtual nlohmann::json
    printValueAsJSON(EvalState & state, bool strict, NixStringContext & context, bool copyToStore = true) const;

    /**
     * Print the value as XML. Defaults to unevaluated
     */
    virtual void printValueAsXML(
        EvalState & state,
        bool strict,
        bool location,
        XMLWriter & doc,
        NixStringContext & context,
        PathSet & drvsSeen,
        const PosIdx pos) const;

    virtual ~ExternalValueBase() {};
};

std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v);

class ListBuilder
{
    const size_t size;
    Value * inlineSingleton[1] = {nullptr};
public:
    Value ** elems;
    ListBuilder(EvalState & state, size_t size);

    // NOTE: Can be noexcept because we are just copying integral values and
    // raw pointers.
    ListBuilder(ListBuilder && x) noexcept
        : size(x.size)
        , inlineSingleton{x.inlineSingleton[0]}
        , elems(size <= 1 ? inlineSingleton : x.elems)
    {
    }

    Value *& operator[](size_t n)
    {
        return elems[n];
    }

    typedef Value ** iterator;

    iterator begin()
    {
        return &elems[0];
    }
    iterator end()
    {
        return &elems[size];
    }

    friend struct Value;
};

struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) Value
{
private:
    // NOTE: For all of our tagged values, the first value must be the tag.
    // NOTE: Each tagged value has two, eight-byte fields.

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedBoolean
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tag;
        alignas(NUM_BYTES_FOR_ADDRESSING) bool boolean;
    public:
        inline bool getBoolean() const
        {
            return boolean;
        }
    };
    static_assert(sizeof(TaggedBoolean) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedInteger
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tag;
        alignas(NUM_BYTES_FOR_ADDRESSING) NixInt integer;
    public:
        inline NixInt getInteger() const
        {
            return integer;
        }
    };
    static_assert(sizeof(TaggedInteger) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedFloat
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tag;
        alignas(NUM_BYTES_FOR_ADDRESSING) NixFloat fpoint;
    public:
        inline NixFloat getFloat() const
        {
            return fpoint;
        }
    };
    static_assert(sizeof(TaggedFloat) == NUM_BYTES_FOR_ADDRESSING * 2);

    /**
     * Strings in the evaluator carry a so-called `context` which
     * is a list of strings representing store paths.  This is to
     * allow users to write things like
     *
     *   "--with-freetype2-library=" + freetype + "/lib"
     *
     * where `freetype` is a derivation (or a source to be copied
     * to the store).  If we just concatenated the strings without
     * keeping track of the referenced store paths, then if the
     * string is used as a derivation attribute, the derivation
     * will not have the correct dependencies in its inputDrvs and
     * inputSrcs.

     * The semantics of the context is as follows: when a string
     * with context C is used as a derivation attribute, then the
     * derivations in C will be added to the inputDrvs of the
     * derivation, and the other store paths in C will be added to
     * the inputSrcs of the derivations.

     * For canonicity, the store paths should be in sorted order.
     */
    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedStringWithContext
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tagged_c_str; // const char *
        alignas(NUM_BYTES_FOR_ADDRESSING) const char ** context;  // must be in sorted order
    public:
        inline const char * get_c_str() const
        {
            return reinterpret_cast<const char *>(removeInternalTypeTag(tagged_c_str));
        }
        inline const char ** getContext() const
        {
            return context;
        }
    };
    static_assert(sizeof(TaggedStringWithContext) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedPath
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t taggedSourceAccessorPtr; // SourceAccessor *
        alignas(NUM_BYTES_FOR_ADDRESSING) const char * path;
    public:
        inline SourceAccessor * getSourceAccessor() const
        {
            return reinterpret_cast<SourceAccessor *>(removeInternalTypeTag(taggedSourceAccessorPtr));
        }
        inline const char * getPath() const
        {
            return path;
        }
    };
    static_assert(sizeof(TaggedPath) == NUM_BYTES_FOR_ADDRESSING * 2);

    // TODO(@connorbaker): Stuck with singleton lists instead of two-element lists because there's no way to ensure
    // the array recieved by the caller has the untagged pointers without:
    // 1. Duplicating the array
    // 2. Removing the type tag from the pointers in the array and breaking the struct on subsequent calls
    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedSingletonList
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tag; // Value * const *
        alignas(NUM_BYTES_FOR_ADDRESSING) Value * singleton[1];
    public:
        inline Value * const * getElems() const
        {
            return singleton;
        }
    };
    static_assert(sizeof(TaggedSingletonList) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedBigList
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t taggedElems; // Value * const *
        alignas(NUM_BYTES_FOR_ADDRESSING) size_t size;
    public:
        inline Value * const * getElems() const
        {
            return reinterpret_cast<Value * const *>(removeInternalTypeTag(taggedElems));
        }
        inline size_t getSize() const
        {
            return size;
        }
    };
    static_assert(sizeof(TaggedBigList) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedBindings
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tag;
        alignas(NUM_BYTES_FOR_ADDRESSING) Bindings * attrs;
    public:
        // TODO(@connorbaker): This should never be needed.
        inline Bindings * getMutableAttrs() const
        {
            return attrs;
        }
        inline const Bindings * getAttrs() const
        {
            return attrs;
        }
    };
    static_assert(sizeof(TaggedBindings) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedClosureThunk
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t taggedEnvPtr; // Env *
        alignas(NUM_BYTES_FOR_ADDRESSING) Expr * expr;
    public:
        inline Env * getEnv() const
        {
            return reinterpret_cast<Env *>(removeInternalTypeTag(taggedEnvPtr));
        }
        inline Expr * getExpr() const
        {
            return expr;
        }
    };
    static_assert(sizeof(TaggedClosureThunk) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedFunctionApplicationThunk
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t taggedLeftValuePtr; // Value *
        alignas(NUM_BYTES_FOR_ADDRESSING) Value * right;
    public:
        inline Value * getLeft() const
        {
            return reinterpret_cast<Value *>(removeInternalTypeTag(taggedLeftValuePtr));
        }
        inline Value * getRight() const
        {
            return right;
        }
    };
    static_assert(sizeof(TaggedFunctionApplicationThunk) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedLambda
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t taggedEnvPtr; // Env *
        alignas(NUM_BYTES_FOR_ADDRESSING) ExprLambda * fun;
    public:
        inline Env * getEnv() const
        {
            return reinterpret_cast<Env *>(removeInternalTypeTag(taggedEnvPtr));
        }
        inline ExprLambda * getFun() const
        {
            return fun;
        }
    };
    static_assert(sizeof(TaggedLambda) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedPrimOp
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tag;
        alignas(NUM_BYTES_FOR_ADDRESSING) PrimOp * primOp;
    public:
        inline PrimOp * getPrimOp() const
        {
            return primOp;
        }
    };
    static_assert(sizeof(TaggedPrimOp) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) TaggedExternalValueBase
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t tag;
        alignas(NUM_BYTES_FOR_ADDRESSING) ExternalValueBase * external;
    public:
        inline ExternalValueBase * getExternal() const
        {
            return external;
        }
    };
    static_assert(sizeof(TaggedExternalValueBase) == NUM_BYTES_FOR_ADDRESSING * 2);

    struct alignas(NUM_BYTES_FOR_ADDRESSING * 2) _InternalTaggedValue
    {
    private:
        friend struct Value;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t taggedFirst;
        alignas(NUM_BYTES_FOR_ADDRESSING) uintptr_t second;
    public:
        inline uintptr_t getInternalType() const
        {
            return getInternalTypeTag(taggedFirst);
        }
    };
    static_assert(sizeof(_InternalTaggedValue) == NUM_BYTES_FOR_ADDRESSING * 2);

    union Payload
    {
        // Primitives
        TaggedBoolean taggedBoolean;
        TaggedInteger taggedInteger;
        TaggedFloat taggedFloat;

        TaggedStringWithContext taggedStringWithContext;

        TaggedPath taggedPath;

        // Data structures
        TaggedSingletonList taggedSingletonList;
        TaggedBigList taggedBigList;
        TaggedBindings taggedBindings;

        // Functions, etc.
        TaggedClosureThunk taggedThunk;
        TaggedFunctionApplicationThunk taggedApp;
        TaggedLambda taggedLambda;
        TaggedPrimOp taggedPrimOp;
        TaggedFunctionApplicationThunk taggedPrimOpApp;
        TaggedExternalValueBase taggedExternal;

        // Internal
        _InternalTaggedValue _internalTaggedValue;

        // Default constructor
        Payload()
        {
            _internalTaggedValue.taggedFirst = setInternalTypeTag(0, tUninitialized);
            _internalTaggedValue.second = 0;
        }
    };
    static_assert(sizeof(Payload) == NUM_BYTES_FOR_ADDRESSING * 2);

    Payload payload;

    inline uintptr_t getInternalType() const
    {
        return payload._internalTaggedValue.getInternalType();
    }

    friend std::string showType(const Value & v);

    // TODO(@connorbaker): This should not exist.
    friend EvalState;
    inline Bindings * mutableAttrs() const
    {
        assert(getInternalType() == tAttrs);
        return payload.taggedBindings.getMutableAttrs();
    }

public:
    void print(EvalState & state, std::ostream & str, PrintOptions options = PrintOptions{});

    /**
     * A value becomes valid when it is initialized. We don't use this
     * in the evaluator; only in the bindings, where the slight extra
     * cost is warranted because of inexperienced callers.
     */
    inline bool isValid() const
    {
        return getInternalType() != tUninitialized;
    }

    /**
     * Check whether forcing this value requires a trivial amount of
     * computation. In particular, function applications are
     * non-trivial.
     */
    bool isTrivial() const;

    /**
     * Returns the normal type of a Value. This only returns nThunk if
     * the Value hasn't been forceValue'd
     *
     * @param invalidIsThunk Instead of aborting an an invalid (probably
     * 0, so uninitialized) internal type, return `nThunk`.
     */
    inline ValueType type(bool invalidIsThunk = false) const
    {
        switch (getInternalType()) {
        case tUninitialized:
            break;
        case tNull:
            return nNull;
        case tBool:
            return nBool;
        case tInt:
            return nInt;
        case tFloat:
            return nFloat;
        case tString:
            return nString;
        case tPath:
            return nPath;
        case tList1:
        case tListN:
            return nList;
        case tAttrs:
            return nAttrs;
        case tThunk:
        case tApp:
            return nThunk;
        case tLambda:
        case tPrimOp:
        case tPrimOpApp:
            return nFunction;
        case tExternal:
            return nExternal;
        }
        if (invalidIsThunk)
            return nThunk;
        else
            unreachable();
    }

    // Functions needed to distinguish the type
    // These should be removed eventually, by putting the functionality that's
    // needed by callers into methods of this type

    // type() == nList
    inline bool isList() const
    {
        return getInternalType() == tList1 || getInternalType() == tListN;
    };

    // type() == nThunk
    inline bool isThunk() const
    {
        return getInternalType() == tThunk;
    };
    inline bool isApp() const
    {
        return getInternalType() == tApp;
    };
    inline bool isBlackhole() const;

    // type() == nFunction
    inline bool isLambda() const
    {
        return getInternalType() == tLambda;
    };
    inline bool isPrimOp() const
    {
        return getInternalType() == tPrimOp;
    };
    inline bool isPrimOpApp() const
    {
        return getInternalType() == tPrimOpApp;
    };

    inline void mkNull()
    {
        payload._internalTaggedValue.taggedFirst = setInternalTypeTag(0, tNull);
    }

    inline void mkBool(bool b)
    {
        TaggedBoolean taggedBoolean;
        taggedBoolean.tag = setInternalTypeTag(0, tBool);
        taggedBoolean.boolean = b;
        payload.taggedBoolean = taggedBoolean;
    }

    inline void mkInt(NixInt::Inner n)
    {
        mkInt(NixInt{n});
    }

    inline void mkInt(NixInt n)
    {
        TaggedInteger taggedInteger;
        taggedInteger.tag = setInternalTypeTag(0, tInt);
        taggedInteger.integer = n;
        payload.taggedInteger = taggedInteger;
    }

    inline void mkFloat(NixFloat n)
    {
        TaggedFloat taggedFloat;
        taggedFloat.tag = setInternalTypeTag(0, tFloat);
        taggedFloat.fpoint = n;
        payload.taggedFloat = taggedFloat;
    }

    void mkString(std::string_view s);

    void mkString(std::string_view s, const NixStringContext & context);

    void mkStringMove(const char * s, const NixStringContext & context);

    inline void mkString(const SymbolStr & s)
    {
        mkString(s.c_str());
    }

    inline void mkString(const char * s, const char ** context = nullptr)
    {
        TaggedStringWithContext taggedStringWithContext;
        taggedStringWithContext.tagged_c_str = setInternalTypeTag(reinterpret_cast<uintptr_t>(s), tString);
        taggedStringWithContext.context = context;
        payload.taggedStringWithContext = taggedStringWithContext;
    }

    void mkPath(const SourcePath & path);

    void mkPath(std::string_view path);

    inline void mkPath(SourceAccessor * accessor, const char * path)
    {
        TaggedPath taggedPath;
        taggedPath.taggedSourceAccessorPtr = setInternalTypeTag(reinterpret_cast<uintptr_t>(accessor), tPath);
        taggedPath.path = path;
        payload.taggedPath = taggedPath;
    }

    inline void mkList(const ListBuilder & builder)
    {
        if (builder.size == 1) {
            TaggedSingletonList taggedSingletonList;
            taggedSingletonList.tag = setInternalTypeTag(0, tList1);
            taggedSingletonList.singleton[0] = builder.inlineSingleton[0];
            payload.taggedSingletonList = taggedSingletonList;
        } else {
            TaggedBigList taggedBigList;
            taggedBigList.taggedElems = setInternalTypeTag(reinterpret_cast<uintptr_t>(builder.elems), tListN);
            taggedBigList.size = builder.size;
            payload.taggedBigList = taggedBigList;
        }
    }

    Value & mkAttrs(BindingsBuilder & bindings);

    inline void mkAttrs(Bindings * a)
    {
        TaggedBindings taggedBindings;
        taggedBindings.tag = setInternalTypeTag(0, tAttrs);
        taggedBindings.attrs = a;
        payload.taggedBindings = taggedBindings;
    }

    inline void mkThunk(Env * e, Expr * ex)
    {
        TaggedClosureThunk taggedThunk;
        taggedThunk.taggedEnvPtr = setInternalTypeTag(reinterpret_cast<uintptr_t>(e), tThunk);
        taggedThunk.expr = ex;
        payload.taggedThunk = taggedThunk;
    }

    inline void mkApp(Value * l, Value * r)
    {
        TaggedFunctionApplicationThunk taggedApp;
        taggedApp.taggedLeftValuePtr = setInternalTypeTag(reinterpret_cast<uintptr_t>(l), tApp);
        taggedApp.right = r;
        payload.taggedApp = taggedApp;
    }

    inline void mkLambda(Env * e, ExprLambda * f)
    {
        TaggedLambda taggedLambda;
        taggedLambda.taggedEnvPtr = setInternalTypeTag(reinterpret_cast<uintptr_t>(e), tLambda);
        taggedLambda.fun = f;
        payload.taggedLambda = taggedLambda;
    }

    void mkPrimOp(PrimOp * p);

    inline void mkPrimOpApp(Value * l, Value * r)
    {
        TaggedFunctionApplicationThunk taggedPrimOpApp;
        taggedPrimOpApp.taggedLeftValuePtr = setInternalTypeTag(reinterpret_cast<uintptr_t>(l), tPrimOpApp);
        taggedPrimOpApp.right = r;
        payload.taggedPrimOpApp = taggedPrimOpApp;
    }

    inline void mkExternal(ExternalValueBase * e)
    {
        TaggedExternalValueBase taggedExternal;
        taggedExternal.tag = setInternalTypeTag(0, tExternal);
        taggedExternal.external = e;
        payload.taggedExternal = taggedExternal;
    }

    inline void mkBlackhole();

    inline bool boolean() const
    {
        return payload.taggedBoolean.getBoolean();
    }

    inline NixInt integer() const
    {
        return payload.taggedInteger.getInteger();
    }

    inline NixFloat fpoint() const
    {
        return payload.taggedFloat.getFloat();
    }

    inline std::string_view string_view() const
    {
        assert(getInternalType() == tString);
        return std::string_view(payload.taggedStringWithContext.get_c_str());
    }

    inline const char * c_str() const
    {
        assert(getInternalType() == tString);
        return payload.taggedStringWithContext.get_c_str();
    }

    inline const char ** context() const
    {
        assert(getInternalType() == tString);
        return payload.taggedStringWithContext.getContext();
    }

    inline TaggedPath path() const
    {
        assert(getInternalType() == tPath);
        return payload.taggedPath;
    }

    inline SourcePath sourcePath() const
    {
        assert(getInternalType() == tPath);
        return SourcePath(
            ref(payload.taggedPath.getSourceAccessor()->shared_from_this()),
            CanonPath(CanonPath::unchecked_t(), payload.taggedPath.getPath()));
    }

    inline Value * const * listElems()
    {
        const auto ty = getInternalType();
        assert(ty == tList1 || ty == tListN);
        return ty == tList1 ? payload.taggedSingletonList.getElems() : payload.taggedBigList.getElems();
    }

    inline std::span<Value * const> listItems() const
    {
        assert(isList());
        return std::span<Value * const>(listElems(), listSize());
    }

    inline Value * const * listElems() const
    {
        const auto ty = getInternalType();
        assert(ty == tList1 || ty == tListN);
        return ty == tList1 ? payload.taggedSingletonList.getElems() : payload.taggedBigList.getElems();
    }

    inline size_t listSize() const
    {
        const auto ty = getInternalType();
        assert(ty == tList1 || ty == tListN);
        return ty == tList1 ? 1 : payload.taggedBigList.getSize();
    }

    inline const Bindings * attrs() const
    {
        assert(getInternalType() == tAttrs);
        return payload.taggedBindings.getAttrs();
    }

    inline TaggedClosureThunk thunk() const
    {
        assert(getInternalType() == tThunk);
        return payload.taggedThunk;
    }

    inline TaggedLambda lambda() const
    {
        assert(getInternalType() == tLambda);
        return payload.taggedLambda;
    }

    inline TaggedFunctionApplicationThunk app() const
    {
        assert(getInternalType() == tApp);
        return payload.taggedApp;
    }

    inline TaggedFunctionApplicationThunk primOpApp() const
    {
        assert(getInternalType() == tPrimOpApp);
        return payload.taggedPrimOpApp;
    }

    inline const PrimOp * primOp() const
    {
        assert(getInternalType() == tPrimOp);
        return payload.taggedPrimOp.getPrimOp();
    }

    /**
     * For a `tPrimOpApp` value, get the original `PrimOp` value.
     */
    const PrimOp * primOpAppPrimOp() const;

    inline ExternalValueBase * external() const
    {
        assert(getInternalType() == tExternal);
        return payload.taggedExternal.getExternal();
    }

    PosIdx determinePos(const PosIdx pos) const;
};
static_assert(sizeof(Value) == NUM_BYTES_FOR_ADDRESSING * 2);

extern ExprBlackHole eBlackHole;

inline bool Value::isBlackhole() const
{
    return getInternalType() == tThunk && payload.taggedThunk.getExpr() == (Expr *) &eBlackHole;
}

inline void Value::mkBlackhole()
{
    mkThunk(nullptr, (Expr *) &eBlackHole);
}

typedef std::vector<Value *, traceable_allocator<Value *>> ValueVector;
typedef std::unordered_map<
    Symbol,
    Value *,
    std::hash<Symbol>,
    std::equal_to<Symbol>,
    traceable_allocator<std::pair<const Symbol, Value *>>>
    ValueMap;
typedef std::map<Symbol, ValueVector, std::less<Symbol>, traceable_allocator<std::pair<const Symbol, ValueVector>>>
    ValueVectorMap;

/**
 * A value allocated in traceable memory.
 */
typedef std::shared_ptr<Value *> RootValue;

RootValue allocRootValue(Value * v);

void forceNoNullByte(std::string_view s, std::function<Pos()> = nullptr);

}
