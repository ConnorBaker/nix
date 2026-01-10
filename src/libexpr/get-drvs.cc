#include "nix/expr/get-drvs.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/derivations.hh"
#include "nix/store/store-api.hh"
#include "nix/store/path-with-outputs.hh"

#include <gc/gc_allocator.h>

#include <cstring>
#include <regex>

namespace nix {

PackageInfo::PackageInfo(EvalState & state, std::string attrPath, Value * attrsValue)
    : state(&state)
    , attrsValue(attrsValue)
    , attrPath(std::move(attrPath))
{
}

PackageInfo::PackageInfo(EvalState & state, ref<Store> store, const std::string & drvPathWithOutputs)
    : state(&state)
    , attrsValue(nullptr)
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
    if (name == "" && attrsValue) {
        auto i = attrsValue->attrsGet(state->s.name);
        if (!i)
            state->error<TypeError>("derivation name missing").debugThrow();
        name = state->forceStringNoCtx(*i.value, noPos, "while evaluating the 'name' attribute of a derivation");
    }
    return name;
}

std::string PackageInfo::querySystem() const
{
    if (system == "" && attrsValue) {
        auto i = attrsValue->attrsGet(state->s.system);
        system =
            !i ? "unknown"
               : state->forceStringNoCtx(*i.value, i.pos, "while evaluating the 'system' attribute of a derivation");
    }
    return system;
}

std::optional<StorePath> PackageInfo::queryDrvPath() const
{
    if (!drvPath && attrsValue) {
        if (auto i = attrsValue->attrsGet(state->s.drvPath)) {
            NixStringContext context;
            auto found = state->coerceToStorePath(
                i.pos, *i.value, context, "while evaluating the 'drvPath' attribute of a derivation");
            try {
                found.requireDerivation();
            } catch (Error & e) {
                e.addTrace(state->positions[i.pos], "while evaluating the 'drvPath' attribute of a derivation");
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
    if (!outPath && attrsValue) {
        auto i = attrsValue->attrsGet(state->s.outPath);
        NixStringContext context;
        if (i)
            outPath = state->coerceToStorePath(
                i.pos, *i.value, context, "while evaluating the output path of a derivation");
    }
    if (!outPath)
        throw UnimplementedError("CA derivations are not yet supported");
    return *outPath;
}

PackageInfo::Outputs PackageInfo::queryOutputs(bool withPaths, bool onlyOutputsToInstall)
{
    if (outputs.empty()) {
        /* Get the 'outputs' list. */
        Value::AttrRef i;
        if (attrsValue && (i = attrsValue->attrsGet(state->s.outputs))) {
            state->forceList(*i.value, i.pos, "while evaluating the 'outputs' attribute of a derivation");

            /* For each output... */
            for (auto elem : i.value->listView()) {
                std::string output(
                    state->forceStringNoCtx(*elem, i.pos, "while evaluating the name of an output of a derivation"));

                if (withPaths) {
                    /* Evaluate the corresponding set. */
                    auto out = attrsValue->attrsGet(state->symbols.create(output));
                    if (!out)
                        continue; // FIXME: throw error?
                    state->forceAttrs(*out.value, i.pos, "while evaluating an output of a derivation");

                    /* And evaluate its 'outPath' attribute. */
                    auto outPath = out.value->attrsGet(state->s.outPath);
                    if (!outPath)
                        continue; // FIXME: throw error?
                    NixStringContext context;
                    outputs.emplace(
                        output,
                        state->coerceToStorePath(
                            outPath.pos, *outPath.value, context, "while evaluating an output path of a derivation"));
                } else
                    outputs.emplace(output, std::nullopt);
            }
        } else
            outputs.emplace("out", withPaths ? std::optional{queryOutPath()} : std::nullopt);
    }

    if (!onlyOutputsToInstall || !attrsValue)
        return outputs;

    Value::AttrRef i;
    if (attrsValue && (i = attrsValue->attrsGet(state->s.outputSpecified))
        && state->forceBool(*i.value, i.pos, "while evaluating the 'outputSpecified' attribute of a derivation")) {
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
            throw errMsg;
        Outputs result;
        for (auto elem : outTI->listView()) {
            if (elem->type() != nString)
                throw errMsg;
            auto out = outputs.find(elem->string_view());
            if (out == outputs.end())
                throw errMsg;
            result.insert(*out);
        }
        return result;
    }
}

std::string PackageInfo::queryOutputName() const
{
    if (outputName == "" && attrsValue) {
        auto i = attrsValue->attrsGet(state->s.outputName);
        outputName =
            i ? state->forceStringNoCtx(*i.value, noPos, "while evaluating the output name of a derivation") : "";
    }
    return outputName;
}

Value * PackageInfo::getMeta()
{
    if (metaValue)
        return metaValue;
    if (!attrsValue)
        return nullptr;
    auto a = attrsValue->attrsGet(state->s.meta);
    if (!a)
        return nullptr;
    state->forceAttrs(*a.value, a.pos, "while evaluating the 'meta' attribute of a derivation");
    metaValue = a.value;
    return metaValue;
}

StringSet PackageInfo::queryMetaNames()
{
    StringSet res;
    if (!getMeta())
        return res;
    metaValue->forEachAttr([&](Symbol name, Value *, PosIdx) {
        res.emplace(state->symbols[name]);
    });
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
        if (v.attrsGet(state->s.outPath))
            return false;
        bool allValid = true;
        v.forEachAttr([&](Symbol, Value * value, PosIdx) {
            if (!checkMeta(*value))
                allValid = false;
        });
        return allValid;
    } else
        return v.type() == nInt || v.type() == nBool || v.type() == nString || v.type() == nFloat;
}

Value * PackageInfo::queryMeta(const std::string & name)
{
    if (!getMeta())
        return nullptr;
    auto a = metaValue->attrsGet(state->symbols.create(name));
    if (!a || !checkMeta(*a.value))
        return nullptr;
    return a.value;
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
    auto sym = state->symbols.create(name);
    auto builder = state->buildBindings();
    if (metaValue)
        metaValue->forEachAttr([&](Symbol attrName, Value * attrValue, PosIdx attrPos) {
            if (attrName != sym)
                builder.insert(attrName, attrValue, attrPos);
        });
    if (v)
        builder.insert(sym, v);
    metaValue = state->allocValue();
    metaValue->mkAttrs(builder);
}

/* Cache for already considered attrsets. */
/* Use gc_allocator so the GC can see Value* pointers stored in the set's tree nodes */
typedef std::set<Value *, std::less<Value *>, gc_allocator<Value *>> Done;

/* Evaluate value `v'.  If it evaluates to a set of type `derivation',
   then put information about it in `drvs' (unless it's already in `done').
   The result boolean indicates whether it makes sense
   for the caller to recursively search for derivations in `v'. */
static bool getDerivation(
    EvalState & state,
    Value & v,
    const std::string & attrPath,
    PackageInfos & drvs,
    Done & done,
    bool ignoreAssertionFailures)
{
    try {
        state.forceValue(v, v.determinePos(noPos));
        if (!state.isDerivation(v))
            return true;

        /* Remove spurious duplicates (e.g., a set like `rec { x =
           derivation {...}; y = x;}'. */
        if (!done.insert(&v).second)
            return false;

        PackageInfo drv(state, attrPath, &v);

        drv.queryName();

        drvs.push_back(drv);

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
    getDerivation(state, v, "", drvs, done, ignoreAssertionFailures);
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
    Value & autoArgs,
    PackageInfos & drvs,
    Done & done,
    bool ignoreAssertionFailures)
{
    auto _level = state.addCallDepth(vIn.determinePos(noPos));

    // Allocate v on the GC heap, not the stack, because PackageInfo stores
    // a pointer to it via attrsValue which must remain valid after this function returns.
    Value * vPtr = state.allocValue();
    Value & v = *vPtr;
    state.autoCallFunction(autoArgs, vIn, v);

    /* Process the expression. */
    if (!getDerivation(state, v, pathPrefix, drvs, done, ignoreAssertionFailures))
        ;

    else if (v.type() == nAttrs) {

        /* !!! undocumented hackery to support combining channels in
           nix-env.cc. */
        bool combineChannels = static_cast<bool>(v.attrsGet(state.symbols.create("_combineChannels")));

        /* Consider the attributes in sorted order to get more
           deterministic behaviour in nix-env operations (e.g. when
           there are names clashes between derivations, the derivation
           bound to the attribute with the "lower" name should take
           precedence). */
        // Use gc_allocator so the GC can see Value* pointers stored in the vector
        struct AttrEntry { Symbol name; Value * value; PosIdx pos; };
        std::vector<AttrEntry, gc_allocator<AttrEntry>> sortedAttrs;
        v.forEachAttr([&](Symbol name, Value * value, PosIdx pos) {
            sortedAttrs.push_back({name, value, pos});
        });
        std::sort(sortedAttrs.begin(), sortedAttrs.end(), [&](const AttrEntry & a, const AttrEntry & b) {
            return std::string_view(state.symbols[a.name]) < std::string_view(state.symbols[b.name]);
        });

        for (const auto & i : sortedAttrs) {
            std::string_view symbol{state.symbols[i.name]};
            try {
                debug("evaluating attribute '%1%'", symbol);
                if (!isAttrPathComponent(symbol))
                    continue;
                std::string pathPrefix2 = addToPath(pathPrefix, symbol);
                if (combineChannels)
                    getDerivations(state, *i.value, pathPrefix2, autoArgs, drvs, done, ignoreAssertionFailures);
                else if (getDerivation(state, *i.value, pathPrefix2, drvs, done, ignoreAssertionFailures)) {
                    /* If the value of this attribute is itself a set,
                    should we recurse into it?  => Only if it has a
                    `recurseForDerivations = true' attribute. */
                    if (i.value->type() == nAttrs) {
                        auto j = i.value->attrsGet(state.s.recurseForDerivations);
                        if (j
                            && state.forceBool(
                                *j.value, j.pos, "while evaluating the attribute `recurseForDerivations`"))
                            getDerivations(
                                state, *i.value, pathPrefix2, autoArgs, drvs, done, ignoreAssertionFailures);
                    }
                }
            } catch (Error & e) {
                e.addTrace(state.positions[i.pos], "while evaluating the attribute '%s'", symbol);
                throw;
            }
        }
    }

    else if (v.type() == nList) {
        auto listView = v.listView();
        for (auto [n, elem] : enumerate(listView)) {
            std::string pathPrefix2 = addToPath(pathPrefix, fmt("%d", n));
            if (getDerivation(state, *elem, pathPrefix2, drvs, done, ignoreAssertionFailures))
                getDerivations(state, *elem, pathPrefix2, autoArgs, drvs, done, ignoreAssertionFailures);
        }
    }

    else
        state.error<TypeError>("expression does not evaluate to a derivation (or a set or list of those)").debugThrow();
}

void getDerivations(
    EvalState & state,
    Value & v,
    const std::string & pathPrefix,
    Value & autoArgs,
    PackageInfos & drvs,
    bool ignoreAssertionFailures)
{
    Done done;
    getDerivations(state, v, pathPrefix, autoArgs, drvs, done, ignoreAssertionFailures);
}

} // namespace nix
