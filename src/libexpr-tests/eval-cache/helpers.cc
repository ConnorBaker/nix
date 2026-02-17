#include "helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_cache::test {

void assertAttrValueEquals(const AttrValue & a, const AttrValue & b, SymbolTable & symbols)
{
    ASSERT_EQ(a.index(), b.index()) << "AttrValue variant index mismatch";

    std::visit(overloaded{
        [&](const std::vector<Symbol> & va) {
            auto & vb = std::get<std::vector<Symbol>>(b);
            ASSERT_EQ(va.size(), vb.size()) << "FullAttrs: different number of children";
            for (size_t i = 0; i < va.size(); i++)
                EXPECT_EQ(std::string_view(symbols[va[i]]),
                           std::string_view(symbols[vb[i]]))
                    << "FullAttrs: child name mismatch at index " << i;
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

} // namespace nix::eval_cache::test
