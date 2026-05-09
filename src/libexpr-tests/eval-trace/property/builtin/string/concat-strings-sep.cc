#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P29 — concatStringsSep Soundness and Precision
//
// Expression: builtins.concatStringsSep ":" [(readFile slotA) (readFile slotB)]
//
// Soundness: changing slotA invalidates (RawBytes dep from readFile A).
// Precision: an unrelated file does not invalidate.
//
// Each readFile records its own FileBytes dep.
// concatStringsSep calls maybeRecordRawContentDep on each operand string.
class EvalTraceProperty_ConcatStringsSep : public TraceCacheFixture {
public:
    EvalTraceProperty_ConcatStringsSep() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-concat-strings-sep");
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

// P29a: Soundness — changing fileA invalidates.
TEST_F(EvalTraceProperty_ConcatStringsSep, FileAChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto contentA = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            auto contentB = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            auto newContentA = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            RC_PRE(newContentA != contentA);

            auto handleA = std::make_shared<TempTextFile>(contentA);
            auto handleB = std::make_shared<TempTextFile>(contentB);

            DepSlot slotA;
            slotA.kind = DepSlot::Kind::File;
            slotA.path = handleA->path;
            slotA.fileHandle = handleA;
            slotA.currentValue = contentA;
            slotA.setOriginal(contentA);

            std::string nixCode =
                "builtins.concatStringsSep \":\" [(builtins.readFile "
                + handleA->path.string() + ") (builtins.readFile "
                + handleB->path.string() + ")]";

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

            // Mutate fileA.
            slotA.mutate(newContentA);
            invalidateFileCache(slotA.path);

            // Warm eval must re-evaluate.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore.
            slotA.restore();
            invalidateFileCache(slotA.path);
        },
        makeParams);
}

// P29b: Precision — changing an unrelated file does not invalidate.
TEST_F(EvalTraceProperty_ConcatStringsSep, UnrelatedFile_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto contentA = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            auto contentB = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            auto unrelatedContent = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));

            auto handleA = std::make_shared<TempTextFile>(contentA);
            auto handleB = std::make_shared<TempTextFile>(contentB);

            std::string nixCode =
                "builtins.concatStringsSep \":\" [(builtins.readFile "
                + handleA->path.string() + ") (builtins.readFile "
                + handleB->path.string() + ")]";

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
