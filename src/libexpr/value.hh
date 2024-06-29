#pragma once
///@file

#include <cassert>
#include <climits>
#include <new>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>

#include "symbol-table.hh"
#include "value/context.hh"
#include "source-path.hh"
#include "print-options.hh"

#if HAVE_BOEHMGC
#include <gc/gc_allocator.h>
#endif
#include <nlohmann/json_fwd.hpp>

namespace nix {

[[gnu::always_inline]]
inline void * allocAligned(size_t numElems, size_t elemSize)
{
    // elemSize must be a multiple of two
    if (elemSize & (elemSize - 1)) throw std::invalid_argument("alignment must be a power of two");
    const size_t totalSize = numElems * elemSize;
    void * p;
#if HAVE_BOEHMGC
    p = GC_memalign(elemSize, totalSize);
#else
    p = aligned_alloc(elemSize, totalSize);
#endif
    if (!p) throw std::bad_alloc();
    memset(p, 0, totalSize);
    return p;
}


struct Value;
class BindingsBuilder;


/**
 * This type abstracts over all actual value types in the language,
 * grouping together implementation details like tList*, different function
 * types, and types in non-normal form (so thunks and co.)
 */
typedef enum {
    nThunk,
    nInt,
    nFloat,
    nBool,
    nString,
    nPath,
    nNull,
    nAttrs,
    nList,
    nFunction,
    nExternal
} ValueType;

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

typedef int64_t NixInt;
typedef double NixFloat;

extern ExprBlackHole eBlackHole;

/**
 * External values must descend from ExternalValueBase, so that
 * type-agnostic nix functions (e.g. showType) can be implemented
 */
class ExternalValueBase
{
    friend std::ostream & operator << (std::ostream & str, const ExternalValueBase & v);
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
    virtual std::string coerceToString(EvalState & state, const PosIdx & pos, NixStringContext & context, bool copyMore, bool copyToStore) const;

    /**
     * Compare to another value of the same type. Defaults to uncomparable,
     * i.e. always false.
     */
    virtual bool operator ==(const ExternalValueBase & b) const;

    /**
     * Print the value as JSON. Defaults to unconvertable, i.e. throws an error
     */
    virtual nlohmann::json printValueAsJSON(EvalState & state, bool strict,
        NixStringContext & context, bool copyToStore = true) const;

    /**
     * Print the value as XML. Defaults to unevaluated
     */
    virtual void printValueAsXML(EvalState & state, bool strict, bool location,
        XMLWriter & doc, NixStringContext & context, PathSet & drvsSeen,
        const PosIdx pos) const;

    virtual ~ExternalValueBase()
    {
    };
};

std::ostream & operator << (std::ostream & str, const ExternalValueBase & v);


class ListBuilder
{
    const size_t size;
    Value * inlineElems[2] = {nullptr, nullptr};
public:
    Value * * elems;
    ListBuilder(EvalState & state, size_t size);

    ListBuilder(ListBuilder && x)
        : size(x.size)
        , inlineElems{x.inlineElems[0], x.inlineElems[1]}
        , elems(size <= 2 ? inlineElems : x.elems)
    { }

    Value * & operator [](size_t n)
    { return elems[n]; }

    typedef Value * * iterator;

    iterator begin() { return &elems[0]; }
    iterator end() { return &elems[size]; }

    friend struct Value;
};

struct alignas(32) Value
{
private:
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
    struct StringWithContext {
        const char * c_str;
        const char * * context; // must be in sorted order
    };

    struct Path {
        SourceAccessor * accessor;
        const char * path;
    };

    struct Null {};

    struct ClosureThunk {
        Env * env;
        Expr * expr;
    };

    struct FuncAppThunk 
    { Value * left, * right; };

    struct FuncPrimOpAppThunk
    { Value * left, * right; };

    struct Lambda {
        Env * env;
        ExprLambda * fun;
    };

    struct SmallList1
    { Value * elems[1]; };

    struct SmallList2
    { Value * elems[2]; };

    struct BigList {
        size_t size;
        Value * const * elems;
    };

    using Payload = std::variant<
        std::monostate,                    // tUninitialized
        NixInt,                            // tInt
        bool,                              // tBool
        StringWithContext,                 // tString
        Path,                              // tPath
        Null,                              // tNull
        Bindings *,                        // tAttrs
        SmallList1,                        // tList1,
        SmallList2,                        // tList2,
        BigList,                           // tListN
        ClosureThunk,                      // tThunk
        FuncAppThunk,                      // tApp
        Lambda,                            // tLambda
        PrimOp *,                          // tPrimOp
        FuncPrimOpAppThunk,                // tPrimOpApp
        ExternalValueBase *,               // tExternal
        NixFloat                           // tFloat
    >;
    Payload payload;

    friend std::string showType(const Value & v);

public:
    /**
     * A value becomes valid when it is initialized. We don't use this
     * in the evaluator; only in the bindings, where the slight extra
     * cost is warranted because of inexperienced callers.
     */
    /* inline */ bool isValid() const
    { return !std::holds_alternative<std::monostate>(payload); }

    /**
     * Check whether forcing this value requires a trivial amount of
     * computation. In particular, function applications are
     * non-trivial.
     */
    // Defined in eval.hh
    /* inline */ bool isTrivial() const;


    /**
     * Returns the normal type of a Value. This only returns nThunk if
     * the Value hasn't been forceValue'd
     *
     * @param invalidIsThunk Instead of aborting an an invalid (probably
     * 0, so uninitialized) internal type, return `nThunk`.
     */
    /* inline */ ValueType type(bool invalidIsThunk = false) const
    {
        if (isNull()) {
            return nNull;
        } else if (isBool()) {
            return nBool;
        } else if (isInt()) {
            return nInt;
        } else if (isFloat()) {
            return nFloat;
        } else if (isString()) {
            return nString;
        } else if (isPath()) {
            return nPath;
        } else if (isList()) {
            return nList;
        } else if (isAttrs()) {
            return nAttrs;
        } else if (isFunction()) {
            return nFunction;
        } else if (isThunk(invalidIsThunk)) {
            return nThunk;
        } else if (isExternal()) {
            return nExternal;
        } else {
            abort();
        }
    }


    // Methods for nulls
    /* inline */ bool isNull() const
    { return std::holds_alternative<Null>(payload); }

    /* inline */ void mkNull()
    { payload.emplace<Null>(Null{}); }


    // Methods for boolean values
    /* inline */ bool isBool() const
    { return std::holds_alternative<bool>(payload); }

    /* inline */ void mkBool(bool b)
    { payload.emplace<bool>(b); }

    /* inline */ bool boolean() const
    { return std::get<bool>(payload); }


    // Methods for integers
    /* inline */ bool isInt() const
    { return std::holds_alternative<NixInt>(payload); }

    /* inline */ void mkInt(NixInt n)
    { payload.emplace<NixInt>(n); }

    /* inline */ NixInt integer() const
    { return std::get<NixInt>(payload); }


    // Methods for floats
    /* inline */ bool isFloat() const
    { return std::holds_alternative<NixFloat>(payload); };

    /* inline */ void mkFloat(NixFloat n)
    { payload.emplace<NixFloat>(n); }

    /* inline */ NixFloat fpoint() const
    { return std::get<NixFloat>(payload); }


    // Methods for strings
    /* inline */ bool isString() const
    { return std::holds_alternative<StringWithContext>(payload); }

    /* inline */ bool isStringWithContext() const
    { return isString() && (context() != nullptr); }

    /* inline */ void mkString(const Symbol & s)
    { mkString(((const std::string &) s).c_str()); }

    /* inline */ void mkString(const char * s, const char * * context = nullptr)
    {
        assert(s);
        payload.emplace<StringWithContext>(StringWithContext{ .c_str = s, .context = context });
        assert(isValid());
    }

    // Defined in eval.cc
    /* inline */ void mkString(std::string_view s);

    // Defined in eval.cc
    /* inline */ void mkString(std::string_view s, const NixStringContext & context);

    // Defined in eval.cc
    /* inline */ void mkStringMove(const char * s, const NixStringContext & context);

    /* inline */ const char * c_str() const
    { return std::get<StringWithContext>(payload).c_str; }

    /* inline */ std::string_view string_view() const
    { return std::string_view(c_str()); }

    /* inline */ const char * * context() const
    { return std::get<StringWithContext>(payload).context; }


    // Methods for paths
    /* inline */ bool isPath() const
    { return std::holds_alternative<Path>(payload); }

    /* inline */ void mkPath(const SourcePath & path);

    /* inline */ void mkPath(std::string_view path);

    /* inline */ void mkPath(SourceAccessor * accessor, const char * path)
    {
        assert(accessor);
        assert(path);
        payload.emplace<Path>(Path{ .accessor = accessor, .path = path });
        assert(isValid());
    }

    /* inline */ const char * pathString() const
    {
        auto path = std::get<Path>(payload);
        assert(path.path);
        return path.path;
    }

    /* inline */ SourceAccessor * pathAccessor() const
    {
        auto path = std::get<Path>(payload);
        assert(path.accessor);
        return path.accessor;
    }

    /* inline */ SourcePath getSourcePath() const
    {
        auto nixPath = std::get<Path>(payload);
        return SourcePath(
            ref(nixPath.accessor->shared_from_this()),
            CanonPath(CanonPath::unchecked_t(), nixPath.path));
    }


    // Methods for lists
    /* inline */ bool isList() const
    {
        return std::holds_alternative<SmallList1>(payload)
            || std::holds_alternative<SmallList2>(payload)
            || std::holds_alternative<BigList>(payload);
    }

    // Non-const version
    /* inline */ Value * const * listElems() const
    {
         return std::visit([](auto& arg) -> Value * const * {
            using ArgType = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<ArgType, SmallList1>) {
                return arg.elems;
            } else if constexpr (std::is_same_v<ArgType, SmallList2>) {
                return arg.elems;
            } else if constexpr (std::is_same_v<ArgType, BigList>) {
                return arg.elems;
            } else {
                abort();
            }
        }, payload);
    }

    /* inline */ void mkList(const ListBuilder & builder)
    {
        if (builder.size == 1)
            payload.emplace<SmallList1>(SmallList1{ .elems = { builder.elems[0] } });
        else if (builder.size == 2)
            payload.emplace<SmallList2>(SmallList2{ .elems = { builder.elems[0], builder.elems[1] } });
        else
            payload.emplace<BigList>(BigList{ .size = builder.size, .elems = builder.elems });
    }

    /* inline */ size_t listSize() const
    {
        return std::visit([](auto& arg) -> size_t {
            using ArgType = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<ArgType, SmallList1>) {
                return 1;
            } else if constexpr (std::is_same_v<ArgType, SmallList2>) {
                return 2;
            } else if constexpr (std::is_same_v<ArgType, BigList>) {
                return arg.size;
            } else {
                abort();
            }
        }, payload);
    }

    /* inline */ std::span<Value * const> listItems() const
    { return std::span<Value * const>(listElems(), listSize()); }


    // Methods for attributes
    /* inline */ bool isAttrs() const
    { return std::holds_alternative<Bindings *>(payload); }

    /* inline */ void mkAttrs(Bindings * a)
    {
        assert(a);
        payload.emplace<Bindings *>(a);
        assert(isValid());
    }

    Value & mkAttrs(BindingsBuilder & bindings);

    /* inline */ Bindings * attrs() const
    {
        auto attrs = std::get<Bindings *>(payload);
        assert(attrs);
        return attrs;
    }


    // Methods for lambdas
    /* inline */ bool isLambda() const
    { return std::holds_alternative<Lambda>(payload); };

    /* inline */ void mkLambda(Env * e, ExprLambda * f)
    {
        assert(e);
        assert(f);
        payload.emplace<Lambda>(Lambda{ .env = e, .fun = f });
        assert(isValid());
    }

    /* inline */ Env * lambdaEnv() const
    { return std::get<Lambda>(payload).env; }

    /* inline */ ExprLambda * lambdaFun() const
    { return std::get<Lambda>(payload).fun; }


    // Methods for primops
    /* inline */ bool isPrimOp() const
    { return std::holds_alternative<PrimOp *>(payload); };

    // Defined in eval.hh
    /* inline */ void mkPrimOp(PrimOp * p);

    /* inline */ const PrimOp * primOp() const
    {
        auto primOp = std::get<PrimOp *>(payload);
        assert(primOp);
        return primOp;
    }

    /**
     * Get the primitive operation that this value is a part of.
     */
    /* inline */ const Value * getPrimOp() const {
        auto valuePtr = this;
        std::cerr << "getPrimOp: starting at " << valuePtr;
        while (valuePtr && valuePtr->isPrimOpApp()) {
            valuePtr = valuePtr->primOpAppLeft();
            std::cerr << "getPrimOp: -> " << valuePtr;
        }
        assert(valuePtr->isPrimOp());
        std::cerr << "getPrimOp: finished at " << valuePtr << std::endl;
        return valuePtr;
    }


    // Methods for primitive operation applications
    /* inline */ bool isPrimOpApp() const
    { return std::holds_alternative<FuncPrimOpAppThunk>(payload); };

    /* inline */ void mkPrimOpApp(Value * l, Value * r)
    {
        assert(l);
        assert(l->isValid());
        assert(r);
        assert(r->isValid());
        payload.emplace<FuncPrimOpAppThunk>(FuncPrimOpAppThunk{ .left = l, .right = r });
        assert(isValid());
    }

    /* inline */ Value * primOpAppLeft() const
    {
        auto primOpApp = std::get<FuncPrimOpAppThunk>(payload);
        assert(primOpApp.left);
        assert(primOpApp.left->isValid());
        return primOpApp.left;
    }

    /* inline */ Value * primOpAppRight() const
    {
        auto primOpApp = std::get<FuncPrimOpAppThunk>(payload);
        assert(primOpApp.right);
        assert(primOpApp.right->isValid());
        return primOpApp.right;
    }

    /**
     * For a `tPrimOpApp` value, get the original `PrimOp` value.
     */
    // TODO(@connorbaker): This seems like getPrimOp without the assert and an additional dereference?
    /* inline */ const PrimOp * primOpAppPrimOp() const
    {
        auto valuePtr = this;
        std::cerr << "primOpAppPrimOp: starting at " << valuePtr;
        while (valuePtr && valuePtr->isPrimOpApp()) {
            valuePtr = valuePtr->primOpAppLeft();
            std::cerr << "primOpAppPrimOp: -> " << valuePtr;
        }

        if (!valuePtr) {
            std::cerr << "primOpAppPrimOp: -> nullptr" << std::endl;
            return nullptr;
        }
        auto primOpPtr = valuePtr->primOp();
        std::cerr << "primOpAppPrimOp: finished at " << primOpPtr << std::endl;
        return primOpPtr;
    }
    // {
    //     Value * left = primOpAppLeft();
    //     while (left && !left->isPrimOp()) {
    //         left = left->primOpAppLeft();
    //     }

    //     if (!left)
    //         return nullptr;
    //     return left->primOp();
    // }


    // Common utility for functions
    /* inline */ bool isFunction() const
    { return isLambda() || isPrimOp() || isPrimOpApp(); }


    // Methods for closures
    /* inline */ bool isClosure() const
    { return std::holds_alternative<ClosureThunk>(payload); };

    /* inline */ void mkClosure(Env * e, Expr * ex)
    {
        // assert(e);
        assert(ex);
        payload.emplace<ClosureThunk>(ClosureThunk{ .env = e, .expr = ex });
        assert(isValid());
    }

    /* inline */ Env * closureEnv() const
    {
        auto closure = std::get<ClosureThunk>(payload);
        assert(closure.env);
        return closure.env;
    }

    /* inline */ Expr * closureExpr() const
    {
        auto closure = std::get<ClosureThunk>(payload);
        assert(closure.expr);
        return closure.expr;
    }


    // Methods for function applications
    /* inline */ bool isApp() const
    { return std::holds_alternative<FuncAppThunk>(payload); };

    /* inline */ void mkApp(Value * l, Value * r)
    {
        assert(l);
        assert(l->isValid());
        assert(r);
        assert(r->isValid());
        payload.emplace<FuncAppThunk>(FuncAppThunk{ .left = l, .right = r });
        assert(isValid());
    }

    /* inline */ Value * appLeft() const
    {
        auto app = std::get<FuncAppThunk>(payload);
        assert(app.left);
        assert(app.left->isValid());
        return app.left;
    }

    /* inline */ Value * appRight() const
    {
        auto app = std::get<FuncAppThunk>(payload);
        assert(app.right);
        assert(app.right->isValid());
        return app.right;
    }


    // Common utility for thunks
    /* inline */ bool isThunk(bool invalidIsThunk = false) const
    { return isClosure() || isApp() || (invalidIsThunk && !isValid()); }


    // Methods for external values
    /* inline */ bool isExternal() const
    { return std::holds_alternative<ExternalValueBase *>(payload); };

    /* inline */ void mkExternal(ExternalValueBase * e)
    { payload.emplace<ExternalValueBase *>(e); }

    /* inline */ ExternalValueBase * external() const
    { return std::get<ExternalValueBase *>(payload); }

    /* inline */ bool isBlackhole() const
    {
        auto maybeThunk = std::get_if<ClosureThunk>(&payload);
        return maybeThunk && maybeThunk->expr == (Expr*) &eBlackHole;
    }

    /* inline */ void mkBlackhole()
    { mkClosure(nullptr, (Expr *) &eBlackHole); }


    


    // Utility methods
    // Defined in eval.hh
    /* inline */ PosIdx determinePos(const PosIdx pos) const;

    // Defined in eval.cc
    void print(EvalState &state, std::ostream &str, PrintOptions options = PrintOptions {});
};


#if HAVE_BOEHMGC
typedef std::vector<Value *, traceable_allocator<Value *>> ValueVector;
typedef std::map<Symbol, Value *, std::less<Symbol>, traceable_allocator<std::pair<const Symbol, Value *>>> ValueMap;
typedef std::map<Symbol, ValueVector, std::less<Symbol>, traceable_allocator<std::pair<const Symbol, ValueVector>>> ValueVectorMap;
#else
typedef std::vector<Value *> ValueVector;
typedef std::map<Symbol, Value *> ValueMap;
typedef std::map<Symbol, ValueVector> ValueVectorMap;
#endif


/**
 * A value allocated in traceable memory.
 */
typedef std::shared_ptr<Value *> RootValue;

RootValue allocRootValue(Value * v);

}
