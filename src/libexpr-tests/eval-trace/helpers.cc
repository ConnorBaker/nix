#include "helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::test {

void assertCachedResultEquals(const CachedResult & a, const CachedResult & b, SymbolTable & symbols)
{
    ASSERT_EQ(a.index(), b.index()) << "CachedResult variant index mismatch";

    std::visit(overloaded{
        [&](const attrs_t & va) {
            auto & vb = std::get<attrs_t>(b);
            ASSERT_EQ(va.names.size(), vb.names.size()) << "FullAttrs: different number of children";
            for (size_t i = 0; i < va.names.size(); i++)
                EXPECT_EQ(std::string_view(symbols[va.names[i]]),
                           std::string_view(symbols[vb.names[i]]))
                    << "FullAttrs: child name mismatch at index " << i;
            ASSERT_EQ(va.origins.size(), vb.origins.size()) << "FullAttrs: different number of origins";
            for (size_t i = 0; i < va.origins.size(); i++) {
                EXPECT_EQ(va.origins[i].depSource, vb.origins[i].depSource) << "Origin depSource mismatch at " << i;
                EXPECT_EQ(va.origins[i].depKey, vb.origins[i].depKey) << "Origin depKey mismatch at " << i;
                EXPECT_EQ(va.origins[i].dataPath, vb.origins[i].dataPath) << "Origin dataPath mismatch at " << i;
                EXPECT_EQ(va.origins[i].format, vb.origins[i].format) << "Origin format mismatch at " << i;
            }
            ASSERT_EQ(va.originIndices.size(), vb.originIndices.size()) << "FullAttrs: different originIndices size";
            for (size_t i = 0; i < va.originIndices.size(); i++)
                EXPECT_EQ(va.originIndices[i], vb.originIndices[i]) << "originIndices mismatch at " << i;
        },
        [&](const string_t & va) {
            auto & vb = std::get<string_t>(b);
            EXPECT_EQ(va.first, vb.first) << "String: value mismatch";
            EXPECT_EQ(va.second, vb.second) << "String: context mismatch";
        },
        [&](const placeholder_t &) {
            EXPECT_TRUE(std::holds_alternative<placeholder_t>(b));
        },
        [&](const missing_t &) {
            EXPECT_TRUE(std::holds_alternative<missing_t>(b));
        },
        [&](const misc_t &) {
            EXPECT_TRUE(std::holds_alternative<misc_t>(b));
        },
        [&](const failed_t &) {
            EXPECT_TRUE(std::holds_alternative<failed_t>(b));
        },
        [&](bool va) {
            EXPECT_EQ(va, std::get<bool>(b)) << "Bool mismatch";
        },
        [&](const int_t & va) {
            EXPECT_EQ(va.x.value, std::get<int_t>(b).x.value) << "Int mismatch";
        },
        [&](const std::vector<std::string> & va) {
            auto & vb = std::get<std::vector<std::string>>(b);
            ASSERT_EQ(va.size(), vb.size()) << "ListOfStrings: different sizes";
            for (size_t i = 0; i < va.size(); i++)
                EXPECT_EQ(va[i], vb[i]) << "ListOfStrings: element mismatch at " << i;
        },
        [&](const path_t & va) {
            EXPECT_EQ(va.path, std::get<path_t>(b).path) << "Path mismatch";
        },
        [&](const null_t &) {
            EXPECT_TRUE(std::holds_alternative<null_t>(b));
        },
        [&](const float_t & va) {
            EXPECT_DOUBLE_EQ(va.x, std::get<float_t>(b).x) << "Float mismatch";
        },
        [&](const list_t & va) {
            EXPECT_EQ(va.size, std::get<list_t>(b).size) << "List: size mismatch";
        },
    }, a);
}

} // namespace nix::eval_trace::test
