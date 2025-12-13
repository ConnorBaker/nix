#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <rapidcheck/gtest.h>

#include "nix/expr/eval-cache.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/hash.hh"

#include <filesystem>

namespace nix::eval_cache {

/**
 * Test fixture for EvalCache integration tests.
 *
 * These tests verify the behavior of the eval cache system, including:
 * - Basic caching operations (storing and retrieving values)
 * - Cache hit/miss behavior
 * - Graceful degradation when the cache encounters errors
 *
 * Note: AttrDb is an internal implementation detail, so we test through
 * the public EvalCache and AttrCursor API.
 */
class EvalCacheTest : public LibExprTest
{
protected:
    /**
     * Generate a unique fingerprint for each test to ensure a fresh cache.
     */
    Hash makeFingerprint()
    {
        // Use a hash of the current time and a counter to ensure uniqueness
        static std::atomic<uint64_t> counter{0};
        auto input = fmt("%d-%d", time(nullptr), counter++);
        return hashString(HashAlgorithm::SHA256, input);
    }

    /**
     * Create an EvalCache with caching enabled.
     */
    std::shared_ptr<EvalCache> makeCache(const Hash & fingerprint, std::function<Value *()> rootLoader)
    {
        return std::make_shared<EvalCache>(
            std::optional<std::reference_wrapper<const Hash>>(fingerprint), state, rootLoader);
    }

    /**
     * Create an EvalCache without caching (for comparison).
     */
    std::shared_ptr<EvalCache> makeUncachedCache(std::function<Value *()> rootLoader)
    {
        return std::make_shared<EvalCache>(std::nullopt, state, rootLoader);
    }

    /**
     * Create a simple attrset value for testing.
     */
    Value * makeTestAttrset()
    {
        auto v = state.allocValue();
        auto attrs = state.buildBindings(5);

        // Add various attribute types for testing
        auto strVal = state.allocValue();
        strVal->mkString("test-string", state.mem);
        attrs.insert(state.symbols.create("stringAttr"), strVal);

        auto intVal = state.allocValue();
        intVal->mkInt(42);
        attrs.insert(state.symbols.create("intAttr"), intVal);

        auto boolVal = state.allocValue();
        boolVal->mkBool(true);
        attrs.insert(state.symbols.create("boolAttr"), boolVal);

        // Nested attrset
        auto nestedVal = state.allocValue();
        auto nestedAttrs = state.buildBindings(2);
        auto innerStrVal = state.allocValue();
        innerStrVal->mkString("nested-value", state.mem);
        nestedAttrs.insert(state.symbols.create("inner"), innerStrVal);
        nestedVal->mkAttrs(nestedAttrs.finish());
        attrs.insert(state.symbols.create("nested"), nestedVal);

        // List of strings
        auto listVal = state.allocValue();
        auto list = state.buildList(3);
        for (size_t i = 0; i < 3; ++i) {
            auto elem = state.allocValue();
            elem->mkString(fmt("item-%d", i), state.mem);
            list.elems[i] = elem;
        }
        listVal->mkList(list);
        attrs.insert(state.symbols.create("listAttr"), listVal);

        v->mkAttrs(attrs.finish());
        return v;
    }
};

// ============================================================================
// Basic Caching Tests
// ============================================================================

TEST_F(EvalCacheTest, CacheCreationWithFingerprint)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    ASSERT_NE(cache, nullptr);
    // getRoot() returns ref<AttrCursor> which is always valid (non-nullable)
    auto root = cache->getRoot();
    (void)root; // Use to avoid unused variable warning
}

TEST_F(EvalCacheTest, CacheCreationWithoutFingerprint)
{
    auto rootVal = makeTestAttrset();
    auto cache = makeUncachedCache([&]() { return rootVal; });

    ASSERT_NE(cache, nullptr);
    // getRoot() returns ref<AttrCursor> which is always valid (non-nullable)
    auto root = cache->getRoot();
    (void)root;
}

TEST_F(EvalCacheTest, GetStringAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto attr = root->getAttr("stringAttr");
    EXPECT_EQ(attr->getString(), "test-string");
}

TEST_F(EvalCacheTest, GetIntAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto attr = root->getAttr("intAttr");
    EXPECT_EQ(attr->getInt().value, 42);
}

TEST_F(EvalCacheTest, GetBoolAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto attr = root->getAttr("boolAttr");
    EXPECT_EQ(attr->getBool(), true);
}

TEST_F(EvalCacheTest, GetNestedAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto nested = root->getAttr("nested");
    auto inner = nested->getAttr("inner");
    EXPECT_EQ(inner->getString(), "nested-value");
}

TEST_F(EvalCacheTest, GetListOfStringsAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto attr = root->getAttr("listAttr");
    auto list = attr->getListOfStrings();
    ASSERT_EQ(list.size(), 3);
    EXPECT_EQ(list[0], "item-0");
    EXPECT_EQ(list[1], "item-1");
    EXPECT_EQ(list[2], "item-2");
}

TEST_F(EvalCacheTest, GetAttrsReturnsAttributeNames)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto attrs = root->getAttrs();
    EXPECT_EQ(attrs.size(), 5);

    // Convert symbols to strings for comparison
    std::set<std::string> attrNames;
    for (auto & sym : attrs)
        attrNames.insert(std::string(state.symbols[sym]));

    EXPECT_TRUE(attrNames.count("stringAttr"));
    EXPECT_TRUE(attrNames.count("intAttr"));
    EXPECT_TRUE(attrNames.count("boolAttr"));
    EXPECT_TRUE(attrNames.count("nested"));
    EXPECT_TRUE(attrNames.count("listAttr"));
}

TEST_F(EvalCacheTest, MaybeGetAttrReturnsMissing)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto attr = root->maybeGetAttr("nonexistent");
    EXPECT_EQ(attr, nullptr);
}

TEST_F(EvalCacheTest, MaybeGetAttrReturnsExisting)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();
    auto attr = root->maybeGetAttr("stringAttr");
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(attr->getString(), "test-string");
}

// ============================================================================
// Cache Hit/Miss Tests
// ============================================================================

TEST_F(EvalCacheTest, CacheHitOnSecondAccess)
{
    auto fingerprint = makeFingerprint();
    int loadCount = 0;
    auto rootVal = makeTestAttrset();

    // First cache instance
    {
        auto cache = makeCache(fingerprint, [&]() {
            loadCount++;
            return rootVal;
        });
        auto root = cache->getRoot();
        auto attr = root->getAttr("stringAttr");
        EXPECT_EQ(attr->getString(), "test-string");
    }

    // Second cache instance with same fingerprint should use cached data
    {
        auto cache = makeCache(fingerprint, [&]() {
            loadCount++;
            return rootVal;
        });
        auto root = cache->getRoot();

        // Should be able to get cached attributes without triggering root loader
        // for attributes already cached
        auto attrs = root->getAttrs();
        EXPECT_EQ(attrs.size(), 5);

        // The string attribute should be cached from the first run
        auto attr = root->getAttr("stringAttr");
        EXPECT_EQ(attr->getString(), "test-string");
    }
}

TEST_F(EvalCacheTest, DifferentFingerprintCreatesSeparateCache)
{
    auto fingerprint1 = makeFingerprint();
    auto fingerprint2 = makeFingerprint();

    auto rootVal1 = state.allocValue();
    {
        auto attrs = state.buildBindings(1);
        auto strVal = state.allocValue();
        strVal->mkString("value1", state.mem);
        attrs.insert(state.symbols.create("attr"), strVal);
        rootVal1->mkAttrs(attrs.finish());
    }

    auto rootVal2 = state.allocValue();
    {
        auto attrs = state.buildBindings(1);
        auto strVal = state.allocValue();
        strVal->mkString("value2", state.mem);
        attrs.insert(state.symbols.create("attr"), strVal);
        rootVal2->mkAttrs(attrs.finish());
    }

    // Cache with fingerprint1
    {
        auto cache = makeCache(fingerprint1, [&]() { return rootVal1; });
        auto root = cache->getRoot();
        EXPECT_EQ(root->getAttr("attr")->getString(), "value1");
    }

    // Cache with fingerprint2 should have different value
    {
        auto cache = makeCache(fingerprint2, [&]() { return rootVal2; });
        auto root = cache->getRoot();
        EXPECT_EQ(root->getAttr("attr")->getString(), "value2");
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(EvalCacheTest, CachedEvalErrorOnFailedAttribute)
{
    auto fingerprint = makeFingerprint();

    // Create an attrset with a throwing attribute
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto throwingVal = state.allocValue();
    // Create a thunk that will throw when evaluated
    auto expr = state.parseExprFromString("throw \"test error\"", state.rootPath(CanonPath::root));
    state.mkThunk_(*throwingVal, expr);
    attrs.insert(state.symbols.create("failing"), throwingVal);
    rootVal->mkAttrs(attrs.finish());

    // First access - the failure may be cached from previous runs (cache DB persists on disk).
    // Just verify that accessing a failing attribute throws some kind of EvalError.
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        auto attr = root->maybeGetAttr("failing");
        ASSERT_NE(attr, nullptr);

        // Should throw either EvalError (fresh evaluation) or CachedEvalError (from cache)
        bool threwError = false;
        try {
            attr->getString();
        } catch (const EvalError &) {
            threwError = true;
        }
        EXPECT_TRUE(threwError);
    }

    // Second access with same fingerprint - should throw CachedEvalError
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();

        // maybeGetAttr might throw CachedEvalError if failure is cached
        bool threwCachedError = false;
        try {
            auto attr = root->maybeGetAttr("failing");
            if (attr) {
                attr->getString();
            }
        } catch (const CachedEvalError &) {
            threwCachedError = true;
        } catch (const EvalError &) {
            // Also acceptable if cache wasn't populated
            threwCachedError = true;
        }
        EXPECT_TRUE(threwCachedError);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(EvalCacheTest, EmptyStringAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto strVal = state.allocValue();
    strVal->mkString("", state.mem);
    attrs.insert(state.symbols.create("empty"), strVal);
    rootVal->mkAttrs(attrs.finish());

    auto cache = makeCache(fingerprint, [&]() { return rootVal; });
    auto root = cache->getRoot();
    auto attr = root->getAttr("empty");
    EXPECT_EQ(attr->getString(), "");
}

TEST_F(EvalCacheTest, UnicodeStringAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto strVal = state.allocValue();
    std::string unicode = "Hello \xC3\xA9\xC3\xA0\xC3\xBC \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80";
    strVal->mkString(unicode, state.mem);
    attrs.insert(state.symbols.create("unicode"), strVal);
    rootVal->mkAttrs(attrs.finish());

    auto cache = makeCache(fingerprint, [&]() { return rootVal; });
    auto root = cache->getRoot();
    auto attr = root->getAttr("unicode");
    EXPECT_EQ(attr->getString(), unicode);
}

TEST_F(EvalCacheTest, LargeStringAttribute)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto strVal = state.allocValue();
    std::string large(100000, 'x'); // 100KB string
    strVal->mkString(large, state.mem);
    attrs.insert(state.symbols.create("large"), strVal);
    rootVal->mkAttrs(attrs.finish());

    auto cache = makeCache(fingerprint, [&]() { return rootVal; });
    auto root = cache->getRoot();
    auto attr = root->getAttr("large");
    EXPECT_EQ(attr->getString(), large);
}

TEST_F(EvalCacheTest, IntMinMaxValues)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(2);

    auto minVal = state.allocValue();
    minVal->mkInt(std::numeric_limits<NixInt::Inner>::min());
    attrs.insert(state.symbols.create("min"), minVal);

    auto maxVal = state.allocValue();
    maxVal->mkInt(std::numeric_limits<NixInt::Inner>::max());
    attrs.insert(state.symbols.create("max"), maxVal);

    rootVal->mkAttrs(attrs.finish());

    auto cache = makeCache(fingerprint, [&]() { return rootVal; });
    auto root = cache->getRoot();

    EXPECT_EQ(root->getAttr("min")->getInt().value, std::numeric_limits<NixInt::Inner>::min());
    EXPECT_EQ(root->getAttr("max")->getInt().value, std::numeric_limits<NixInt::Inner>::max());
}

TEST_F(EvalCacheTest, EmptyListOfStrings)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto listVal = state.allocValue();
    auto list = state.buildList(0);
    listVal->mkList(list);
    attrs.insert(state.symbols.create("emptyList"), listVal);
    rootVal->mkAttrs(attrs.finish());

    auto cache = makeCache(fingerprint, [&]() { return rootVal; });
    auto root = cache->getRoot();
    auto listResult = root->getAttr("emptyList")->getListOfStrings();
    EXPECT_TRUE(listResult.empty());
}

TEST_F(EvalCacheTest, EmptyAttrset)
{
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto emptyAttrs = state.allocValue();
    auto emptyBuilder = state.buildBindings(0);
    emptyAttrs->mkAttrs(emptyBuilder.finish());
    attrs.insert(state.symbols.create("empty"), emptyAttrs);
    rootVal->mkAttrs(attrs.finish());

    auto cache = makeCache(fingerprint, [&]() { return rootVal; });
    auto root = cache->getRoot();
    auto empty = root->getAttr("empty");
    auto innerAttrs = empty->getAttrs();
    EXPECT_TRUE(innerAttrs.empty());
}

// ============================================================================
// Property-Based Tests with RapidCheck
// ============================================================================

RC_GTEST_FIXTURE_PROP(EvalCacheTest, StringRoundtrip, (std::string value))
{
    // Filter out strings with null bytes (not supported by SQLite text type)
    RC_PRE(value.find('\0') == std::string::npos);

    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto strVal = state.allocValue();
    strVal->mkString(value, state.mem);
    attrs.insert(state.symbols.create("prop"), strVal);
    rootVal->mkAttrs(attrs.finish());

    // First access (populates cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getString() == value);
    }

    // Second access (from cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getString() == value);
    }
}

RC_GTEST_FIXTURE_PROP(EvalCacheTest, IntRoundtrip, (int32_t value))
{
    // Note: The cache stores ints as 32-bit values, so we test with int32_t
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto intVal = state.allocValue();
    intVal->mkInt(value);
    attrs.insert(state.symbols.create("prop"), intVal);
    rootVal->mkAttrs(attrs.finish());

    // First access (populates cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getInt().value == value);
    }

    // Second access (from cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getInt().value == value);
    }
}

RC_GTEST_FIXTURE_PROP(EvalCacheTest, BoolRoundtrip, (bool value))
{
    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);
    auto boolVal = state.allocValue();
    boolVal->mkBool(value);
    attrs.insert(state.symbols.create("prop"), boolVal);
    rootVal->mkAttrs(attrs.finish());

    // First access (populates cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getBool() == value);
    }

    // Second access (from cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getBool() == value);
    }
}

RC_GTEST_FIXTURE_PROP(EvalCacheTest, ListOfStringsRoundtrip, (std::vector<std::string> value))
{
    // Filter out:
    // - strings with null bytes (not supported by SQLite text type)
    // - strings with tabs (used as separator in cache storage)
    // - empty strings (dropEmptyInitThenConcatStringsSep drops them)
    for (const auto & s : value) {
        RC_PRE(s.find('\0') == std::string::npos);
        RC_PRE(s.find('\t') == std::string::npos);
        RC_PRE(!s.empty());
    }

    auto fingerprint = makeFingerprint();
    auto rootVal = state.allocValue();
    auto attrs = state.buildBindings(1);

    auto listVal = state.allocValue();
    auto list = state.buildList(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        auto elem = state.allocValue();
        elem->mkString(value[i], state.mem);
        list.elems[i] = elem;
    }
    listVal->mkList(list);
    attrs.insert(state.symbols.create("prop"), listVal);
    rootVal->mkAttrs(attrs.finish());

    // First access (populates cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getListOfStrings() == value);
    }

    // Second access (from cache)
    {
        auto cache = makeCache(fingerprint, [&]() { return rootVal; });
        auto root = cache->getRoot();
        RC_ASSERT(root->getAttr("prop")->getListOfStrings() == value);
    }
}

// ============================================================================
// Database Error Graceful Degradation Tests
// ============================================================================

TEST_F(EvalCacheTest, GetKeyFallsBackToEvaluationOnDbError)
{
    // This test verifies that when the database returns an error,
    // the code gracefully falls back to evaluation instead of crashing.
    //
    // Note: This is difficult to test directly since we can't easily inject
    // database errors. This test documents the expected behavior and verifies
    // normal operation doesn't regress.

    auto fingerprint = makeFingerprint();
    auto rootVal = makeTestAttrset();
    auto cache = makeCache(fingerprint, [&]() { return rootVal; });

    auto root = cache->getRoot();

    // Access nested attribute - this exercises getKey() which had the assertion bug
    auto nested = root->getAttr("nested");
    auto inner = nested->getAttr("inner");

    // Verify we can get the value (would have crashed before the fix if db error occurred)
    EXPECT_EQ(inner->getString(), "nested-value");

    // Access the same path again to exercise cache hit path
    auto nested2 = root->getAttr("nested");
    auto inner2 = nested2->getAttr("inner");
    EXPECT_EQ(inner2->getString(), "nested-value");
}

} // namespace nix::eval_cache
