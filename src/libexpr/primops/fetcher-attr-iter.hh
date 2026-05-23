#pragma once
///@file
///
/// Internal helper for fetcher primops that iterate `args[0]->attrs()` and
/// dispatch each attribute name to a handler, raising a uniform
/// "unsupported argument" error for unrecognised names.  Used by
/// `prim_fetchClosure`, `prim_fetchMercurial`, and the shared `fetch` helper
/// behind `prim_fetchurl`/`prim_fetchTarball`.
///
/// This header is not installed.  It exists to deduplicate the
/// `if (n == "x") ... else if (n == "y") ... else error` chain that those
/// primops independently re-implement.

#include "nix/expr/attr-set.hh"
#include "nix/expr/eval.hh"

#include <functional>
#include <string_view>

namespace nix {

/**
 * One entry in a fetcher primop's name-to-handler table.
 *
 * The handler is called with the matching `Attr` and is free to capture
 * per-fetcher state.  `std::function` is acceptable here because fetcher
 * primops are not on the evaluator's hot path: they invoke the network
 * stack and are dominated by I/O.
 */
struct FetcherAttrHandler
{
    std::string_view name;
    std::function<void(const Attr &)> handler;
};

/**
 * Iterate the attributes of an attrset argument passed to a fetcher primop
 * and dispatch each name to its handler from `handlers`.  Unrecognised
 * names raise an `EvalError` of the form "unsupported argument '<name>' to
 * '<primopName>'".
 *
 * The error position is taken from each unrecognised attribute's `attr.pos`
 * so the diagnostic points at the offending attribute, not at the fetcher
 * call as a whole.
 *
 * The handlers list is searched linearly, which is fine for the
 * fetcher-arity attribute counts (typically 3-5).
 *
 * Post-iteration validation (e.g. "url is required") is the caller's
 * responsibility and runs after this returns.
 */
template<class Handlers>
inline void iterateFetcherAttrs(
    EvalState & state, const Bindings & attrs, std::string_view primopName, const Handlers & handlers)
{
    for (auto & attr : attrs) {
        std::string_view n(state.symbols[attr.name]);
        bool matched = false;
        for (auto & entry : handlers) {
            if (entry.name == n) {
                entry.handler(attr);
                matched = true;
                break;
            }
        }
        if (!matched) {
            state.error<EvalError>("unsupported argument '%s' to '%s'", n, primopName).atPos(attr.pos).debugThrow();
        }
    }
}

} // namespace nix
