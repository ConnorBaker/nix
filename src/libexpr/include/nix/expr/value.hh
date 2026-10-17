#pragma once
///@file

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <exception>
#include <span>
#include <string_view>
#include <type_traits>
#include <concepts>

#include "nix/expr/eval-gc.hh"
#include "nix/expr/value/context.hh"
#include "nix/util/source-path.hh"
#include "nix/expr/print-options.hh"
#include "nix/util/checked-arithmetic.hh"

#include <boost/unordered/unordered_flat_map_fwd.hpp>
#include <nlohmann/json_fwd.hpp>

#if defined(__x86_64__) && defined(__SSE2__)
#  include <emmintrin.h>
#endif

namespace nix {

struct Value;
class BindingsBuilder;

/**
 * Internal type discriminator, which is more detailed than `ValueType`, as
 * it specifies the exact representation used (for types that have multiple
 * possible representations).
 *
 * @warning The ordering is very significant. See ValueStorage::getInternalType() for details
 * about how this is mapped into the alignment bits to save significant memory.
 * This also restricts the number of internal types represented with distinct memory layouts.
 */
enum InternalType {
    tUninitialized = 0,
    /* layout: Single/zero field payload */
    tInt = 1,
    tBool,
    tNull,
    tFloat,
    tFailed,
    tExternal,
    tPrimOp,
    tAttrs,
    /* layout: Pair of pointers payload */
    tFirstPairOfPointers,
    tListSmall = tFirstPairOfPointers,
    tPrimOpApp,
    tApp,
    tThunk,
    tLambda,
    tLastPairOfPointers = tLambda,
    /* layout: Single untaggable field */
    tFirstSingleUntaggable,
    tListN = tFirstSingleUntaggable,
    tString,
    tPath,
    tNumberOfInternalTypes, // Must be last
};

/**
 * This type abstracts over all actual value types in the language,
 * grouping together implementation details like tList*, different function
 * types, and types in non-normal form (so thunks and co.)
 */
typedef enum {
    nThunk,
    nFailed,
    nInt,
    nFloat,
    nBool,
    nString,
    nPath,
    nNull,
    nAttrs,
    nList,
    nFunction,
    nExternal,
} ValueType;

class Bindings;
struct Env;
struct Expr;
struct ExprLambda;
struct ExprBlackHole;
struct PrimOp;
class Symbol;
class SymbolStr;
class PosIdx;
struct Pos;
class StorePath;
class EvalState;
class EvalMemory;
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
     * Print the value as JSON. Defaults to unconvertible, i.e. throws an error
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
        StringSet & drvsSeen,
        const PosIdx pos) const;

    virtual ~ExternalValueBase() {};
};

std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v);

class ListBuilder
{
    const size_t size;
    Value * inlineElems[2] = {nullptr, nullptr};
public:
    Value ** elems;
    ListBuilder(EvalMemory & mem, size_t size);

    ListBuilder(ListBuilder && x) noexcept
        : size(x.size)
        , inlineElems{x.inlineElems[0], x.inlineElems[1]}
        , elems(size <= 2 ? inlineElems : x.elems)
    {
    }

    ListBuilder(const ListBuilder &) = delete;
    ListBuilder & operator=(ListBuilder &&) = delete;
    ListBuilder & operator=(const ListBuilder &) = delete;
    ~ListBuilder() = default;

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

class StringData
{
public:
    using size_type = std::size_t;

    size_type size_;
    char data_[];

    /*
     * This in particular ensures that we cannot have a `StringData`
     * that we use by value, which is just what we want!
     *
     * Dynamically sized types aren't a thing in C++ and even flexible array
     * members are a language extension and beyond the realm of standard C++.
     * Technically, sizeof data_ member is 0 and the intended way to use flexible
     * array members is to allocate sizeof(StrindData) + count * sizeof(char) bytes
     * and the compiler will consider alignment restrictions for the FAM.
     *
     */

    StringData(StringData &&) = delete;
    StringData & operator=(StringData &&) = delete;
    StringData(const StringData &) = delete;
    StringData & operator=(const StringData &) = delete;
    ~StringData() = default;

private:
    StringData() = delete;

    explicit StringData(size_type size)
        : size_(size)
    {
    }

public:
    /**
     * Allocate StringData on the (possibly) GC-managed heap and copy
     * the contents of s to it.
     */
    static const StringData & make(EvalMemory & mem, std::string_view s);

    /**
     * Allocate StringData on the (possibly) GC-managed heap.
     * @param size Length of the string (without the NUL terminator).
     */
    static StringData & alloc(EvalMemory & mem, size_t size);

    size_t size() const
    {
        return size_;
    }

    char * data() noexcept
    {
        return data_;
    }

    const char * data() const noexcept
    {
        return data_;
    }

    const char * c_str() const noexcept
    {
        return data_;
    }

    constexpr std::string_view view() const noexcept
    {
        return std::string_view(data_, size_);
    }

    template<size_t N>
    struct Static;

    static StringData & make(std::pmr::memory_resource & resource, std::string_view s)
    {
        auto & res =
            *new (resource.allocate(sizeof(StringData) + s.size() + 1, alignof(StringData))) StringData(s.size());
        std::memcpy(res.data_, s.data(), s.size());
        res.data_[s.size()] = '\0';
        return res;
    }
};

namespace detail {

/**
 * Implementation mixin class for defining the public types
 * In can be inherited from by the actual ValueStorage implementations
 * for free due to Empty Base Class Optimization (EBCO).
 */
struct ValueBase
{
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
    struct StringWithContext
    {
        const StringData * str;

        /**
         * The type of the context itself.
         *
         * Currently, it is length-prefixed array of pointers to
         * null-terminated strings. The strings are specially formatted
         * to represent a flattening of the recursive sum type that is a
         * context element.
         *
         * @see NixStringContext for an more easily understood type,
         * that of the "builder" for this data structure.
         */
        struct Context
        {
            using value_type = const StringData *;
            using size_type = std::size_t;
            using iterator = const value_type *;

            Context(size_type size)
                : size_(size)
            {
            }

        private:
            /**
             * Number of items in the array
             */
            size_type size_;

            /**
             * @pre must be in sorted order
             */
            value_type elems[];

        public:
            iterator begin() const
            {
                return elems;
            }

            iterator end() const
            {
                return elems + size();
            }

            size_type size() const
            {
                return size_;
            }

            /**
             * @return null pointer when context.empty()
             */
            static Context * fromBuilder(const NixStringContext & context, EvalMemory & mem);
        };

        /**
         * May be null for a string without context.
         */
        const Context * context;
    };

    struct Path
    {
        SourceAccessor * accessor;
        const StringData * path;
    };

    struct Null
    {};

    struct ClosureThunk
    {
        Env * env;
        Expr * expr;
    };

    struct FunctionApplicationThunk
    {
        Value *left, *right;
    };

    /**
     * Like FunctionApplicationThunk, but must be a distinct type in order to
     * resolve overloads to `tPrimOpApp` instead of `tApp`.
     * This type helps with the efficient implementation of arity>=2 primop calls.
     */
    struct PrimOpApplicationThunk
    {
        Value *left, *right;
    };

    struct Lambda
    {
        Env * env;
        ExprLambda * fun;
    };

    using SmallList = std::array<Value *, 2>;

    struct List
    {
        size_t size;
        Value * const * elems;
    };

    /**
     * Wrapper that stores a std::exception_ptr on the GC heap with a finaliser
     * that runs the exception_ptr destructor (which is refcounted internally).
     * This is not a part of the Failed structure to avoid cycles with finalisers,
     * which Boehm warns about.
     */
    struct ExceptionRef : gc_cleanup
    {
        ExceptionRef(std::exception_ptr ex)
            : ex(std::move(ex))
        {
            assert(this->ex);
        }

        ExceptionRef(ExceptionRef &&) = delete;
        ExceptionRef(const ExceptionRef &) = delete;
        ExceptionRef & operator=(ExceptionRef &&) = delete;
        ExceptionRef & operator=(const ExceptionRef &) = delete;

        /* To appease -Wweak-vtables. */
        virtual ~ExceptionRef();

        std::exception_ptr ex;
    };

    struct Failed : gc
    {
        ExceptionRef * exRef;

        /**
         * Optional value for recovering `RecoverableEvalError`
         * Must be set iff `ex` is an instance of `RecoverableEvalError`.
         */
        Value * recoveryValue;

        Failed(std::exception_ptr ex, Value * recoveryValue)
            : exRef(new /* ExceptionRef : gc_cleanup */ ExceptionRef(ex))
            , recoveryValue(recoveryValue)
        {
        }

        [[noreturn]] void rethrow() const
        {
            try {
                std::rethrow_exception(exRef->ex);
            } catch (BaseError & e) {
                /* Rethrow the copy of the exception - not the original one.
                   Stack tracing mechanisms rely on being able to modify the exceptions
                   they catch by reference. */
                e.throwClone();
            } catch (...) {
                throw;
            }
            unreachable();
        }
    };
};

template<typename T>
struct PayloadTypeToInternalType
{};

/**
 * All stored types must be distinct (not type aliases) for the purposes of
 * overload resolution in setStorage. This ensures there's a bijection from
 * InternalType <-> C++ type.
 */
#define NIX_VALUE_STORAGE_FOR_EACH_FIELD(MACRO)                     \
    MACRO(NixInt, integer, tInt)                                    \
    MACRO(bool, boolean, tBool)                                     \
    MACRO(ValueBase::StringWithContext, string, tString)            \
    MACRO(ValueBase::Path, path, tPath)                             \
    MACRO(ValueBase::Null, null_, tNull)                            \
    MACRO(const Bindings *, attrs, tAttrs)                          \
    MACRO(ValueBase::List, bigList, tListN)                         \
    MACRO(ValueBase::SmallList, smallList, tListSmall)              \
    MACRO(ValueBase::ClosureThunk, thunk, tThunk)                   \
    MACRO(ValueBase::FunctionApplicationThunk, app, tApp)           \
    MACRO(ValueBase::Lambda, lambda, tLambda)                       \
    MACRO(PrimOp *, primOp, tPrimOp)                                \
    MACRO(ValueBase::PrimOpApplicationThunk, primOpApp, tPrimOpApp) \
    MACRO(ExternalValueBase *, external, tExternal)                 \
    MACRO(ValueBase::Failed *, failed, tFailed)                     \
    MACRO(NixFloat, fpoint, tFloat)

#define NIX_VALUE_PAYLOAD_TYPE(T, FIELD_NAME, DISCRIMINATOR) \
    template<>                                               \
    struct PayloadTypeToInternalType<T>                      \
    {                                                        \
        static constexpr InternalType value = DISCRIMINATOR; \
    };

NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_PAYLOAD_TYPE)

#undef NIX_VALUE_PAYLOAD_TYPE

template<typename T>
inline constexpr InternalType payloadTypeToInternalType = PayloadTypeToInternalType<T>::value;

} // namespace detail

/**
 * Discriminated union of types stored in the value.
 * The union discriminator is @ref InternalType enumeration.
 *
 * This class can be specialized with a non-type template parameter
 * of pointer size for more optimized data layouts on when pointer alignment
 * bits can be used for storing the discriminator.
 *
 * All specializations of this type need to implement getStorage, setStorage and
 * getInternalType methods.
 */
template<std::size_t ptrSize, typename Enable = void>
class ValueStorage : public detail::ValueBase
{
protected:
    using Payload = union
    {
#define NIX_VALUE_STORAGE_DEFINE_FIELD(T, FIELD_NAME, DISCRIMINATOR) T FIELD_NAME;
        NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_STORAGE_DEFINE_FIELD)
#undef NIX_VALUE_STORAGE_DEFINE_FIELD
    };

private:
    InternalType internalType = tUninitialized;
    Payload payload;

protected:
#define NIX_VALUE_STORAGE_GET_IMPL(K, FIELD_NAME, DISCRIMINATOR) \
    void getStorage(K & val) const noexcept                      \
    {                                                            \
        assert(internalType == DISCRIMINATOR);                   \
        val = payload.FIELD_NAME;                                \
    }

#define NIX_VALUE_STORAGE_SET_IMPL(K, FIELD_NAME, DISCRIMINATOR) \
    void setStorage(K val) noexcept                              \
    {                                                            \
        payload.FIELD_NAME = val;                                \
        internalType = DISCRIMINATOR;                            \
    }

    NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_STORAGE_GET_IMPL)
    NIX_VALUE_STORAGE_FOR_EACH_FIELD(NIX_VALUE_STORAGE_SET_IMPL)

#undef NIX_VALUE_STORAGE_SET_IMPL
#undef NIX_VALUE_STORAGE_GET_IMPL
#undef NIX_VALUE_STORAGE_FOR_EACH_FIELD

    /** Get internal type currently occupying the storage. */
    InternalType getInternalType() const noexcept
    {
        return internalType;
    }

    static bool isAtomic()
    {
        return false;
    }
};

namespace detail {

/* Whether to use a specialization of ValueStorage that does bitpacking into
   alignment niches. */
template<std::size_t ptrSize>
inline constexpr bool useBitPackedValueStorage = (ptrSize == 8) && (__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= 16);

/* inc2b-1 (parallel eval): armed iff an Executor with eval-cores > 1 exists.
   While disarmed (eval-cores == 1, the default and the inc2b-1 gate) every
   value write is a plain store -- byte-identical to inc2a, zero added atomics,
   no waiters can exist. When armed, the write that *completes a concurrent
   force* (the one landing on a pdPending cell) is published with a release
   exchange so a waiter's acquire-load sees the whole value, and awaiters are
   woken. A relaxed load + predicted-not-taken branch is the only cost on the
   disarmed hot path. */
extern std::atomic<bool> parallelForcingArmed;

/* Wake any threads blocked in `EvalState::waitOnThunk` on the value at
   `valueAddr`. Defined in parallel-eval.cc; declared here so the storage's
   completion-publish can call it without pulling in the executor headers. */
void notifyValueWaiters(void * valueAddr) noexcept;

} // namespace detail

/**
 * Value storage that is optimized for 64 bit systems.
 * Packs discriminator bits into the pointer alignment niches.
 */
template<std::size_t ptrSize>
class alignas(16)
    ValueStorage<ptrSize, std::enable_if_t<detail::useBitPackedValueStorage<ptrSize>>> : public detail::ValueBase
{
    /* Needs a dependent type name in order for member functions (and
     * potentially ill-formed bit casts) to be SFINAE'd out.
     *
     * Otherwise some member functions could possibly be instantiated for 32 bit
     * systems and fail due to an unsatisfied constraint.
     */
    template<std::size_t size>
    struct PackedPointerTypeStruct
    {
        using type = std::uint64_t;
    };

    using PackedPointer = typename PackedPointerTypeStruct<ptrSize>::type;
    using Payload = std::array<PackedPointer, 2>;
    /* Inc2a: the 16-byte storage stays a plain two-word payload (word 0 = the
       tag word "p0", word 1 = "p1"), so the ubiquitous readers/writers
       (getInternalType / getStorage / setStorage via loadPayload/updatePayload)
       keep Inc1's exact `ldp`/`stp` codegen -- no per-access cost and no
       hot-path frame change.

       The concurrent-forcing protocol reaches word 0 atomically through
       `std::atomic_ref` ONLY at the force site: an acquire load to classify and
       a compare-exchange to blackhole the cell (pdThunk -> pdPending). That is
       DetSys's "two atomics per force", confined to the force path rather than
       every value write. Word 0 sits at offset 0 of a 16-aligned Value, so it is
       8-aligned and a valid atomic_ref target.

       The old __m128i MOVAPS torn-read scheme (an x86-only defense against a
       concurrent Boehm collector reading a half-written Value) is dropped: under
       nogc+mimalloc the only concurrent reader is another eval worker, which
       goes through this acquire/release protocol. On aarch64 (our build) Inc1
       already used this plain path, so loadPayload/updatePayload are unchanged.

       sizeof stays 16 / alignof 16 (Payload is two 8-byte words, class is
       alignas(16)), so the two-level clock's `>>4` granule and the striped-arena
       math are unchanged (asserted after Value -- Risk R1). */
    Payload payloadWords = {};

    using AtomicWord0 = std::atomic_ref<PackedPointer>;

    static constexpr int discriminatorBits = 3;
    static constexpr PackedPointer discriminatorMask = (PackedPointer(1) << discriminatorBits) - 1;

    /* inc2b-1: a pending (being-forced) cell packs the forcing thread's id in
       the word above the discriminator bits, and carries "a waiter is blocked
       on this cell" as the top bit. There is no `pdAwaited` primary
       discriminator (all eight 3-bit slots are taken, see below), so awaited is
       a flag on the pdPending word rather than a distinct tag. Both pending and
       awaited therefore still read `pd == pdPending` in the low bits, so every
       blackhole/pending observer (isBlackhole, getInternalType) is unchanged.

         pending, no waiter :  pdPending | (tid << discriminatorBits)
         pending, awaited   :  pdPending | (tid << discriminatorBits) | awaitedBit

       tid is small (<= eval-cores + 1, i.e. a few bits), so it never reaches
       awaitedBit. */
    static constexpr PackedPointer awaitedBit = PackedPointer(1) << 63;

    /**
     * The value is stored as a pair of 8-byte double words. All pointers are assumed
     * to be 8-byte aligned. This gives us at most 6 bits of discriminator bits
     * of free storage. In some cases when one double word can't be tagged the whole
     * discriminator is stored in the first double word.
     *
     * The layout of discriminator bits is determined by the 3 bits of PrimaryDiscriminator,
     * which are always stored in the lower 3 bits of the first dword of the payload.
     * The memory layout has 3 types depending on the PrimaryDiscriminator value.
     *
     * PrimaryDiscriminator::pdSingleDWord - Only the second dword carries the data.
     * That leaves the first 8 bytes free for storing the InternalType in the upper
     * bits.
     *
     * PrimaryDiscriminator::pdListN - pdPath - Only has 3 available padding bits
     * because:
     * - tListN needs a size, whose lower bits we can't borrow.
     * - tString and tPath have C-string fields, which don't necessarily need to
     * be aligned.
     *
     * In this case we reserve their discriminators directly in the PrimaryDiscriminator
     * bits stored in payload[0].
     *
     * PrimaryDiscriminator::pdPairOfPointers - Payloads that consist of a pair of pointers.
     * In this case the 3 lower bits of payload[1] can be tagged.
     *
     * The primary discriminator with value 0 is reserved for uninitialized Values,
     * which are useful for diagnostics in C bindings.
     */
    enum PrimaryDiscriminator : int {
        pdUninitialized = 0,
        pdSingleDWord, //< layout: Single/zero field payload
        /* The order of these enumerations must be the same as in InternalType. */
        pdListN, //< layout: Single untaggable field.
        pdString,
        pdPath,
        pdPairOfPointers, //< layout: Pair of pointers payload (tListSmall/tPrimOpApp/tLambda)
        /* Inc2a: thunks/apps get their own primary discriminator so a single
           atomic read of p0 decides "forceable", and the blackhole CAS flips
           p0 pdThunk -> pdPending without touching p1. tThunk/tApp keep their
           existing p1 subtag, decoded exactly like pdPairOfPointers. pdPending
           is a cell mid-force (the cross-thread blackhole); at threads=1 it is
           only ever the self-recursion sentinel. Eight primary discriminators
           still fit in the 3 discriminator bits. */
        pdThunk, //< layout: Pair of pointers payload (tThunk/tApp), forceable
        pdPending, //< a cell currently being forced (blackhole)
    };

    [[gnu::always_inline]]
    void updatePayload(Payload payload) noexcept
    {
        /* Byte-identical to Inc1: a plain two-word store (stp). Value creation
           and the force-result publish both go through here. At threads=1 no
           reader races the write. Inc3's escape/notify seam makes the
           force-completion publish a release store + old-tag check (via
           AtomicWord0) once the Executor and waiters exist. */
        payloadWords = payload;
    }

    [[gnu::always_inline]]
    Payload loadPayload() const noexcept
    {
        /* Byte-identical to Inc1: a plain two-word load (ldp). The force site
           reads word 0 with the atomic acquire loader below for cross-thread
           ordering. */
        return payloadWords;
    }

    /* inc2b-1: the store used by the mk* / setStorage result-producing writes
       (NOT by the copy/move/default constructors, which keep the plain
       `updatePayload`). This is where a concurrently-forced thunk's result is
       published.

       Fast path (disarmed, i.e. eval-cores == 1, the inc2b-1 gate): a plain
       two-word store (`stp`) -- byte-identical to inc2a. The only addition is a
       relaxed bool load + a predicted-not-taken branch.

       Slow path (armed AND the cell is currently pdPending, i.e. this write
       completes a concurrent force): write word 1 first, then publish word 0
       with a release exchange so a waiter's acquire-load of word 0 orders the
       plain read of word 1 after it. The exchange returns the old word 0; if it
       had the awaited bit, a waiter is blocked and is woken.

       This stays correct because a fresh cell is zero-initialised (mi_zalloc /
       the zeroed arena), so its word 0 is pdUninitialised (0) and never
       spuriously reads as pdPending; only a cell the force site marked pending
       takes the publish branch. */
    [[gnu::always_inline]]
    void publishOrStorePayload(Payload payload) noexcept
    {
        /* Keep the inlined footprint minimal (this is in every mk* or assignment,
           so it affects the stack frame of every value-producing eval): just a
           relaxed bool load + predicted-not-taken branch, then the plain store.
           The armed completion (atomic release exchange + wake) is out of line. */
        if (detail::parallelForcingArmed.load(std::memory_order_relaxed)) [[unlikely]] {
            if ((payloadWords[0] & discriminatorMask) == pdPending) {
                publishCompletionSlow(payload);
                return;
            }
        }
        payloadWords = payload;
    }

    [[gnu::noinline]]
    void publishCompletionSlow(Payload payload) noexcept
    {
        /* This write completes a concurrently-forced thunk. Publish word 1 then
           word 0 with a release exchange so a waiter's acquire-load of word 0
           orders its plain read of word 1 after it; wake an awaiter if one is
           flagged. */
        payloadWords[1] = payload[1];
        PackedPointer old = AtomicWord0(payloadWords[0]).exchange(payload[0], std::memory_order_release);
        if (old & awaitedBit)
            detail::notifyValueWaiters(this);
    }

protected:
    /* --- Inc2a concurrent-forcing protocol primitives (protected: called by
       Value's public fv* wrappers). These reach word 0 (payloadWords[0], the
       tag word) atomically via std::atomic_ref; everything else stays plain. */

    /* The force site's single ordered read of the tag word. p1 (word 1) is read
       after this (via fpWord1), so a release-published value is seen whole. */
    [[gnu::always_inline]]
    PackedPointer fpLoadWord0Acquire() noexcept
    {
        return AtomicWord0(payloadWords[0]).load(std::memory_order_acquire);
    }

    [[gnu::always_inline]]
    PackedPointer fpWord0Plain() const noexcept
    {
        return payloadWords[0];
    }

    [[gnu::always_inline]]
    PackedPointer fpWord1() const noexcept
    {
        return payloadWords[1];
    }

    static bool fpTagIsThunk(PackedPointer w0) noexcept
    {
        return (w0 & discriminatorMask) == pdThunk;
    }

    static bool fpTagIsPending(PackedPointer w0) noexcept
    {
        return (w0 & discriminatorMask) == pdPending;
    }

    /* inc2b-1 pending-word encoding helpers (see awaitedBit above). */
    static PackedPointer makePendingWord(uint32_t threadId) noexcept
    {
        return static_cast<PackedPointer>(pdPending) | (PackedPointer(threadId) << discriminatorBits);
    }

    static uint32_t pendingThreadId(PackedPointer w0) noexcept
    {
        return static_cast<uint32_t>((w0 & ~awaitedBit) >> discriminatorBits);
    }

    static bool tagIsAwaited(PackedPointer w0) noexcept
    {
        return (w0 & awaitedBit) != 0;
    }

    static bool fpSubtagIsApp(PackedPointer w1) noexcept
    {
        return (w1 & discriminatorMask) == PackedPointer(tApp - tFirstPairOfPointers);
    }

    /* Elect the unique forcer: CAS word 0 from the captured thunk tag
       `expected` (a genuine pdThunk word) to pdPending (the blackhole).
       `expected` is updated with the live word 0 on failure -- a failure means
       another worker raced us to this cell (it is now pending/awaited/finished),
       so the loser reads `expected` to decide whether to wait.

       Note: this writes a bare pdPending (no forcing-thread id), byte-identical
       to inc2a, so the inlined force site needs no thread-local read on the hot
       path and keeps inc2a's stack-parity edge. The forcing thread is not
       recorded in the tag in inc2b-1; distinguishing a same-thread self-cycle
       from a cross-thread wait while threads run (eval-cores > 1) is deferred to
       inc2c. At eval-cores == 1 a pending cell is always a self-cycle, detected
       via the executor being disabled (see EvalState::forceValuePending). */
    [[gnu::always_inline]]
    bool fpMarkPending(PackedPointer & expected) noexcept
    {
        return AtomicWord0(payloadWords[0])
            .compare_exchange_strong(
                expected, static_cast<PackedPointer>(pdPending), std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /* Mark a pending cell as awaited (a waiter is about to block on it): CAS
       word 0 from `expected` (pdPending, not yet awaited) to the same word with
       `awaitedBit` set. On failure `expected` holds the live word 0 (either the
       cell was finished in the meantime, or another waiter already set awaited).
       Acquire on both so a success/finished-observation orders the value read
       after it. */
    [[gnu::always_inline]]
    bool fpCasPendingToAwaited(PackedPointer & expected) noexcept
    {
        return AtomicWord0(payloadWords[0])
            .compare_exchange_strong(
                expected, expected | awaitedBit, std::memory_order_acquire, std::memory_order_acquire);
    }

    bool fpIsPendingTag() const noexcept
    {
        /* A relaxed/plain read of the tag suffices (single-threaded, or on an
           already-published cell). */
        return fpTagIsPending(payloadWords[0]);
    }

    void fpSetPending(PackedPointer p1bits) noexcept
    {
        updatePayload({static_cast<PackedPointer>(pdPending), p1bits});
    }

private:
    template<typename T>
        requires std::is_pointer_v<T>
    static T untagPointer(PackedPointer val) noexcept
    {
        return std::bit_cast<T>(val & ~discriminatorMask);
    }

    PrimaryDiscriminator getPrimaryDiscriminator(PackedPointer firstDWord) const noexcept
    {
        return static_cast<PrimaryDiscriminator>(firstDWord & discriminatorMask);
    }

    static void assertAligned(PackedPointer val) noexcept
    {
        assert((val & discriminatorMask) == 0 && "Pointer is not 8 bytes aligned");
    }

    template<InternalType type>
    void setSingleDWordPayload(PackedPointer untaggedVal) noexcept
    {
        Payload payload;
        /* There's plenty of free upper bits in the first dword, which is
           used only for the discriminator. */
        payload[0] = static_cast<int>(pdSingleDWord) | (static_cast<int>(type) << discriminatorBits);
        payload[1] = untaggedVal;
        publishOrStorePayload(payload);
    }

    template<PrimaryDiscriminator discriminator, typename T, typename U>
    void setUntaggablePayload(T * firstPtrField, U untaggableField) noexcept
    {
        Payload payload;
        static_assert(discriminator >= pdListN && discriminator <= pdPath);
        auto firstFieldPayload = std::bit_cast<PackedPointer>(firstPtrField);
        assertAligned(firstFieldPayload);
        payload[0] = static_cast<int>(discriminator) | firstFieldPayload;
        payload[1] = std::bit_cast<PackedPointer>(untaggableField);
        publishOrStorePayload(payload);
    }

    template<InternalType type, typename T, typename U>
    void setPairOfPointersPayload(T * firstPtrField, U * secondPtrField) noexcept
    {
        Payload payload;
        static_assert(type >= tFirstPairOfPointers && type <= tLastPairOfPointers);
        /* Inc2a: tThunk/tApp go under pdThunk so p0 alone marks a forceable
           cell; the remaining pairs stay pdPairOfPointers. The p1 subtag is
           identical in both, so getInternalType decodes them the same way. */
        constexpr PrimaryDiscriminator primary = (type == tThunk || type == tApp) ? pdThunk : pdPairOfPointers;
        {
            auto firstFieldPayload = std::bit_cast<PackedPointer>(firstPtrField);
            assertAligned(firstFieldPayload);
            payload[0] = static_cast<int>(primary) | firstFieldPayload;
        }
        {
            auto secondFieldPayload = std::bit_cast<PackedPointer>(secondPtrField);
            assertAligned(secondFieldPayload);
            payload[1] = (type - tFirstPairOfPointers) | secondFieldPayload;
        }
        publishOrStorePayload(payload);
    }

    template<typename T, typename U>
        requires std::is_pointer_v<T> && std::is_pointer_v<U>
    void getPairOfPointersPayload(T & firstPtrField, U & secondPtrField) const noexcept
    {
        Payload payload = loadPayload();
        firstPtrField = untagPointer<T>(payload[0]);
        secondPtrField = untagPointer<U>(payload[1]);
    }

public:
    ValueStorage()
    {
        updatePayload({});
    }

    ValueStorage(const ValueStorage & other)
    {
        updatePayload(other.loadPayload());
    }

    ValueStorage & operator=(const ValueStorage & other)
    {
        /* inc2b-1: many eval paths complete a force via `v = *other` (copy
           assignment) rather than a mk*, so the completion-publish must cover
           it too. `this` is a live object (valid, zero-inited word 0), so the
           pdPending check is safe here (unlike the copy *constructor*, whose
           `this` is uninitialised and stays on the plain path). */
        publishOrStorePayload(other.loadPayload());
        return *this;
    }

    ValueStorage(ValueStorage && other) noexcept
    {
        updatePayload(other.loadPayload());
        other.updatePayload({}); // Zero out rhs
    }

    ValueStorage & operator=(ValueStorage && other) noexcept
    {
        publishOrStorePayload(other.loadPayload());
        other.updatePayload({}); // Zero out rhs
        return *this;
    }

    ~ValueStorage() noexcept {}

protected:
    static bool isAtomic();

    /** Get internal type currently occupying the storage. */
    InternalType getInternalType() const noexcept
    {
        Payload payload = loadPayload();
        switch (auto pd = getPrimaryDiscriminator(payload[0])) {
        case pdUninitialized:
            /* Discriminator value of zero is used to distinguish uninitialized values. */
            return tUninitialized;
        case pdSingleDWord:
            /* Payloads that only use up a single double word store the InternalType
               in the upper bits of the first double word. */
            return InternalType(payload[0] >> discriminatorBits);
        /* The order must match that of the enumerations defined in InternalType. */
        case pdListN:
        case pdString:
        case pdPath:
            return static_cast<InternalType>(tFirstSingleUntaggable + (pd - pdListN));
        case pdPairOfPointers:
        case pdThunk:
            /* pdThunk (tThunk/tApp) and pdPairOfPointers (tListSmall/
               tPrimOpApp/tLambda) share the p1 subtag encoding, so they decode
               the same way; only p0's primary tag distinguishes "forceable". */
            return static_cast<InternalType>(tFirstPairOfPointers + (payload[1] & discriminatorMask));
        case pdPending:
            /* A cell mid-force (blackhole). Reported as a thunk, exactly as the
               old &eBlackHole sentinel thunk was, so type()/determinePos/
               printing are unchanged. */
            return tThunk;
        [[unlikely]] default:
            nixUnreachableWhenHardened();
        }
    }

#define NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS(TYPE, MEMBER_A, MEMBER_B)                                   \
                                                                                                       \
    void getStorage(TYPE & val) const noexcept                                                         \
    {                                                                                                  \
        getPairOfPointersPayload(val MEMBER_A, val MEMBER_B);                                          \
    }                                                                                                  \
                                                                                                       \
    void setStorage(TYPE val) noexcept                                                                 \
    {                                                                                                  \
        setPairOfPointersPayload<detail::payloadTypeToInternalType<TYPE>>(val MEMBER_A, val MEMBER_B); \
    }

    NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS(SmallList, [0], [1])
    NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS(PrimOpApplicationThunk, .left, .right)
    NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS(FunctionApplicationThunk, .left, .right)
    NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS(ClosureThunk, .env, .expr)
    NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS(Lambda, .env, .fun)

#undef NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS

    void getStorage(NixInt & integer) const noexcept
    {
        Payload payload = loadPayload();
        /* PackedPointerType -> int64_t here is well-formed, since the standard requires
           this conversion to follow 2's complement rules. This is just a no-op. */
        integer = NixInt(payload[1]);
    }

    void getStorage(bool & boolean) const noexcept
    {
        Payload payload = loadPayload();
        boolean = payload[1];
    }

    void getStorage(Null & null) const noexcept {}

    void getStorage(NixFloat & fpoint) const noexcept
    {
        Payload payload = loadPayload();
        fpoint = std::bit_cast<NixFloat>(payload[1]);
    }

    void getStorage(ExternalValueBase *& external) const noexcept
    {
        Payload payload = loadPayload();
        external = std::bit_cast<ExternalValueBase *>(payload[1]);
    }

    void getStorage(PrimOp *& primOp) const noexcept
    {
        Payload payload = loadPayload();
        primOp = std::bit_cast<PrimOp *>(payload[1]);
    }

    void getStorage(const Bindings *& attrs) const noexcept
    {
        Payload payload = loadPayload();
        attrs = std::bit_cast<Bindings *>(payload[1]);
    }

    void getStorage(List & list) const noexcept
    {
        Payload payload = loadPayload();
        list.elems = untagPointer<decltype(list.elems)>(payload[0]);
        list.size = payload[1];
    }

    void getStorage(StringWithContext & string) const noexcept
    {
        Payload payload = loadPayload();
        string.context = untagPointer<decltype(string.context)>(payload[0]);
        string.str = std::bit_cast<const StringData *>(payload[1]);
    }

    void getStorage(Path & path) const noexcept
    {
        Payload payload = loadPayload();
        path.accessor = untagPointer<decltype(path.accessor)>(payload[0]);
        path.path = std::bit_cast<const StringData *>(payload[1]);
    }

    void getStorage(Failed *& failed) const noexcept
    {
        Payload payload = loadPayload();
        failed = std::bit_cast<Failed *>(payload[1]);
    }

    void setStorage(NixInt integer) noexcept
    {
        setSingleDWordPayload<tInt>(integer.value);
    }

    void setStorage(bool boolean) noexcept
    {
        setSingleDWordPayload<tBool>(boolean);
    }

    void setStorage(Null path) noexcept
    {
        setSingleDWordPayload<tNull>(0);
    }

    void setStorage(NixFloat fpoint) noexcept
    {
        setSingleDWordPayload<tFloat>(std::bit_cast<PackedPointer>(fpoint));
    }

    void setStorage(ExternalValueBase * external) noexcept
    {
        setSingleDWordPayload<tExternal>(std::bit_cast<PackedPointer>(external));
    }

    void setStorage(PrimOp * primOp) noexcept
    {
        setSingleDWordPayload<tPrimOp>(std::bit_cast<PackedPointer>(primOp));
    }

    void setStorage(const Bindings * bindings) noexcept
    {
        setSingleDWordPayload<tAttrs>(std::bit_cast<PackedPointer>(bindings));
    }

    void setStorage(List list) noexcept
    {
        setUntaggablePayload<pdListN>(list.elems, list.size);
    }

    void setStorage(StringWithContext string) noexcept
    {
        setUntaggablePayload<pdString>(string.context, string.str);
    }

    void setStorage(Path path) noexcept
    {
        setUntaggablePayload<pdPath>(path.accessor, path.path);
    }

    void setStorage(Failed * failed) noexcept
    {
        setSingleDWordPayload<tFailed>(std::bit_cast<PackedPointer>(failed));
    }
};

/**
 * View into a list of Value * that is itself immutable.
 *
 * Since not all representations of ValueStorage can provide
 * a pointer to a const array of Value * this proxy class either
 * stores the small list inline or points to the big list.
 */
class ListView
{
    using SpanType = std::span<Value * const>;
    using SmallList = detail::ValueBase::SmallList;
    using List = detail::ValueBase::List;

    std::variant<SmallList, List> raw;

public:
    ListView(SmallList list)
        : raw(list)
    {
    }

    ListView(List list)
        : raw(list)
    {
    }

    Value * const * data() const & noexcept
    {
        return std::visit(
            overloaded{
                [](const SmallList & list) { return list.data(); }, [](const List & list) { return list.elems; }},
            raw);
    }

    std::size_t size() const noexcept
    {
        return std::visit(
            overloaded{
                [](const SmallList & list) -> std::size_t { return list.back() == nullptr ? 1 : 2; },
                [](const List & list) -> std::size_t { return list.size; }},
            raw);
    }

    Value * operator[](std::size_t i) const noexcept
    {
        return data()[i];
    }

    SpanType span() const &
    {
        return SpanType(data(), size());
    }

    /* Ensure that no dangling views can be created accidentally, as that
       would lead to hard to diagnose bugs that only affect small lists. */
    SpanType span() && = delete;
    Value * const * data() && noexcept = delete;

    /**
     * Random-access iterator that only allows iterating over a constant range
     * of mutable Value pointers.
     *
     * @note Not a pointer to minimize potential misuses and implicitly relying
     * on the iterator being a pointer.
     **/
    class iterator
    {
    public:
        using value_type = Value *;
        using pointer = const value_type *;
        using reference = const value_type &;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::random_access_iterator_tag;

    private:
        pointer ptr = nullptr;

        friend class ListView;

        iterator(pointer ptr)
            : ptr(ptr)
        {
        }

    public:
        iterator() = default;

        reference operator*() const
        {
            return *ptr;
        }

        const value_type * operator->() const
        {
            return ptr;
        }

        reference operator[](difference_type diff) const
        {
            return ptr[diff];
        }

        iterator & operator++()
        {
            ++ptr;
            return *this;
        }

        iterator operator++(int)
        {
            pointer tmp = ptr;
            ++*this;
            return iterator(tmp);
        }

        iterator & operator--()
        {
            --ptr;
            return *this;
        }

        iterator operator--(int)
        {
            pointer tmp = ptr;
            --*this;
            return iterator(tmp);
        }

        iterator & operator+=(difference_type diff)
        {
            ptr += diff;
            return *this;
        }

        iterator operator+(difference_type diff) const
        {
            return iterator(ptr + diff);
        }

        friend iterator operator+(difference_type diff, const iterator & rhs)
        {
            return iterator(diff + rhs.ptr);
        }

        iterator & operator-=(difference_type diff)
        {
            ptr -= diff;
            return *this;
        }

        iterator operator-(difference_type diff) const
        {
            return iterator(ptr - diff);
        }

        difference_type operator-(const iterator & rhs) const
        {
            return ptr - rhs.ptr;
        }

        std::strong_ordering operator<=>(const iterator & rhs) const = default;
    };

    using const_iterator = iterator;

    iterator begin() const &
    {
        return data();
    }

    iterator end() const &
    {
        return data() + size();
    }

    /* Ensure that no dangling iterators can be created accidentally, as that
       would lead to hard to diagnose bugs that only affect small lists. */
    iterator begin() && = delete;
    iterator end() && = delete;
};

static_assert(std::random_access_iterator<ListView::iterator>);

struct Value : public ValueStorage<sizeof(void *)>
{
    friend std::string showType(const Value & v);

    /**
     * Empty list constant.
     *
     * This is _not_ a singleton. Pointer equality is _not_ sufficient.
     */
    static Value vEmptyList;

    /**
     * `null` constant.
     *
     * This is _not_ a singleton. Pointer equality is _not_ sufficient.
     */
    static Value vNull;

    /**
     * `true` constant.
     *
     * This is _not_ a singleton. Pointer equality is _not_ sufficient.
     */
    static Value vTrue;

    /**
     * `true` constant.
     *
     * This is _not_ a singleton. Pointer equality is _not_ sufficient.
     */
    static Value vFalse;

private:
    template<InternalType... discriminator>
    bool isa() const noexcept
    {
        return ((getInternalType() == discriminator) || ...);
    }

    template<typename T>
    T getStorage() const noexcept
    {
        if (getInternalType() != detail::payloadTypeToInternalType<T>) [[unlikely]]
            nixUnreachableWhenHardened();
        T out;
        ValueStorage::getStorage(out);
        return out;
    }

public:

    /**
     * Never modify the backing `Value` object!
     */
    static Value * toPtr(SymbolStr str) noexcept;

    void print(EvalState & state, std::ostream & str, PrintOptions options = PrintOptions{});

    // Functions needed to distinguish the type
    // These should be removed eventually, by putting the functionality that's
    // needed by callers into methods of this type

    // type() == nThunk
    inline bool isThunk() const
    {
        return isa<tThunk>();
    }

    inline bool isApp() const
    {
        return isa<tApp>();
    }

    inline bool isBlackhole() const;

    // type() == nFunction
    inline bool isLambda() const
    {
        return isa<tLambda>();
    }

    inline bool isPrimOp() const
    {
        return isa<tPrimOp>();
    }

    inline bool isPrimOpApp() const
    {
        return isa<tPrimOpApp>();
    }

    inline bool isFailed() const
    {
        return isa<tFailed>();
    }

    /**
     * Returns the normal type of a Value. This only returns nThunk if
     * the Value hasn't been forceValue'd
     *
     * @tparam invalidIsThunk Instead of UB an an invalid (probably
     * 0, so uninitialized) internal type, return `nThunk`.
     */
    template<bool invalidIsThunk = false>
    inline ValueType type() const
    {
        /* Explicit lookup table. switch() might compile down (and it does at least with GCC 14)
           to a jump table. Let's help the compiler a bit here. */
        static constexpr auto table = [] {
            std::array<ValueType, tNumberOfInternalTypes> t{};
            t[tUninitialized] = nThunk;
            t[tInt] = nInt;
            t[tBool] = nBool;
            t[tNull] = nNull;
            t[tFloat] = nFloat;
            t[tFailed] = nFailed;
            t[tExternal] = nExternal;
            t[tAttrs] = nAttrs;
            t[tPrimOp] = nFunction;
            t[tLambda] = nFunction;
            t[tPrimOpApp] = nFunction;
            t[tApp] = nThunk;
            t[tThunk] = nThunk;
            t[tListSmall] = nList;
            t[tListN] = nList;
            t[tString] = nString;
            t[tPath] = nPath;
            return t;
        }();

        auto it = getInternalType();
        if (it == tUninitialized || it >= tNumberOfInternalTypes) [[unlikely]] {
            if constexpr (invalidIsThunk)
                return nThunk;
            else
                nixUnreachableWhenHardened();
        }

        return table[it];
    }

    /**
     * A value becomes valid when it is initialized. We don't use this
     * in the evaluator; only in the bindings, where the slight extra
     * cost is warranted because of inexperienced callers.
     */
    inline bool isValid() const noexcept
    {
        return !isa<tUninitialized>();
    }

    /**
     * Whether the value has been evaluated to WHNF, i.e. non-deep successful
     * evaluation result, or successful "root of a value".
     * See https://nix.dev/manual/nix/latest/language/evaluation.html?highlight=whnf#values
     */
    inline bool isWHNF() const noexcept
    {
        switch (getInternalType()) {
        case tUninitialized:
            panic("attempt to use uninitialized Value");
        case tFailed:
        case tApp:
        case tThunk:
            return false;
        case tInt:
        case tBool:
        case tNull:
        case tFloat:
        case tExternal:
        case tPrimOp:
        case tAttrs:
        case tListSmall:
        case tPrimOpApp: // primop-app is known to be a function, which is WHNF
        case tLambda:
        case tListN:
        case tString:
        case tPath:
            return true;
        case tNumberOfInternalTypes:
            unreachable();
        }
        unreachable();
    }

    inline void mkInt(NixInt::Inner n) noexcept
    {
        mkInt(NixInt{n});
    }

    inline void mkInt(NixInt n) noexcept
    {
        setStorage(NixInt{n});
    }

    inline void mkBool(bool b) noexcept
    {
        setStorage(b);
    }

    void mkStringNoCopy(const StringData & s, const Value::StringWithContext::Context * context = nullptr) noexcept
    {
        setStorage(StringWithContext{.str = &s, .context = context});
    }

    void mkString(std::string_view s, EvalMemory & mem);

    void mkString(std::string_view s, const NixStringContext & context, EvalMemory & mem);

    void mkStringMove(const StringData & s, const NixStringContext & context, EvalMemory & mem);

    void mkPath(const SourcePath & path, EvalMemory & mem);

    inline void mkPath(SourceAccessor * accessor, const StringData & path) noexcept
    {
        setStorage(Path{.accessor = accessor, .path = &path});
    }

    inline void mkNull() noexcept
    {
        setStorage(Null{});
    }

    inline void mkAttrs(const Bindings * a) noexcept
    {
        setStorage(a);
    }

    Value & mkAttrs(BindingsBuilder & bindings);

    void mkList(const ListBuilder & builder) noexcept
    {
        switch (builder.size) {
        case 0:
            setStorage(List{.size = 0, .elems = nullptr});
            break;
        case 1:
            setStorage(std::array<Value *, 2>{builder.inlineElems[0], nullptr});
            break;
        case 2:
            setStorage(std::array<Value *, 2>{builder.inlineElems[0], builder.inlineElems[1]});
            break;
        default:
            setStorage(List{.size = builder.size, .elems = builder.elems});
            break;
        }
    }

    inline void mkThunk(Env * e, Expr * ex) noexcept
    {
        setStorage(ClosureThunk{.env = e, .expr = ex});
    }

    inline void mkApp(Value * l, Value * r) noexcept
    {
        setStorage(FunctionApplicationThunk{.left = l, .right = r});
    }

    inline void mkLambda(Env * e, ExprLambda * f) noexcept
    {
        setStorage(Lambda{.env = e, .fun = f});
    }

    inline void mkBlackhole();

    void mkPrimOp(PrimOp * p);

    inline void mkPrimOpApp(Value * l, Value * r) noexcept
    {
        setStorage(PrimOpApplicationThunk{.left = l, .right = r});
    }

    /**
     * For a `tPrimOpApp` value, get the original `PrimOp` value.
     */
    const PrimOp * primOpAppPrimOp() const;

    inline void mkExternal(ExternalValueBase * e) noexcept
    {
        setStorage(e);
    }

    inline void mkFloat(NixFloat n) noexcept
    {
        setStorage(n);
    }

    inline void mkFailed(std::exception_ptr e, Value * recovery) noexcept
    {
        setStorage(new Value::Failed(e, recovery));
    }

    bool isList() const noexcept
    {
        return isa<tListSmall, tListN>();
    }

    ListView listView() const noexcept
    {
        return isa<tListSmall>() ? ListView(getStorage<SmallList>()) : ListView(getStorage<List>());
    }

    size_t listSize() const noexcept
    {
        return isa<tListSmall>() ? (getStorage<SmallList>()[1] == nullptr ? 1 : 2) : getStorage<List>().size;
    }

    PosIdx determinePos(const PosIdx pos) const;

    /**
     * Check whether forcing this value requires a trivial amount of
     * computation. In particular, function applications are
     * non-trivial.
     */
    bool isTrivial() const;

    SourcePath path() const
    {
        return SourcePath(
            ref(pathAccessor()->shared_from_this()), CanonPath(CanonPath::unchecked_t(), std::string(pathStrView())));
    }

    const StringData & string_data() const noexcept
    {
        return *getStorage<StringWithContext>().str;
    }

    const char * c_str() const noexcept
    {
        return getStorage<StringWithContext>().str->data();
    }

    std::string_view string_view() const noexcept
    {
        return string_data().view();
    }

    const Value::StringWithContext::Context * context() const noexcept
    {
        return getStorage<StringWithContext>().context;
    }

    ExternalValueBase * external() const noexcept
    {
        return getStorage<ExternalValueBase *>();
    }

    const Bindings * attrs() const noexcept
    {
        return getStorage<const Bindings *>();
    }

    const PrimOp * primOp() const noexcept
    {
        return getStorage<PrimOp *>();
    }

    bool boolean() const noexcept
    {
        return getStorage<bool>();
    }

    NixInt integer() const noexcept
    {
        return getStorage<NixInt>();
    }

    NixFloat fpoint() const noexcept
    {
        return getStorage<NixFloat>();
    }

    Lambda lambda() const noexcept
    {
        return getStorage<Lambda>();
    }

    /* Raw payload access for copying values out of a
       job region. */
    InternalType internalType() const noexcept
    {
        return getInternalType();
    }

    /* --- Inc2a force-site protocol (used by EvalState::forceValue) --- */
    static constexpr uint64_t fvPtrMask = ~uint64_t(0b111);
    [[gnu::always_inline]] uint64_t fvWord0Acquire() noexcept
    {
        return fpLoadWord0Acquire();
    }
    [[gnu::always_inline]] uint64_t fvWord0Plain() const noexcept
    {
        return fpWord0Plain();
    }
    [[gnu::always_inline]] uint64_t fvWord1() const noexcept
    {
        return fpWord1();
    }
    [[gnu::always_inline]] static bool fvIsThunkTag(uint64_t w0) noexcept
    {
        return fpTagIsThunk(w0);
    }
    [[gnu::always_inline]] static bool fvIsPendingTag(uint64_t w0) noexcept
    {
        return fpTagIsPending(w0);
    }
    [[gnu::always_inline]] static bool fvIsAppSubtag(uint64_t w1) noexcept
    {
        return fpSubtagIsApp(w1);
    }
    /* inc2b-1: elect the unique forcer (CAS the thunk tag to pdPending). */
    [[gnu::always_inline]] bool fvMarkPending(uint64_t & w0) noexcept
    {
        return fpMarkPending(w0);
    }
    /* inc2b-1: a waiter marks the pending cell as awaited before blocking. */
    [[gnu::always_inline]] bool fvCasAwaited(uint64_t & w0) noexcept
    {
        return fpCasPendingToAwaited(w0);
    }
    [[gnu::always_inline]] static uint32_t fvPendingThreadId(uint64_t w0) noexcept
    {
        return pendingThreadId(w0);
    }
    [[gnu::always_inline]] static bool fvIsAwaited(uint64_t w0) noexcept
    {
        return tagIsAwaited(w0);
    }
    /* Wake threads blocked in waitOnThunk on this value (used by the slow
       completion path and by notifyWaiters callers). */
    [[gnu::always_inline]] void fvNotifyWaiters() noexcept
    {
        detail::notifyValueWaiters(this);
    }
    /* A value is "finished" once it is no longer a thunk/app/pending cell --
       i.e. it has reached WHNF (or tFailed). Used by builtins.__parallel to
       skip values that are already evaluated. A pending (being-forced) cell
       reports internalType()==tThunk, so it is correctly treated as unfinished. */
    [[gnu::always_inline]] bool isFinished() const noexcept
    {
        auto it = getInternalType();
        return it != tThunk && it != tApp;
    }

    const StringData * stringData() const noexcept
    {
        return getStorage<StringWithContext>().str;
    }

    const StringData * pathData() const noexcept
    {
        return getStorage<Path>().path;
    }

    List rawList() const noexcept
    {
        return getStorage<List>();
    }

    SmallList rawSmallList() const noexcept
    {
        return getStorage<SmallList>();
    }

    void setRawList(List l) noexcept
    {
        setStorage(l);
    }

    void setRawSmallList(SmallList l) noexcept
    {
        setStorage(l);
    }

    void setRawFailed(Failed * f) noexcept
    {
        setStorage(f);
    }

    ClosureThunk thunk() const noexcept
    {
        return getStorage<ClosureThunk>();
    }

    PrimOpApplicationThunk primOpApp() const noexcept
    {
        return getStorage<PrimOpApplicationThunk>();
    }

    FunctionApplicationThunk app() const noexcept
    {
        return getStorage<FunctionApplicationThunk>();
    }

    const char * pathStr() const noexcept
    {
        return getStorage<Path>().path->c_str();
    }

    std::string_view pathStrView() const noexcept
    {
        return getStorage<Path>().path->view();
    }

    SourceAccessor * pathAccessor() const noexcept
    {
        return getStorage<Path>().accessor;
    }

    Failed & failed() const noexcept
    {
        auto p = getStorage<Failed *>();
        assert(p);
        return *p;
    }
};

extern ExprBlackHole eBlackHole;

static_assert(
    sizeof(Value) == 16,
    "Inc2a/R1: Value must stay 16 bytes so the two-level clock's >>4 granule and the striped-arena math are unchanged.");
static_assert(
    alignof(Value) == 16, "Inc2a/R1: Value must stay 16-byte aligned (atomic p0 publish + arena cell layout).");

bool Value::isBlackhole() const
{
    /* The cross-thread blackhole is now the pdPending tag, not an &eBlackHole
       sentinel thunk. Observers (printing, showType, tryFixupBlackHolePos)
       are unchanged because a pending cell still reports type()==nThunk. */
    return fpIsPendingTag();
}

void Value::mkBlackhole()
{
    /* Mark a cell as being forced (pdPending). Keep a thunk-shaped p1 pointing
       at eBlackHole so any stray thunk()/determinePos read matches the old
       sentinel. Only tests and the force-site recursion check build/observe one
       now; the force site itself marks pending via the CAS (fvMarkPending). */
    constexpr uint64_t subtag = uint64_t(tThunk - tFirstPairOfPointers);
    fpSetPending(subtag | std::bit_cast<uint64_t>((Expr *) &eBlackHole));
}

typedef std::vector<Value *, traceable_allocator<Value *>> ValueVector;
typedef boost::unordered_flat_map<
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
} // namespace nix
