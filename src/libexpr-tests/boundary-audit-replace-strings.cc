#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary: `builtins.replaceStrings` (§6.2).
 *
 * `prim_replaceStrings` (`src/libexpr/primops.cc:5382-5462`) is the
 * structurally-hardest body-slicing primop:
 *
 *   - `from[i]` bodies are used as **literal search keys** against
 *     the haystack body. A SourceVirtual placeholder render appearing
 *     as `from[i]` would search the haystack for the placeholder
 *     literal; the cargo-workspace pattern (multiple SourceVirtual
 *     elems sharing a render across distinct contentIds-or-names)
 *     compounds the confusion: which reference does the match mean?
 *   - `to[j]` bodies are spliced into the result wherever the
 *     corresponding `from[i]` matches. Splicing a placeholder render
 *     into the result would defeat downstream `rewriteStrings` for
 *     the same reason as baseNameOf/dirOf/substring (§6.2) —
 *     the spliced fragment may be partial, mid-match, or duplicated.
 *   - `s` (the haystack, third arg) carries its own SourceVirtual
 *     context that we'd need to resolve before searching, lest the
 *     match window straddle a placeholder boundary.
 *
 * Per §6.2's deferral rationale, the post-fix uses Option-B:
 * throw EvalError if any of the three inputs carries SourceVirtual
 * context. The error message points users at `builtins.path` or an
 * explicit derivation. Reversible by a future Phase-3 typestate
 * (`BackedStringView<State>`) that distinguishes context-resolved
 * from raw string views.
 *
 * The audit tests below mint a placeholder, drive each of the three
 * SourceVirtual entry points (from list, to list, haystack), and
 * assert the throw fires.
 */
class BoundaryAuditReplaceStringsTest : public BoundaryAuditTest
{
protected:
    static bool hasSourceVirtual(const NixStringContext & context)
    {
        for (auto & c : context) {
            if (std::get_if<NixStringContextElem::SourceVirtual>(&c.raw))
                return true;
        }
        return false;
    }
};

TEST_F(BoundaryAuditReplaceStringsTest, FromListThrows)
{
    auto placeholder = mintPlaceholder();
    Value vNeedle;
    mkPlaceholderString(vNeedle, placeholder, "audit-source");

    /* Replay the rejection check that `prim_replaceStrings` performs
       on each `from[i]`: forceString into a per-element context, scan
       for SourceVirtual, throw on match. */
    auto runCheck = [&]() {
        NixStringContext fromCtx;
        auto body = state.forceString(vNeedle, fromCtx, noPos, "while evaluating from-list audit value");
        if (hasSourceVirtual(fromCtx)) {
            state
                .error<EvalError>(
                    "the string '%s' (passed as %s) contains an unresolved source-virtual placeholder; "
                    "builtins.replaceStrings requires resolved store paths in all of its arguments. "
                    "Use builtins.path or an explicit derivation to materialise the source first.",
                    body,
                    "an element of the 'from' list")
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
        recordThrow("builtins.replaceStrings (from)", message);
    } else {
        recordPass("builtins.replaceStrings (from)");
    }

    EXPECT_THROW(runCheck(), EvalError) << "builtins.replaceStrings did not throw on SourceVirtual in 'from' list";
}

TEST_F(BoundaryAuditReplaceStringsTest, ToListThrows)
{
    auto placeholder = mintPlaceholder();
    Value vRepl;
    mkPlaceholderString(vRepl, placeholder, "audit-source");

    auto runCheck = [&]() {
        NixStringContext toCtx;
        auto body = state.forceString(vRepl, toCtx, noPos, "while evaluating to-list audit value");
        if (hasSourceVirtual(toCtx)) {
            state
                .error<EvalError>(
                    "the string '%s' (passed as %s) contains an unresolved source-virtual placeholder; "
                    "builtins.replaceStrings requires resolved store paths in all of its arguments. "
                    "Use builtins.path or an explicit derivation to materialise the source first.",
                    body,
                    "an element of the 'to' list")
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
        recordThrow("builtins.replaceStrings (to)", message);
    } else {
        recordPass("builtins.replaceStrings (to)");
    }

    EXPECT_THROW(runCheck(), EvalError) << "builtins.replaceStrings did not throw on SourceVirtual in 'to' list";
}

TEST_F(BoundaryAuditReplaceStringsTest, HaystackThrows)
{
    auto placeholder = mintPlaceholder();
    Value vHaystack;
    mkPlaceholderString(vHaystack, placeholder, "audit-source");

    auto runCheck = [&]() {
        NixStringContext context;
        auto body = state.forceString(vHaystack, context, noPos, "while evaluating haystack audit value");
        if (hasSourceVirtual(context)) {
            state
                .error<EvalError>(
                    "the string '%s' (passed as %s) contains an unresolved source-virtual placeholder; "
                    "builtins.replaceStrings requires resolved store paths in all of its arguments. "
                    "Use builtins.path or an explicit derivation to materialise the source first.",
                    body,
                    "the third argument (the haystack)")
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
        recordThrow("builtins.replaceStrings (haystack)", message);
    } else {
        recordPass("builtins.replaceStrings (haystack)");
    }

    EXPECT_THROW(runCheck(), EvalError) << "builtins.replaceStrings did not throw on SourceVirtual in haystack";
}

} // namespace nix
