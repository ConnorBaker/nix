#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"

#include <boost/container/static_vector.hpp>
#include <boost/iterator/function_output_iterator.hpp>

#include <algorithm>
#include <functional>
#include <ranges>
#include <optional>
#include <span>
#include <vector>

namespace nix {

class EvalMemory;
struct Value;

/**
 * Map one attribute name to its value.
 */
struct Attr
{
    /* the placement of `name` and `pos` in this struct is important.
       both of them are uint32 wrappers, they are next to each other
       to make sure that Attr has no padding on 64 bit machines. that
       way we keep Attr size at two words with no wasted space. */
    Symbol name;
    PosIdx pos;
    Value * value = nullptr;
    Attr(Symbol name, Value * value, PosIdx pos = noPos)
        : name(name)
        , pos(pos)
        , value(value) {};
    Attr() {};

    auto operator<=>(const Attr & a) const
    {
        return name <=> a.name;
    }

    /**
     * Arrow operator for pointer-like access syntax compatibility.
     * Allows `attr->name` as an alias for `attr.name` when attr is a value, not a pointer.
     * This is useful for code that iterates over Attr values but was written for Attr pointers.
     */
    const Attr * operator->() const noexcept { return this; }
    Attr * operator->() noexcept { return this; }
};

static_assert(
    sizeof(Attr) == 2 * sizeof(uint32_t) + sizeof(Value *),
    "performance of the evaluator is highly sensitive to the size of Attr. "
    "avoid introducing any padding into Attr if at all possible, and do not "
    "introduce new fields that need not be present for almost every instance.");

/**
 * Bindings contains all the attributes of an attribute set. It uses a
 * Structure-of-Arrays (SoA) layout for better cache efficiency during
 * iteration and lookup operations.
 *
 * Memory layout after header:
 *   [names: Symbol × capacity] [padding to 8-byte align] [values: Value* × capacity] [positions: PosIdx × capacity]
 *
 * Bindings can be efficiently `//`-composed into an intrusive linked list of "layers"
 * that saves on copies and allocations. Each lookup (@see Bindings::get) traverses
 * this linked list until a matching attribute is found (thus overlays earlier in
 * the list take precedence). For iteration over the whole Bindings, an on-the-fly
 * k-way merge is performed by Bindings::iterator class.
 */
class Bindings
{
public:
    using size_type = uint32_t;

    /**
     * An instance of bindings objects with 0 attributes.
     * This object must never be modified.
     */
    static Bindings emptyBindings;

private:
    // SoA layout with 24-byte header (same as original AoS).
    // Fields: baseLayer (8) + pos (4) + numAttrs (4) + numAttrsInChain (4) + capacity_ (4) = 24 bytes.
    // numLayers was removed; it's computed on-the-fly by walking the baseLayer chain.

    /**
     * Bindings that this attrset is "layered" on top of.
     */
    const Bindings * baseLayer = nullptr;

public:
    PosIdx pos;

private:
    /**
     * Number of attributes in the SoA arrays.
     */
    size_type numAttrs = 0;

    /**
     * Number of attributes with unique names in the layer chain.
     *
     * This is the *real* user-facing size of bindings, whereas @ref numAttrs is
     * an implementation detail of the data structure.
     */
    size_type numAttrsInChain = 0;

    /**
     * Capacity of the SoA arrays. Needed to compute offsets to names and values arrays.
     */
    size_type capacity_ = 0;

    /**
     * Structure-of-Arrays layout optimized to avoid padding:
     *   [positions: PosIdx × cap] [names: Symbol × cap] [values: Value* × cap]
     * Since PosIdx and Symbol are both 4 bytes, and header is 24 bytes (8-byte aligned),
     * the values array (8-byte aligned) starts at offset 24 + 8*cap, which is always 8-byte aligned.
     */
    PosIdx positions_[0];

    Bindings() = default;
    Bindings(const Bindings &) = delete;
    Bindings(Bindings &&) = delete;
    Bindings & operator=(const Bindings &) = delete;
    Bindings & operator=(Bindings &&) = delete;

    ~Bindings() = default;

    friend class BindingsBuilder;
    friend class EvalMemory;

    /**
     * Maximum length of the Bindings layer chains.
     */
    static constexpr unsigned maxLayers = 8;

    /**
     * SoA array accessors. Memory layout (no padding needed):
     *   [Header 24B] [positions: PosIdx × cap] [names: Symbol × cap] [values: Value* × cap]
     */
    PosIdx * positionsArray() noexcept { return positions_; }
    const PosIdx * positionsArray() const noexcept { return positions_; }

    Symbol * namesArray() noexcept
    {
        return reinterpret_cast<Symbol *>(positions_ + capacity_);
    }

    const Symbol * namesArray() const noexcept
    {
        return reinterpret_cast<const Symbol *>(positions_ + capacity_);
    }

    Value ** valuesArray() noexcept
    {
        // Values array starts after names array; always 8-byte aligned since
        // header (24B) + positions (4B × cap) + names (4B × cap) = 24 + 8*cap
        return reinterpret_cast<Value **>(namesArray() + capacity_);
    }

    Value * const * valuesArray() const noexcept
    {
        return reinterpret_cast<Value * const *>(namesArray() + capacity_);
    }

    /**
     * Get attribute at index as an Attr (for compatibility).
     */
    Attr attrAt(size_type idx) const noexcept
    {
        return Attr(namesArray()[idx], const_cast<Value *>(valuesArray()[idx]), positionsArray()[idx]);
    }

public:
    size_type size() const
    {
        return numAttrsInChain;
    }

    bool empty() const
    {
        return size() == 0;
    }

    class iterator
    {
    public:
        using value_type = Attr;
        using pointer = const value_type *;
        using reference = const value_type &;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;

        friend class Bindings;

    private:
        /**
         * Cursor into a Bindings layer, using index-based access for SoA layout.
         */
        struct BindingsCursor
        {
            /**
             * Pointer to the bindings layer this cursor iterates over.
             */
            const Bindings * bindings;

            /**
             * Current index into the SoA arrays.
             */
            size_type current;

            /**
             * One past the end index.
             */
            size_type end;

            /**
             * Priority of the value. Lesser values have more priority (i.e. they override
             * attributes that appear later in the linked list of Bindings).
             */
            uint32_t priority;

            Symbol currentName() const noexcept
            {
                return bindings->namesArray()[current];
            }

            Attr get() const noexcept
            {
                return bindings->attrAt(current);
            }

            bool empty() const noexcept
            {
                return current == end;
            }

            void increment() noexcept
            {
                ++current;
            }

            void consume(Symbol name) noexcept
            {
                while (!empty() && currentName() <= name)
                    ++current;
            }

            // Comparison by (name, priority) - lower priority wins for equal names
            bool operator==(const BindingsCursor & o) const noexcept
            {
                return currentName() == o.currentName() && priority == o.priority;
            }

            auto operator<=>(const BindingsCursor & o) const noexcept
            {
                if (auto cmp = currentName() <=> o.currentName(); cmp != 0)
                    return cmp;
                return priority <=> o.priority;
            }
        };

        using QueueStorageType = boost::container::static_vector<BindingsCursor, maxLayers>;

        /**
         * Comparator implementing the override priority / name ordering
         * for BindingsCursor.
         */
        static constexpr auto comp = std::greater<BindingsCursor>();

        /**
         * A priority queue used to implement an on-the-fly k-way merge.
         */
        QueueStorageType cursorHeap;

        /**
         * Cached Attr for the current position (reconstructed from SoA on demand).
         */
        mutable Attr currentAttr_;

        /**
         * Whether the iterator is valid (not at end).
         */
        bool valid_ = false;

        /**
         * Whether iterating over a single layer (no merge needed).
         */
        bool doMerge = true;

        /**
         * For non-merge iteration: current index and the bindings pointer.
         */
        size_type simpleIndex_ = 0;
        const Bindings * simpleBindings_ = nullptr;

        void push(BindingsCursor cursor) noexcept
        {
            cursorHeap.push_back(cursor);
            std::ranges::make_heap(cursorHeap, comp);
        }

        [[nodiscard]] BindingsCursor pop() noexcept
        {
            std::ranges::pop_heap(cursorHeap, comp);
            auto cursor = cursorHeap.back();
            cursorHeap.pop_back();
            return cursor;
        }

        iterator & finished() noexcept
        {
            valid_ = false;
            return *this;
        }

        void next(BindingsCursor cursor) noexcept
        {
            currentAttr_ = cursor.get();
            valid_ = true;
            cursor.increment();

            if (!cursor.empty())
                push(cursor);
        }

        void updateCurrentFromSimple() noexcept
        {
            currentAttr_ = simpleBindings_->attrAt(simpleIndex_);
        }

        std::optional<BindingsCursor> consumeAllUntilCurrentName() noexcept
        {
            auto cursor = pop();
            Symbol lastHandledName = currentAttr_.name;

            while (cursor.currentName() <= lastHandledName) {
                cursor.consume(lastHandledName);
                if (!cursor.empty())
                    push(cursor);

                if (cursorHeap.empty())
                    return std::nullopt;

                cursor = pop();
            }

            return cursor;
        }

        explicit iterator(const Bindings & attrs) noexcept
            : doMerge(attrs.baseLayer != nullptr)
        {
            auto pushBindings = [this, priority = unsigned{0}](const Bindings & layer) mutable {
                push(
                    BindingsCursor{
                        .bindings = &layer,
                        .current = 0,
                        .end = layer.numAttrs,
                        .priority = priority++,
                    });
            };

            if (!doMerge) {
                if (attrs.empty())
                    return;

                simpleBindings_ = &attrs;
                simpleIndex_ = 0;
                updateCurrentFromSimple();
                valid_ = true;
                pushBindings(attrs);  // Still need for end detection

                return;
            }

            const Bindings * layer = &attrs;
            while (layer) {
                if (layer->numAttrs != 0)
                    pushBindings(*layer);
                layer = layer->baseLayer;
            }

            if (cursorHeap.empty())
                return;

            next(pop());
        }

    public:
        iterator() = default;

        reference operator*() const noexcept
        {
            return currentAttr_;
        }

        pointer operator->() const noexcept
        {
            return &currentAttr_;
        }

        iterator & operator++() noexcept
        {
            if (!doMerge) {
                ++simpleIndex_;
                if (simpleIndex_ == cursorHeap.front().end)
                    return finished();
                updateCurrentFromSimple();
                return *this;
            }

            if (cursorHeap.empty())
                return finished();

            auto cursor = consumeAllUntilCurrentName();
            if (!cursor)
                return finished();

            next(*cursor);
            return *this;
        }

        iterator operator++(int) noexcept
        {
            iterator tmp = *this;
            ++*this;
            return tmp;
        }

        bool operator==(const iterator & rhs) const noexcept
        {
            return valid_ == rhs.valid_ && (!valid_ || currentAttr_.name == rhs.currentAttr_.name);
        }
    };

    using const_iterator = iterator;

    void push_back(const Attr & attr)
    {
        namesArray()[numAttrs] = attr.name;
        valuesArray()[numAttrs] = attr.value;
        positionsArray()[numAttrs] = attr.pos;
        ++numAttrs;
        numAttrsInChain = numAttrs;
    }

    /**
     * Result of a lookup operation. Contains index and bindings pointer
     * to avoid reconstructing an Attr unless needed.
     */
    struct LookupResult
    {
        const Bindings * bindings;
        size_type index;

        Symbol name() const noexcept { return bindings->namesArray()[index]; }
        Value * value() const noexcept { return const_cast<Value *>(bindings->valuesArray()[index]); }
        PosIdx pos() const noexcept { return bindings->positionsArray()[index]; }
        Attr attr() const noexcept { return bindings->attrAt(index); }

        // Compatibility: allow using as Attr*-like pointer
        Value * operator->() const noexcept { return value(); }
    };

    /**
     * Get attribute by name. Returns optional LookupResult for efficient access.
     */
    std::optional<LookupResult> find(Symbol name) const noexcept
    {
        auto searchLayer = [](const Bindings & chunk, Symbol key) -> std::optional<size_type> {
            auto first = chunk.namesArray();
            auto last = first + chunk.numAttrs;
            auto i = std::lower_bound(first, last, key);
            if (i != last && *i == key)
                return static_cast<size_type>(i - first);
            return std::nullopt;
        };

        const Bindings * currentChunk = this;
        while (currentChunk) {
            if (auto idx = searchLayer(*currentChunk, name))
                return LookupResult{currentChunk, *idx};
            currentChunk = currentChunk->baseLayer;
        }

        return std::nullopt;
    }

    /**
     * Get attribute by name or nullopt if no such attribute exists.
     * Returns Attr by value since SoA doesn't store Attr objects.
     */
    std::optional<Attr> get(Symbol name) const noexcept
    {
        if (auto result = find(name))
            return result->attr();
        return std::nullopt;
    }

    /**
     * Check if the layer chain is full (has maxLayers layers).
     * Computed by walking the baseLayer chain.
     */
    bool isLayerListFull() const noexcept
    {
        unsigned count = 1;
        for (auto * p = baseLayer; p; p = p->baseLayer)
            if (++count >= maxLayers)
                return true;
        return false;
    }

    /**
     * Test if this bindings has a base layer (i.e., is the result of `//`).
     */
    bool isLayered() const noexcept
    {
        return baseLayer != nullptr;
    }

    const_iterator begin() const
    {
        return const_iterator(*this);
    }

    const_iterator end() const
    {
        return const_iterator();
    }

    /**
     * Get attribute at position by value (SoA doesn't store Attr objects).
     * Only valid for non-layered bindings.
     */
    Attr operator[](size_type idx) const
    {
        if (isLayered()) [[unlikely]]
            unreachable();
        return attrAt(idx);
    }

    /**
     * Direct access to name at index (for sorting and other internal operations).
     */
    Symbol & nameAt(size_type idx) { return namesArray()[idx]; }
    Value *& valueAt(size_type idx) { return valuesArray()[idx]; }
    PosIdx & posAt(size_type idx) { return positionsArray()[idx]; }

    /**
     * Set attribute at index (for in-place construction).
     */
    void setAt(size_type idx, Symbol name, Value * value, PosIdx pos = noPos)
    {
        namesArray()[idx] = name;
        valuesArray()[idx] = value;
        positionsArray()[idx] = pos;
    }

    void setAt(size_type idx, const Attr & attr)
    {
        setAt(idx, attr.name, attr.value, attr.pos);
    }

    /**
     * Set the number of attributes (for algorithms that populate the array directly).
     */
    void setSize(size_type n)
    {
        numAttrs = n;
        numAttrsInChain = n;
    }

    void sort();

    /**
     * Returns the attributes in lexicographically sorted order.
     * Returns Attr by value since SoA doesn't store Attr objects.
     */
    std::vector<Attr> lexicographicOrder(const SymbolTable & symbols) const
    {
        std::vector<Attr> res;
        res.reserve(size());
        for (const auto & a : *this)
            res.push_back(a);
        std::ranges::sort(res, [&](const Attr & a, const Attr & b) {
            std::string_view sa = symbols[a.name], sb = symbols[b.name];
            return sa < sb;
        });
        return res;
    }

    /**
     * Get the number of attributes in this layer (not including base layers).
     */
    size_type localSize() const noexcept { return numAttrs; }

    friend class EvalMemory;
};

static_assert(std::forward_iterator<Bindings::iterator>);
static_assert(std::ranges::forward_range<Bindings>);
static_assert(
    sizeof(Bindings) == 24,
    "Bindings header size changed. If intentional, update this assert and the comment in the class definition.");

/**
 * A wrapper around Bindings that ensures that its always in sorted
 * order at the end. The only way to consume a BindingsBuilder is to
 * call finish(), which sorts the bindings.
 */
class BindingsBuilder final
{
public:
    // needed by std::back_inserter
    using value_type = Attr;
    using size_type = Bindings::size_type;

private:
    Bindings * bindings;
    Bindings::size_type capacity_;

    friend class EvalMemory;

    BindingsBuilder(EvalMemory & mem, SymbolTable & symbols, Bindings * bindings, size_type capacity)
        : bindings(bindings)
        , capacity_(capacity)
        , mem(mem)
        , symbols(symbols)
    {
    }

    bool hasBaseLayer() const noexcept
    {
        return bindings->baseLayer;
    }

    /**
     * If the bindings gets "layered" on top of another we need to recalculate
     * the number of unique attributes in the chain.
     *
     * This is done by either iterating over the base "layer" and the newly added
     * attributes and counting duplicates. If the base "layer" is big this approach
     * is inefficient and we fall back to doing per-element binary search in the base
     * "layer".
     */
    void finishSizeIfNecessary()
    {
        if (!hasBaseLayer())
            return;

        auto & base = *bindings->baseLayer;
        auto localNames = std::span(bindings->namesArray(), bindings->localSize());

        Bindings::size_type duplicates = 0;

        /* If the base bindings is smaller than the newly added attributes
           iterate using std::set_intersection to run in O(|base| + |attrs|) =
           O(|attrs|). Otherwise use an O(|attrs| * log(|base|)) per-attr binary
           search to check for duplicates. Note that if we are in this code path then
           |attrs| <= bindingsUpdateLayerRhsSizeThreshold, which 16 by default. We are
           optimizing for the case when a small attribute set gets "layered" on top of
           a much larger one. When attrsets are already small it's fine to do a linear
           scan, but we should avoid expensive iterations over large "base" attrsets. */
        if (localNames.size() > base.size()) {
            // Create a names-only view of base for intersection
            // The base iterator yields Attrs, so we extract names
            std::vector<Symbol> baseNames;
            baseNames.reserve(base.size());
            for (const auto & attr : base)
                baseNames.push_back(attr.name);

            std::set_intersection(
                baseNames.begin(),
                baseNames.end(),
                localNames.begin(),
                localNames.end(),
                boost::make_function_output_iterator([&]([[maybe_unused]] auto && _) { ++duplicates; }));
        } else {
            for (Symbol name : localNames) {
                if (base.find(name))
                    ++duplicates;
            }
        }

        bindings->numAttrsInChain = base.numAttrsInChain + localNames.size() - duplicates;
    }

public:
    std::reference_wrapper<EvalMemory> mem;
    std::reference_wrapper<SymbolTable> symbols;

    void insert(Symbol name, Value * value, PosIdx pos = noPos)
    {
        insert(Attr(name, value, pos));
    }

    void insert(const Attr & attr)
    {
        push_back(attr);
    }

    void push_back(const Attr & attr)
    {
        assert(bindings->numAttrs < capacity_);
        bindings->push_back(attr);
    }

    /**
     * "Layer" the newly constructured Bindings on top of another attribute set.
     *
     * This effectively performs an attribute set merge, while giving preference
     * to attributes from the newly constructed Bindings in case of duplicate attribute
     * names.
     *
     * This operation amortizes the need to copy over all attributes and allows
     * for efficient implementation of attribute set merges (ExprOpUpdate::eval).
     */
    void layerOnTopOf(const Bindings & base) noexcept
    {
        bindings->baseLayer = &base;
    }

    Value & alloc(Symbol name, PosIdx pos = noPos);

    Value & alloc(std::string_view name, PosIdx pos = noPos);

    Bindings * finish()
    {
        bindings->sort();
        finishSizeIfNecessary();
        return bindings;
    }

    Bindings * alreadySorted()
    {
        finishSizeIfNecessary();
        return bindings;
    }

    size_t capacity() const noexcept
    {
        return capacity_;
    }

    void grow(BindingsBuilder newBindings)
    {
        for (auto & i : *bindings)
            newBindings.push_back(i);
        std::swap(*this, newBindings);
    }

    friend struct ExprAttrs;
};

} // namespace nix
