#include "helpers.hh"
#include "nix/expr/eval-result-serialise.hh"

#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/store/store-api.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

class EvalResultSerialiseTest : public LibExprTest
{
protected:
    /**
     * Serialize → deserialize → compare.
     */
    void roundtrip(const AttrValue & input)
    {
        auto cbor = serializeAttrValue(input, state.symbols);
        ASSERT_FALSE(cbor.empty());
        auto output = deserializeAttrValue(cbor, state.symbols);
        assertAttrValueEquals(input, output, state.symbols);
    }
};

// ── String roundtrips ────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_String_NoContext)
{
    roundtrip(string_t{"hello", {}});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_String_Empty)
{
    roundtrip(string_t{"", {}});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_String_WithContext)
{
    NixStringContext ctx;
    ctx.insert(NixStringContextElem::Opaque{
        .path = StorePath{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello"}});
    roundtrip(string_t{"some-string", std::move(ctx)});
}

// ── Bool roundtrips ──────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_Bool_True)
{
    roundtrip(true);
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Bool_False)
{
    roundtrip(false);
}

// ── Int roundtrips ───────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_Int_Positive)
{
    roundtrip(int_t{NixInt{42}});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Int_Negative)
{
    roundtrip(int_t{NixInt{-1}});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Int_Zero)
{
    roundtrip(int_t{NixInt{0}});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Int_MaxMin)
{
    roundtrip(int_t{NixInt{std::numeric_limits<int64_t>::max()}});
    roundtrip(int_t{NixInt{std::numeric_limits<int64_t>::min()}});
}

// ── Float roundtrips ─────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_Float)
{
    // Note: float roundtrips through std::to_string / std::stod so precision may differ
    roundtrip(float_t{3.140000});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Float_Zero)
{
    roundtrip(float_t{0.0});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Float_Negative)
{
    roundtrip(float_t{-1.5});
}

// ── Null ─────────────────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_Null)
{
    roundtrip(null_t{});
}

// ── Path ─────────────────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_Path)
{
    roundtrip(path_t{"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-hello"});
}

// ── FullAttrs ────────────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_FullAttrs_Empty)
{
    roundtrip(std::vector<Symbol>{});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_FullAttrs_One)
{
    roundtrip(std::vector<Symbol>{createSymbol("a")});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_FullAttrs_Many)
{
    roundtrip(std::vector<Symbol>{
        createSymbol("a"), createSymbol("b"), createSymbol("c")});
}

// ── ListOfStrings ────────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_ListOfStrings)
{
    roundtrip(std::vector<std::string>{"a", "b", "c"});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_ListOfStrings_Empty)
{
    // Empty ListOfStrings tokenizes to empty vector via tab splitting
    roundtrip(std::vector<std::string>{});
}

// ── List ─────────────────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_List)
{
    roundtrip(list_t{5});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_List_Zero)
{
    roundtrip(list_t{0});
}

// ── Sentinel types ───────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Roundtrip_Failed)
{
    roundtrip(failed_t{});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Missing)
{
    roundtrip(missing_t{});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Misc)
{
    roundtrip(misc_t{});
}

TEST_F(EvalResultSerialiseTest, Roundtrip_Placeholder)
{
    roundtrip(placeholder_t{});
}

// ── Negative tests ───────────────────────────────────────────────────

TEST_F(EvalResultSerialiseTest, Deserialize_InvalidCBOR)
{
    std::vector<uint8_t> garbage = {0xFF, 0xFE, 0x01, 0x02};
    EXPECT_THROW(deserializeAttrValue(garbage, state.symbols), std::exception);
}

TEST_F(EvalResultSerialiseTest, Deserialize_WrongVersion)
{
    // Create valid CBOR but with wrong version
    nlohmann::json j;
    j["v"] = 999;
    j["t"] = static_cast<int>(AttrType::Null);
    auto cbor = nlohmann::json::to_cbor(j);
    EXPECT_THROW(deserializeAttrValue(cbor, state.symbols), Error);
}

TEST_F(EvalResultSerialiseTest, Deserialize_MissingTypeField)
{
    // Valid CBOR with version but no type field
    nlohmann::json j;
    j["v"] = 1;
    auto cbor = nlohmann::json::to_cbor(j);
    EXPECT_THROW(deserializeAttrValue(cbor, state.symbols), std::exception);
}

TEST_F(EvalResultSerialiseTest, Deserialize_EmptyInput)
{
    std::vector<uint8_t> empty;
    EXPECT_THROW(deserializeAttrValue(empty, state.symbols), std::exception);
}

// ── EvalTrace serialization tests ────────────────────────────────────

class EvalTraceSerialiseTest : public LibExprTest
{
public:
    EvalTraceSerialiseTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    // Dummy dep set path for trace serialization tests
    StorePath dummyDepSetPath{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-eval-deps"};

    void traceRoundtrip(
        const AttrValue & result,
        const std::optional<StorePath> & parent,
        const std::optional<int64_t> & contextHash)
    {
        auto cbor = serializeEvalTrace(result, parent, contextHash, dummyDepSetPath,
                                        state.symbols, *state.store);
        ASSERT_FALSE(cbor.empty());
        auto trace = deserializeEvalTrace(cbor, state.symbols, *state.store);
        assertAttrValueEquals(result, trace.result, state.symbols);
        EXPECT_EQ(trace.contextHash, contextHash);
        if (parent) {
            ASSERT_TRUE(trace.parent.has_value());
            EXPECT_EQ(state.store->printStorePath(*trace.parent),
                      state.store->printStorePath(*parent));
        } else {
            EXPECT_FALSE(trace.parent.has_value());
        }
        EXPECT_EQ(state.store->printStorePath(trace.depSetPath),
                  state.store->printStorePath(dummyDepSetPath));
    }
};

TEST_F(EvalTraceSerialiseTest, Roundtrip_NoDeps_NoParent)
{
    traceRoundtrip(string_t{"hello", {}}, std::nullopt, std::nullopt);
}

TEST_F(EvalTraceSerialiseTest, Roundtrip_WithParent)
{
    auto parentPath = StorePath{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-eval-root"};
    traceRoundtrip(null_t{}, parentPath, std::nullopt);
}

TEST_F(EvalTraceSerialiseTest, Roundtrip_ContextHash)
{
    traceRoundtrip(true, std::nullopt, int64_t{0xDEADBEEF});
}

TEST_F(EvalTraceSerialiseTest, Roundtrip_Failed)
{
    traceRoundtrip(failed_t{}, std::nullopt, std::nullopt);
}

// ── Dep set blob serialization tests ─────────────────────────────────

TEST_F(EvalTraceSerialiseTest, DepSet_Roundtrip_Empty)
{
    auto compressed = serializeDepSet({});
    auto deps = deserializeDepSet(compressed);
    EXPECT_TRUE(deps.empty());
}

TEST_F(EvalTraceSerialiseTest, DepSet_Roundtrip_WithDeps)
{
    std::vector<Dep> deps = {
        makeContentDep("/test.nix", "hello"),
        makeEnvVarDep("HOME", "/home/user"),
    };
    auto sorted = sortAndDedupDeps(deps);
    auto compressed = serializeDepSet(sorted);
    auto roundtripped = deserializeDepSet(compressed);
    ASSERT_EQ(roundtripped.size(), 2u);
    EXPECT_EQ(roundtripped[0].type, DepType::Content);
    EXPECT_EQ(roundtripped[1].type, DepType::EnvVar);
}

TEST_F(EvalTraceSerialiseTest, DepSet_Roundtrip_AllTypes)
{
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "content"),
        makeEnvVarDep("VAR", "value"),
        makeExistenceDep("/path", true),
        makeSystemDep("x86_64-linux"),
    };
    auto sorted = sortAndDedupDeps(deps);
    auto compressed = serializeDepSet(sorted);
    auto roundtripped = deserializeDepSet(compressed);
    ASSERT_EQ(roundtripped.size(), 4u);
}

TEST_F(EvalTraceSerialiseTest, DepSet_Deterministic)
{
    // Same deps in different order should produce the same compressed output
    // (both are sorted+deduped before serialization)
    std::vector<Dep> deps1 = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    std::vector<Dep> deps2 = {
        makeContentDep("/a.nix", "a"),
        makeEnvVarDep("B", "val"),
    };
    auto compressed1 = serializeDepSet(sortAndDedupDeps(deps1));
    auto compressed2 = serializeDepSet(sortAndDedupDeps(deps2));
    EXPECT_EQ(compressed1, compressed2);
}

TEST_F(EvalTraceSerialiseTest, Deserialize_InvalidCBOR)
{
    std::vector<uint8_t> garbage = {0xFF, 0xFE};
    EXPECT_THROW(deserializeEvalTrace(garbage, state.symbols, *state.store), std::exception);
}

TEST_F(EvalTraceSerialiseTest, Deserialize_WrongVersion)
{
    nlohmann::json j;
    j["v"] = 999;
    j["ds"] = "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-eval-deps";
    j["r"] = nlohmann::json::binary(std::vector<uint8_t>{});
    j["p"] = nullptr;
    j["c"] = nullptr;
    auto cbor = nlohmann::json::to_cbor(j);
    EXPECT_THROW(deserializeEvalTrace(cbor, state.symbols, *state.store), Error);
}

// ── computeDepStructHash tests ───────────────────────────────────────

TEST_F(EvalTraceSerialiseTest, DepStructHash_DeterministicOrdering)
{
    // Deps in different order should produce the same struct hash
    std::vector<Dep> deps1 = {
        makeEnvVarDep("B", "val1"),
        makeContentDep("/a.nix", "content1"),
    };
    std::vector<Dep> deps2 = {
        makeContentDep("/a.nix", "content2"),
        makeEnvVarDep("B", "val2"),
    };
    EXPECT_EQ(computeDepStructHash(deps1), computeDepStructHash(deps2));
}

TEST_F(EvalTraceSerialiseTest, DepStructHash_SameKeysDifferentValues)
{
    // Same keys, different expectedHash values → same struct hash
    std::vector<Dep> deps1 = {makeContentDep("/a.nix", "hello")};
    std::vector<Dep> deps2 = {makeContentDep("/a.nix", "world")};
    EXPECT_EQ(computeDepStructHash(deps1), computeDepStructHash(deps2));
}

TEST_F(EvalTraceSerialiseTest, DepStructHash_DifferentKeys)
{
    // Different dep keys → different struct hash
    std::vector<Dep> deps1 = {makeContentDep("/a.nix", "x")};
    std::vector<Dep> deps2 = {makeContentDep("/b.nix", "x")};
    EXPECT_NE(computeDepStructHash(deps1), computeDepStructHash(deps2));
}

TEST_F(EvalTraceSerialiseTest, DepStructHash_EmptyDeps)
{
    // Empty deps → consistent hash
    auto h1 = computeDepStructHash({});
    auto h2 = computeDepStructHash({});
    EXPECT_EQ(h1, h2);
}

TEST_F(EvalTraceSerialiseTest, DepContentHashWithParent_DifferentParents)
{
    // Same deps + different parents → different hashes
    std::vector<Dep> deps = {makeContentDep("/a.nix", "val")};
    auto parent1 = StorePath{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-eval-parent1"};
    auto parent2 = StorePath{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-eval-parent2"};
    auto h1 = computeDepContentHashWithParent(deps, parent1, *state.store);
    auto h2 = computeDepContentHashWithParent(deps, parent2, *state.store);
    EXPECT_NE(h1, h2);
}

TEST_F(EvalTraceSerialiseTest, DepContentHashWithParent_SameParent)
{
    // Same deps + same parent → same hash
    std::vector<Dep> deps = {makeContentDep("/a.nix", "val")};
    auto parent = StorePath{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-eval-parent"};
    auto h1 = computeDepContentHashWithParent(deps, parent, *state.store);
    auto h2 = computeDepContentHashWithParent(deps, parent, *state.store);
    EXPECT_EQ(h1, h2);
}

// ── sortAndDedupDeps tests ───────────────────────────────────────────

TEST_F(EvalTraceSerialiseTest, SortAndDedup_RemovesDuplicates)
{
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        makeContentDep("/a.nix", "a"), // exact duplicate
        makeContentDep("/b.nix", "b"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(sorted.size(), 2u);
}

TEST_F(EvalTraceSerialiseTest, SortAndDedup_SortsCorrectly)
{
    // Sorted by (type, source, key)
    std::vector<Dep> deps = {
        makeEnvVarDep("Z", "val"),     // type=4
        makeContentDep("/z.nix", "c"), // type=1
        makeContentDep("/a.nix", "c"), // type=1
    };
    auto sorted = sortAndDedupDeps(deps);
    ASSERT_EQ(sorted.size(), 3u);
    // Content (type=1) before EnvVar (type=4)
    EXPECT_EQ(sorted[0].type, DepType::Content);
    EXPECT_EQ(sorted[0].key, "/a.nix");
    EXPECT_EQ(sorted[1].type, DepType::Content);
    EXPECT_EQ(sorted[1].key, "/z.nix");
    EXPECT_EQ(sorted[2].type, DepType::EnvVar);
}

TEST_F(EvalTraceSerialiseTest, SortAndDedup_DedupByKeyNotHash)
{
    // Same (type, source, key) but different expectedHash → deduped
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "content-v1"),
        makeContentDep("/a.nix", "content-v2"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(sorted.size(), 1u);
}

TEST_F(EvalTraceSerialiseTest, SortAndDedup_EmptyInput)
{
    auto sorted = sortAndDedupDeps({});
    EXPECT_TRUE(sorted.empty());
}

// ── Pre-sorted variant equivalence tests ─────────────────────────────

TEST_F(EvalTraceSerialiseTest, PreSorted_ContentHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeDepContentHash(deps), computeDepContentHashFromSorted(sorted));
}

TEST_F(EvalTraceSerialiseTest, PreSorted_StructHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeDepStructHash(deps), computeDepStructHashFromSorted(sorted));
}

TEST_F(EvalTraceSerialiseTest, PreSorted_ParentHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    auto parent = StorePath{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-eval-parent"};
    EXPECT_EQ(
        computeDepContentHashWithParent(deps, parent, *state.store),
        computeDepContentHashWithParentFromSorted(sorted, parent, *state.store));
}

} // namespace nix::eval_cache
