#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"

#include <boost/container/small_vector.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <ranges>
#include <optional>

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
        , value(value)
    {
    }

    constexpr Attr() {}

    auto operator<=>(const Attr & a) const
    {
        return name <=> a.name;
    }
};

static_assert(
    sizeof(Attr) == 2 * sizeof(uint32_t) + sizeof(Value *),
    "performance of the evaluator is highly sensitive to the size of Attr. "
    "avoid introducing any padding into Attr if at all possible, and do not "
    "introduce new fields that need not be present for almost every instance.");

/**
 * Bindings contains all the attributes of an attribute set. It is defined
 * by its size and its capacity, the capacity being the number of Attr
 * elements allocated after this structure, while the size corresponds to
 * the number of elements already inserted in this structure.
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

    PosIdx pos;

    /**
     * An instance of bindings objects with 0 attributes.
     * This object must never be modified.
     */
    static const constinit Bindings emptyBindings;

private:
    /**
     * Number of attributes in the attrs FAM (Flexible Array Member).
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
     * Length of the layers list.
     */
    uint32_t numLayers = 1;

    /**
     * Flexible array member of attributes. A layered Bindings starts it with
     * one slot, its `Layering`; an unlayered one, most of them, has none, and
     * a 16-byte header.
     */
    Attr attrs[0];

    /**
     * Of a layered Bindings: the Bindings it is layered on top of, and the
     * Bindings whose array holds its attributes when it shares the array of
     * (a layer of) an update's (`//`) right operand instead of copying it
     * (else null: they follow the slot; a layered `arrayOf`'s follow its
     * slot). Pointers to the objects, not into them: the garbage
     * collector ignores interior pointers, and these keep the objects alive.
     */
    struct Layering
    {
        const Bindings * base;
        const Bindings * arrayOf;
    };

    static_assert(sizeof(Layering) <= sizeof(Attr));

    Layering layering() const noexcept
    {
        Layering l;
        std::memcpy(&l, attrs, sizeof l);
        return l;
    }

    /** Where this Bindings' own attributes go. */
    Attr * own() noexcept
    {
        return attrs + (numLayers > 1);
    }

    const Attr * data() const noexcept
    {
        if (numLayers <= 1)
            return attrs;
        auto l = layering();
        return l.arrayOf ? l.arrayOf->attrs + (l.arrayOf->numLayers > 1) : attrs + 1;
    }

    /**
     * Count the attributes of the chain, this layer's minus those that
     * also are in the layers below.
     */
    void countChain();

    constexpr Bindings() = default;
    Bindings(const Bindings &) = delete;
    Bindings(Bindings &&) = delete;
    Bindings & operator=(const Bindings &) = delete;
    Bindings & operator=(Bindings &&) = delete;

    ~Bindings() = default;

    friend class BindingsBuilder;

    /**
     * The layers a Bindings iterator holds without allocating. Longer chains
     * exist (see ExprOpUpdate::eval); their iterators allocate.
     */
    static constexpr unsigned inlineLayers = 8;

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
        struct BindingsCursor
        {
            /**
             * Attr that the cursor currently points to.
             */
            pointer current;

            /**
             * How many Attrs of the contiguous buffer are left, from `current` on (a
             * count, not an end pointer: a cursor is 16 bytes, not 24).
             */
            size_type remaining;

            /**
             * Priority of the value. Lesser values have more priority (i.e. they override
             * attributes that appear later in the linked list of Bindings).
             */
            uint32_t priority;

            pointer operator->() const noexcept
            {
                return current;
            }

            reference get() const noexcept
            {
                return *current;
            }

            bool empty() const noexcept
            {
                return remaining == 0;
            }

            void increment() noexcept
            {
                ++current;
                --remaining;
            }

            GENERATE_CMP(BindingsCursor, me->current->name, me->priority)
        };

        using QueueStorageType = boost::container::small_vector<BindingsCursor, inlineLayers>;

        /**
         * One cursor per non-empty layer. Merging, each step emits the least
         * name, from the cursor of highest priority that has it, and moves
         * every cursor at that name past it (the names it shadows). A chain
         * has few layers (at most log2 of the sizes an update sums plus one,
         * see ExprOpUpdate::eval), so a linear scan finds the least cursor
         * with fewer instructions than a heap's sift.
         */
        QueueStorageType cursorHeap;

        /**
         * The attribute the iterator currently points to.
         */
        pointer current = nullptr;

        /**
         * Whether iterating over a single attribute and not a merge chain.
         */
        bool doMerge = true;

        iterator & finished() noexcept
        {
            current = nullptr;
            return *this;
        }

        /* Point at the least name of the cursors, the highest priority's. */
        void settle() noexcept
        {
            if (cursorHeap.empty()) {
                current = nullptr;
                return;
            }
            auto best = cursorHeap.begin();
            for (auto i = best + 1; i != cursorHeap.end(); ++i)
                if (*i < *best)
                    best = i;
            current = best->current;
        }

        /* Not noexcept: a chain of more than `inlineLayers` layers allocates
           its cursors, and failing to is `std::bad_alloc`. */
        explicit iterator(const Bindings & attrs)
            : doMerge(attrs.baseLayer())
        {
            auto pushBindings = [this, priority = unsigned{0}](const Bindings & layer) mutable {
                cursorHeap.push_back(
                    BindingsCursor{
                        .current = layer.data(),
                        .remaining = layer.numAttrs,
                        .priority = priority++,
                    });
            };

            if (!doMerge) {
                if (attrs.empty())
                    return;

                current = attrs.data();
                pushBindings(attrs);

                return;
            }

            for (auto layer = &attrs; layer; layer = layer->baseLayer())
                if (layer->numAttrs != 0)
                    pushBindings(*layer);

            settle();
        }

    public:
        iterator() = default;

        reference operator*() const noexcept
        {
            return *current;
        }

        pointer operator->() const noexcept
        {
            return current;
        }

        iterator & operator++() noexcept
        {
            if (!doMerge) {
                ++current;
                if (current == cursorHeap.front().current + cursorHeap.front().remaining)
                    return finished();
                return *this;
            }

            auto name = current->name;
            for (size_t k = 0; k < cursorHeap.size();) {
                auto & c = cursorHeap[k];
                if (c->name == name) {
                    c.increment();
                    if (c.empty()) {
                        c = cursorHeap.back();
                        cursorHeap.pop_back();
                        continue;
                    }
                }
                ++k;
            }
            settle();
            return *this;
        }

        iterator operator++(int)
        {
            iterator tmp = *this;
            ++*this;
            return tmp;
        }

        bool operator==(const iterator & rhs) const noexcept
        {
            return current == rhs.current;
        }
    };

    using const_iterator = iterator;

    void push_back(const Attr & attr)
    {
        own()[numAttrs++] = attr;
        numAttrsInChain = numAttrs;
    }

    /**
     * Get attribute by name or nullptr if no such attribute exists.
     */
    const Attr * get(Symbol name) const noexcept
    {
        auto getInChunk = [key = Attr{name, nullptr}](const Bindings & chunk) -> const Attr * {
            auto first = chunk.data();
            auto last = first + chunk.numAttrs;
            /* Most layers that updates put over a set are small: a name
               outside a layer's range skips its search. */
            if (first == last || key < *first || last[-1] < key)
                return nullptr;
            const Attr * i = std::lower_bound(first, last, key);
            if (i != last && i->name == key.name)
                return i;
            return nullptr;
        };

        const Bindings * currentChunk = this;
        while (currentChunk) {
            const Attr * maybeAttr = getInChunk(*currentChunk);
            if (maybeAttr)
                return maybeAttr;
            currentChunk = currentChunk->baseLayer();
        }

        return nullptr;
    }

    /**
     * The attributes of a Bindings that is not layered, without an iterator.
     */
    std::span<const Attr> unlayered() const
    {
        if (isLayered()) [[unlikely]]
            unreachable();
        return {data(), numAttrs};
    }

    /** The Bindings whose own array holds this layer's attributes. */
    const Bindings * owner() const noexcept
    {
        return isLayered() && layering().arrayOf ? layering().arrayOf : this;
    }

    /** The attributes of this layer alone, without those of the layers below. */
    std::span<const Attr> layer() const noexcept
    {
        return {data(), numAttrs};
    }

    /**
     * Bindings that this attrset is "layered" on top of.
     */
    const Bindings * baseLayer() const noexcept
    {
        return numLayers > 1 ? layering().base : nullptr;
    }

    /**
     * Test if the length of the linked list of layers is greater than 1.
     */
    bool isLayered() const noexcept
    {
        return numLayers > 1;
    }

    const_iterator begin() const
    {
        return const_iterator(*this);
    }

    const_iterator end() const
    {
        return const_iterator();
    }

    Attr & operator[](size_type pos)
    {
        if (isLayered()) [[unlikely]]
            unreachable();
        return attrs[pos];
    }

    const Attr & operator[](size_type pos) const
    {
        if (isLayered()) [[unlikely]]
            unreachable();
        return data()[pos];
    }

    void sort();

    /**
     * Returns the attributes in lexicographically sorted order.
     */
    std::vector<const Attr *> lexicographicOrder(const SymbolTable & symbols) const
    {
        std::vector<const Attr *> res;
        res.reserve(size());
        std::ranges::transform(*this, std::back_inserter(res), [](const Attr & a) { return &a; });
        std::ranges::sort(res, [&](const Attr * a, const Attr * b) {
            std::string_view sa = symbols[a->name], sb = symbols[b->name];
            return sa < sb;
        });
        return res;
    }

    friend class EvalMemory;
};

static_assert(std::forward_iterator<Bindings::iterator>);
static_assert(std::ranges::forward_range<Bindings>);

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
        return bindings->isLayered();
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
        if (hasBaseLayer())
            bindings->countChain();
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
     *
     * Before any attribute is added; the slot this takes (`Bindings::Layering`)
     * counts in the capacity.
     */
    void layerOnTopOf(const Bindings & base) noexcept
    {
        assert(bindings->numAttrs == 0 && capacity_ > 0);
        bindings->numLayers = base.numLayers + 1;
        Bindings::Layering l{&base, nullptr};
        std::memcpy(static_cast<void *>(bindings->attrs), &l, sizeof l);
        --capacity_;
    }

    Value & alloc(Symbol name, PosIdx pos = noPos);

    Value & alloc(std::string_view name, PosIdx pos = noPos);

    const Bindings * finish()
    {
        bindings->sort();
        finishSizeIfNecessary();
        return bindings;
    }

    const Bindings * alreadySorted()
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
