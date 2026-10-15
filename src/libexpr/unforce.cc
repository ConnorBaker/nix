/* PROTOTYPE: unforcing thunks between jobs.

   Thunk update in `forceValue` is the evaluator's only mutation of existing
   cells. While a job (an outermost unforce scope) runs, the pre-image of
   every *old* cell it forces is logged; at the end of the job each logged
   cell is either reverted to its pre-image, replaced by a derivation stub,
   or promoted (its forced value copied out of the job region). Afterwards no
   old cell points into the job region, which is then released wholesale. */

#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/value.hh"
#include "nix/util/environment-variables.hh"

#include <sys/mman.h>
#include <pthread.h>

#include <cstdlib>
#include <optional>
#include <set>
#include <unordered_map>

#include "expr-config-private.hh"

#if NIX_USE_BOEHMGC
#  include <gc/gc_mark.h>
#endif

#if NIX_UNFORCE_MIMALLOC
#  include <mimalloc.h>
#  include <mimalloc-stats.h>
#endif

namespace nix {

EvalRegion evalRegion;
BumpArena evalBaseArena;

/* The address range freed by the last region release. */
static std::pair<uintptr_t, uintptr_t> lastReleased{0, 0};
const bool evalUseBaseArena = [] {
    auto s = getenv("NIX_NOGC_BUMP");
    return s && std::string_view(s) != "0";
}();

static const bool useHugePages = [] {
    auto s = getenv("NIX_NOGC_THP");
    return s && std::string_view(s) != "0";
}();

static void * mapArena(size_t n)
{
    auto p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        throw std::bad_alloc();
    if (useHugePages)
        madvise(p, n, MADV_HUGEPAGE);
    return p;
}

void * BumpArena::refill(size_t n)
{
    static constexpr size_t chunk = size_t(64) << 20;
    /* A large object gets its own mapping, so the current chunk stays. */
    if (n > chunk / 4) {
        auto p = (char *) mapArena(n);
        chunks.emplace_back(p, p + n);
        return p;
    }
    auto p = mapArena(chunk);
    cur = (char *) p;
    chunks.emplace_back(cur, cur + chunk);
    end = cur + chunk;
    return alloc(n);
}

namespace {

/* Whether unforcing happens at all is the `eval-unforce` setting; these
   NIX_UNFORCE_* variables are for experiments and debugging, and default to
   the measured-best choices. */
struct Settings
{
    /* "": log every forced cell, revert all, let Boehm reclaim.
       "values": Values in a job region. "all": everything in it. */
    std::string region = getEnv("NIX_UNFORCE_REGION").value_or("all");
    bool tripwire = getEnv("NIX_UNFORCE_TRIPWIRE").value_or("0") != "0";
    bool stubs = getEnv("NIX_UNFORCE_STUB").value_or("0") != "0";
    /* Promote a cell forced in this many jobs (0: never). Needs "all". */
    uint32_t promoteAfter = std::stoul(getEnv("NIX_UNFORCE_PROMOTE").value_or("2"));
    bool stats = getEnv("NIX_UNFORCE_STATS").value_or("0") != "0";
    /* Keep one region mapped across jobs and zero on allocation, instead of
       mapping fresh (kernel-zeroed) pages for every job. */
    bool reuse = getEnv("NIX_UNFORCE_REUSE").value_or("0") != "0";
    /* "bump": one flat region per outermost scope. "mimalloc": a mimalloc
       heap per scope, in an exclusive arena; every scope is a job, and
       promotion copies into the enclosing scope's heap. */
    std::string backend = getEnv("NIX_UNFORCE_BACKEND").value_or("bump");
};

const Settings & unforceSettings()
{
    static const Settings s;
    return s;
}

/* The tripwire keeps every released region reserved, so it uses smaller
   ones: 64 GiB regions would exhaust a 48-bit address space after about
   4000 jobs. */
size_t regionSize()
{
    return unforceSettings().tripwire ? size_t(4) << 30 : size_t(64) << 30;
}

struct Stats
{
    size_t jobs = 0, logged = 0, reverted = 0, stubbed = 0, promoted = 0, copiedBytes = 0, regionPeak = 0, relogged = 0;
} stats;

bool nested()
{
    return unforceSettings().backend == "mimalloc" && !unforceSettings().region.empty();
}

/* With NIX_UNFORCE_STATS: print a summary of all scopes when the process
   exits. */
static void printStatsAtExit()
{
    [[maybe_unused]] static bool registered =
        (std::atexit([] {
             std::cerr << fmt(
                 "unforce: %s %d, logged %d, reverted %d, stubbed %d, promoted %d (relogged %d), copied %d MiB, "
                 "largest scope %d MiB\n",
                 nested() ? "scopes" : "jobs",
                 stats.jobs,
                 stats.logged,
                 stats.reverted,
                 stats.stubbed,
                 stats.promoted,
                 stats.relogged,
                 stats.copiedBytes >> 20,
                 stats.regionPeak >> 20);
         }),
         true);
}

/* One scope of the mimalloc backend. `counts` counts, per cell older than
   this scope, in how many of its child scopes the cell was forced. */
struct Frame
{
    size_t logMark;
    void * heap;
    std::unordered_map<const Value *, uint32_t> counts;
    size_t bytes = 0;
};

std::vector<Frame> frames;

#if NIX_UNFORCE_MIMALLOC
mi_arena_id_t arenaId = nullptr;
uintptr_t arenaLo = 0, arenaHi = 0;
mi_theap_t * currentTheap = nullptr;

bool inArena(const void * p)
{
    return (uintptr_t) p >= arenaLo && (uintptr_t) p < arenaHi;
}

bool inHeap(void * heap, const void * p)
{
    return inArena(p) && mi_heap_contains((mi_heap_t *) heap, p);
}

void setCurrentHeap(void * heap)
{
    evalRegion.heap = heap;
    evalRegion.allTypes = heap != nullptr;
    currentTheap = heap ? mi_heap_theap((mi_heap_t *) heap) : nullptr;
}
#endif

#if NIX_USE_BOEHMGC
GC_push_other_roots_proc prevPushOtherRoots = nullptr;

/* Objects outside the region that only the region refers to must survive
   collections during the job. */
void pushRegionRoots()
{
    if (prevPushOtherRoots)
        prevPushOtherRoots();
    if (evalRegion.cur)
        GC_push_all(evalRegion.base, evalRegion.cur);
#  if NIX_UNFORCE_MIMALLOC
    /* Push the committed part of each area of each scope heap. */
    for (auto & f : frames)
        mi_heap_visit_blocks(
            (mi_heap_t *) f.heap,
            false,
            [](const mi_heap_t *, const mi_heap_area_t * area, void * block, size_t, void *) -> bool {
                if (!block && area && area->committed)
                    GC_push_all(area->blocks, (char *) area->blocks + area->committed);
                return true;
            },
            nullptr);
#  endif
}
#endif

void hookCollector()
{
#if NIX_USE_BOEHMGC
    static bool hooked = [] {
        prevPushOtherRoots = GC_get_push_other_roots();
        GC_set_push_other_roots(pushRegionRoots);
        return true;
    }();
    (void) hooked;
#endif
}

void openRegion(bool allTypes)
{
    hookCollector();
    static char * spare = nullptr;
    bool dirty = false;
    if (unforceSettings().reuse && spare)
        dirty = true;
    else
        spare = (char *) mapArena(regionSize());
    evalRegion.base = evalRegion.cur = spare;
    evalRegion.end = evalRegion.base + regionSize();
    evalRegion.allTypes = allTypes;
    evalRegion.dirty = dirty;
    if (!unforceSettings().reuse)
        spare = nullptr;
}

/* With NIX_UNFORCE_VERIFY, check the invariant that makes releasing a
   region safe: no live object outside it still points into it. Every live
   mimalloc block is scanned word by word, so with Boehm disabled (and
   without NIX_NOGC_BUMP) this covers all long-lived evaluator objects; it is
   conservative, so a hit names a word that merely looks like a pointer. */
void verifyNoEscapes()
{
    /* Precise enough: the base arena holds only evaluator objects, so a hit
       is an evaluator object (or string bytes) pointing into the region. */
    if (evalUseBaseArena) {
        uintptr_t lo = (uintptr_t) evalRegion.base, hi = (uintptr_t) evalRegion.cur;
        size_t bytes = 0, hits = 0;
        for (auto [start, end] : evalBaseArena.chunks) {
            /* The current chunk is only filled up to `cur`. */
            if (evalBaseArena.cur >= start && evalBaseArena.cur <= end)
                end = evalBaseArena.cur;
            bytes += end - start;
            for (auto w = (const uintptr_t *) start; w < (const uintptr_t *) end; ++w)
                if (auto v = *w & ~uintptr_t(7); v >= lo && v < hi) {
                    if (hits++ < 5)
                        std::cerr << fmt(
                            "unforce verify: arena word at %x = %x points into the region\n", (uintptr_t) w, *w);
                }
        }
        std::cerr << fmt(
            "unforce verify: job %d scanned the base arena (%d MiB), %d pointers into the region\n",
            stats.jobs,
            bytes >> 20,
            hits);
        return;
    }
#if NIX_UNFORCE_MIMALLOC
    struct Scan
    {
        uintptr_t lo, hi;
        size_t blocks = 0, bytes = 0, hits = 0;
    } scan{(uintptr_t) evalRegion.base, (uintptr_t) evalRegion.cur};

    mi_heap_visit_blocks(
        mi_heap_main(),
        true,
        [](const mi_heap_t *, const mi_heap_area_t *, void * block, size_t size, void * arg) -> bool {
            auto & scan = *(Scan *) arg;
            if (!block)
                return true;
            scan.blocks++;
            scan.bytes += size;
            auto words = (const uintptr_t *) block;
            for (size_t i = 0; i < size / sizeof(uintptr_t); ++i)
                /* Strip Value's tag bits. */
                if (auto w = words[i] & ~uintptr_t(7); w >= scan.lo && w < scan.hi) {
                    if (scan.hits++ < 3) {
                        std::cerr << fmt(
                            "unforce verify: block %x (%d bytes) word %d points into the region\n",
                            (uintptr_t) block,
                            size,
                            i);
                        if (getenv("NIX_UNFORCE_VERIFY_DUMP")) {
                            std::string line;
                            for (size_t j = 0; j < std::min<size_t>(size / 8, 40); ++j)
                                line += fmt(" %016x", words[j]);
                            std::cerr << "  words:" << line << "\n";
                            std::string text;
                            for (size_t j = 0; j < std::min<size_t>(size, 320); ++j) {
                                char c = ((const char *) block)[j];
                                text += c >= 32 && c < 127 ? c : '.';
                            }
                            std::cerr << "  text: " << text << "\n";
                        }
                    }
                }
            return true;
        },
        &scan);
    std::cerr << fmt(
        "unforce verify: job %d scanned %d blocks (%d MiB), %d pointers into the region\n",
        stats.jobs,
        scan.blocks,
        scan.bytes >> 20,
        scan.hits);
#endif
}

/* With the tripwire, the pages stay reserved but inaccessible, so a missed
   escape faults instead of reading reused memory. */
void releaseRegion()
{
    if (getenv("NIX_UNFORCE_VERIFY"))
        verifyNoEscapes();
    auto base = evalRegion.base;
    stats.regionPeak = std::max(stats.regionPeak, size_t(evalRegion.cur - base));
    lastReleased = {(uintptr_t) base, (uintptr_t) evalRegion.cur};
    evalRegion = EvalRegion{};
    if (unforceSettings().reuse)
        return;
    if (unforceSettings().tripwire) {
        mprotect(base, regionSize(), PROT_NONE);
        madvise(base, regionSize(), MADV_DONTNEED);
    } else
        munmap(base, regionSize());
}

bool isScalar(const Value & v)
{
    switch (v.internalType()) {
    case tInt:
    case tBool:
    case tNull:
    case tFloat:
    case tString:
    case tPath:
        return true;
    case tUninitialized:
    case tFailed:
    case tExternal:
    case tPrimOp:
    case tAttrs:
    case tListSmall:
    case tPrimOpApp:
    case tApp:
    case tThunk:
    case tLambda:
    case tListN:
    case tNumberOfInternalTypes:
        return false;
    }
    return false;
}

/* Recognise a derivation without forcing anything, so that no new log
   entries appear while a job is being closed. */
bool isDerivationNoForce(EvalState & state, const Value & v)
{
    if (v.internalType() != tAttrs)
        return false;
    auto type = v.attrs()->get(state.s.type);
    return type && type->value->internalType() == tString && type->value->string_view() == "derivation";
}

/* Copies a graph out of the job region, precisely: objects outside the
   region are shared, objects inside are copied once (sharing and cycles
   are preserved through the forwarding table). Callers pause the region. */
struct Evacuator
{
    EvalState & state;
    std::unordered_map<const void *, void *> forward;
    std::vector<Value *> values;
    std::vector<std::pair<Env *, size_t>> envs;

    /* The scope being closed, for the mimalloc backend; its copies go to
       wherever allocation currently goes (the enclosing scope's heap). */
    void * youngHeap = nullptr;

    bool young(const void * p) const
    {
#if NIX_UNFORCE_MIMALLOC
        if (youngHeap)
            return inHeap(youngHeap, p);
#endif
        return evalRegion.contains(p);
    }

    template<typename T>
    T * forwarded(const T * p)
    {
        auto i = forward.find(p);
        return i == forward.end() ? nullptr : (T *) i->second;
    }

    void * copyBytes(const void * p, size_t n)
    {
        stats.copiedBytes += n;
        auto q = state.mem.allocBytes(n);
        memcpy(q, p, n);
        return q;
    }

    Value * value(Value * v)
    {
        if (!v || !young(v))
            return v;
        if (auto f = forwarded(v))
            return f;
        auto n = state.allocValue();
        stats.copiedBytes += sizeof(Value);
        *n = *v;
        forward.emplace(v, n);
        values.push_back(n);
        return n;
    }

    Env * env(Env * e)
    {
        if (!e || !young(e))
            return e;
        if (auto f = forwarded(e))
            return f;
        auto size = *(const size_t *) ((const char *) e - 16);
        auto & n = state.mem.allocEnv(size);
        stats.copiedBytes += sizeof(Env) + size * sizeof(Value *);
        n.up = e->up;
        std::copy_n(e->values, size, n.values);
        forward.emplace(e, &n);
        envs.emplace_back(&n, size);
        return &n;
    }

    const StringData * string(const StringData * s)
    {
        if (!s || !young(s))
            return s;
        if (auto f = forwarded(s))
            return f;
        auto n = (const StringData *) copyBytes(s, sizeof(StringData) + s->size() + 1);
        forward.emplace(s, (void *) n);
        return n;
    }

    using Context = Value::StringWithContext::Context;

    const Context * context(const Context * c)
    {
        if (!c || !young(c))
            return c;
        if (auto f = forwarded(c))
            return f;
        auto n =
            new (state.mem.allocBytes(sizeof(Context) + c->size() * sizeof(Context::value_type))) Context(c->size());
        auto elems = const_cast<Context::value_type *>(n->begin());
        for (size_t i = 0; i < c->size(); ++i)
            elems[i] = string(c->begin()[i]);
        forward.emplace(c, n);
        return n;
    }

    const Bindings * bindings(const Bindings * b)
    {
        if (!young(b))
            return b;
        if (auto f = forwarded(b))
            return f;
        auto bytes = sizeof(Bindings) + b->layerSize() * sizeof(Attr);
        stats.copiedBytes += bytes;
        auto n = b->copyLayer(
            state.mem.allocBytes(bytes),
            [&](Value * v) { return value(v); },
            [&](const Bindings * base) { return bindings(base); });
        forward.emplace(b, (void *) n);
        return n;
    }

    /* Rewrite the pointers of a copied value. */
    void scan(Value & v)
    {
        switch (v.internalType()) {
        case tString:
            v.mkStringNoCopy(*string(v.stringData()), context(v.context()));
            break;
        case tPath:
            v.mkPath(v.pathAccessor(), *string(v.pathData()));
            break;
        case tAttrs:
            v.mkAttrs(bindings(v.attrs()));
            break;
        case tListSmall: {
            auto l = v.rawSmallList();
            v.setRawSmallList({value(l[0]), value(l[1])});
            break;
        }
        case tListN: {
            auto l = v.rawList();
            auto elems = l.elems;
            if (young(elems)) {
                auto n = (Value **) state.mem.allocBytes(l.size * sizeof(Value *));
                stats.copiedBytes += l.size * sizeof(Value *);
                for (size_t i = 0; i < l.size; ++i)
                    n[i] = value(elems[i]);
                elems = n;
            }
            v.setRawList({.size = l.size, .elems = elems});
            break;
        }
        case tThunk:
            if (auto t = v.thunk(); t.env)
                v.mkThunk(env(t.env), t.expr);
            break;
        case tApp:
            v.mkApp(value(v.app().left), value(v.app().right));
            break;
        case tPrimOpApp:
            v.mkPrimOpApp(value(v.primOpApp().left), value(v.primOpApp().right));
            break;
        case tLambda:
            v.mkLambda(env(v.lambda().env), v.lambda().fun);
            break;
        case tFailed:
            if (young(v.failed().recoveryValue))
                v.setRawFailed(new Value::Failed(v.failed().exRef->ex, value(v.failed().recoveryValue)));
            break;
        case tUninitialized:
        case tInt:
        case tBool:
        case tNull:
        case tFloat:
        case tExternal:
        case tPrimOp:
        case tNumberOfInternalTypes:
            break;
        }
    }

    /* Copy everything still pending. */
    void drain()
    {
        while (!values.empty() || !envs.empty()) {
            if (!values.empty()) {
                auto v = values.back();
                values.pop_back();
                scan(*v);
            } else {
                auto [e, size] = envs.back();
                envs.pop_back();
                e->up = env(e->up);
                for (size_t i = 0; i < size; ++i)
                    e->values[i] = value(e->values[i]);
            }
        }
    }

    Value copy(const Value & root)
    {
        Value res = root;
        scan(res);
        drain();
        return res;
    }
};

std::unordered_map<const Value *, std::pair<std::string, const Value *>> selectRegistry;
std::unordered_map<const Value *, std::string> sourceRegistry;

/* Builds derivation stubs: forced scalars are kept (copied out of the
   region), nested derivations (outputs) become stubs too, and every other
   attribute becomes `getAttr name source`, where `source` recomputes the
   whole derivation from a recipe outside the region. Callers pause the
   region. */
struct Stubber
{
    EvalState & state;
    Evacuator & evacuator;
    std::unordered_map<const Bindings *, Value *> memo;
    std::unordered_map<uint32_t, Value *> selectors;
    Value * getAttr = nullptr;

    Value * selector(Symbol name)
    {
        auto & sel = selectors[name.getId()];
        if (!sel) {
            if (!getAttr)
                getAttr = &state.getBuiltin("getAttr");
            auto nameV = state.allocValue();
            nameV->mkString(state.symbols[name], state.mem);
            sel = state.allocValue();
            sel->mkPrimOpApp(getAttr, nameV);
        }
        return sel;
    }

    Value * select(Value * source, Symbol name)
    {
        auto res = state.allocValue();
        res->mkApp(selector(name), source);
        if (getenv("NIX_UNFORCE_DEBUG"))
            selectRegistry[res] = {std::string(state.symbols[name]), source};
        if (getenv("NIX_UNFORCE_DEBUG")) {
            std::set<const Value *> seen{res};
            size_t depth = 0;
            for (auto v = source; v; ++depth) {
                if (!seen.insert(v).second) {
                    std::cerr << fmt(
                        "unforce debug: cycle selecting '%s' after %d steps\n", state.symbols[name], depth);
                    break;
                }
                if (v->internalType() == tApp)
                    v = v->app().right;
                else
                    break;
            }
            if (depth > 8)
                std::cerr << fmt("unforce debug: selector chain depth %d for '%s'\n", depth, state.symbols[name]);
        }
        return res;
    }

    Value * stub(const Value & drv, Value * source, unsigned depth)
    {
        if (auto i = memo.find(drv.attrs()); i != memo.end())
            return i->second;
        auto res = state.allocValue();
        memo.emplace(drv.attrs(), res);
        auto attrs = state.buildBindings(drv.attrs()->size());
        for (auto & a : *drv.attrs()) {
            auto & v = *a.value;
            Value * n;
            if (isScalar(v))
                n = evacuator.value(a.value);
            else if (depth < 2 && isDerivationNoForce(state, v))
                n = stub(v, select(source, a.name), depth + 1);
            else
                n = select(source, a.name);
            attrs.insert(a.name, n, a.pos);
        }
        res->mkAttrs(attrs.alreadySorted());
        return res;
    }
};

} // namespace

void unforceDebugChain(EvalState & state, const Value * v)
{
    std::set<const Value *> seen;
    for (int k = 0; k < 40 && v; ++k) {
        bool again = !seen.insert(v).second;
        auto t = v->internalType();
        std::string what = t == tApp ? "app" : t == tThunk ? "thunk" : t == tAttrs ? "attrs" : fmt("type %d", (int) t);
        if (auto i = sourceRegistry.find(v); i != sourceRegistry.end())
            what += " [" + i->second + "]";
        if (auto i = selectRegistry.find(v); i != selectRegistry.end()) {
            std::cerr << fmt(
                "  %d: %x select '%s' %s%s\n", k, (uintptr_t) v, i->second.first, what, again ? " (REPEAT)" : "");
            if (again)
                return;
            v = i->second.second;
            continue;
        }
        std::cerr << fmt("  %d: %x %s%s\n", k, (uintptr_t) v, what, again ? " (REPEAT)" : "");
        if (again || t != tApp)
            return;
        v = v->app().right;
    }
}

void * evalRegionHeapAlloc(size_t n)
{
#if NIX_UNFORCE_MIMALLOC
    /* Most evaluator objects are small (a Value is 16 bytes); mimalloc's
       small path skips its size-class dispatch. */
    auto p = n <= MI_SMALL_SIZE_MAX ? mi_theap_zalloc_small(currentTheap, n) : mi_theap_zalloc(currentTheap, n);
    if (!p)
        throw std::bad_alloc();
    frames.back().bytes += n;
    return p;
#else
    unreachable();
#endif
}

#if !NIX_USE_BOEHMGC
void * evalBaseAlloc(size_t n)
{
#  if NIX_UNFORCE_MIMALLOC
    return n <= MI_SMALL_SIZE_MAX ? mi_zalloc_small(n) : mi_zalloc(n);
#  else
    return calloc(n, 1);
#  endif
}
#endif

bool evalRegionHeapContains(const void * p)
{
#if NIX_UNFORCE_MIMALLOC
    return inHeap(evalRegion.heap, p);
#else
    unreachable();
#endif
}

bool unforceMayHaveFreed(const void * p)
{
    if (unforceSettings().region.empty())
        return true;
#if NIX_UNFORCE_MIMALLOC
    /* Destroyed heaps' pages go back to the arena for any later heap. */
    if (nested())
        return inArena(p);
#endif
    return (uintptr_t) p >= lastReleased.first && (uintptr_t) p < lastReleased.second;
}

std::string unforceStatsJSON()
{
    if (!stats.jobs)
        return "";
    return fmt(
        R"({"scopes":%d,"logged":%d,"reverted":%d,"stubbed":%d,"promoted":%d,"relogged":%d,"copiedBytes":%d,"largestScopeBytes":%d})",
        stats.jobs,
        stats.logged,
        stats.reverted,
        stats.stubbed,
        stats.promoted,
        stats.relogged,
        stats.copiedBytes,
        stats.regionPeak);
}

static size_t allocatorErrors = 0;

std::string allocatorStatsJSON()
{
#if NIX_UNFORCE_MIMALLOC
    /* mi_process_info covers the whole process, whichever collector owns
       the evaluator's heap; mi_stats_get_json adds mimalloc's own counters
       (arena reservation and commit, mmap and purge calls). */
    size_t elapsed, user, sys, rss, peakRss, commit, peakCommit, faults;
    mi_process_info(&elapsed, &user, &sys, &rss, &peakRss, &commit, &peakCommit, &faults);
    auto mi = mi_stats_get_json(0, nullptr);
    auto res = fmt(
        R"({"mimallocVersion":%d,"currentRss":%d,"peakRss":%d,"currentCommit":%d,"peakCommit":%d,"pageFaults":%d,"errors":%d,"mimalloc":%s})",
        mi_version(),
        rss,
        peakRss,
        commit,
        peakCommit,
        faults,
        allocatorErrors,
        mi ? mi : "null");
    mi_free(mi);
    return res;
#else
    return "";
#endif
}

void initAllocator()
{
#if NIX_UNFORCE_MIMALLOC
    /* Count mimalloc's errors (it reports them itself as well); throwing
       from here would unwind through C. */
    mi_register_error([](int, void *) { allocatorErrors++; }, nullptr);
#endif
}

static size_t generation = 0;

size_t unforceGeneration()
{
    return generation;
}

size_t unforceCurrentScopeBytes()
{
    if (!frames.empty())
        return frames.back().bytes;
    return evalRegion.cur ? size_t(evalRegion.cur - evalRegion.base) : 0;
}

bool unforceEnabled(const EvalState & state)
{
    return state.settings.unforce && !state.debugRepl;
}

UnforceBatch::UnforceBatch(EvalState & state, bool enabled)
    : state(enabled && unforceEnabled(state) ? &state : nullptr)
{
}

UnforceBatch::~UnforceBatch()
{
    end();
}

void UnforceBatch::openAt(const void * stackMark)
{
    if (state && !mark) {
        /* Only a scope that is not nested in another one is a job, except
           with the mimalloc backend, where every scope is. */
        job = nested() || state->isolationDepth == 0;
        mark = state->beginUnforceScope(stackMark);
    }
}

void UnforceBatch::open()
{
    /* The visiting runs in our caller's callees. */
    openAt(__builtin_dwarf_cfa());
}

bool UnforceBatch::full() const
{
    return mark && unforceCurrentScopeBytes() >= state->settings.unforceBatchSize;
}

UnforceBatch::Element::Element(UnforceBatch & batch)
    : batch(batch)
{
    /* The visiting runs in our caller's callees. */
    batch.openAt(__builtin_dwarf_cfa());
}

UnforceBatch::Element::~Element()
{
    if (batch.full())
        batch.end();
}

void UnforceBatch::end()
{
    if (!mark)
        return;
    state->endUnforceScope(*std::exchange(mark, std::nullopt));
    if (onEnd)
        onEnd(job);
}

#if NIX_UNFORCE_MIMALLOC
/* With NIX_UNFORCE_VERIFY: before a scope heap is destroyed, check that no
   evaluator object outside it points into it. The enclosing scopes' heaps
   and the base arena hold evaluator objects only, so every word of them is
   checked against the dying heap. */
static void verifyHeapScope(void * dying)
{
    struct Scan
    {
        void * dying;
        size_t blocks = 0, bytes = 0, hits = 0;

        void words(const void * start, size_t size)
        {
            auto w = (const uintptr_t *) start;
            for (size_t i = 0; i < size / sizeof(uintptr_t); ++i)
                if (auto p = (const void *) (w[i] & ~uintptr_t(7)); inHeap(dying, p))
                    if (hits++ < 5)
                        std::cerr << fmt("unforce verify: word at %x points into the dying scope\n", (uintptr_t) &w[i]);
        }
    } scan{dying};

    for (auto & f : frames)
        mi_heap_visit_blocks(
            (mi_heap_t *) f.heap,
            true,
            [](const mi_heap_t *, const mi_heap_area_t *, void * block, size_t size, void * arg) -> bool {
                auto & scan = *(Scan *) arg;
                if (block) {
                    scan.blocks++;
                    scan.bytes += size;
                    scan.words(block, size);
                }
                return true;
            },
            &scan);
    if (evalUseBaseArena)
        for (auto [start, end] : evalBaseArena.chunks) {
            if (evalBaseArena.cur >= start && evalBaseArena.cur <= end)
                end = evalBaseArena.cur;
            scan.bytes += end - start;
            scan.words(start, end - start);
        }
    std::cerr << fmt(
        "unforce verify: scope %d: scanned %d enclosing-scope blocks and the base arena (%d MiB), %d pointers into it\n",
        stats.jobs,
        scan.blocks,
        scan.bytes >> 20,
        scan.hits);
}

static void beginHeapScope(size_t logMark)
{
    hookCollector();
    if (!arenaId) {
        /* The arena bounds what all open scopes hold at once. It costs every
           scope too: each heap other than the main one allocates and zeroes
           page bitmaps for every arena it uses, 62 of them with a bit per
           64 KiB slice (0.5 MiB for 4 GiB; mimalloc src/arena.c,
           mi_arena_pages_alloc). Arenas above 16 GiB are split into children
           (mimalloc d4881d33). mimalloc 3.5.1 also leaked those bitmaps once
           they exceeded 512 KiB, i.e. for arenas above 4 GiB (fixed in
           3.5.2, 6c45ba09). */
        static const size_t arenaBytes = std::stoul(getEnv("NIX_UNFORCE_ARENA_GB").value_or("4")) << 30;
        if (mi_reserve_os_memory_ex(arenaBytes, false, false, true, &arenaId) != 0)
            throw Error("could not reserve the mimalloc arena for unforce scopes");
        size_t size = 0;
        arenaLo = (uintptr_t) mi_arena_area(arenaId, &size);
        arenaHi = arenaLo + size;
    }
    auto heap = mi_heap_new_in_arena(arenaId);
    if (!heap)
        throw std::bad_alloc();
    frames.push_back(Frame{.logMark = logMark, .heap = heap});
    setCurrentHeap(heap);
}

/* Close the innermost heap scope: every cell older than it that it forced
   is reverted, or promoted, i.e. copied into the enclosing scope's heap (or,
   for the outermost scope, out of all scopes). A promoted cell older than
   the enclosing scope too now points into that scope, so it is logged there
   again. Then the scope's heap is destroyed wholesale. */
static void endHeapScope(EvalState & state, size_t mark)
{
    auto & cfg = unforceSettings();
    auto & log = state.undoLog;
    Frame f = std::move(frames.back());
    frames.pop_back();
    Frame * parent = frames.empty() ? nullptr : &frames.back();
    stats.jobs++;
    generation++;
    stats.logged += log.size() - mark;

    std::unordered_map<Value *, Value> preImages;
    std::vector<Value *> targets;
    for (auto i = mark; i < log.size(); ++i)
        if (preImages.try_emplace(log[i].target, log[i].preImage).second)
            targets.push_back(log[i].target);

    setCurrentHeap(parent ? parent->heap : nullptr);
    std::vector<EvalState::UndoEntry> relog;
    {
        std::optional<PauseRegion> pause;
        if (!parent)
            pause.emplace();
        Evacuator evacuator{state};
        evacuator.youngHeap = f.heap;
        auto & counts = parent ? parent->counts : state.forceCounts;
        for (auto target : targets) {
            auto & pre = preImages.at(target);
            if (cfg.promoteAfter && ++counts[target] >= cfg.promoteAfter) {
                *target = evacuator.copy(*target);
                stats.promoted++;
                /* The parent restores it, unless it is a slot of a frame
                   that will have returned by then. */
                if (parent && !inHeap(parent->heap, target) && !unforceIsScopeStackSlot(target))
                    relog.push_back({target, pre});
                continue;
            }
            *target = pre;
            stats.reverted++;
        }
    }

    log.resize(mark);
    for (auto & e : relog)
        log.push_back(e);
    stats.relogged += relog.size();
    if (!parent)
        log.shrink_to_fit();
    stats.regionPeak = std::max(stats.regionPeak, f.bytes);
    if (getenv("NIX_UNFORCE_VERIFY"))
        verifyHeapScope(f.heap);
    mi_heap_destroy((mi_heap_t *) f.heap);

    if (cfg.stats)
        printStatsAtExit();
}
#endif

/* Per open scope, the stack pointer of its opener (see
   `unforceIsScopeStackSlot`), and this thread's lowest stack address. */
static thread_local std::vector<const char *> scopeStackMarks;
static thread_local const char * stackLow = [] {
    pthread_attr_t attr;
    void * addr = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
    }
    return (const char *) addr;
}();

bool unforceIsScopeStackSlot(const void * p)
{
    if (scopeStackMarks.empty() || !stackLow)
        return false;
    /* The scope that will restore the slot: every scope with the mimalloc
       backend, only the job (the outermost scope) with the bump one. */
    auto mark = nested() ? scopeStackMarks.back() : scopeStackMarks.front();
    return (const char *) p >= stackLow && (const char *) p < mark;
}

size_t EvalState::beginUnforceScope(const void * stackMark)
{
    /* The canonical frame address is the caller's stack pointer at the call:
       frames the caller enters from now on lie below it, its own locals at
       or above. */
    scopeStackMarks.push_back((const char *) (stackMark ? stackMark : __builtin_dwarf_cfa()));
#if NIX_UNFORCE_MIMALLOC
    if (nested()) {
        isolationDepth++;
        beginHeapScope(undoLog.size());
        return undoLog.size();
    }
#endif
    if (!isolationDepth++ && !unforceSettings().region.empty())
        openRegion(unforceSettings().region == "all");
    return undoLog.size();
}

void EvalState::endUnforceScope(size_t mark)
{
    scopeStackMarks.pop_back();
#if NIX_UNFORCE_MIMALLOC
    if (nested()) {
        isolationDepth--;
        endHeapScope(*this, mark);
        return;
    }
#endif
    /* Only the outermost scope is a job. */
    if (--isolationDepth)
        return;

    auto & cfg = unforceSettings();
    stats.jobs++;
    generation++;
    stats.logged += undoLog.size() - mark;

    /* A cell may have been forced more than once (after a recoverable
       error); its oldest pre-image is the one from before the job. */
    std::unordered_map<Value *, Value> preImages;
    std::vector<Value *> targets;
    for (auto i = mark; i < undoLog.size(); ++i)
        if (preImages.try_emplace(undoLog[i].target, undoLog[i].preImage).second)
            targets.push_back(undoLog[i].target);

    {
        PauseRegion pause;
        Evacuator evacuator{*this};
        Stubber stubber{*this, evacuator};
        bool canCopy = evalRegion.allTypes;

        for (auto target : targets) {
            auto & pre = preImages.at(target);
            if (canCopy && cfg.promoteAfter && ++forceCounts[target] >= cfg.promoteAfter) {
                *target = evacuator.copy(*target);
                stats.promoted++;
                continue;
            }
            if (cfg.stubs && evalRegion.cur && isDerivationNoForce(*this, *target)) {
                auto source = allocValue();
                *source = pre;
                if (getenv("NIX_UNFORCE_DEBUG")) {
                    auto n = target->attrs()->get(s.name);
                    sourceRegistry[source] =
                        fmt("source for %s (job %d, pre-image %s)",
                            n && n->value->internalType() == tString ? std::string(n->value->string_view()) : "?",
                            stats.jobs,
                            pre.internalType() == tApp     ? "app"
                            : pre.internalType() == tThunk ? "thunk"
                                                           : "other");
                }
                /* Memoise within one stub only (outputs refer to each other).
                   Another cell holding the same derivation, such as an alias
                   whose recipe is `self.foo`, must not share this stub: its
                   fallbacks would select from a recipe that evaluates to the
                   stub itself. */
                stubber.memo.clear();
                *target = *stubber.stub(*target, source, 0);
                evacuator.drain();
                stats.stubbed++;
                continue;
            }
            *target = pre;
            stats.reverted++;
        }
    }

    undoLog.resize(mark);
    undoLog.shrink_to_fit();
    if (evalRegion.base)
        releaseRegion();

    if (cfg.stats)
        printStatsAtExit();
}

} // namespace nix
