#include "nix/expr/value.hh"
#include "nix/expr/static-string-data.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval.hh"
#include <type_traits>

#include "nix/store/tests/libstore.hh"
#include <gtest/gtest.h>

namespace nix {

class ValueTest : public LibStoreTest
{};

TEST_F(ValueTest, unsetValue)
{
    Value unsetValue;
    ASSERT_EQ(false, unsetValue.isValid());
    ASSERT_EQ(nThunk, unsetValue.type</*invalidIsThunk=*/true>());
}

TEST_F(ValueTest, vInt)
{
    Value vInt;
    vInt.mkInt(42);
    ASSERT_EQ(true, vInt.isValid());
}

/* The boundary the compiler holds (doc/lazy-store/04-derivation.md, section
   1.2): a string value's bytes are unreachable from outside `Value`'s friends,
   and the exec token cannot be made, copied or conjured by a cast. */
template<typename V>
concept ReadsBytes = requires(const V & v) { v.string_view(); } || requires(const V & v) { v.c_str(); }
                     || requires(const V & v) { v.string_data(); };
static_assert(!ReadsBytes<Value>);
static_assert(!std::is_default_constructible_v<FinishedEvaluation>);
static_assert(!std::is_copy_constructible_v<FinishedEvaluation>);
static_assert(!std::is_trivially_copyable_v<FinishedEvaluation>);

TEST_F(ValueTest, staticString)
{
    Value vStr1;
    Value vStr2;
    vStr1.mkStringNoCopy("foo"_sds);
    vStr2.mkStringNoCopy("foo"_sds);

    auto & sd1 = RawValueBytes::data(vStr1);
    auto & sd2 = RawValueBytes::data(vStr2);

    // The strings should be the same
    ASSERT_EQ(sd1.view(), sd2.view());

    // The strings should also be backed by the same (static) allocation
    ASSERT_EQ(&sd1, &sd2);
}

} // namespace nix
