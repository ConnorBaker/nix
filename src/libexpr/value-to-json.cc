#include "nix/expr/value-to-json.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/util/signals.hh"

#include <cstdlib>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <gc/gc_allocator.h>

namespace nix {
using json = nlohmann::json;

// TODO: rename. It doesn't print.
json printValueAsJSON(
    EvalState & state, bool strict, Value & v, const PosIdx pos, NixStringContext & context, bool copyToStore)
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
        if (copyToStore)
            out = state.store->printStorePath(state.copyPathToStore(context, v.path()));
        else
            out = v.path().path.abs();
        break;

    case nNull:
        // already initialized as null
        break;

    case nAttrs: {
        auto maybeString = state.tryAttrsToString(pos, v, context, false, false);
        if (maybeString) {
            out = *maybeString;
            break;
        }
        if (auto i = v.attrsGet(state.s.outPath))
            return printValueAsJSON(state, strict, *i.value, i.pos, context, copyToStore);
        else {
            out = json::object();
            // Build a sorted list of attributes
            // Use gc_allocator so the GC can see Value* pointers stored in the vector
            std::vector<std::tuple<std::string_view, Value *, PosIdx>, gc_allocator<std::tuple<std::string_view, Value *, PosIdx>>> sorted;
            v.forEachAttr([&](Symbol name, Value * value, PosIdx pos) {
                sorted.emplace_back(state.symbols[name], value, pos);
            });
            std::sort(sorted.begin(), sorted.end(), [](auto & a, auto & b) {
                return std::get<0>(a) < std::get<0>(b);
            });
            for (auto & [name, value, attrPos] : sorted) {
                try {
                    out.emplace(name, printValueAsJSON(state, strict, *value, attrPos, context, copyToStore));
                } catch (Error & e) {
                    e.addTrace(state.positions[attrPos], HintFmt("while evaluating attribute '%1%'", name));
                    throw;
                }
            }
        }
        break;
    }

    case nList: {
        out = json::array();
        int i = 0;
        for (auto elem : v.listView()) {
            try {
                out.push_back(printValueAsJSON(state, strict, *elem, pos, context, copyToStore));
            } catch (Error & e) {
                e.addTrace(state.positions[pos], HintFmt("while evaluating list element at index %1%", i));
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
    case nFunction:
        state.error<TypeError>("cannot convert %1% to JSON", showType(v)).atPos(v.determinePos(pos)).debugThrow();
    }
    return out;
}

void printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    std::ostream & str,
    NixStringContext & context,
    bool copyToStore)
{
    try {
        str << printValueAsJSON(state, strict, v, pos, context, copyToStore);
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
