#pragma once
///@file
///
/// Internal helper for primops that iterate `args[0]->attrs()` and
/// dispatch each attribute name to a handler, raising a uniform
/// "unsupported argument" error for unrecognised names.  Used by
/// `prim_fetchClosure`, `prim_fetchMercurial`, the shared `fetch`
/// helper behind `prim_fetchurl`/`prim_fetchTarball`, and `prim_path`.
///
/// This header is not installed.  It exists to deduplicate the
/// `if (n == "x") ... else if (n == "y") ... else error` chain that
/// those primops independently re-implemented.

#include "nix/expr/attr-set.hh"
#include "nix/expr/eval.hh"

#include <functional>
#include <string_view>

namespace nix {

/**
 * One entry in a name-to-handler table.
 *
 * The handler is called with the matching `Attr` and is free to capture
 * per-primop state.  `std::function` is acceptable here because every
 * caller is dominated by I/O (fetcher primops) or by store I/O
 * (`prim_path`'s `addPath` step), so the type-erasure cost is well
 * within the noise of the surrounding work.
 */
struct NamedAttrHandler
{
    std::string_view name;
    std::function<void(const Attr &)> handler;
};

/**
 * Iterate the attributes of an attrset argument passed to a primop and
 * dispatch each name to its handler from `handlers`.  Unrecognised
 * names raise an `EvalError` of the form "unsupported argument
 * '<name>' to '<primopName>'".
 *
 * The error position is taken from each unrecognised attribute's
 * `attr.pos` so the diagnostic points at the offending attribute, not
 * at the primop call as a whole.
 *
 * The handlers list is searched linearly, which is fine for the
 * attribute counts these primops accept (typically 3-6).
 *
 * Post-iteration validation (e.g. "url is required") is the caller's
 * responsibility and runs after this returns.
 */
template<class Handlers>
inline void iterateNamedAttrs(
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
