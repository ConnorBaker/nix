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
#include <sys/resource.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

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
       heap per scope, in an exclusive arena; every scope frees its memory
       when it ends, and only the outermost one promotes. */
    std::string backend = getEnv("NIX_UNFORCE_BACKEND").value_or("bump");
    /* How a job's promotion set is closed under "producer-of" (see
       `decideJob`): "on" (promote the producers), "off" (counts only, the
       old behaviour; for negative controls). */
    std::string markers = getEnv("NIX_UNFORCE_MARKERS").value_or("on");
    /* The closure's step budget per job (`decideJob`): a fixed allowance
       plus a number of steps per KiB the job allocated. Dev overrides. */
    uint64_t walkBudgetBase = std::stoull(getEnv("NIX_UNFORCE_WALK_BUDGET_BASE").value_or("1000000"));
    uint64_t walkBudgetPerKiB = std::stoull(getEnv("NIX_UNFORCE_WALK_BUDGET_PER_KIB").value_or("64"));
    /* What a job whose closure runs out of budget does: "promote" every
       target, or "revert" every target. Both keep identity. */
    std::string walkFallback = getEnv("NIX_UNFORCE_WALK_FALLBACK").value_or("promote");
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
    /* Targets logged in a job that an earlier job had forced and reverted. */
    size_t reforced = 0;
    /* Job memory released, summed over jobs. */
    size_t freedBytes = 0;
    /* Force markers: promotions added by the producer closure, closure
       rounds, young-graph nodes visited by the closure, jobs whose closure
       changed anything, and the bookkeeping: bytes the markers add to log
       entries, and the largest transient bookkeeping of one job close
       (estimated). */
    size_t markerPromoted = 0, markerRounds = 0, markerSteps = 0, markerJobs = 0;
    /* The closure's budget: jobs that ran out of it (and so promoted, or
       reverted, every target), the most steps one job took, and the most
       steps per KiB of job memory one job took. */
    size_t budgetJobs = 0, maxJobSteps = 0, maxStepsPerKiB = 0;
    size_t markerLogBytes = 0, markerPeakBytes = 0;
    /* Jobs that promoted nothing because client code ran in them, and the
       cells reverted at their ends. */
    size_t clientJobs = 0, revertedInClientJobs = 0;
    /* Cells reverted by nested scopes of the mimalloc backend, which never
       promote (so these are reverted whatever the promotion threshold). */
    size_t innerScopeReverts = 0;
} stats;

/* Whether client code ran in the open job (`unforceNoteClientCode`). */
bool clientCodeInJob = false;

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
                 "largest scope %d MiB; reforced %d, freed %d MiB; markers %s: promoted %d, "
                 "rounds %d, steps %d, jobs %d, log %d KiB, peak %d KiB, over budget %d (%s), max job steps %d, "
                 "max steps per KiB %d; client-code jobs %d, reverted in client-code jobs %d, "
                 "inner-scope reverts %d\n",
                 nested() ? "scopes" : "jobs",
                 stats.jobs,
                 stats.logged,
                 stats.reverted,
                 stats.stubbed,
                 stats.promoted,
                 stats.relogged,
                 stats.copiedBytes >> 20,
                 stats.regionPeak >> 20,
                 stats.reforced,
                 stats.freedBytes >> 20,
                 unforceSettings().markers,
                 stats.markerPromoted,
                 stats.markerRounds,
                 stats.markerSteps,
                 stats.markerJobs,
                 stats.markerLogBytes >> 10,
                 stats.markerPeakBytes >> 10,
                 stats.budgetJobs,
                 unforceSettings().walkFallback,
                 stats.maxJobSteps,
                 stats.maxStepsPerKiB,
                 stats.clientJobs,
                 stats.revertedInClientJobs,
                 stats.innerScopeReverts);
         }),
         true);
}

/* One scope of the mimalloc backend. */
struct Frame
{
    size_t logMark;
    void * heap;
    size_t bytes = 0;
    /* Value cells are carved from chunks of the heap in allocation order,
       so that a cell's address gives its allocation time (`CellClock`). */
    std::vector<char *> chunks;
    char * cellCur = nullptr;
    char * cellEnd = nullptr;
};

/* Bytes per cell chunk of the mimalloc backend, and cells per chunk. */
constexpr size_t cellChunkBytes = 32 << 10;
constexpr size_t cellsPerChunk = cellChunkBytes / 16;

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

/* The allocation clock of the innermost scope: the bump cursor, or with
   the mimalloc backend the time of the next cell (see `CellClock`). It only
   grows while a scope is open. */
static uint64_t unforceClock()
{
#if NIX_UNFORCE_MIMALLOC
    if (nested()) {
        if (frames.empty() || frames.back().chunks.empty())
            return 0;
        auto & f = frames.back();
        return (f.chunks.size() - 1) * cellsPerChunk + (f.cellCur - f.chunks.back()) / 16;
    }
#endif
    return (uintptr_t) evalRegion.cur;
}

/* Log indices of the logged forces that are running, innermost last. */
static std::vector<size_t> activeForces;

/* Forget running forces logged at or after `mark` (none should be). */
static void dropActiveForces(size_t mark)
{
    while (!activeForces.empty() && activeForces.back() >= mark)
        activeForces.pop_back();
}

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

    /* A list's element array, copied once however many list values share
       it (lists are immutable, and every array has the size it was built
       with, so the values sharing one also share its size). */
    Value * const * elements(Value * const * elems, size_t size)
    {
        if (!young(elems))
            return elems;
        if (auto f = forwarded(elems))
            return f;
        auto n = (Value **) state.mem.allocBytes(size * sizeof(Value *));
        stats.copiedBytes += size * sizeof(Value *);
        forward.emplace(elems, n);
        for (size_t i = 0; i < size; ++i)
            n[i] = value(elems[i]);
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
            v.setRawList({.size = l.size, .elems = elements(l.elems, l.size)});
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

/* ---------------------------------------------------------------------------
   Force markers: closing a job's promotion set under "producer-of".

   Promotion breaks the evaluator's pointer identity (`eqValues` short-cuts on
   `&v1 == &v2`; Nixpkgs relies on it for `hostPlatform == buildPlatform`)
   when a promoted cell keeps a copy of a cell that a reverted target rebuilds
   when it is re-forced: two cells then stand for one cell of an evaluation
   without unforcing. Force counts cannot rule this out (an earlier promotion
   creates new old thunks with count 0, so a consumer can reach the threshold
   before its producer), so the counts only nominate promotions; this closure
   makes them safe.

   Definitions, for one job (outermost scope). Time is the job's allocation
   clock (`unforceClock`): the bump cursor, or the cell clock of the mimalloc
   backend's chunks; a young object's time a(x) is its address (bump), or its
   chunk position (mimalloc cells; other mimalloc objects have no time).
   - Targets T: old cells forced in the job (the undo log). Every logged force
     of t has an interval [s, e) of times: s when it was logged, e when it
     returned or threw (`endLoggedForce`). Forcing is LIFO, so the intervals
     form a forest (`parent`), and innermost(a) is the deepest interval
     containing time a.
   - The creator of a young object x is the innermost cell (young or old)
     being forced when x was allocated; owner(x) is the first old cell on the
     creator chain x, creator(x), creator(creator(x)), ... (a target), or
     none (allocated by the driver outside any force). Re-forcing owner(x) in
     a later job is the only thing that can build x again.
   - c(t) is t's content at job end; R(c) the young objects reachable from
     it (through cells, Bindings layers, lists, envs, thunk and app
     operands, lambdas, failure recovery values), as the Evacuator copies.
   - S ⊆ T is the promoted set and Evac(S) = ∪_{r∈S} R(c(r)), copied with one
     forwarding table. Identity is only observable on Value cells (no
     Bindings or list shortcut in `eqValues`, `CompareValues` or `elem`).

   Invariant (OC, ownership closure): every young cell X ∈ Evac(S) has
   owner(X) ∈ S or no owner.

   OC is sufficient: a reverted target t re-forced later builds only cells
   that t owns, which are fresh: none of them survives, because a survivor is
   in Evac(S) and is owned by a member of S, and t ∉ S. A surviving copy is
   never rebuilt, because its owner is promoted (it is a value and is not
   re-forced), and every cell on a creator chain below an owner is built only
   by re-running the owner. So each cell of the evaluation without unforcing
   has at most one counterpart alive at any time, and the history-independence
   argument for pure revert (notes/reviews/identity-reliance-review.md §3.2)
   applies to the heap at the next job's start. OC is also necessary in
   general: a cell X ∈ Evac(S) owned by a reverted t is rebuilt as X' when t
   is re-forced, and `X == X'` is observable whenever X holds a function.

   Lemma V (no upward visibility). While t is being forced, cells owned by t
   are accessible only to t's own evaluation and to forces of young cells
   owned by t. Reason: a target u forced inside t starts from its pre-image,
   which refers only to old memory (old cells are written only by their own
   logged forces, A1/A3 of the review); u gains access to young objects only
   by allocating them (owned by u or below) or by reading the contents of
   completed forces, and t's content does not exist until t ends. A young
   cell forced inside t but created before t started (not owned by t)
   likewise runs on its own closure. Hence no cell owned by t is allocated
   while a target nested in t is running.

   Lemma C (closed region). If t owns X and X is reachable at job end from a
   promoted r ≠ t, then X ∈ R(c(t)), along a path whose interior consists of
   objects owned by t. Reason: before t ends, no object not owned by t points
   at an object owned by t (Lemma V; containers are immutable, a cell is
   written only by its own force, whose evaluation is closed over its creator's
   data). After t ends, t's evaluation has no frame left, and objects owned by
   t are found only through c(t): forcing a young thunk owned by t (found in
   c(t), or through the environment of a lambda in c(t)) puts what it builds
   into that thunk's own content, which is owned by t and stays reachable
   from c(t) through the thunk's cell; another evaluation that reads c(t)
   copies pointers that are reachable from c(t) already.

   Candidates. Let X ∈ Evac(S) be owned by t ∉ S. If t was running when X was
   allocated, then innermost(a(X)) is t's interval (Lemma V: no nested target
   was running); otherwise t had ended, a(X) ≥ e. Either way a(X) ≥ s, and
   by Lemma C every object on the path from c(t) satisfies the same, so
   allowed(t, a) := a ≥ s ∧ (a ≥ e ∨ innermost(a) is t's interval) holds
   along it. So t is a candidate producer iff a walk of R(c(t)) that never
   enters an object with ¬allowed(t, a(x)) reaches a cell of Evac(S). For a
   target with several intervals (forced again after an error) only a ≥ s
   of the first interval is used. Objects without a time are never pruned:
   that only adds candidates (with no times at all the walk is plain
   reachability, option C's candidate set, which Lemma C also covers).

   This is where the markers differ from allocation-interval attribution
   (option E of notes/research/identity-reliance.md, rejected as unsound):
   E names innermost(a(X)) as X's producer. That misses producers whose young
   thunk is forced inside a later target's interval (the "precedes" and
   "follows" demos in tests/functional/eval-unforce-identity/: the producer
   ended before X was allocated). Here the interval only *rules out* targets
   (those that started after X, or that were inside a nested target when X was
   allocated), and reachability from the target's own content, which the
   heap does preserve (Lemma C), supplies the producers that ended earlier.
   Reachability alone (option C) over-promotes every consumer; the interval
   test removes consumers that only read data allocated before they started.

   Rule. Promote the candidates (NIX_UNFORCE_MARKERS=on): S is the least
   superset of the count-nominated set S0 that satisfies OC under the
   candidate test, computed by rounds until no reverted target is a
   candidate; the promoted set is then copied with one Evacuator, so the
   shared objects keep one copy. The decision copies nothing: Evac(S) is
   marked by walking, so it runs before the job's memory is released, and
   only the copies themselves allocate during the release. The alternative,
   refusing every member of S0 that reaches a candidate's cell (the
   greatest subset of S0 satisfying OC), retains less per job but can starve:
   a consumer that forces a producer's young thunks in the job where the
   producer is nominated is a candidate (the walk cannot tell whether that
   young thunk was created before the consumer started), and in a Nixpkgs
   traversal that recurs in every job, so the fixpoint would never be
   promoted. The counts (K) stay a heuristic for which cells to consider;
   correctness depends only on the closure.

   Premises: A1–A4 of the review (every write to old memory is logged;
   old memory allocated during a job, i.e. fileEvalCache cells and C-API
   objects, is inert; no escapes; deterministic evaluation), and exceptions
   carry no evaluator values. Derivation stubs (NIX_UNFORCE_STUB) change
   identity by construction and are not covered. With the mimalloc backend
   only the outermost scope promotes (nested scopes revert everything), so
   the relogging of promoted cells into an enclosing scope cannot break OC.
   --------------------------------------------------------------------------- */

/* Times of young objects: the address in the bump region; for the mimalloc
   backend, a cell's position in its scope's chunks (no time otherwise). */
struct CellClock
{
    bool bump = true;
    /* (chunk start, time of its first cell), sorted by address. */
    std::vector<std::pair<uintptr_t, uint64_t>> chunks;

    std::optional<uint64_t> operator()(const void * p) const
    {
        auto a = (uintptr_t) p;
        if (bump)
            return a;
        auto i =
            std::upper_bound(chunks.begin(), chunks.end(), a, [](uintptr_t a, const auto & c) { return a < c.first; });
        if (i == chunks.begin())
            return std::nullopt;
        --i;
        if (a >= i->first + cellChunkBytes)
            return std::nullopt;
        return i->second + (a - i->first) / 16;
    }
};

/* A job's targets, in the order of their first log entries (= the order
   their first forces started). A cell may have been forced more than once
   (after a recoverable error); its oldest pre-image is the one from before
   the job. */
struct JobTargets
{
    std::vector<Value *> cells;
    std::vector<Value> preImages;
    /* The log index of each target's first entry, and whether it has more. */
    std::vector<size_t> first;
    std::vector<bool> repeated;

    JobTargets(const decltype(EvalState::undoLog) & log, size_t mark)
    {
        std::unordered_map<const Value *, size_t> index;
        for (auto i = mark; i < log.size(); ++i) {
            auto [it, fresh] = index.try_emplace(log[i].target, cells.size());
            if (fresh) {
                cells.push_back(log[i].target);
                preImages.push_back(log[i].preImage);
                first.push_back(i);
                repeated.push_back(false);
            } else
                repeated[it->second] = true;
        }
    }

    size_t size() const
    {
        return cells.size();
    }
};

/* The force intervals of one job (log entries [mark, size)), compiled for
   the candidate test: per target, its first interval [s, e), whether it has
   more, and the intervals of the first one's direct children, which are
   disjoint and sorted. */
struct ForceMarkers
{
    struct Span
    {
        uint64_t s, e;
        bool single;
        uint32_t c0, c1;
    };

    std::vector<Span> spans;
    std::vector<std::pair<uint64_t, uint64_t>> childIntervals;

    ForceMarkers(const decltype(EvalState::undoLog) & log, size_t mark, uint64_t jobEnd, const JobTargets & targets)
    {
        /* A force still open at job end (none should be) ends with the job. */
        auto endOf = [&](size_t i) {
            auto e = log[i].end;
            return e == EvalState::UndoEntry::openForce ? jobEnd : e;
        };
        auto n = log.size() - mark;
        std::vector<uint32_t> begin(n + 1);
        for (auto j = mark; j < log.size(); ++j)
            if (log[j].parent >= int64_t(mark))
                begin[log[j].parent - mark + 1]++;
        for (size_t i = 0; i < n; ++i)
            begin[i + 1] += begin[i];
        childIntervals.resize(begin[n]);
        auto fill = begin;
        /* Log order is start order, so each list comes out sorted. */
        for (auto j = mark; j < log.size(); ++j)
            if (log[j].parent >= int64_t(mark))
                childIntervals[fill[log[j].parent - mark]++] = {log[j].start, endOf(j)};
        for (size_t i = 0; i < targets.size(); ++i) {
            auto f = targets.first[i];
            spans.push_back(
                {log[f].start, endOf(f), !targets.repeated[i], begin[f - mark], begin[f - mark + 1]});
        }
    }

    /* allowed(t, a) of the comment above: whether target `t` (its span)
       can own an object allocated at time `a`. Inside t's own interval,
       innermost(a) is t unless a lies in one of its children's intervals
       (the intervals form a forest). With several intervals, only a >= s
       of the first is used. */
    bool allowed(const Span & t, uint64_t a) const
    {
        if (a < t.s)
            return false;
        if (!t.single || a >= t.e)
            return true;
        auto b = childIntervals.begin() + t.c0, e = childIntervals.begin() + t.c1;
        auto i = std::upper_bound(b, e, a, [](uint64_t a, const auto & c) { return a < c.first; });
        return i == b || !(a < std::prev(i)->second);
    }
};

/* Which objects belong to the job: its bump region, or (mimalloc backend)
   its scope heap. */
struct YoungSpace
{
    void * heap = nullptr;

    bool operator()(const void * p) const
    {
#if NIX_UNFORCE_MIMALLOC
        if (heap)
            return inHeap(heap, p);
#endif
        return evalRegion.contains(p);
    }
};

/* Per young object, three flags of the closure (`Closure`): in Evac(S),
   visited by the current walk, and known to reach no Evac(S) cell from
   the walks still to come (a memo). */
enum Flag { inEvac, visited, memo, nFlags };

/* The flags of the bump region's objects: a bitmap per flag, one bit per
   16-byte granule (bump allocations are 16-byte aligned), in 64 KiB pieces
   of the region that are allocated when first touched. At most 3 bits per
   16 bytes of the job, 2.3 %. */
struct RegionMarks
{
    static constexpr size_t granuleShift = 4, pieceShift = 16;
    static constexpr size_t words = (size_t(1) << (pieceShift - granuleShift)) / 64;

    struct Piece
    {
        uint64_t bits[nFlags][words] = {};
    };

    uintptr_t base;
    std::vector<std::unique_ptr<Piece>> pieces;
    size_t allocated = 0;

    RegionMarks(const char * base, const char * cur)
        : base((uintptr_t) base)
        , pieces(((cur - base) >> pieceShift) + 1)
    {
    }

    bool test(const void * p, Flag f) const
    {
        auto g = ((uintptr_t) p - base) >> granuleShift;
        auto & piece = pieces[g >> (pieceShift - granuleShift)];
        return piece && (piece->bits[f][(g >> 6) % words] >> (g & 63) & 1);
    }

    void set(const void * p, Flag f)
    {
        auto g = ((uintptr_t) p - base) >> granuleShift;
        auto & piece = pieces[g >> (pieceShift - granuleShift)];
        if (!piece) {
            piece = std::make_unique<Piece>();
            allocated++;
        }
        piece->bits[f][(g >> 6) % words] |= uint64_t(1) << (g & 63);
    }

    void clear(const void * p, Flag f)
    {
        auto g = ((uintptr_t) p - base) >> granuleShift;
        if (auto & piece = pieces[g >> (pieceShift - granuleShift)])
            piece->bits[f][(g >> 6) % words] &= ~(uint64_t(1) << (g & 63));
    }

    void clearAll(Flag f)
    {
        for (auto & piece : pieces)
            if (piece)
                std::fill(std::begin(piece->bits[f]), std::end(piece->bits[f]), 0);
    }

    size_t bytes() const
    {
        return pieces.capacity() * sizeof(pieces[0]) + allocated * sizeof(Piece);
    }
};

/* The same for the mimalloc backend, whose objects are not in one range. */
struct HeapMarks
{
    std::unordered_map<const void *, uint8_t> flags;

    bool test(const void * p, Flag f) const
    {
        auto i = flags.find(p);
        return i != flags.end() && (i->second >> f & 1);
    }

    void set(const void * p, Flag f)
    {
        flags[p] |= 1 << f;
    }

    void clear(const void * p, Flag f)
    {
        if (auto i = flags.find(p); i != flags.end())
            i->second &= ~(1 << f);
    }

    void clearAll(Flag f)
    {
        for (auto & [p, b] : flags)
            b &= ~(1 << f);
    }

    size_t bytes() const
    {
        return flags.size() * 32 + flags.bucket_count() * sizeof(void *);
    }
};

/* The closure of a job ran out of its step budget. */
struct OverBudget
{};

/* The producer closure of one job: walks of young object graphs along the
   edges the Evacuator copies, without copying.

   Cost. Every walk takes steps (objects popped), and a job may take
   `budget` of them. Three things keep the count down:
   - Evac(S) is marked once per object: marking stops at marked objects,
     so all of a job's marking is linear in Evac(S).
   - A list's element array is visited once per walk however many list
     values share it.
   - Memo. Within a round Evac(S) is fixed. A walk from target w that ends
     without a hit has shown, for every object x it visited (pruned ones
     included), that no Evac(S) cell is reachable from x through objects
     with times >= θ, where θ = s(w), or e(w) if it pruned an object inside
     its own interval (a child's); without such prunes every object of time
     >= s(w) it met was allowed. A later walk u never enters an object of
     time < s(u) (allowed(u, a) implies a >= s(u)), so if s(u) >= θ, u can
     skip x: it would find no hit below x. Targets are walked in start
     order, so a memo with θ = s(w) holds for every later walk of the round;
     one with θ = e(w) is held back until a walk starts at or after e(w).
     A walk that hits leaves no memo (its target joins S anyway), and the
     memos are dropped when a round ends, since S grows. */
template<typename Marks>
struct Closure
{
    enum Kind : uint8_t { kValue, kEnv, kBindings };

    const YoungSpace & young;
    const CellClock & clock;
    Marks marks;
    std::vector<std::pair<const void *, Kind>> stack;
    /* Objects visited by the current walk. */
    std::vector<const void *> touched;
    /* Memos waiting for a walk that starts at or after their θ. */
    std::vector<std::pair<uint64_t, std::vector<const void *>>> pending;
    /* Steps taken in this job, and how many it may take. */
    uint64_t steps = 0, budget = UINT64_MAX;
    size_t peak = 0;

    void step()
    {
        if (++steps > budget) [[unlikely]]
            throw OverBudget{};
    }

    /* Call `f(p, kind)` for each object `v` refers to directly, except a
       young list element array, which goes to `array(elems, size)`. */
    template<typename F, typename A>
    void children(const Value & v, F && f, A && array) const
    {
        switch (v.internalType()) {
        case tAttrs:
            f(v.attrs(), kBindings);
            break;
        case tListSmall: {
            auto l = v.rawSmallList();
            f(l[0], kValue);
            f(l[1], kValue);
            break;
        }
        case tListN: {
            auto l = v.rawList();
            if (young(l.elems))
                array(l.elems, l.size);
            break;
        }
        case tThunk:
            f(v.thunk().env, kEnv);
            break;
        case tApp:
            f(v.app().left, kValue);
            f(v.app().right, kValue);
            break;
        case tPrimOpApp:
            f(v.primOpApp().left, kValue);
            f(v.primOpApp().right, kValue);
            break;
        case tLambda:
            f(v.lambda().env, kEnv);
            break;
        case tFailed:
            f(v.failed().recoveryValue, kValue);
            break;
        case tUninitialized:
        case tInt:
        case tBool:
        case tNull:
        case tFloat:
        case tString:
        case tPath:
        case tExternal:
        case tPrimOp:
        case tNumberOfInternalTypes:
            break;
        }
    }

    template<typename F, typename A>
    void children(const void * p, Kind kind, F && f, A && array) const
    {
        switch (kind) {
        case kValue:
            children(*(const Value *) p, f, array);
            break;
        case kEnv: {
            auto e = (const Env *) p;
            auto size = *(const size_t *) ((const char *) e - 16);
            f(e->up, kEnv);
            for (size_t i = 0; i < size; ++i)
                f(e->values[i], kValue);
            break;
        }
        case kBindings: {
            auto b = (const Bindings *) p;
            b->forEachLayerValue([&](const Value * v) { f(v, kValue); });
            f(b->layerBase(), kBindings);
            break;
        }
        }
    }

    /* Add R(content), every young object reachable from `content` without
       pruning, to Evac(S). */
    void mark(const Value & content)
    {
        stack.clear();
        auto add = [&](const void * p, Kind k) {
            if (p && young(p) && !marks.test(p, inEvac)) {
                marks.set(p, inEvac);
                stack.emplace_back(p, k);
            }
        };
        auto array = [&](Value * const * elems, size_t size) {
            if (!marks.test(elems, inEvac)) {
                marks.set(elems, inEvac);
                for (size_t i = 0; i < size; ++i)
                    add(elems[i], kValue);
            }
        };
        children(content, add, array);
        while (!stack.empty()) {
            auto [p, kind] = stack.back();
            stack.pop_back();
            step();
            children(p, kind, add, array);
        }
    }

    /* Whether a walk from target t's content, pruned by `allowed`, reaches
       a cell of Evac(S): t is a candidate producer. */
    bool candidate(const Value & content, const ForceMarkers & markers, const ForceMarkers::Span & t)
    {
        /* Memos of earlier walks that hold from here on. */
        std::erase_if(pending, [&](auto & m) {
            if (m.first > t.s)
                return false;
            for (auto p : m.second)
                marks.set(p, memo);
            return true;
        });

        stack.clear();
        touched.clear();
        auto push = [&](const void * p, Kind k) {
            if (!p || !young(p) || marks.test(p, visited) || marks.test(p, memo))
                return;
            marks.set(p, visited);
            touched.push_back(p);
            stack.emplace_back(p, k);
        };
        auto array = [&](Value * const * elems, size_t size) {
            if (marks.test(elems, visited) || marks.test(elems, memo))
                return;
            marks.set(elems, visited);
            touched.push_back(elems);
            for (size_t i = 0; i < size; ++i)
                push(elems[i], kValue);
        };
        bool hit = false, ownPruned = false;
        children(content, push, array);
        while (!stack.empty()) {
            auto [p, kind] = stack.back();
            stack.pop_back();
            step();
            if (auto a = clock(p); a && !markers.allowed(t, *a)) {
                ownPruned |= *a >= t.s;
                continue;
            }
            if (kind == kValue && marks.test(p, inEvac)) {
                hit = true;
                break;
            }
            children(p, kind, push, array);
        }

        for (auto p : touched)
            marks.clear(p, visited);
        notePeak();
        if (!hit) {
            auto theta = ownPruned ? t.e : t.s;
            if (theta <= t.s)
                for (auto p : touched)
                    marks.set(p, memo);
            else
                pending.emplace_back(theta, std::move(touched));
        }
        return hit;
    }

    /* A round ended and S grew: the memos no longer hold. */
    void dropMemos()
    {
        pending.clear();
        marks.clearAll(memo);
    }

    void notePeak()
    {
        auto bytes = marks.bytes() + stack.capacity() * sizeof(stack[0]) + touched.capacity() * sizeof(void *);
        for (auto & m : pending)
            bytes += m.second.capacity() * sizeof(void *);
        peak = std::max(peak, bytes);
    }
};

/* For tests of the fatal path: fail the decision's allocations. */
static const bool failDecide = getEnv("NIX_UNFORCE_FAIL_DECIDE").value_or("0") != "0";

/* The closure's rounds: find every reverted target that is a candidate
   against the current Evac(S), then promote them all. The candidate test
   is monotone in S, so this reaches the least fixpoint, whatever the order.
   Returns the promotions added. */
template<typename Marks>
size_t closePromotions(
    Closure<Marks> & closure, const JobTargets & targets, const ForceMarkers & markers, std::vector<bool> & promote)
{
    for (size_t i = 0; i < targets.size(); ++i)
        if (promote[i])
            closure.mark(*targets.cells[i]);
    size_t added = 0;
    while (true) {
        stats.markerRounds++;
        std::vector<size_t> found;
        for (size_t i = 0; i < targets.size(); ++i)
            if (!promote[i] && closure.candidate(*targets.cells[i], markers, markers.spans[i]))
                found.push_back(i);
        if (found.empty())
            break;
        closure.dropMemos();
        for (auto i : found) {
            promote[i] = true;
            added++;
            closure.mark(*targets.cells[i]);
        }
    }
    return added;
}

/* Decide which of a job's targets to promote: the counts in `counts`
   nominate, and the producer closure adds what identity needs (see above).
   This is the fallible step before the job's memory is released: it copies
   and writes nothing but its own bookkeeping, so the caller turns an
   allocation failure here into a fatal error before any target has
   changed. `youngBytes` is the memory the job allocated, which sets the
   closure's step budget. Returns a flag per target.

   The budget. The closure is a walk per reverted target per round, and
   targets can share young structure that every one of them may reach, so
   even with the memo it can take steps quadratic in the job's size (review
   S1: walk-2000-100000 took 400 M steps before the memo). A job that runs
   out of budget falls back to a promotion set that needs no walk and keeps
   identity on its own:
   - "promote" (the default): S = T. Every young cell's owner is a target
     or nothing, so OC holds. This is what K = 1 does for every job: the
     job keeps what it forced, as without unforcing, and its CPU cost is
     the copy, linear in what is kept.
   - "revert": S = {} (pure revert). Nothing is kept, so a hot producer is
     rebuilt in later jobs; a workload that kept exceeding the budget
     would recompute it every job.
   Neither is partial: the fallback replaces the whole decision. */
std::vector<bool> decideJob(
    EvalState & state,
    size_t mark,
    const JobTargets & targets,
    bool canCopy,
    std::unordered_map<const Value *, uint32_t> & counts,
    const YoungSpace & young,
    const CellClock & clock,
    uint64_t jobEnd,
    size_t youngBytes)
{
    auto & cfg = unforceSettings();
    std::vector<bool> promote(targets.size());
    bool counting = canCopy && cfg.promoteAfter;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!counting)
            continue;
        auto before = counts[targets.cells[i]]++;
        if (before)
            stats.reforced++;
        promote[i] = before + 1 >= cfg.promoteAfter;
    }

    /* Objects that client code built in this job were allocated outside the
       region: the closure cannot see them, and re-forcing the cell that
       called the client builds them again. Pure revert keeps identity. */
    if (clientCodeInJob) {
        stats.clientJobs++;
        return std::vector<bool>(targets.size());
    }

    bool any = std::find(promote.begin(), promote.end(), true) != promote.end();
    if (!any || cfg.markers == "off" || !canCopy)
        return promote;

    if (failDecide)
        throw std::bad_alloc();

    ForceMarkers markers(state.undoLog, mark, jobEnd, targets);
    auto budget = cfg.walkBudgetBase + cfg.walkBudgetPerKiB * (youngBytes >> 10);
    uint64_t steps = 0;
    size_t peak = 0;
    auto decide = [&](auto & closure) {
        closure.budget = budget;
        auto closed = promote;
        try {
            auto added = closePromotions(closure, targets, markers, closed);
            promote = std::move(closed);
            stats.markerPromoted += added;
            if (added)
                stats.markerJobs++;
        } catch (OverBudget &) {
            stats.budgetJobs++;
            promote.assign(targets.size(), cfg.walkFallback != "revert");
        }
        closure.notePeak();
        steps = closure.steps;
        peak = closure.peak;
    };
    if (young.heap) {
        Closure<HeapMarks> closure{young, clock};
        decide(closure);
    } else {
        Closure<RegionMarks> closure{young, clock, RegionMarks(evalRegion.base, evalRegion.cur)};
        decide(closure);
    }

    stats.markerSteps += steps;
    stats.maxJobSteps = std::max<size_t>(stats.maxJobSteps, steps);
    stats.maxStepsPerKiB = std::max<size_t>(stats.maxStepsPerKiB, steps / std::max<size_t>(1, youngBytes >> 10));
    stats.markerPeakBytes = std::max(stats.markerPeakBytes, peak + markers.spans.capacity() * sizeof(markers.spans[0])
                                                                + markers.childIntervals.capacity() * 16);
    return promote;
}

/* An allocation failed at the end of a job. Nothing can be released
   partially, so this is fatal (REDESIGN-memory L7). */
[[noreturn]] void unforceFatal(const char * what) noexcept
{
    struct rlimit rl;
    std::string limit = getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
                            ? std::to_string(rl.rlim_cur >> 20) + " MiB"
                            : "unlimited";
    fprintf(
        stderr,
        "error: unforce: out of memory while %s.\n"
        "The process could not allocate more memory (address-space limit, 'ulimit -v': %s; "
        "otherwise the memory the system will commit).\n"
        "Raise the limit, or disable unforcing with '--option eval-unforce false'.\n",
        what,
        limit.c_str());
    std::abort();
}

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

void * evalRegionHeapAllocCell()
{
#if NIX_UNFORCE_MIMALLOC
    auto & f = frames.back();
    if (f.cellCur == f.cellEnd) [[unlikely]] {
        auto chunk = (char *) mi_theap_zalloc(currentTheap, cellChunkBytes);
        if (!chunk)
            throw std::bad_alloc();
        f.chunks.push_back(chunk);
        f.cellCur = chunk;
        f.cellEnd = chunk + cellChunkBytes;
        f.bytes += cellChunkBytes;
    }
    auto p = f.cellCur;
    f.cellCur += 16;
    return p;
#else
    unreachable();
#endif
}

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
        R"({"scopes":%d,"logged":%d,"reverted":%d,"stubbed":%d,"promoted":%d,"relogged":%d,"copiedBytes":%d,"largestScopeBytes":%d,)"
        R"("reforced":%d,"promotedBytes":%d,"freedBytes":%d,"markers":"%s","markerPromoted":%d,)"
        R"("markerRounds":%d,"markerSteps":%d,"markerJobs":%d,"markerLogBytes":%d,"markerPeakBytes":%d,)"
        R"("markerBudgetJobs":%d,"markerFallback":"%s","markerMaxJobSteps":%d,"markerMaxStepsPerKiB":%d,"clientJobs":%d,)"
        R"("revertedInClientJobs":%d,"innerScopeReverts":%d})",
        stats.jobs,
        stats.logged,
        stats.reverted,
        stats.stubbed,
        stats.promoted,
        stats.relogged,
        stats.copiedBytes,
        stats.regionPeak,
        stats.reforced,
        stats.copiedBytes,
        stats.freedBytes,
        unforceSettings().markers,
        stats.markerPromoted,
        stats.markerRounds,
        stats.markerSteps,
        stats.markerJobs,
        stats.markerLogBytes,
        stats.markerPeakBytes,
        stats.budgetJobs,
        unforceSettings().walkFallback,
        stats.maxJobSteps,
        stats.maxStepsPerKiB,
        stats.clientJobs,
        stats.revertedInClientJobs,
        stats.innerScopeReverts);
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
   is reverted, or, for the outermost scope, possibly promoted (copied out of
   all scopes; see `decideJob`). Then the scope's heap is destroyed
   wholesale. */
static void endHeapScope(EvalState & state, size_t mark)
{
    auto & cfg = unforceSettings();
    auto & log = state.undoLog;
    auto jobEnd = unforceClock();
    Frame f = std::move(frames.back());
    frames.pop_back();
    Frame * parent = frames.empty() ? nullptr : &frames.back();
    stats.jobs++;
    generation++;
    stats.logged += log.size() - mark;
    dropActiveForces(mark);

    std::optional<JobTargets> jt;
    std::vector<bool> promoted;
    try {
        jt.emplace(log, mark);
        if (!parent) {
            CellClock clock{.bump = false};
            for (size_t i = 0; i < f.chunks.size(); ++i)
                clock.chunks.emplace_back((uintptr_t) f.chunks[i], i * cellsPerChunk);
            std::sort(clock.chunks.begin(), clock.chunks.end());
            promoted = decideJob(state, mark, *jt, true, state.forceCounts, YoungSpace{f.heap}, clock, jobEnd, f.bytes);
        }
    } catch (std::bad_alloc &) {
        unforceFatal("deciding which values to keep at the end of an unforce scope");
    }

    setCurrentHeap(parent ? parent->heap : nullptr);
    if (parent) {
        /* Only the outermost scope promotes, as with the bump backend: a
           nested scope reverts everything it forced, which keeps identity
           (pure revert), whereas a cell promoted into the enclosing scope
           would be relogged there and could be reverted after a cell holding
           a copy of its data was promoted. */
        for (size_t i = 0; i < jt->size(); ++i) {
            *jt->cells[i] = jt->preImages[i];
            stats.reverted++;
            stats.innerScopeReverts++;
        }
    } else
        try {
            PauseRegion pause;
            Evacuator evacuator{state};
            evacuator.youngHeap = f.heap;
            for (size_t i = 0; i < jt->size(); ++i)
                if (promoted[i]) {
                    *jt->cells[i] = evacuator.copy(*jt->cells[i]);
                    stats.promoted++;
                } else {
                    *jt->cells[i] = jt->preImages[i];
                    stats.reverted++;
                    if (clientCodeInJob)
                        stats.revertedInClientJobs++;
                }
        } catch (std::bad_alloc &) {
            unforceFatal("promoting values out of an unforce scope");
        }

    log.resize(mark);
    if (!parent) {
        log.shrink_to_fit();
        clientCodeInJob = false;
    }
    stats.freedBytes += f.bytes;
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

void unforceNoteClientCode()
{
    if (!scopeStackMarks.empty())
        clientCodeInJob = true;
}

bool unforceIsScopeStackSlot(const void * p)
{
    if (scopeStackMarks.empty() || !stackLow)
        return false;
    /* The scope that will restore the slot: every scope with the mimalloc
       backend, only the job (the outermost scope) with the bump one. */
    auto mark = nested() ? scopeStackMarks.back() : scopeStackMarks.front();
    return (const char *) p >= stackLow && (const char *) p < mark;
}

size_t EvalState::logForce(Value & v)
{
    UndoEntry e{&v, v};
    e.start = unforceClock();
    e.parent = activeForces.empty() ? -1 : int64_t(activeForces.back());
    undoLog.push_back(e);
    stats.markerLogBytes += sizeof(UndoEntry) - offsetof(UndoEntry, start);
    activeForces.push_back(undoLog.size() - 1);
    return undoLog.size();
}

void EvalState::endLoggedForce(size_t marker)
{
    auto i = marker - 1;
    /* Every logged force ends here, innermost first; anything above `i`
       would belong to a scope that closed while this force ran. */
    while (!activeForces.empty() && activeForces.back() > i)
        activeForces.pop_back();
    if (!activeForces.empty() && activeForces.back() == i) {
        activeForces.pop_back();
        if (i < undoLog.size())
            undoLog[i].end = unforceClock();
    }
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
    auto jobEnd = unforceClock();
    dropActiveForces(mark);

    /* Before anything is released: decide what to promote. */
    std::optional<JobTargets> jt;
    std::vector<bool> promoted;
    try {
        jt.emplace(undoLog, mark);
        promoted = decideJob(
            *this,
            mark,
            *jt,
            evalRegion.allTypes,
            forceCounts,
            YoungSpace{},
            CellClock{},
            jobEnd,
            evalRegion.cur - evalRegion.base);
    } catch (std::bad_alloc &) {
        unforceFatal("deciding which values to keep at the end of an unforce batch");
    }

    /* The release: its only allocations are the promotion copies. */
    try {
        PauseRegion pause;
        Evacuator evacuator{*this};
        Stubber stubber{*this, evacuator};
        auto & targets = jt->cells;
        for (size_t i = 0; i < targets.size(); ++i)
            if (promoted[i]) {
                *targets[i] = evacuator.copy(*targets[i]);
                stats.promoted++;
            }

        for (size_t i = 0; i < targets.size(); ++i) {
            auto target = targets[i];
            if (promoted[i])
                continue;
            auto & pre = jt->preImages[i];
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
            if (clientCodeInJob)
                stats.revertedInClientJobs++;
        }
    } catch (std::bad_alloc &) {
        unforceFatal("promoting values out of an unforce batch");
    }

    if (evalRegion.base)
        stats.freedBytes += evalRegion.cur - evalRegion.base;
    clientCodeInJob = false;
    undoLog.resize(mark);
    undoLog.shrink_to_fit();
    if (evalRegion.base)
        releaseRegion();

    if (cfg.stats)
        printStatsAtExit();
}

} // namespace nix
