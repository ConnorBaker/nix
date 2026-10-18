#include "nix/expr/value-to-json.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/store/store-api.hh"
#include "nix/util/signals.hh"

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace nix {
using json = nlohmann::json;

/**
 * Automatic parallel pre-forcing for strict whole-structure JSON
 * serialization (ported from DetSys's `value-to-json.cc`).
 *
 * This force-evaluates `v` and then fans out per-child deep-force work
 * onto the evaluator's `Executor`, so that the (serial) serialization
 * pass in `printValueAsJSONRecurse` finds values already in WHNF (or
 * being computed concurrently, in which case it blocks on the normal
 * forcing protocol) instead of forcing them one at a time.
 *
 * Only the *forcing* is parallelized; serialization stays strictly
 * serial, which is what keeps the `__toString`/`outPath` peeling, the
 * attribute ordering, the error traces and the output bytes identical
 * to the serial path. Work is fire-and-forget (no `Completion`): any
 * exception thrown while force-evaluating here is swallowed, and the
 * serial pass re-forces the value and surfaces the real diagnostic.
 *
 * Fan-out stops at derivations / `__toString`-coercible attrsets (leaf
 * nodes), matching DetSys and the serial peel in
 * `printValueAsJSONRecurse`: the serial pass peels through `outPath` /
 * `__toString` rather than descending into every attribute of a
 * derivation, so the set of values force-evaluated here is a subset of
 * those the serial pass forces. (Lists are forced shallowly only; like
 * DetSys we do not fan out list elements -- the serial pass forces
 * them.) The recursion is expressed via spawned work items rather than
 * C++ recursion, so each call's own stack use is shallow.
 *
 * Runs only off a worker thread (`!amWorkerThread`); nested
 * `parallelForceDeep` invocations (which execute *on* worker threads)
 * keep spawning child work directly.
 */
static void parallelForceDeep(EvalState & state, Value & v, PosIdx pos)
{
    state.forceValue(v, pos);

    Executor::WorkItems work;

    /* Only attrsets fan out; everything else (including lists, like
       DetSys) is left to the serial pass. We deliberately avoid a
       `switch` on `v.type()` here so we don't have to enumerate every
       `ValueType` (`-Werror=switch-enum`). */
    if (v.type() == nAttrs
        /* Leaf: `__toString`-coercible attrset. The serial pass
           peels/coerces it rather than recursing into its attributes. */
        && !v.attrs()->get(state.s.toString)
        /* Leaf: derivation-like attrset carrying `outPath`. The serial
           pass peels to `outPath` and never forces the siblings. */
        && !v.attrs()->get(state.s.outPath)) {
        for (auto & a : *v.attrs())
            work.emplace_back(
                [rv(allocRootValue(a.value)), pos(a.pos), &state]() { parallelForceDeep(state, **rv, pos); },
                uint8_t(0));
    }

    state.executor->spawn(std::move(work));
}

// TODO: rename. It doesn't print.
static json printValueAsJSONRecurse(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    NixStringContext & context,
    bool copyToStore,
    bool unforce)
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
        out = state.peelToStringOutPath(
            pos, v, /*checkToStringReturn=*/true, [&](Value * peeled, bool cameThroughToString) -> json {
                if (peeled->type() != nAttrs) {
                    // Historical quirk preserved here for reproducibility:
                    // In some coercions, Nix would coerce paths to a raw string
                    // if they came from a __toString result.
                    auto copyToStore2 = copyToStore && !cameThroughToString;
                    return printValueAsJSONRecurse(state, strict, *peeled, pos, context, copyToStore2, unforce);
                }
                // Peelable attrs handled. Returned attrs are not peelable.
                // Quirk: builtins.toJSON { outPath.foo = true; } == "{\"foo\":true}"
                // All that remains is to return a JSON object.
                json obj = json::object();
                UnforceBatch batch(state, unforce);
                for (auto & a : peeled->attrs()->lexicographicOrder(state.symbols)) {
                    try {
                        UnforceBatch::Element element(batch);
                        obj.emplace(
                            state.symbols[a->name],
                            printValueAsJSONRecurse(state, strict, *a->value, a->pos, context, copyToStore, unforce));
                    } catch (Error & e) {
                        e.addTrace(
                            state.positions[a->pos],
                            HintFmt("while evaluating attribute '%1%'", state.symbols[a->name]));
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
        UnforceBatch batch(state, unforce);
        for (auto elem : v.listView()) {
            try {
                UnforceBatch::Element element(batch);
                out.push_back(printValueAsJSONRecurse(state, strict, *elem, pos, context, copyToStore, unforce));
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
    case nFailed:
    case nFunction:
        state.error<TypeError>("cannot convert %1% to JSON", showType(v)).atPos(v.determinePos(pos)).debugThrow();
    }
    return out;
}

json printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    NixStringContext & context,
    bool copyToStore,
    bool unforce)
{
    /* Auto-parallel pre-force of the whole structure before the serial
       serialization pass. Guarded on strictness (lazy serialization
       must not force), the executor being enabled (eval-cores > 1 under
       the parallel-eval feature), and not already running on a worker
       thread. Byte-identity is preserved because only forcing is
       parallelized; serialization below remains serial. */
    if (strict && state.executor->enabled && !Executor::amWorkerThread)
        parallelForceDeep(state, v, pos);

    return printValueAsJSONRecurse(state, strict, v, pos, context, copyToStore, unforce);
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
    bool unforce)
{
    try {
        str << printValueAsJSON(state, strict, v, pos, context, copyToStore, unforce);
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
