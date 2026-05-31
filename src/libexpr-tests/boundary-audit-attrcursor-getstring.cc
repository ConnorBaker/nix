#include "boundary-audit-fixture.hh"

#include "nix/expr/eval-cache.hh"

namespace nix {

/**
 * Bypass-site boundary: `eval_cache::AttrCursor::getString()`
 * (uncached path).
 *
 * `src/libexpr/eval-cache.cc:570-589` — `getString()` has a cached
 * branch (returns the persisted body) and an uncached fall-through:
 *
 *     auto & v = forceValue();
 *     ...
 *     return v.type() == nString ? std::string(v.string_view())
 *                                : v.path().to_string();
 *
 * `forceValue()` (eval-cache.cc:411-453) was patched in PROPOSAL.md
 * §6.4.3 to resolve `SourceVirtual` placeholders at write time so
 * the cache row is clean. But the fall-through return at line 588
 * uses `v.string_view()` directly on the in-memory `Value`, which
 * still carries the original placeholder render in its body and
 * SourceVirtual elements in its context. Callers reaching this
 * uncached path (no `db`, or first-evaluation row not yet
 * persisted) get the placeholder bytes verbatim.
 *
 * Bypass callers identified in the adversarial sweep:
 *
 *   - `src/nix/flake.cc:978`  — `welcomeText->getString()` →
 *     `notice(renderMarkdownToTerminal(...))` (terminal).
 *   - `src/nix/flake.cc:1283, 1289, 1416, 1418, 1435` — names and
 *     descriptions in `nix flake show`.
 *   - `src/nix/search.cc:110, 114` — name and description in
 *     `nix search`.
 *
 * None of these callers inspect context (that's why they invoke
 * the no-context overload), so no caller-side rewrite catches the
 * leak.
 *
 * Cover-fix shape (Design A): mirror the canonical
 * `resolveSourceVirtualContext` + `ensureLazyPathsCopied` +
 * `rewriteStrings` triple already used in `forceValue` (line 429)
 * and `prim_hashString` (`primops.cc:4941`). Resolve constructively
 * inside `getString()` rather than throwing — this preserves the
 * "the body" contract that no-context callers expect.
 *
 * **Test scaffolding.** We construct an `EvalCache` with
 * `std::nullopt` as the fingerprint so `db == nullptr` (constructor
 * `eval-cache.cc:302-308`). With no DB, `forceValue()` skips the
 * write-time resolution branch entirely, leaving the in-memory
 * `Value` carrying its original `SourceVirtual` context. The
 * subsequent `getString()` call lands directly on the uncached
 * fall-through — exactly the codepath under audit. We then check
 * the returned body for placeholder text.
 */
class BoundaryAuditAttrCursorGetStringTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditAttrCursorGetStringTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value rootValue;
    mkPlaceholderString(rootValue, placeholder, "audit-source");

    std::string body;
    try {
        /* `std::nullopt` ⇒ `db = nullptr` ⇒ no cache writes, no
           cache reads, no `forceValue` write-time resolution. The
           returned body comes purely from `getString()`'s
           uncached fall-through. */
        auto cache =
            std::make_shared<eval_cache::EvalCache>(std::nullopt, state, [&]() -> Value * { return &rootValue; });
        auto root = cache->getRoot();
        body = root->getString();
    } catch (Error & e) {
        recordThrow("AttrCursor::getString (uncached)", e.what());
        FAIL() << "AttrCursor::getString threw: " << e.what();
        return;
    }

    auto leak = detectLeak(body, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("AttrCursor::getString (uncached)", leak);
    } else {
        recordPass("AttrCursor::getString (uncached)");
    }

    EXPECT_FALSE(leak.any()) << "AttrCursor::getString returned placeholder text on the uncached path: `"
                             << leak.firstLeakedText << "` (body=`" << body << "`)";
}

} // namespace nix
