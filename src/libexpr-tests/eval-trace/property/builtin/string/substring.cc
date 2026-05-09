#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P27 — substring — Whole-File Dep
//
// Expression: builtins.substring 0 3 (builtins.readFile slot)
//
// Property: changing ANY byte of the file invalidates, even bytes outside the
// extracted substring. This documents the current design choice: FileBytes is a
// whole-file dep, so partial reads record whole-file deps.
//
// substring calls maybeRecordRawContentDep which records a RawBytes dep on the
// entire TextObject (whole-file FileBytes provenance).
class EvalTraceProperty_Substring : public TraceCacheFixture {
public:
    EvalTraceProperty_Substring() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-substring");
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

// P27a: changing any byte (including beyond the substring) invalidates.
TEST_F(EvalTraceProperty_Substring, FileChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Use a file with at least 6 printable ASCII chars so that
            // both "within substring" and "beyond substring" bytes exist.
            auto content = *rc::gen::container<std::string>(
                6, rc::gen::inRange('!', '~'));

            auto handle = std::make_shared<TempTextFile>(content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::File;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            std::string nixCode =
                "builtins.substring 0 3 (builtins.readFile "
                + handle->path.string() + ")";

            // Mutate beyond the first 3 chars (bytes 3+ are outside the substring).
            std::string mutated = content;
            mutated[5] = (mutated[5] == '~') ? '!' : (char)(mutated[5] + 1);
            RC_PRE(mutated != content);

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

            // Mutate byte outside substring.
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval must re-evaluate (whole-file dep, not partial dep).
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

} // namespace nix::eval_trace::proptest
