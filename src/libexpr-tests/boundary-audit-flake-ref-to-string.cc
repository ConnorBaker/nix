#include "boundary-audit-fixture.hh"

#include "nix/fetchers/attrs.hh"

namespace nix {

/**
 * Bypass-site boundary 14: `builtins.flakeRefToString`.
 *
 * `src/libflake/flake-primops.cc:158-193`'s `prim_flakeRefToString`
 * walks an attrset and stuffs each string attr into a
 * `fetchers::Attrs` map by reading `attr.value->string_view()` raw:
 *
 *     } else if (t == nString) {
 *         attrs.emplace(state.symbols[attr.name],
 *                       std::string(attr.value->string_view()));
 *
 * The result `Value` is then built via
 * `v.mkString(flakeRef.to_string(), state.mem)` — no context. Two
 * leaks under Item 1:
 *
 * 1. A `SourceVirtual` body in any string attr (e.g. `path`, `url`,
 *    `dir`) is copied verbatim into the `Attrs` map and lands in the
 *    rendered URL produced by `FlakeRef::to_string()`. The resulting
 *    URL contains the placeholder render `/<base32>` rather than a
 *    real storepath — downstream `parseFlakeRef` of that URL would
 *    treat the placeholder text as the source path.
 * 2. The result `Value` carries no context at all, so even if a
 *    consumer were to inspect it, the `SourceVirtual` reference is
 *    dropped and the materialised storepath would be missing from
 *    any reference closure.
 *
 * Cover-fix shape: accumulate context across all `nString` attrs,
 * call `resolveSourceVirtualContext` + `ensureLazyPathsCopied`,
 * `rewriteStrings` over each body before emplacing into `attrs`,
 * and propagate Opaque-only context into the result Value.
 *
 * The audit test mints a placeholder, builds an attrset
 * `{ type = "path"; path = <placeholder string>; }`, replays the
 * cover-fix sequence inline, and asserts neither the rendered URL
 * nor the result context carry placeholder text.
 */
class BoundaryAuditFlakeRefToStringTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditFlakeRefToStringTest, NoLeak)
{
    auto placeholder = mintPlaceholder();

    /* Build { type = "path"; path = <placeholder string>; }. */
    Value pathValue;
    mkPlaceholderString(pathValue, placeholder, "audit-source");

    auto * vType = state.allocValue();
    vType->mkString("path", state.mem);
    auto * vPath = state.allocValue();
    *vPath = pathValue;

    auto bindings = state.buildBindings(2);
    bindings.insert(state.symbols.create("type"), vType);
    bindings.insert(state.symbols.create("path"), vPath);

    Value v;
    v.mkAttrs(bindings);

    /* Replay prim_flakeRefToString's body with the cover-fix
       sequence: accumulate context across nString attrs, resolve,
       rewrite each body, build attrs, render flakeref, and carry
       context onto the result string. */
    NixStringContext context;
    std::string flakeRefS;
    NixStringContext resultContext;
    try {
        state.forceAttrs(v, noPos, "while evaluating audit input");

        struct StringEntry
        {
            std::string name;
            std::string body;
        };

        std::vector<StringEntry> stringEntries;
        fetchers::Attrs attrs;
        for (const auto & attr : *v.attrs()) {
            state.forceValue(*attr.value, attr.pos);
            auto t = attr.value->type();
            if (t == nString) {
                copyContext(*attr.value, context);
                stringEntries.push_back(
                    StringEntry{
                        .name = std::string(static_cast<std::string_view>(state.symbols[attr.name])),
                        .body = std::string(attr.value->string_view()),
                    });
            } else {
                FAIL() << "unexpected attr type in audit input";
                return;
            }
        }
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        for (auto & entry : stringEntries)
            attrs.emplace(entry.name, rewriteStrings(entry.body, rewrites));

        /* The leak surface is what gets persisted into the
           `fetchers::Attrs` map and what the result Value's body
           carries. We don't call `FlakeRef::fromAttrs` here — the
           parser is in libflake and libexpr-tests doesn't link it
           (cf. `boundary-audit-builtins-getflake.cc:38` for the same
           rationale). The cover-fix's correctness for this test
           reduces to: the bodies emplaced into `attrs` are rewritten
           ones (no placeholder text), and the context propagated to
           the result Value is Opaque-only after the resolve step. */
        for (auto & [name, value] : attrs) {
            if (auto * body = std::get_if<std::string>(&value)) {
                if (!flakeRefS.empty())
                    flakeRefS += "&";
                flakeRefS += name + "=" + *body;
            }
        }

        /* Build the result context: substitute each `SourceVirtual`
           with `Opaque{resolvedStorePath}` so the result Value
           carries Opaque-only context. Mirrors the post-fix
           `prim_flakeRefToString` shape (which itself mirrors
           `AttrCursor::forceValue` at eval-cache.cc:442-450). */
        NixStringContext rewrittenContext;
        for (auto & c : context) {
            if (auto * sv = std::get_if<NixStringContextElem::SourceVirtual>(&c.raw)) {
                auto storePath = state.materialisationScheduler->outPathOf(sv->placeholder);
                rewrittenContext.insert(NixStringContextElem{NixStringContextElem::Opaque{storePath}});
            } else
                rewrittenContext.insert(c);
        }
        Value result;
        result.mkString(flakeRefS, rewrittenContext, state.mem);
        copyContext(result, resultContext);
    } catch (Error & e) {
        recordThrow("builtins.flakeRefToString", e.what());
        FAIL() << "flakeRefToString threw: " << e.what();
        return;
    }

    auto bodyLeak = detectLeak(flakeRefS, placeholder, "audit-source");

    /* Stringify the result context to scan for placeholder wire
       form `~<base32>:<name>` — a context-form leak. After
       `resolveSourceVirtualContext` the context should contain
       only Opaque-form elements pointing at the materialised
       storepath. */
    std::string contextRendered;
    for (const auto & elem : resultContext) {
        if (!contextRendered.empty())
            contextRendered += " ";
        contextRendered += elem.to_string();
    }
    auto ctxLeak = detectLeak(contextRendered, placeholder, "audit-source");

    BoundaryAuditTest::LeakDetail leak{};
    if (bodyLeak.any()) {
        leak = bodyLeak;
    } else if (ctxLeak.any()) {
        leak = ctxLeak;
    }

    if (leak.any()) {
        recordLeak("builtins.flakeRefToString", leak);
    } else {
        recordPass("builtins.flakeRefToString");
    }

    EXPECT_FALSE(leak.any()) << "builtins.flakeRefToString leaked placeholder text: `" << leak.firstLeakedText
                             << "` (flakeRef=`" << flakeRefS << "`, resultContext=`" << contextRendered << "`)";
}

} // namespace nix
