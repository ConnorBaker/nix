#include "boundary-audit-fixture.hh"

#include <span>

namespace nix {

/**
 * Closes the inline-replay seam left by `boundary-audit-body-slicing.cc`
 * and `boundary-audit-replace-strings.cc` ("test the seam, not just the
 * part").
 *
 * Those two audit files assert that the body-slicing primops reject a
 * `SourceVirtual` placeholder, but they do so by *hand-copying* the
 * production throw into a local `runCheck` lambda
 * (`boundary-audit-body-slicing.cc:72-86`, etc.). The real primops
 * (`prim_baseNameOf`, `prim_dirOf`, `prim_substring`,
 * `prim_replaceStrings` in `src/libexpr/primops.cc`) are never invoked,
 * so DELETING any production `state.error<EvalError>(...).debugThrow()`
 * guard leaves those tests GREEN — the regression they claim to gate
 * silently reopens.
 *
 * This file calls the REAL primops via `state.getBuiltin(name)` +
 * `state.callFunction(...)`. When `callFunction` is handed a span with
 * the primop's full arity, it executes `fn->impl(...)` eagerly
 * (`src/libexpr/eval.cc:1749`), so the production guard's
 * `EvalError` propagates straight out of `callFunction` — no separate
 * force needed.
 *
 * NON-VACUITY: each `EXPECT_THROW` passes *only because* the production
 * primop still throws on `SourceVirtual` context. Deleting (or weakening
 * to a no-op) any of the four `prim_*` guards in `primops.cc` makes the
 * corresponding `callFunction` RETURN a sliced value instead of throwing
 * → the matching `EXPECT_THROW` fails. That is the exact mutation the
 * inline-replay versions cannot detect.
 *
 * Builtin-name note: `RegisterPrimOp` strips a leading `__` before the
 * primop is inserted into `builtins` (`src/libexpr/eval.cc:596-597`,
 * `607`), and `getBuiltin` looks the name up in that stripped table
 * (`eval.cc:618-625`). So the lookup keys are `"baseNameOf"`, `"dirOf"`,
 * `"substring"` (registered as `"__substring"`), and `"replaceStrings"`
 * (registered as `"__replaceStrings"`) — NOT the `__`-prefixed forms.
 */
class BoundaryAuditPropertyTest : public BoundaryAuditTest
{
protected:
    /**
     * Build a fresh placeholder-bearing string `Value` (nString, so it
     * hits `prim_dirOf`'s guarded string branch rather than the nPath
     * branch that skips the guard — see `primops.cc:2226`).
     */
    Value * makePlaceholderArg(const SourcePlaceholder & placeholder)
    {
        auto * v = state.allocValue();
        mkPlaceholderString(*v, placeholder, "audit-source");
        return v;
    }
};

TEST_F(BoundaryAuditPropertyTest, BaseNameOfRealPrimopThrows)
{
    auto placeholder = mintPlaceholder();
    auto * arg = makePlaceholderArg(placeholder);

    Value & fn = state.getBuiltin("baseNameOf");
    Value vRes;
    Value * args[] = {arg};

    /* Calls the REAL prim_baseNameOf (primops.cc:2172). Its
       coerceToString preserves the SourceVirtual context elem, then the
       guard loop throws EvalError. The inline replay in
       boundary-audit-body-slicing.cc never reaches this code. */
    EXPECT_THROW(state.callFunction(fn, std::span<Value *>(args), vRes, noPos), EvalError)
        << "real builtins.baseNameOf did not throw on SourceVirtual context — "
           "production guard (primops.cc:2186-2197) deleted/weakened, body-slice would leak placeholder";
}

TEST_F(BoundaryAuditPropertyTest, DirOfRealPrimopThrows)
{
    auto placeholder = mintPlaceholder();
    auto * arg = makePlaceholderArg(placeholder);

    Value & fn = state.getBuiltin("dirOf");
    Value vRes;
    Value * args[] = {arg};

    /* Calls the REAL prim_dirOf (primops.cc:2223). The arg is nString,
       so forceValue selects the guarded else-branch (the nPath branch
       at primops.cc:2226-2228 carries no SourceVirtual and would not
       throw). */
    EXPECT_THROW(state.callFunction(fn, std::span<Value *>(args), vRes, noPos), EvalError)
        << "real builtins.dirOf did not throw on SourceVirtual context — "
           "production guard (primops.cc:2243-2254) deleted/weakened, body-slice would leak placeholder";
}

TEST_F(BoundaryAuditPropertyTest, SubstringRealPrimopThrows)
{
    auto placeholder = mintPlaceholder();
    auto * argStr = makePlaceholderArg(placeholder);

    /* substring start len s — the guard is on the third (string) arg.
       start/len are plain ints and carry no context. len MUST be
       non-zero: prim_substring has a `len == 0` fast path
       (primops.cc:5070-5076) that returns the empty string BEFORE the
       guard runs. start=0, len=10 reaches the guard. */
    auto * argStart = state.allocValue();
    argStart->mkInt(0);
    auto * argLen = state.allocValue();
    argLen->mkInt(10);

    Value & fn = state.getBuiltin("substring");
    Value vRes;
    Value * args[] = {argStart, argLen, argStr};

    /* Calls the REAL prim_substring (primops.cc:5044). callFunction
       applies all three args at once, so the primop runs eagerly and
       the guard on args[2] (primops.cc:5096-5107) throws. */
    EXPECT_THROW(state.callFunction(fn, std::span<Value *>(args), vRes, noPos), EvalError)
        << "real builtins.substring did not throw on SourceVirtual context in the string arg — "
           "production guard (primops.cc:5096-5107) deleted/weakened, body-slice would leak placeholder";
}

TEST_F(BoundaryAuditPropertyTest, ReplaceStringsRealPrimopThrows)
{
    auto placeholder = mintPlaceholder();
    auto * argHaystack = makePlaceholderArg(placeholder);

    /* replaceStrings from to s — drive the haystack (third arg) entry
       point with empty `from`/`to` lists. With equal-length empty lists
       the from-loop is a no-op and the haystack guard
       (primops.cc:5598, via rejectIfSourceVirtual) fires immediately,
       independent of any match. (The `to`-list guard is only reached on
       a successful match, so it needs a contrived matching `from`; the
       haystack path is the robust single-call trigger and exercises the
       same production `rejectIfSourceVirtual` lambda.) */
    auto fromBuilder = state.buildList(0);
    auto * argFrom = state.allocValue();
    argFrom->mkList(fromBuilder);

    auto toBuilder = state.buildList(0);
    auto * argTo = state.allocValue();
    argTo->mkList(toBuilder);

    Value & fn = state.getBuiltin("replaceStrings");
    Value vRes;
    Value * args[] = {argFrom, argTo, argHaystack};

    /* Calls the REAL prim_replaceStrings (primops.cc:5544). The
       inline replay in boundary-audit-replace-strings.cc reimplements
       this rejection rather than invoking the primop. */
    EXPECT_THROW(state.callFunction(fn, std::span<Value *>(args), vRes, noPos), EvalError)
        << "real builtins.replaceStrings did not throw on SourceVirtual context in the haystack — "
           "production guard (primops.cc:5566-5580, 5598) deleted/weakened, would leak placeholder";
}

} // namespace nix
