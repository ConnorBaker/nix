#include "nix/expr/get-drvs.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/store/path-with-outputs.hh"
#include "nix/util/finally.hh"

#include <cstring>
#include <optional>

namespace nix {

PackageInfo::PackageInfo(EvalState & state, std::string attrPath, const Bindings * attrs)
    : state(&state)
    , attrs(attrs)
    , attrPath(std::move(attrPath))
{
}

PackageInfo::PackageInfo(EvalState & state, ref<Store> store, const std::string & drvPathWithOutputs)
    : state(&state)
    , attrs(nullptr)
    , attrPath("")
{
    auto [drvPath, selectedOutputs] = parsePathWithOutputs(*store, drvPathWithOutputs);

    this->drvPath = drvPath;

    auto drv = store->derivationFromPath(drvPath);

    name = drvPath.name();

    if (selectedOutputs.size() > 1)
        throw Error("building more than one derivation output is not supported, in '%s'", drvPathWithOutputs);

    outputName = selectedOutputs.empty() ? getOr(drv.env, "outputName", "out") : *selectedOutputs.begin();

    auto i = drv.outputs.find(outputName);
    if (i == drv.outputs.end())
        throw Error("derivation '%s' does not have output '%s'", store->printStorePath(drvPath), outputName);
    auto & [outputName, output] = *i;

    outPath = {output.path(*store, drv.name, outputName)};
}

std::string PackageInfo::queryName() const
{
    if (name == "" && attrs) {
        auto i = attrs->get(state->s.name);
        if (!i)
            state->error<TypeError>("derivation name missing").debugThrow();
        name = state->forceStringNoCtx(*i->value, noPos, "while evaluating the 'name' attribute of a derivation");
    }
    return name;
}

std::string PackageInfo::querySystem() const
{
    if (system == "" && attrs) {
        auto i = attrs->get(state->s.system);
        system =
            !i ? "unknown"
               : state->forceStringNoCtx(*i->value, i->pos, "while evaluating the 'system' attribute of a derivation");
    }
    return system;
}

std::optional<StorePath> PackageInfo::queryDrvPath() const
{
    if (!drvPath && attrs) {
        if (auto i = attrs->get(state->s.drvPath)) {
            NixStringContext context;
            auto found = state->coerceToStorePath(
                i->pos, *i->value, context, "while evaluating the 'drvPath' attribute of a derivation");
            try {
                found.requireDerivation();
            } catch (Error & e) {
                e.addTrace(state->positions[i->pos], "while evaluating the 'drvPath' attribute of a derivation");
                throw;
            }
            drvPath = {std::move(found)};
        } else
            drvPath = {std::nullopt};
    }
    return drvPath.value_or(std::nullopt);
}

StorePath PackageInfo::requireDrvPath() const
{
    if (auto drvPath = queryDrvPath())
        return *drvPath;
    throw Error("derivation does not contain a 'drvPath' attribute");
}

StorePath PackageInfo::queryOutPath() const
{
    if (!outPath && attrs) {
        auto i = attrs->get(state->s.outPath);
        NixStringContext context;
        if (i)
            outPath = state->coerceToStorePath(
                i->pos, *i->value, context, "while evaluating the output path of a derivation");
    }
    if (!outPath)
        throw Error("derivation does not have attribute 'outPath'");
    return *outPath;
}

PackageInfo::Outputs PackageInfo::queryOutputs(bool withPaths, bool onlyOutputsToInstall)
{
    if (outputs.empty()) {
        /* Get the ‘outputs’ list. */
        const Attr * i;
        if (attrs && (i = attrs->get(state->s.outputs))) {
            state->forceList(*i->value, i->pos, "while evaluating the 'outputs' attribute of a derivation");

            /* For each output... */
            for (auto elem : i->value->listView()) {
                std::string output(
                    state->forceStringNoCtx(*elem, i->pos, "while evaluating the name of an output of a derivation"));

                if (withPaths) {
                    /* Evaluate the corresponding set. */
                    auto out = attrs->get(state->symbols.create(output));
                    if (!out)
                        continue; // FIXME: throw error?
                    state->forceAttrs(*out->value, i->pos, "while evaluating an output of a derivation");

                    /* And evaluate its ‘outPath’ attribute. */
                    auto outPath = out->value->attrs()->get(state->s.outPath);
                    if (!outPath)
                        continue; // FIXME: throw error?
                    NixStringContext context;
                    outputs.emplace(
                        output,
                        state->coerceToStorePath(
                            outPath->pos, *outPath->value, context, "while evaluating an output path of a derivation"));
                } else
                    outputs.emplace(output, std::nullopt);
            }
        } else
            outputs.emplace("out", withPaths ? std::optional{queryOutPath()} : std::nullopt);
    }

    if (!onlyOutputsToInstall || !attrs)
        return outputs;

    const Attr * i;
    if (attrs && (i = attrs->get(state->s.outputSpecified))
        && state->forceBool(*i->value, i->pos, "while evaluating the 'outputSpecified' attribute of a derivation")) {
        Outputs result;
        auto out = outputs.find(queryOutputName());
        if (out == outputs.end())
            throw Error("derivation does not have output '%s'", queryOutputName());
        result.insert(*out);
        return result;
    }

    else {
        /* Check for `meta.outputsToInstall` and return `outputs` reduced to that. */
        const Value * outTI = queryMeta("outputsToInstall");
        if (!outTI)
            return outputs;
        auto errMsg = Error("this derivation has bad 'meta.outputsToInstall'");
        /* ^ this shows during `nix-env -i` right under the bad derivation */
        if (!outTI->isList())
            throw std::move(errMsg);
        Outputs result;
        for (auto elem : outTI->listView()) {
            if (elem->type() != nString)
                throw std::move(errMsg);
            auto out = outputs.find(elem->string_view());
            if (out == outputs.end())
                throw std::move(errMsg);
            result.insert(*out);
        }
        return result;
    }
}

std::string PackageInfo::queryOutputName() const
{
    if (outputName == "" && attrs) {
        auto i = attrs->get(state->s.outputName);
        outputName =
            i ? state->forceStringNoCtx(*i->value, noPos, "while evaluating the output name of a derivation") : "";
    }
    return outputName;
}

const Bindings * PackageInfo::getMeta()
{
    if (meta)
        return meta;
    if (!attrs)
        return 0;
    auto a = attrs->get(state->s.meta);
    if (!a)
        return 0;
    state->forceAttrs(*a->value, a->pos, "while evaluating the 'meta' attribute of a derivation");
    meta = a->value->attrs();
    return meta;
}

StringSet PackageInfo::queryMetaNames()
{
    StringSet res;
    if (!getMeta())
        return res;
    for (auto & i : *meta)
        res.emplace(state->symbols[i.name]);
    return res;
}

bool PackageInfo::checkMeta(Value & v)
{
    auto _level = state->addCallDepth(v.determinePos(noPos));

    state->forceValue(v, v.determinePos(noPos));
    if (v.type() == nList) {
        for (auto elem : v.listView())
            if (!checkMeta(*elem))
                return false;
        return true;
    } else if (v.type() == nAttrs) {
        if (v.attrs()->get(state->s.outPath))
            return false;
        for (auto & i : *v.attrs())
            if (!checkMeta(*i.value))
                return false;
        return true;
    } else
        return v.type() == nInt || v.type() == nBool || v.type() == nString || v.type() == nFloat;
}

Value * PackageInfo::queryMeta(const std::string & name)
{
    if (!getMeta())
        return 0;
    auto a = meta->get(state->symbols.create(name));
    if (!a || !checkMeta(*a->value))
        return 0;
    return a->value;
}

std::string PackageInfo::queryMetaString(const std::string & name)
{
    Value * v = queryMeta(name);
    if (!v || v->type() != nString)
        return "";
    return std::string{v->string_view()};
}

NixInt PackageInfo::queryMetaInt(const std::string & name, NixInt def)
{
    Value * v = queryMeta(name);
    if (!v)
        return def;
    if (v->type() == nInt)
        return v->integer();
    if (v->type() == nString) {
        /* Backwards compatibility with before we had support for
           integer meta fields. */
        if (auto n = string2Int<NixInt::Inner>(v->string_view()))
            return NixInt{*n};
    }
    return def;
}

NixFloat PackageInfo::queryMetaFloat(const std::string & name, NixFloat def)
{
    Value * v = queryMeta(name);
    if (!v)
        return def;
    if (v->type() == nFloat)
        return v->fpoint();
    if (v->type() == nString) {
        /* Backwards compatibility with before we had support for
           float meta fields. */
        if (auto n = string2Float<NixFloat>(v->string_view()))
            return *n;
    }
    return def;
}

bool PackageInfo::queryMetaBool(const std::string & name, bool def)
{
    Value * v = queryMeta(name);
    if (!v)
        return def;
    if (v->type() == nBool)
        return v->boolean();
    if (v->type() == nString) {
        /* Backwards compatibility with before we had support for
           Boolean meta fields. */
        if (v->string_view() == "true")
            return true;
        if (v->string_view() == "false")
            return false;
    }
    return def;
}

void PackageInfo::setMeta(const std::string & name, Value * v)
{
    getMeta();
    auto attrs = state->buildBindings(1 + (meta ? meta->size() : 0));
    auto sym = state->symbols.create(name);
    if (meta)
        for (auto i : *meta)
            if (i.name != sym)
                attrs.insert(i);
    if (v)
        attrs.insert(sym, v);
    meta = attrs.finish();
}

/* Cache for already considered attrsets. */
typedef std::set<const Bindings *> Done;

/* Where found derivations go. With `unforce`, every attribute is processed
   in its own unforce scope, so the sink must finish with a derivation
   before returning: afterwards its attributes may be thunks again. */
struct DrvSink
{
    std::function<void(PackageInfo &)> fun;
    bool unforce = false;
};

/* Evaluate value `v'.  If it evaluates to a set of type `derivation',
   then put information about it in `drvs' (unless it's already in `done').
   The result boolean indicates whether it makes sense
   for the caller to recursively search for derivations in `v'. */
static bool getDerivation(
    EvalState & state,
    Value & v,
    const std::string & attrPath,
    const DrvSink & drvs,
    Done & done,
    bool ignoreAssertionFailures)
{
    try {
        state.forceValue(v, v.determinePos(noPos));
        if (!state.isDerivation(v))
            return true;

        /* Remove spurious duplicates (e.g., a set like `rec { x =
           derivation {...}; y = x;}'. */
        /* Addresses are no identity once values can be unforced: the
           sink dedupes instead. */
        if (!drvs.unforce && !done.insert(v.attrs()).second)
            return false;

        PackageInfo drv(state, attrPath, v.attrs());

        drv.queryName();

        drvs.fun(drv);

        return false;

    } catch (AssertionError & e) {
        if (ignoreAssertionFailures)
            return false;
        throw;
    }
}

std::optional<PackageInfo> getDerivation(EvalState & state, Value & v, bool ignoreAssertionFailures)
{
    Done done;
    PackageInfos drvs;
    getDerivation(state, v, "", DrvSink{[&](PackageInfo & d) { drvs.push_back(d); }}, done, ignoreAssertionFailures);
    if (drvs.size() != 1)
        return {};
    return std::move(drvs.front());
}

static std::string addToPath(const std::string & s1, std::string_view s2)
{
    return s1.empty() ? std::string(s2) : s1 + "." + s2;
}

static bool isAttrPathComponent(std::string_view symbol)
{
    if (symbol.empty())
        return false;

    /* [A-Za-z_] */
    unsigned char first = symbol[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_'))
        return false;

    /* [A-Za-z0-9-_+]* */
    for (unsigned char c : symbol.substr(1)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '+')
            continue;
        return false;
    }

    return true;
}

static void getDerivations(
    EvalState & state,
    Value & vIn,
    const std::string & pathPrefix,
    const Bindings & autoArgs,
    const DrvSink & drvs,
    Done & done,
    bool ignoreAssertionFailures);

/* Inc3 S4-b-a1: process ONE drv-set attribute (the attrs-branch loop body),
   factored out so the authoritative serial pass AND the warm pre-force tasks run
   the SAME code -> the warm tasks force exactly what the serial pass forces. */
static void processDrvSetElement(
    EvalState & state,
    const Attr & i,
    const std::string & pathPrefix,
    const Bindings & autoArgs,
    const DrvSink & drvs,
    Done & done,
    bool combineChannels,
    bool ignoreAssertionFailures);

/* Inc3 S4-b-a1: true on the MAIN thread while a drv-set fan-out is active, so a
   recursive `getDerivations` (for a `recurseForDerivations` set) does NOT fan out
   again (no nested dispatch). thread_local: workers never set it (they are gated
   out by `!amWorkerThread`). */
static thread_local bool drvSetFanOutActive = false;

static void getDerivations(
    EvalState & state,
    Value & vIn,
    const std::string & pathPrefix,
    const Bindings & autoArgs,
    const DrvSink & drvs,
    Done & done,
    bool ignoreAssertionFailures)
{
    auto _level = state.addCallDepth(vIn.determinePos(noPos));

    Value v;
    state.autoCallFunction(autoArgs, vIn, v);

    /* Process the expression. */
    if (!getDerivation(state, v, pathPrefix, drvs, done, ignoreAssertionFailures))
        ;

    else if (v.type() == nAttrs) {

        /* !!! undocumented hackery to support combining channels in
           nix-env.cc. */
        bool combineChannels = v.attrs()->get(state.symbols.create("_combineChannels"));

        /* Consider the attributes in sorted order to get more
           deterministic behaviour in nix-env operations (e.g. when
           there are names clashes between derivations, the derivation
           bound to the attribute with the "lower" name should take
           precedence). */
        auto order = v.attrs()->lexicographicOrder(state.symbols);

        /* Inc3 S4-b-a1: the drv-set fan-out. When unforcing runs under worker
           threads, PRE-FORCE each element's derivation on a worker (a WARM task,
           running the SAME `processDrvSetElement` into a DISCARD sink), then run
           the authoritative serial pass below -- which is the SOLE producer of
           `drvs` and hits the warm-forced values via the election protocol
           (single producer per shared cell => byte-identical; drvPaths are
           content hashes, not identities). Gated on the MAIN thread only
           (`!amWorkerThread`) and not already inside a fan-out
           (`!drvSetFanOutActive`), so a recursive `getDerivations` runs serially
           (no nested dispatch). Reclamation stays OFF (a1): the barrier only
           awaits quiescence and re-homes (frees nothing). */
        bool fanOut = state.executor && state.executor->enabled && drvs.unforce && !Executor::amWorkerThread
            && !drvSetFanOutActive;

        std::optional<WaveScope> wave;
        bool serialOk = false;
        if (fanOut) {
            drvSetFanOutActive = true;
            wave.emplace(*state.executor);
            /* Inc3 S4-b-b1 (F4c): capture each worker's per-wave START mark BEFORE
               any warm task is dispatched. Guarded on the pool being quiescent so
               the read of worker thread-locals is race-free (the drv-set driver
               has dispatched no wave work yet; any prior executor work has
               drained). If not idle, the marks stay empty and the barrier treats
               the whole worker region as this wave's foam (safe, b1 reverts
               nothing). */
            unforceCaptureWaveStart(state.executor->outstandingWork() == 0);
            for (auto * i : order) {
                std::string_view symbol{state.symbols[i->name]};
                if (!isAttrPathComponent(symbol))
                    continue;
                Executor::WorkItems work;
                work.emplace_back(
                    [&state, i, &pathPrefix, &autoArgs, combineChannels, ignoreAssertionFailures, uf(drvs.unforce)]() {
                        Done localDone;
                        DrvSink discard{[](PackageInfo &) {}, uf};
                        try {
                            processDrvSetElement(
                                state, *i, pathPrefix, autoArgs, discard, localDone, combineChannels,
                                ignoreAssertionFailures);
                        } catch (const Interrupted &) {
                            throw; /* propagate a wave-cancel / teardown */
                        } catch (...) {
                            /* A warm hint's error is dropped -- the serial pass
                               below surfaces the real (first, in-order) error. */
                        }
                    },
                    uint8_t(0));
                wave->spawn(std::move(work));
            }
        }

        /* The barrier runs on EVERY exit from this scope (normal or unwinding),
           so the warm tasks -- which reference `state`/`pathPrefix`/`autoArgs` --
           are always awaited before this call returns. On an abnormal exit
           (`!serialOk`) the wave is CANCELLED first so a sibling the serial pass
           never reached unwinds promptly instead of hanging. */
        Finally drainWave([&]() noexcept {
            if (!wave)
                return;
            drvSetFanOutActive = false;
            if (!serialOk)
                wave->requestCancel();
            try {
                wave->barrier();
            } catch (...) {
                ignoreExceptionInDestructor();
            }
            try {
                state.executor->awaitExecutorIdle();
            } catch (...) {
                ignoreExceptionInDestructor();
            }
            /* F4a + F4c: assemble each participating worker's this-wave stripe
               slice into the registry + validate the delimitation (quiescent;
               frees nothing, reverts nothing, resets no TLS in b1). */
            try {
                unforceRehomeWorkerStripes(state);
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        });

        /* The authoritative serial pass -- byte-identical to base (the sole
           producer of `drvs`). */
        {
            /* With `unforce`, what each attribute's evaluation forced is
               unforced once its batch is done (see `UnforceBatch`). */
            UnforceBatch batch(state, drvs.unforce);
            for (auto * i : order) {
                std::string_view symbol{state.symbols[i->name]};
                UnforceBatch::Element element(batch);
                /* Inc3-S2: the wave barrier classifies this element's result
                   closure by cross-element reachability. */
                if (drvs.unforce)
                    state.noteWaveElementRoot(i->value);
                try {
                    debug("evaluating attribute '%1%'", symbol);
                    processDrvSetElement(
                        state, *i, pathPrefix, autoArgs, drvs, done, combineChannels, ignoreAssertionFailures);
                } catch (Error & e) {
                    e.addTrace(state.positions[i->pos], "while evaluating the attribute '%s'", symbol);
                    throw;
                }
            }
        }
        serialOk = true;
    }

    else if (v.type() == nList) {
        auto listView = v.listView();
        UnforceBatch batch(state, drvs.unforce);
        for (auto [n, elem] : enumerate(listView)) {
            std::string pathPrefix2 = addToPath(pathPrefix, fmt("%d", n));
            UnforceBatch::Element element(batch);
            /* Inc3-S2: record this element's root for the wave barrier's
               reachability-DFS promotion trigger. */
            if (drvs.unforce)
                state.noteWaveElementRoot(elem);
            if (getDerivation(state, *elem, pathPrefix2, drvs, done, ignoreAssertionFailures))
                getDerivations(state, *elem, pathPrefix2, autoArgs, drvs, done, ignoreAssertionFailures);
        }
    }

    else
        state.error<TypeError>("expression does not evaluate to a derivation (or a set or list of those)").debugThrow();
}

/* Inc3 S4-b-a1: one drv-set attribute, exactly as the attrs-branch loop body did
   it -- used by BOTH the authoritative serial pass and the warm pre-force tasks.
   (Mirrors the original inline body; `done` is inert under `unforce`, so a warm
   task's local `done` behaves identically to the serial pass's shared one.) */
static void processDrvSetElement(
    EvalState & state,
    const Attr & i,
    const std::string & pathPrefix,
    const Bindings & autoArgs,
    const DrvSink & drvs,
    Done & done,
    bool combineChannels,
    bool ignoreAssertionFailures)
{
    std::string_view symbol{state.symbols[i.name]};
    if (!isAttrPathComponent(symbol))
        return;
    std::string pathPrefix2 = addToPath(pathPrefix, symbol);
    if (combineChannels)
        getDerivations(state, *i.value, pathPrefix2, autoArgs, drvs, done, ignoreAssertionFailures);
    else if (getDerivation(state, *i.value, pathPrefix2, drvs, done, ignoreAssertionFailures)) {
        /* If the value of this attribute is itself a set, should we recurse into
           it? => Only if it has a `recurseForDerivations = true' attribute. */
        if (i.value->type() == nAttrs) {
            auto j = i.value->attrs()->get(state.s.recurseForDerivations);
            if (j && state.forceBool(*j->value, j->pos, "while evaluating the attribute `recurseForDerivations`"))
                getDerivations(state, *i.value, pathPrefix2, autoArgs, drvs, done, ignoreAssertionFailures);
        }
    }
}

void getDerivations(
    EvalState & state,
    Value & v,
    const std::string & pathPrefix,
    const Bindings & autoArgs,
    PackageInfos & drvs,
    bool ignoreAssertionFailures)
{
    Done done;
    getDerivations(
        state,
        v,
        pathPrefix,
        autoArgs,
        DrvSink{[&](PackageInfo & d) { drvs.push_back(d); }},
        done,
        ignoreAssertionFailures);
}

void forEachDerivation(
    EvalState & state,
    Value & v,
    const std::string & pathPrefix,
    const Bindings & autoArgs,
    std::function<void(PackageInfo &)> sink,
    bool unforce,
    bool ignoreAssertionFailures)
{
    Done done;
    getDerivations(state, v, pathPrefix, autoArgs, DrvSink{std::move(sink), unforce}, done, ignoreAssertionFailures);
}

} // namespace nix
