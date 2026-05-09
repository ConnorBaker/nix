#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P34 — import ./file.nix Soundness
//
// Expression: import <tmpfile.nix>
// where tmpfile.nix contains a simple Nix integer expression.
//
// Soundness: changing the file's content invalidates (FileBytes dep via import).
// import is handled by ExprPath::eval → evalFile → records FileBytes dep.
class EvalTraceProperty_ImportFile : public TraceCacheFixture {
public:
    EvalTraceProperty_ImportFile() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-import-file");
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

// P34a: Soundness — changing imported .nix file invalidates.
TEST_F(EvalTraceProperty_ImportFile, ContentChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Generate two different integer values for the Nix expression.
            auto n1 = *rc::gen::inRange<int>(0, 1000);
            auto n2 = *rc::gen::suchThat(
                rc::gen::inRange<int>(0, 1000),
                [n1](int v) { return v != n1; });

            // Create a .nix file with a simple integer literal.
            std::string contentV1 = std::to_string(n1);
            std::string contentV2 = std::to_string(n2);

            auto handle = std::make_shared<TempExtFile>("nix", contentV1);
            DepSlot slot;
            slot.kind = DepSlot::Kind::File;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = contentV1;
            slot.setOriginal(contentV1);

            // Expression: import <path>
            std::string nixCode = "import " + handle->path.string();

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

            // Change the .nix file content.
            slot.mutate(contentV2);
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

// P34b: Precision — changing an unrelated file does not invalidate import.
TEST_F(EvalTraceProperty_ImportFile, UnrelatedFile_CacheHit)
{
    rc::detail::checkGTestWith(
        [this](std::string unrelatedContent) {
            auto handle = std::make_shared<TempExtFile>("nix", "42");
            std::string nixCode = "import " + handle->path.string();

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
