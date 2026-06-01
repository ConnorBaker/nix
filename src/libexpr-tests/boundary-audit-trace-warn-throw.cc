#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary: diagnostic-embedding primops (the
 * diagnostic-embedding boundary family, PROPOSAL.md §6.2).
 *
 * `prim_trace`, `prim_warn`, `prim_abort`, `prim_throw`, and
 * `prim_addErrorContext` (`src/libexpr/primops.cc`) all embed a
 * string body into stderr/error-trace output. Pre-fix they used
 * `string_view()` / 3-arg `forceString` / `coerceToString` without
 * resolving SourceVirtual context, so a placeholder render would
 * appear verbatim in user-visible diagnostics.
 *
 * Their original framing classified these as cosmetic — they
 * don't persist into derivation hashes or lockfiles. The
 * adversarial-second-pass mitigation switched all five to the
 * canonical resolve-then-rewrite pattern (`copyContext` /
 * 4-arg-`forceString` → `resolveSourceVirtualContext` →
 * `rewriteStrings`) so users see real storepath text in their
 * traces.
 *
 * The audit replays the fix shape against a placeholder-bearing
 * string and asserts no placeholder text leaks into the output.
 * One TEST_F per primop family.
 */
class BoundaryAuditTraceWarnThrowTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditTraceWarnThrowTest, TraceNoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_trace's resolve-and-format sequence. The trace
       output is normally written to stderr via printError; here we
       capture the formatted body that *would* be written. */
    NixStringContext context;
    copyContext(v, context);
    auto rewrites = state.resolveSourceVirtualContext(context);
    state.ensureLazyPathsCopied(context);
    std::string body =
        rewrites.empty() ? std::string{v.string_view()} : rewriteStrings(std::string{v.string_view()}, rewrites);

    auto leak = detectLeak(body, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.trace", leak);
    else
        recordPass("builtins.trace");

    EXPECT_FALSE(leak.any()) << "builtins.trace leaked placeholder text into trace body: `" << leak.firstLeakedText
                             << "` (body=`" << body << "`)";
}

TEST_F(BoundaryAuditTraceWarnThrowTest, WarnNoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_warn's 4-arg forceString + resolve sequence. */
    NixStringContext context;
    auto msgStr = state.forceString(v, context, noPos, "while evaluating audit warn message");
    auto rewrites = state.resolveSourceVirtualContext(context);
    state.ensureLazyPathsCopied(context);
    std::string msg = rewrites.empty() ? std::string{msgStr} : rewriteStrings(std::string{msgStr}, rewrites);

    auto leak = detectLeak(msg, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.warn", leak);
    else
        recordPass("builtins.warn");

    EXPECT_FALSE(leak.any()) << "builtins.warn leaked placeholder text into warn body: `" << leak.firstLeakedText
                             << "` (body=`" << msg << "`)";
}

TEST_F(BoundaryAuditTraceWarnThrowTest, AbortNoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_abort's coerceToString + resolve sequence. The
       resolved body is what gets embedded in the Abort error
       message via `state.error<Abort>("...'%1%'", s)`; we audit the
       resolved body directly. */
    NixStringContext context;
    auto s = state.coerceToString(noPos, v, context, "while evaluating audit abort message").toOwned();
    auto rewrites = state.resolveSourceVirtualContext(context);
    state.ensureLazyPathsCopied(context);
    if (!rewrites.empty())
        s = rewriteStrings(s, rewrites);

    auto leak = detectLeak(s, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.abort", leak);
    else
        recordPass("builtins.abort");

    EXPECT_FALSE(leak.any()) << "builtins.abort leaked placeholder text into error message: `" << leak.firstLeakedText
                             << "` (body=`" << s << "`)";
}

TEST_F(BoundaryAuditTraceWarnThrowTest, ThrowNoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Same shape as Abort — only the EvalError subclass differs. */
    NixStringContext context;
    auto s = state.coerceToString(noPos, v, context, "while evaluating audit throw message").toOwned();
    auto rewrites = state.resolveSourceVirtualContext(context);
    state.ensureLazyPathsCopied(context);
    if (!rewrites.empty())
        s = rewriteStrings(s, rewrites);

    auto leak = detectLeak(s, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.throw", leak);
    else
        recordPass("builtins.throw");

    EXPECT_FALSE(leak.any()) << "builtins.throw leaked placeholder text into error message: `" << leak.firstLeakedText
                             << "` (body=`" << s << "`)";
}

TEST_F(BoundaryAuditTraceWarnThrowTest, AddErrorContextNoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_addErrorContext's coerceToString + resolve
       sequence. The resolved message is fed into HintFmt and added
       as a TracePrint::Always frame; we audit the message body. */
    NixStringContext context;
    auto message =
        state.coerceToString(noPos, v, context, "while evaluating audit error-context message", false, false).toOwned();
    auto rewrites = state.resolveSourceVirtualContext(context);
    state.ensureLazyPathsCopied(context);
    if (!rewrites.empty())
        message = rewriteStrings(message, rewrites);

    auto leak = detectLeak(message, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.addErrorContext", leak);
    else
        recordPass("builtins.addErrorContext");

    EXPECT_FALSE(leak.any()) << "builtins.addErrorContext leaked placeholder text into trace frame: `"
                             << leak.firstLeakedText << "` (body=`" << message << "`)";
}

} // namespace nix
