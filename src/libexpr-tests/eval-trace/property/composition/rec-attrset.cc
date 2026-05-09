// Pattern 7: rec Attrset with File Dep property test.
//
// Expression shape:
//   (rec { val = builtins.readFile <file>;
//          name = "pkg-${val}";
//          qualified = "${name}-release"; }).qualified
//
// This models a package derivation rec { ... } with two levels of
// self-reference: qualified → name → val → file.  The single file dep
// must propagate through both interpolation layers for the cache to
// work correctly.
//
// Two tests are provided:
//
// FileChange_Invalidates (RapidCheck): mutate the file → cache miss.
// ValueConsistency (deterministic): check the exact computed string value
//   for concrete inputs "1.0" and "2.0".

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ───────────────────────────────────────────────────────────────

class EvalTraceProperty_RecAttrset : public TraceCacheFixture {
public:
    EvalTraceProperty_RecAttrset() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-rec-attrset");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

// ── FileChange_Invalidates ────────────────────────────────────────────────
//
// Cold eval, warm hit, mutate the file, invalidateFileCache, warm eval → miss.
TEST_F(EvalTraceProperty_RecAttrset, FileChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeRecAttrsetGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm cache hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Mutate the file to new printable ASCII content.
            auto mutGen = slot.generateMutation();
            auto newValue = *mutGen;
            RC_PRE(newValue != slot.currentValue);
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval — must miss because the file content changed.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 1);
            }

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ── ValueConsistency ──────────────────────────────────────────────────────
//
// Deterministic test: verify the exact computed string through both levels of
// self-reference for concrete file values.
//
//   file="1.0" → val="1.0" → name="pkg-1.0" → qualified="pkg-1.0-release"
//   file="2.0" → val="2.0" → name="pkg-2.0" → qualified="pkg-2.0-release"
TEST_F(EvalTraceProperty_RecAttrset, ValueConsistency)
{
    // Create a temp file with initial content "1.0".
    TempTextFile file("1.0");

    std::string nixCode =
        "(rec { val = builtins.readFile " + file.path.string() + ";"
        " name = \"pkg-${val}\";"
        " qualified = \"${name}-release\"; }).qualified";

    // Cold eval — records trace, result should be "pkg-1.0-release".
    {
        auto cache = makeCache(nixCode);
        Value v = forceRoot(*cache);
        ASSERT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "pkg-1.0-release");
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Mutate file to "2.0" and invalidate.
    file.modify("2.0");
    invalidateFileCache(file.path);

    // Warm eval — must re-evaluate; result should now be "pkg-2.0-release".
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        Value v = forceRoot(*cache);
        EXPECT_EQ(calls, 1);
        ASSERT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "pkg-2.0-release");
    }
}

} // namespace nix::eval_trace::proptest
