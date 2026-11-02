#include "nix/expr/value-to-json.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/unforce.hh"
#include "nix/store/store-api.hh"
#include "nix/util/signals.hh"

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace nix {
using json = nlohmann::json;

/* The cold paths and the path case of printValueAsJSON, out of line. */

[[gnu::noinline]] static json pathToJSON(EvalState & state, Value & v, NixStringContext & context, bool copyToStore)
{
    if (copyToStore)
        return state.store->printStorePath(state.copyPathToStore(context, v.path()));
    else
        return v.path().path.abs();
}

[[gnu::noinline, gnu::cold]] static void addAttrTraceJSON(EvalState & state, Error & e, const Attr & a)
{
    e.addTrace(state.positions[a.pos], HintFmt("while evaluating attribute '%1%'", state.symbols[a.name]));
}

[[gnu::noinline, gnu::cold]] static void addListElemTraceJSON(EvalState & state, Error & e, const PosIdx pos, int i)
{
    e.addTrace(state.positions[pos], HintFmt("while evaluating list element at index %1%", i));
}

[[noreturn, gnu::noinline, gnu::cold]] static void throwNotJSON(EvalState & state, Value & v, const PosIdx pos)
{
    state.error<TypeError>("cannot convert %1% to JSON", showType(v)).atPos(v.determinePos(pos)).debugThrow();
}

/* `top`: `v` is the root of the caller's traversal (`printValueAsJSON`). */
template<bool top>
static json
valueToJSON(EvalState & state, bool strict, Value & v, const PosIdx pos, NixStringContext & context, bool copyToStore)
{
    checkInterrupt();

    auto _level = state.addCallDepth(pos);

    if (strict)
        state.forceValue(v, pos);

    json out;

    switch (v.type()) {

    case nInt:
        out = v.integer().value;
        break;

    case nBool:
        out = v.boolean();
        break;

    case nString:
        copyContext(v, context);
        out = v.string_view();
        break;

    case nPath:
        out = pathToJSON(state, v, context, copyToStore);
        break;

    case nNull:
        // already initialized as null
        break;

    case nAttrs: {
        out = state.peelToStringOutPath(
            pos, v, /*checkToStringReturn=*/true, [&](Value * peeled, bool cameThroughToString) -> json {
                if (peeled->type() != nAttrs) {
                    // Historical quirk preserved here for reproducibility:
                    // In some coercions, Nix would coerce paths to a raw string
                    // if they came from a __toString result.
                    auto copyToStore2 = copyToStore && !cameThroughToString;
                    return valueToJSON<false>(state, strict, *peeled, pos, context, copyToStore2);
                }
                // Peelable attrs handled. Returned attrs are not peelable.
                // Quirk: builtins.toJSON { outPath.foo = true; } == "{\"foo\":true}"
                // All that remains is to return a JSON object.
                json obj = json::object();
                for (auto & a : peeled->attrs()->lexicographicOrder(state.symbols)) {
                    unforceLevelElement(
                        state, *peeled, strict ? UnforceLoop::json : UnforceLoop::other, top && peeled == &v);
                    try {
                        obj.emplace(
                            state.symbols[a->name],
                            valueToJSON<false>(state, strict, *a->value, a->pos, context, copyToStore));
                    } catch (Error & e) {
                        addAttrTraceJSON(state, e, *a);
                        throw;
                    }
                }
                return obj;
            });
        break;
    }

    case nList: {
        out = json::array();
        int i = 0;
        for (auto elem : v.listView()) {
            unforceLevelElement(state, v, strict ? UnforceLoop::json : UnforceLoop::other, top);
            try {
                out.push_back(valueToJSON<false>(state, strict, *elem, pos, context, copyToStore));
            } catch (Error & e) {
                addListElemTraceJSON(state, e, pos, i);
                throw;
            }
            i++;
        }
        break;
    }

    case nExternal:
        return v.external()->printValueAsJSON(state, strict, context, copyToStore);
        break;

    case nFloat:
        out = v.fpoint();
        break;

    case nThunk:
    case nFailed:
    case nFunction:
        throwNotJSON(state, v, pos);
    }
    return out;
}

// TODO: rename. It doesn't print.
json printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    NixStringContext & context,
    bool copyToStore,
    bool traversal)
{
    return traversal ? valueToJSON<true>(state, strict, v, pos, context, copyToStore)
                     : valueToJSON<false>(state, strict, v, pos, context, copyToStore);
}

void JSONSerializationError::anchor() {}

void printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    std::ostream & str,
    NixStringContext & context,
    bool copyToStore,
    bool traversal)
{
    try {
        str << printValueAsJSON(state, strict, v, pos, context, copyToStore, traversal);
    } catch (nlohmann::json::exception & e) {
        throw JSONSerializationError("JSON serialization error: %s", e.what());
    }
}

json ExternalValueBase::printValueAsJSON(
    EvalState & state, bool strict, NixStringContext & context, bool copyToStore) const
{
    state.error<TypeError>("cannot convert %1% to JSON", showType()).debugThrow();
}

} // namespace nix
