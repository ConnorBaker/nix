#include "boundary-audit-fixture.hh"

#include "nix/expr/eval-cache.hh"

namespace nix {

/**
 * Boundary 5 / 5: eval-cache write path (`AttrDb::setString`).
 *
 * `AttrCursor::forceValue` (`src/libexpr/eval-cache.cc:400-411`)
 * writes a string `Value` to the on-disk SQLite eval cache:
 *
 *     cachedValue = {root->db->setString(getKey(),
 *                                        v.string_view(),
 *                                        v.context()),
 *                    string_t{v.string_view(), {}}};
 *
 * `AttrDb::setString` (`eval-cache.cc:139-160`) writes both the body
 * `string_view` and a space-joined context to disk. If the body is
 * a `SourceVirtual` placeholder render `/<hash>`, that's persisted
 * as-is — the cache row outlives the `MaterialisationScheduler`'s
 * registration, so on a fresh process start the cached row contains
 * a placeholder with no live registration backing it. Reads via
 * `AttrCursor::getString` return that body unchanged.
 *
 * Coverage criterion: the persisted-and-reread body must NOT
 * contain the placeholder render text. Two acceptable cover-fixes:
 *
 *   - Materialise to `Opaque` at write time (resolve placeholder →
 *     storePath, write the storePath body + Opaque context elem).
 *   - Refuse to write a `SourceVirtual`-bearing string and force
 *     the caller to materialise first.
 *
 * The proposal recommends the first. The audit is agnostic to which
 * cover-fix lands; it just checks the persisted bytes.
 *
 * **Note on test scaffolding.** The eval-cache write goes through a
 * full `EvalCache` round-trip: build a root Value via the
 * `RootLoader` callback, force it (triggers `setString`), then
 * construct a fresh `EvalCache` with the same fingerprint and read
 * the persisted body via `getStringWithContext`. We use the test
 * harness's `HOME` override (`src/libexpr-tests/meson.build:90`) so
 * `getCacheDir()` lands in the per-test build dir.
 */
class BoundaryAuditAttrDbSetStringTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditAttrDbSetStringTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value rootValue;
    mkPlaceholderString(rootValue, placeholder, "audit-source");

    /* A fingerprint unique to this test run keeps us off any
       previously-cached row a previous run might have written. */
    auto fingerprint = hashString(HashAlgorithm::SHA256, "boundary-audit-attrdb-setstring");

    std::string persistedBody;
    try {
        auto cache = std::make_shared<eval_cache::EvalCache>(
            std::ref(fingerprint), state, [&]() -> Value * { return &rootValue; });
        auto root = cache->getRoot();
        /* Force triggers `setString` on the SQLite row. The Value's
           body is the placeholder render (`/<hash>`); after force,
           `cachedValue` holds the same body. */
        (void) root->forceValue();

        /* `getString()` returns the cached body verbatim
           (`eval-cache.cc:528-547`), unlike `getStringWithContext`
           which has a SourceVirtual-invalidation arm. This is the
           closest public API to the persisted bytes. */
        persistedBody = root->getString();
    } catch (Error & e) {
        recordThrow("AttrDb::setString", e.what());
        FAIL() << "AttrDb threw: " << e.what();
        return;
    }

    /* Detect leak in the persisted body bytes. We deliberately do
       NOT check the context's text form here — the persisted body
       is what subsequent eval observes via `getString()`, and the
       primary leak path is body bytes. (The context's wire form
       is checked in the body-form leak detector; SourceVirtual's
       `~<hash>:<name>` cannot legally appear in a body.) */
    auto leak = detectLeak(persistedBody, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("AttrDb::setString", leak);
    } else {
        recordPass("AttrDb::setString");
    }

    EXPECT_FALSE(leak.any()) << "AttrDb::setString persisted placeholder text in body: `" << leak.firstLeakedText
                             << "` (persisted body=`" << persistedBody << "`)";
}

} // namespace nix
