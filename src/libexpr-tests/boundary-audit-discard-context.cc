#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary: `builtins.unsafeDiscardStringContext`.
 *
 * This primop is the one boundary that *removes* string context, so it is the
 * one place the universal devirtualise sweep can't catch after the fact: every
 * other emit boundary (CLI output, `.drv` builder/args/env, eval-cache write)
 * rewrites a deferred `SourceVirtual` placeholder / `Opaque{stand-in}` to its
 * real store path by keying off the string context. If `unsafeDiscardStringContext`
 * discards the context WITHOUT first resolving, the fake `/<base32>` render
 * survives as a plain string and gets baked verbatim into a `.drv` / printed to
 * stdout — a dangling path that does not and cannot exist (reproduced under
 * `--impure` / `nix repl`, where an unlocked input is deferred).
 *
 * The fix (`prim_unsafeDiscardStringContext`, src/libexpr/primops/context.cc)
 * routes the body through `resolveAndRewrite(text, context)` BEFORE discarding —
 * so the returned string carries the real, materialised store path, then the
 * (now-irrelevant) context is dropped.
 *
 * NON-VACUITY: this drives the REAL primop via getBuiltin + callFunction. If the
 * resolve-before-discard step is removed (reverting to `v.mkString(*s, mem)`),
 * the result body is the placeholder render and `detectLeak` fires → the test
 * fails. So it pins the production fix, not an inline replay.
 */
class BoundaryAuditDiscardContextTest : public BoundaryAuditTest
{
};

TEST_F(BoundaryAuditDiscardContextTest, DiscardResolvesBeforeDroppingContext)
{
    auto placeholder = mintPlaceholder();
    auto * arg = state.allocValue();
    mkPlaceholderString(*arg, placeholder, "audit-source");

    Value & fn = state.getBuiltin("unsafeDiscardStringContext");
    Value vRes;
    Value * args[] = {arg};

    /* Unlike the body-slicing primops, this one must NOT throw — it must
       succeed and return the RESOLVED real path. */
    ASSERT_NO_THROW(state.callFunction(fn, std::span<Value *>(args), vRes, noPos))
        << "builtins.unsafeDiscardStringContext threw on a SourceVirtual-bearing string";

    /* The result is a context-free string (that's the primop's job). */
    ASSERT_EQ(vRes.type(), nString);
    std::string_view body = vRes.string_view();

    /* THE REGRESSION GUARD: the placeholder must have been resolved to a real
       store path BEFORE the context was discarded — so no placeholder text
       (neither the body `/<base32>` form nor the context wire form) survives. */
    auto leak = detectLeak(body, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.unsafeDiscardStringContext", leak);
    else
        recordPass("builtins.unsafeDiscardStringContext");
    EXPECT_FALSE(leak.any()) << "builtins.unsafeDiscardStringContext leaked an unresolved placeholder ("
                             << leak.firstLeakedText
                             << ") — resolve-before-discard removed/weakened; a dangling fake store path would be "
                                "baked into any .drv/output that discards this string's context";

    /* And the resolved body must be a real, valid store path (the materialised
       source), confirming the resolve actually produced something concrete. */
    EXPECT_TRUE(state.store->isStorePath(body))
        << "discarded-context result '" << body << "' is not a store path";
    EXPECT_TRUE(state.store->isValidPath(state.store->parseStorePath(body)))
        << "discarded-context result '" << body << "' is not a VALID store path (dangling)";
}

} // namespace nix
