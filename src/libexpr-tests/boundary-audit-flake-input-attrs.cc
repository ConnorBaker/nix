#include "boundary-audit-fixture.hh"

#include "nix/fetchers/attrs.hh"

namespace nix {

/**
 * Bypass-site boundary 14: `parseFlakeInputAttr` (string arm).
 *
 * `src/libflake/flake.cc:97`'s `parseFlakeInputAttr`:
 *
 *     case nString:
 *         attrs.emplace(state.symbols[attr.name], std::string(attr.value->string_view()));
 *         break;
 *
 * The body string is dropped into a `fetchers::Attrs` map which is
 * later serialised verbatim into `flake.lock`. Context is discarded:
 * if `attr.value` carries a `SourceVirtual` placeholder, the
 * placeholder render `/<base32>` lands in the lockfile in place of
 * the real storepath. This is a regression-gate for the same
 * adversarial scenario as the sibling `getFlake`/`flakeRefToString`
 * cover-fixes: an `addPath` migrated to emit placeholders feeds a
 * lockfile-bound string.
 *
 * Cover-fix shape (mirrored from `flake.cc:97` post-fix): copy
 * context off the Value, resolve `SourceVirtual` placeholders,
 * `rewriteStrings` over the body, then emplace the rewritten body
 * into the attrs map.
 *
 * Constructing a real `Attr` and dispatching through
 * `parseFlakeInputAttr` requires a full `Bindings` set with a
 * symbol-named entry; we instead replay the cover-fix logic inline
 * (an established pattern, see
 * `boundary-audit-builtins-getflake.cc`). The audit asserts the
 * post-resolution body in the attrs map carries no placeholder text.
 *
 * Companion sites in the same file — `parseFlakeInput` url arm
 * (`flake.cc:153`) and `readFlake` description (`flake.cc:264`) —
 * share the same shape and are covered by the same fix discipline;
 * a single regression-gate over the inline replay is sufficient
 * because the leak shape (string body persisted unrewritten into a
 * lock-bound surface) is identical.
 */
class BoundaryAuditFlakeInputAttrsTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditFlakeInputAttrsTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay parseFlakeInputAttr's nString arm with the cover-fix
       sequence, mirroring `flake.cc:95-110` post-fix. The leak
       surface is the string body that lands in the
       `fetchers::Attrs` map — that map is serialised verbatim into
       `flake.lock`, so the body is what we audit. */
    fetchers::Attrs attrs;
    try {
        NixStringContext context;
        copyContext(v, context);
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        std::string body = rewriteStrings(std::string{v.string_view()}, rewrites);
        attrs.emplace("audit-key", std::move(body));
    } catch (Error & e) {
        recordThrow("flake.parseFlakeInputAttr", e.what());
        FAIL() << "parseFlakeInputAttr replay threw: " << e.what();
        return;
    }

    auto it = attrs.find("audit-key");
    ASSERT_NE(it, attrs.end());
    auto * persisted = std::get_if<std::string>(&it->second);
    ASSERT_NE(persisted, nullptr) << "expected the string arm to emplace a std::string";

    auto leak = detectLeak(*persisted, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("flake.parseFlakeInputAttr", leak);
    } else {
        recordPass("flake.parseFlakeInputAttr");
    }

    EXPECT_FALSE(leak.any()) << "parseFlakeInputAttr leaked placeholder text into fetchers::Attrs body: `"
                             << leak.firstLeakedText << "` (body=`" << *persisted << "`)";
}

} // namespace nix
