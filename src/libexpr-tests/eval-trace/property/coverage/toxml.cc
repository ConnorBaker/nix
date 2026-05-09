#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// P-XML — builtins.toXML on traced data
//
// builtins.toXML converts a Nix value to an XML string representation.
// It forces the value, so all deps reachable through the value are recorded.
//
// Tests in this file verify soundness: changing the data that contributes
// to the XML output forces re-evaluation.

class EvalTraceProperty_ToXML : public TraceCacheFixture {
public:
    EvalTraceProperty_ToXML() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-toxml");
    }
};

// P-XML-a: toXML Soundness — changing the value of a JSON key that is passed
// to toXML changes the XML output and forces re-evaluation.
//
// Expression: builtins.toXML (builtins.fromJSON (builtins.readFile f)).x
// where the JSON file initially contains {"x": 42, "y": "other"}.
// Changing x's value changes the XML representation.
TEST_F(EvalTraceProperty_ToXML, ToXML_Soundness)
{
    TempJsonFile file(R"({"x":42,"y":"other"})");
    auto const expr =
        "builtins.toXML (builtins.fromJSON (builtins.readFile "
        + file.path.string() + ")).x";

    // Cold eval: records dep on the file content and evaluates toXML.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString) << "expected string from toXML";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: change x's value — the XML output for the integer changes.
    file.modify(R"({"x":999,"y":"other"})");
    invalidateFileCache(file.path);

    // Warm eval: XML output changed — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after changing x's value (toXML output changes)";
    }
}

} // namespace nix::eval_trace::proptest
