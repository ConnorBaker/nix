#pragma once
///@file

#include "nix/util/sync.hh"
#include "nix/util/hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/attr-path.hh"

#include <functional>
#include <variant>

namespace nix::eval_cache {

struct AttrDb;
class AttrCursor;

/**
 * A lazily-materialised pointer to an evaluator value (a cursor's
 * `_value`, or the eval cache's root value), together with the unforce
 * generation it was materialised in (see `cacheGeneration`/`isStale` in
 * eval-cache.cc).
 *
 * Thread-safety (parallel-eval, eval-cores > 1): under worker-thread
 * fan-out several workers may request the same (shared parent / root)
 * cursor's value concurrently. Both the `RootValue` and the generation it
 * was cached in are therefore held behind a `Sync<>` so that every read and
 * publish is race-free. The field is lazily published and then only
 * re-published when stale; the lock is NOT held across the (possibly long)
 * evaluation that derives the value, only across the check and the publish
 * (see `AttrCursor::getValue`). `value` is falsy until first materialised.
 *
 * Under 2c-3 revert-off nothing in evaluator memory is freed or reverted, so
 * a once-published pointer stays valid; a per-worker generation only causes a
 * harmless re-derivation of the same pointer. At eval-cores == 1 the lock is
 * uncontended and the staleness/generation logic is byte-identical to the
 * pre-parallel cursor.
 */
struct CachedRootValue
{
    RootValue value;
    std::optional<size_t> generation;
};

struct CachedEvalError : CloneableError<CachedEvalError, EvalError>
{
private:
    void anchor() override;
public:
    const ref<AttrCursor> cursor;
    const Symbol attr;

    CachedEvalError(ref<AttrCursor> cursor, Symbol attr);

    /**
     * Evaluate this attribute, which should result in a regular
     * `EvalError` exception being thrown.
     */
    [[noreturn]]
    void force();
};

class EvalCache : public std::enable_shared_from_this<EvalCache>
{
    friend class AttrCursor;
    friend struct CachedEvalError;

    std::shared_ptr<AttrDb> db;
    EvalState & state;
    typedef fun<Value *()> RootLoader;
    RootLoader rootLoader;
    /**
     * The cache's root value and the generation it was loaded in. If it
     * was loaded inside an unforce scope, once a scope has ended since,
     * the value may have been freed (serial / revert-on only), so it is
     * loaded again. Behind a `Sync<>` so concurrent root cursors (e.g.
     * `packages` and `legacyPackages` fanned out as separate work items)
     * can materialise it race-free; the root loader runs under the lock so
     * concurrent requests do not both evaluate it.
     */
    Sync<CachedRootValue> rootValue;

    Value * getRootValue();

public:

    EvalCache(std::optional<std::reference_wrapper<const Hash>> useCache, EvalState & state, RootLoader rootLoader);

    ref<AttrCursor> getRoot();
};

enum AttrType {
    Placeholder = 0,
    FullAttrs = 1,
    String = 2,
    Missing = 3,
    Misc = 4,
    Failed = 5,
    Bool = 6,
    ListOfStrings = 7,
    Int = 8,
};

struct placeholder_t
{};

struct missing_t
{};

struct misc_t
{};

struct failed_t
{};

struct int_t
{
    NixInt x;
};

typedef uint64_t AttrId;
typedef std::pair<AttrId, Symbol> AttrKey;
typedef std::pair<std::string, NixStringContext> string_t;

typedef std::variant<
    std::vector<Symbol>,
    string_t,
    placeholder_t,
    missing_t,
    misc_t,
    failed_t,
    bool,
    int_t,
    std::vector<std::string>>
    AttrValue;

class AttrCursor : public std::enable_shared_from_this<AttrCursor>
{
    friend class EvalCache;
    friend struct CachedEvalError;

    ref<EvalCache> root;
    using Parent = std::optional<std::pair<ref<AttrCursor>, Symbol>>;
    Parent parent;
    /**
     * This cursor's value and the generation it was derived in, derived
     * again from the parent once stale. Behind a `Sync<>`: on a cache miss
     * several child workers may force a shared parent cursor's value
     * concurrently (`getValue` walks up to the parent), so both the pointer
     * and its generation must be published atomically. See `getValue`.
     */
    Sync<CachedRootValue> _value;
    /**
     * The database row id and decoded value for this cursor's attribute.
     *
     * NOT behind a lock, and NOT made race-free by the cursor itself: the
     * cursor relies on its CONSUMER to expand each cursor to a single writer
     * and to fully publish a cursor's children before handing that cursor (or
     * its children) to other threads. `nix search` honours this: a cursor is
     * "expanded" (its children enumerated via `getAttrs`/`maybeGetAttr`, which
     * is what writes `cachedValue`) by exactly one task, and that expansion
     * completes BEFORE any child work item is spawned. The spawn (Executor
     * queue enqueue, a mutex release) and the worker's dequeue (acquire) then
     * establish a happens-before edge, so a child reading its parent's
     * `cachedValue` via `getKey` sees the fully-written, thereafter-immutable
     * value. Under that discipline `cachedValue` is immutable-after-publish
     * across workers and concurrent reads are safe.
     *
     * A future parallel consumer that spawns work on a cursor's children
     * BEFORE that cursor's `cachedValue` is fully published (e.g. fanning out
     * while still enumerating, or re-expanding a cursor from two tasks) WOULD
     * race on this field and must guard it (e.g. `Sync<>` like `_value`) or
     * preserve the expand-before-spawn invariant. (See the field survey in
     * notes/reviews/unforce-parallel-evalcache-threadsafe.md.)
     */
    std::optional<std::pair<AttrId, AttrValue>> cachedValue;

    AttrKey getKey();

    Value & getValue();

    /**
     * If `cachedValue` is unset, try to initialize it from the
     * database. It is not an error if it does not exist. Throw a
     * `CachedEvalError` exception if it does exist but has type
     * `AttrType::Failed`.
     */
    void fetchCachedValue();

public:

    AttrCursor(
        ref<EvalCache> root,
        Parent parent,
        Value * value = nullptr,
        std::optional<std::pair<AttrId, AttrValue>> && cachedValue = {});

    AttrPath getAttrPath() const;

    AttrPath getAttrPath(Symbol name) const;

    std::string getAttrPathStr() const;

    std::string getAttrPathStr(Symbol name) const;

    Suggestions getSuggestionsForAttr(Symbol name);

    std::shared_ptr<AttrCursor> maybeGetAttr(Symbol name);

    std::shared_ptr<AttrCursor> maybeGetAttr(std::string_view name);

    ref<AttrCursor> getAttr(Symbol name);

    ref<AttrCursor> getAttr(std::string_view name);

    /**
     * Get an attribute along a chain of attrsets. Note that this does
     * not auto-call functors or functions.
     */
    OrSuggestions<ref<AttrCursor>> findAlongAttrPath(const AttrPath & attrPath);

    std::string getString();

    string_t getStringWithContext();

    bool getBool();

    NixInt getInt();

    std::vector<std::string> getListOfStrings();

    std::vector<Symbol> getAttrs();

    bool isDerivation();

    Value & forceValue();

    /**
     * Force creation of the .drv file in the Nix store.
     */
    StorePath forceDerivation();
};

} // namespace nix::eval_cache
