#include "nix/fetchers/attrs.hh"

#include <nlohmann/json.hpp>

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <limits>

namespace nix::fetchers {

TEST(LazyAttr, resolveToInt)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "count", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return uint64_t(42);
        }})));
    EXPECT_EQ(maybeGetIntAttr(attrs, "count"), 42);
}

TEST(LazyAttr, resolveToString)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "name", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return std::string("hello");
        }})));
    EXPECT_EQ(maybeGetStrAttr(attrs, "name"), "hello");
}

TEST(LazyAttr, resolveToBool)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "flag", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return Explicit<bool>{true};
        }})));
    EXPECT_EQ(maybeGetBoolAttr(attrs, "flag"), true);
}

TEST(LazyAttr, attrsToJSONForcesLazy)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "x", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return uint64_t(99);
        }})));
    auto json = attrsToJSON(attrs);
    EXPECT_EQ(json["x"], 99);
}

TEST(LazyAttr, attrsToQueryForcesLazy)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "v", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return std::string("val");
        }})));
    auto query = attrsToQuery(attrs);
    EXPECT_EQ(query.at("v"), "val");
}

TEST(LazyAttr, notCalledUntilForced)
{
    int calls = 0;
    Attrs attrs;
    attrs.insert_or_assign(
        "lazy", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = [&calls]() -> ResolvedAttr {
            calls++;
            return uint64_t(1);
        }})));
    EXPECT_EQ(calls, 0);
    maybeGetIntAttr(attrs, "lazy");
    EXPECT_EQ(calls, 1);
}

/* The codec: `encodeAttrs` / `decodeAttrs`, the fetcher cache's row form. */

namespace {

/* Any bytes, a NUL and the empty string included; rapidcheck's own
   `arbitrary<std::string>` never generates a NUL (`gen/Text.hpp`). */
rc::Gen<std::string> attrsCodecBytes()
{
    return rc::gen::container<std::string>(rc::gen::arbitrary<char>());
}

/* Up to five attributes of every resolved type: names and strings of any
   bytes, integers including 0 and the maximum, both Booleans. */
Attrs attrsCodecArbitrary()
{
    Attrs attrs;
    auto n = *rc::gen::inRange(0, 6);
    for (int i = 0; i < n; ++i) {
        auto name = *attrsCodecBytes();
        switch (*rc::gen::inRange(0, 3)) {
        case 0:
            attrs.insert_or_assign(name, *attrsCodecBytes());
            break;
        case 1:
            attrs.insert_or_assign(
                name,
                *rc::gen::oneOf(
                    rc::gen::just<uint64_t>(0),
                    rc::gen::just(std::numeric_limits<uint64_t>::max()),
                    rc::gen::arbitrary<uint64_t>()));
            break;
        default:
            attrs.insert_or_assign(name, Explicit<bool>{*rc::gen::arbitrary<bool>()});
            break;
        }
    }
    return attrs;
}

/* The same name with a value of another type: `"1"` against `1`, `1`
   against `"1"`, `true` against `1`. */
Attr attrsCodecRetype(const Attr & attr)
{
    return std::visit(
        overloaded{
            [](const std::string & v) -> Attr { return uint64_t(v.size()); },
            [](uint64_t v) -> Attr { return std::to_string(v); },
            [](const Explicit<bool> & v) -> Attr { return uint64_t(v.t); },
            [](const LazyAttr &) -> Attr { return uint64_t(0); },
        },
        attr);
}

} // namespace

RC_GTEST_PROP(AttrsCodec, prop_encodeAttrs_decodeAttrs_roundTrip, ())
{
    auto attrs = attrsCodecArbitrary();
    RC_ASSERT(decodeAttrs(encodeAttrs(attrs)) == attrs);
}

RC_GTEST_PROP(AttrsCodec, prop_encodeAttrs_is_injective, ())
{
    auto a = attrsCodecArbitrary();
    /* Half the time a second independent value; otherwise a copy, of which
       half the time one attribute is retyped, so that near-misses are
       generated and not only far ones. */
    Attrs b = *rc::gen::arbitrary<bool>() ? attrsCodecArbitrary() : a;
    if (b == a && !a.empty() && *rc::gen::arbitrary<bool>()) {
        auto i = std::next(b.begin(), *rc::gen::inRange<size_t>(0, b.size()));
        i->second = attrsCodecRetype(i->second);
    }
    RC_ASSERT((encodeAttrs(a) == encodeAttrs(b)) == (a == b));
    RC_ASSERT(encodeAttrs(a) == encodeAttrs(Attrs(a)));
}

/* The format, pinned byte for byte: map order, the tags, the length
   prefixes, an empty name, a NUL inside a string, the largest integer. */
TEST(AttrsCodec, encodingIsPinned)
{
    Attrs attrs{
        {"s", std::string("x\0y", 3)},
        {"", std::string("")},
        {"n", std::numeric_limits<uint64_t>::max()},
        {"b", Explicit<bool>{true}},
    };
    std::string expected("s0:0:b1:b1i1:n18446744073709551615;s1:s3:x\0y", 44);
    EXPECT_EQ(encodeAttrs(attrs), expected);
    EXPECT_EQ(decodeAttrs(expected), attrs);
    EXPECT_EQ(encodeAttrs(Attrs{}), "");
    EXPECT_EQ(decodeAttrs(""), Attrs{});
    EXPECT_EQ(encodeAttrs(Attrs{{"z", uint64_t(0)}, {"f", Explicit<bool>{false}}}), "b1:f0i1:z0;");
}

TEST(AttrsCodec, lazyAttrIsForcedAsAttrsToJSONForcesIt)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "x", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return uint64_t(99);
        }})));
    EXPECT_EQ(encodeAttrs(attrs), "i1:x99;");
    EXPECT_EQ(decodeAttrs(encodeAttrs(attrs)), (Attrs{{"x", uint64_t(99)}}));
}

/* Every proper, non-empty prefix of a one-attribute encoding is refused
   (the empty prefix is the empty map), and so is each malformation. */
TEST(AttrsCodec, truncatedAndMalformedEncodingsThrow)
{
    for (auto & whole : {
             encodeAttrs(Attrs{{"key", std::string("value")}}),
             encodeAttrs(Attrs{{"key", uint64_t(1234)}}),
             encodeAttrs(Attrs{{"key", Explicit<bool>{true}}}),
             encodeAttrs(Attrs{{"", std::string("")}}),
         }) {
        for (size_t n = 1; n < whole.size(); ++n)
            EXPECT_THROW(decodeAttrs(std::string_view(whole).substr(0, n)), Error) << "prefix " << n << " of " << whole;
    }
    for (auto malformed : {
             "x1:k0:",                    // an unknown tag
             "b1:k2",                     // a Boolean that is neither 0 nor 1
             "i1:k12",                    // an integer without its terminator
             "i1:k;",                     // an integer without digits
             "i1:k01;",                   // a leading zero
             "i1:k18446744073709551616;", // one past the maximum
             "i1:k1a;",                   // a non-digit
             "s1:k5:abc",                 // a length past the end
             "s01:k0:",                   // a leading zero in a length
             "s1:k",                      // a name without its value
             "s1:b0:s1:a0:",              // names out of order
             "s1:a0:s1:a0:",              // a duplicate name
             "s1:a0:x",                   // a trailing byte
         })
        EXPECT_THROW(decodeAttrs(malformed), Error) << malformed;
}

} // namespace nix::fetchers
