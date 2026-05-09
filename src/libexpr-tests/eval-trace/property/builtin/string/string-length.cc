#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P28 — stringLength Soundness
//
// Expression: builtins.stringLength (builtins.readFile slot)
//
// Soundness: changing the file content invalidates (RawBytes dep via readFile).
// stringLength calls maybeRecordRawContentDep — same pattern as substring.
class EvalTraceProperty_StringLength : public TraceCacheFixture {
public:
    EvalTraceProperty_StringLength() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-string-length");
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

// P28a: Soundness — changing the file invalidates.
TEST_F(EvalTraceProperty_StringLength, FileChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this](std::string content, std::string newContent) {
            RC_PRE(content != newContent);

            auto handle = std::make_shared<TempTextFile>(content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::File;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            std::string nixCode =
                "builtins.stringLength (builtins.readFile "
                + handle->path.string() + ")";

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

// P28b: Precision — changing an unrelated file does not invalidate.
TEST_F(EvalTraceProperty_StringLength, UnrelatedFile_CacheHit)
{
    rc::detail::checkGTestWith(
        [this](std::string fileContent, std::string unrelatedContent) {
            auto handle = std::make_shared<TempTextFile>(fileContent);

            std::string nixCode =
                "builtins.stringLength (builtins.readFile "
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
