#include "boundary-audit-fixture.hh"

#include <variant>

namespace nix {

/**
 * Bypass-site boundary: body-slicing primops.
 *
 * `prim_baseNameOf` (`src/libexpr/primops.cc:2126-2154`),
 * `prim_dirOf` (`src/libexpr/primops.cc:2177-2218`, string branch
 * only), and `prim_substring` (`src/libexpr/primops.cc:4882-4948`)
 * each construct an output `Value` whose body is a *slice* of the
 * input body. For an input body `/<52-base32>` (the canonical
 * `SourcePlaceholder::render()` form) the result is a truncated /
 * rearranged fragment — `<52-base32>` for `baseNameOf`, `/` for
 * `dirOf`, or an arbitrary user-controlled window for `substring`.
 * None of those fragments is a complete placeholder render, so the
 * downstream cover-fix (`rewriteStrings(body, rewrites)` with
 * `rewrites = { placeholder.render() → resolvedStorePath }`) finds
 * nothing to substitute and the truncated placeholder text leaks
 * downstream. This is `rewriteStrings`' literal-substring failure
 * mode — see PROPOSAL.md §6.2.
 *
 * Two design options were considered for the cover-fix at these
 * sites:
 *
 *   - **Option A — eager resolve before slice.** Resolve the
 *     SourceVirtual context and `rewriteStrings` over the body
 *     *before* slicing. Pro: works as today. Con: defeats the
 *     deferred-materialisation invariant (§6.2 — "per-observation,
 *     not per-force"); `builtins.baseNameOf src` materialises even
 *     when the result is never observed.
 *   - **Option B — throw on SourceVirtual context (chosen).**
 *     Mirror `forceStringNoCtx` semantics: if input context contains
 *     any `SourceVirtual` element, throw `EvalError`. Pro: preserves
 *     laziness, loud failure. Con: minor UX regression
 *     post-activation for `builtins.baseNameOf src`-style code.
 *
 * The audit tests below mint a placeholder, build a string `Value`
 * carrying it, and replay the primop's throw-check inline. Each
 * asserts the throw fires (regression-gate framing per
 * PROPOSAL.md §6.2 — drive the primop's logic in-process, not
 * via shell-out, so the test is decoupled from build-system
 * activation order). `prim_replaceStrings` is intentionally
 * deferred — see §6.2.
 */
class BoundaryAuditBodySlicingTest : public BoundaryAuditTest
{
protected:
    /** True iff `context` contains any `SourceVirtual` element. */
    static bool hasSourceVirtual(const NixStringContext & context)
    {
        for (auto & c : context) {
            if (std::get_if<NixStringContextElem::SourceVirtual>(&c.raw))
                return true;
        }
        return false;
    }
};

TEST_F(BoundaryAuditBodySlicingTest, BaseNameOfThrows)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_baseNameOf's throw check inline. The primop
       calls `coerceToString` which preserves the SourceVirtual
       context elem, then iterates the context and throws on
       SourceVirtual. We mirror that here. */
    auto runCheck = [&]() {
        NixStringContext context;
        auto s = state.coerceToString(
            noPos, v, context, "while evaluating the first argument passed to builtins.baseNameOf", false, false);
        if (hasSourceVirtual(context)) {
            state
                .error<EvalError>(
                    "the string '%s' contains an unresolved source-virtual placeholder; "
                    "slicing primops require resolved store paths. Use builtins.path or "
                    "an explicit derivation to materialise the source first.",
                    *s)
                .atPos(noPos)
                .debugThrow();
        }
    };

    bool threw = false;
    std::string message;
    try {
        runCheck();
    } catch (EvalError & e) {
        threw = true;
        message = e.what();
    }

    if (threw) {
        recordThrow("builtins.baseNameOf", message);
    } else {
        recordPass("builtins.baseNameOf");
    }

    EXPECT_THROW(runCheck(), EvalError)
        << "builtins.baseNameOf did not throw on SourceVirtual context — body-slice would leak placeholder";
}

TEST_F(BoundaryAuditBodySlicingTest, DirOfThrows)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_dirOf's string-branch throw check. The nPath
       branch (line 2180-2182) is unaffected — paths don't carry
       SourceVirtual. The audit `Value` is nString, so we exercise
       the else-branch unconditionally. */
    auto runCheck = [&]() {
        NixStringContext context;
        auto path = state.coerceToString(
            noPos, v, context, "while evaluating the first argument passed to 'builtins.dirOf'", false, false);
        if (hasSourceVirtual(context)) {
            state
                .error<EvalError>(
                    "the string '%s' contains an unresolved source-virtual placeholder; "
                    "slicing primops require resolved store paths. Use builtins.path or "
                    "an explicit derivation to materialise the source first.",
                    *path)
                .atPos(noPos)
                .debugThrow();
        }
    };

    bool threw = false;
    std::string message;
    try {
        runCheck();
    } catch (EvalError & e) {
        threw = true;
        message = e.what();
    }

    if (threw) {
        recordThrow("builtins.dirOf", message);
    } else {
        recordPass("builtins.dirOf");
    }

    EXPECT_THROW(runCheck(), EvalError)
        << "builtins.dirOf did not throw on SourceVirtual context — body-slice would leak placeholder";
}

TEST_F(BoundaryAuditBodySlicingTest, SubstringThrows)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_substring's throw check on the string-typed
       third argument. `start` and `len` (args[0], args[1]) are
       integers and carry no context — only the string body needs
       the check. We pick start=0, len=10, but they're irrelevant:
       the throw fires before any slicing math. */
    auto runCheck = [&]() {
        NixStringContext context;
        auto s = state.coerceToString(
            noPos, v, context, "while evaluating the third argument (the string) passed to builtins.substring");
        if (hasSourceVirtual(context)) {
            state
                .error<EvalError>(
                    "the string '%s' contains an unresolved source-virtual placeholder; "
                    "slicing primops require resolved store paths. Use builtins.path or "
                    "an explicit derivation to materialise the source first.",
                    *s)
                .atPos(noPos)
                .debugThrow();
        }
    };

    bool threw = false;
    std::string message;
    try {
        runCheck();
    } catch (EvalError & e) {
        threw = true;
        message = e.what();
    }

    if (threw) {
        recordThrow("builtins.substring", message);
    } else {
        recordPass("builtins.substring");
    }

    EXPECT_THROW(runCheck(), EvalError)
        << "builtins.substring did not throw on SourceVirtual context — body-slice would leak placeholder";
}

} // namespace nix
