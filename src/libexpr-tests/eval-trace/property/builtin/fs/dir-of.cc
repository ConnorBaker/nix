#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P33 — dirOf Provenance Preservation
//
// Expression: builtins.dirOf (builtins.readFile slot)
//
// Soundness: changing the file content invalidates.
// The dirOf (string branch) uses mergeSemanticHandle to preserve the full
// SemanticHandle through path manipulation.
//
// Note: dirOf on a readFile result returns the directory portion of the file
// path as a string. The dep on the file CONTENT survives through dirOf because
// the provenance is preserved, not because the path contains the content.
//
// Wait — this needs reconsideration. dirOf on a STRING (file content) returns
// the directory component of the string-as-path. The dep from readFile is on
// the FILE CONTENT, not the file path. So dirOf(readFile f) returns a path
// derived from the file's content treated as a path string, and the dep on the
// original content should be preserved.
//
// If the content is not a valid path-like string, Nix may return an empty string
// or the content itself. We test that the dep is preserved regardless.
class EvalTraceProperty_DirOf : public TraceCacheFixture {
public:
    EvalTraceProperty_DirOf() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-dir-of");
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

// P33a: Soundness — changing the file content invalidates.
TEST_F(EvalTraceProperty_DirOf, FileChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto content = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            auto newContent = *rc::gen::container<std::string>(rc::gen::inRange('!', '~'));
            RC_PRE(newContent != content);

            auto handle = std::make_shared<TempTextFile>(content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::File;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            std::string nixCode =
                "builtins.dirOf (builtins.readFile " + handle->path.string() + ")";

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

} // namespace nix::eval_trace::proptest
