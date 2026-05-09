#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P26 — readFile + replaceStrings Soundness and Precision
//
// Expression: builtins.replaceStrings ["a"] ["b"] (builtins.readFile slot)
//
// Soundness: changing the file content invalidates (RawBytes dep via readFile).
// Precision: changing an unrelated file does not invalidate.
class EvalTraceProperty_ReplaceStrings : public TraceCacheFixture {
public:
    EvalTraceProperty_ReplaceStrings() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-replace-strings");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    params.maxDiscardRatio = 50;
    return params;
}

// P26a: Soundness — changing the file invalidates.
TEST_F(EvalTraceProperty_ReplaceStrings, FileChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this](TestExpr readExpr, std::string newContent) {
            // Filter to ReadFileGen (Kind::File) expressions.
            RC_PRE(!readExpr.depSlots.empty());
            auto & slot = readExpr.depSlots[0];
            RC_PRE(slot.kind == DepSlot::Kind::File);
            RC_PRE(newContent != slot.currentValue);

            std::string nixCode =
                "builtins.replaceStrings [\"a\"] [\"b\"] (builtins.readFile "
                + slot.path.string() + ")";

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }
            // Confirm cache hit.
            {
                int n = 0;
                auto cache = makeCache(nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Mutate.
            slot.mutate(newContent);
            invalidateFileCache(slot.path);

            // Warm eval must re-evaluate.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// P26b: Precision — changing an unrelated file does not invalidate.
TEST_F(EvalTraceProperty_ReplaceStrings, UnrelatedFile_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto fileContent = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            auto unrelatedContent = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));

            auto handle = std::make_shared<TempTextFile>(fileContent);

            std::string nixCode =
                "builtins.replaceStrings [\"a\"] [\"b\"] (builtins.readFile "
                + handle->path.string() + ")";

            TempTextFile unrelated(unrelatedContent);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Modify unrelated.
            unrelated.modify(unrelatedContent + "_changed");
            invalidateFileCache(unrelated.path);

            // Warm eval must be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
