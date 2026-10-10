#include "nix/fetchers/attrs.hh"
#include "nix/util/error.hh"

#include <nlohmann/json.hpp>

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <memory>
#include <stdexcept>
#include <limits>
#include <vector>

namespace nix::fetchers {

namespace {

template<typename F>
void expectAttrsError(F && action, const Error & expected)
{
    try {
        action();
        FAIL() << "expected an attribute error";
    } catch (const Error & error) {
        EXPECT_EQ(error.info().msg.str(), expected.info().msg.str());
    }
}

struct AttrTestFailure
{
    unsigned tag;
};

using AttrConsumer = void (*)(const Attrs &);

const AttrConsumer attrsConsumers[]{
    [](const Attrs & attrs) { forceAttr(attrs.at("x")); },
    [](const Attrs & attrs) { attrsToJSON(attrs); },
    [](const Attrs & attrs) { attrsToQuery(attrs); },
    [](const Attrs & attrs) { encodeAttrs(attrs); },
    [](const Attrs & attrs) { maybeGetStrAttr(attrs, "x"); },
    [](const Attrs & attrs) { getStrAttr(attrs, "x"); },
    [](const Attrs & attrs) { maybeGetIntAttr(attrs, "x"); },
    [](const Attrs & attrs) { getIntAttr(attrs, "x"); },
    [](const Attrs & attrs) { maybeGetBoolAttr(attrs, "x"); },
    [](const Attrs & attrs) { getBoolAttr(attrs, "x"); },
};

} // namespace

TEST(LazyAttr, resolvedConsumersPreserveTypesAcrossEagerAndLazyAttrs)
{
    struct Case
    {
        ResolvedAttr value;
        nlohmann::json json;
        std::string query, encoding;
    };

    const Case cases[]{
        {std::string("hello"), "hello", "hello", "s1:x5:hello"},
        {std::string("val"), "val", "val", "s1:x3:val"},
        {std::string(300, 's'), std::string(300, 's'), std::string(300, 's'), "s1:x300:" + std::string(300, 's')},
        {uint64_t(42), uint64_t(42), "42", "i1:x42;"},
        {uint64_t(99), uint64_t(99), "99", "i1:x99;"},
        {uint64_t(1), uint64_t(1), "1", "i1:x1;"},
        {std::string(""), "", "", "s1:x0:"},
        {std::string("x\0y", 3), std::string("x\0y", 3), std::string("x\0y", 3), "s1:x3:" + std::string("x\0y", 3)},
        {uint64_t(0), uint64_t(0), "0", "i1:x0;"},
        {std::numeric_limits<uint64_t>::max(),
         std::numeric_limits<uint64_t>::max(),
         "18446744073709551615",
         "i1:x18446744073709551615;"},
        {Explicit<bool>{false}, false, "0", "b1:x0"},
        {Explicit<bool>{true}, true, "1", "b1:x1"},
    };
    for (const auto & c : cases)
        for (bool lazy : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "value=" << c.json.dump() << " lazy=" << lazy);
            unsigned calls = 0;
            auto computation = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() {
                ++calls;
                return c.value;
            }});
            Attr eager = std::visit([](const auto & v) -> Attr { return v; }, c.value);
            Attrs attrs{{"x", lazy ? Attr(LazyAttr(computation)) : eager}};
            EXPECT_EQ(maybeGetLazyAttr(attrs, "x").has_value(), lazy);
            EXPECT_EQ(calls, 0u);
            auto check = [&](auto action, unsigned forces = 1) {
                calls = 0;
                action();
                EXPECT_EQ(calls, lazy ? forces : 0u);
            };
            check([&] { EXPECT_EQ(forceAttr(attrs.at("x")), c.value); });
            check([&] { EXPECT_EQ(attrsToJSON(attrs), (nlohmann::json{{"x", c.json}})); });
            check([&] { EXPECT_EQ(attrsToQuery(attrs), (StringMap{{"x", c.query}})); });
            check([&] { EXPECT_EQ(encodeAttrs(attrs), c.encoding); });
            for (bool required : {false, true}) {
                auto string = [&] {
                    return required ? std::optional<std::string>(getStrAttr(attrs, "x")) : maybeGetStrAttr(attrs, "x");
                };
                auto integer = [&] {
                    return required ? std::optional<uint64_t>(getIntAttr(attrs, "x")) : maybeGetIntAttr(attrs, "x");
                };
                auto boolean = [&] {
                    return required ? std::optional<bool>(getBoolAttr(attrs, "x")) : maybeGetBoolAttr(attrs, "x");
                };
                check(
                    [&] {
                        if (auto v = std::get_if<std::string>(&c.value)) {
                            EXPECT_EQ(string(), *v);
                        } else {
                            expectAttrsError(
                                string,
                                Error(
                                    "input attribute '%s' is not a string %s",
                                    "x",
                                    nlohmann::json{{"x", c.json}}.dump()));
                        }
                    },
                    c.value.index() == 0 ? 1 : 2);
                check([&] {
                    if (auto v = std::get_if<uint64_t>(&c.value)) {
                        EXPECT_EQ(integer(), *v);
                    } else {
                        expectAttrsError(integer, Error("input attribute '%s' is not an integer", "x"));
                    }
                });
                check([&] {
                    if (auto v = std::get_if<Explicit<bool>>(&c.value)) {
                        EXPECT_EQ(boolean(), v->t);
                    } else {
                        expectAttrsError(boolean, Error("input attribute '%s' is not a Boolean", "x"));
                    }
                });
            }
        }
}

TEST(LazyAttr, missingAttrsDoNotForceUnrelatedComputations)
{
    EXPECT_TRUE(attrsToJSON(Attrs{}).is_null());
    EXPECT_TRUE(attrsToQuery(Attrs{}).empty());
    unsigned calls = 0;
    Attrs attrs{{"other", LazyAttr(make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
                     ++calls;
                     throw AttrTestFailure{17};
                 }}))}};
    EXPECT_FALSE(maybeGetStrAttr(attrs, "missing"));
    EXPECT_FALSE(maybeGetIntAttr(attrs, "missing"));
    EXPECT_FALSE(maybeGetBoolAttr(attrs, "missing"));
    EXPECT_FALSE(maybeGetLazyAttr(attrs, "missing"));
    const Error expected("input attribute '%s' is missing", "missing");
    expectAttrsError([&] { getStrAttr(attrs, "missing"); }, expected);
    expectAttrsError([&] { getIntAttr(attrs, "missing"); }, expected);
    expectAttrsError([&] { getBoolAttr(attrs, "missing"); }, expected);
    EXPECT_EQ(calls, 0u);
}

TEST(LazyAttr, lazyFailuresPropagateWithoutRetry)
{
    unsigned calls = 0;
    Attrs attrs{{"x", LazyAttr(make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
                     ++calls;
                     throw AttrTestFailure{42};
                 }}))}};
    for (auto consume : attrsConsumers) {
        calls = 0;
        try {
            consume(attrs);
            FAIL() << "expected the computation's exception";
        } catch (const AttrTestFailure & error) {
            EXPECT_EQ(error.tag, 42u);
        }
        EXPECT_EQ(calls, 1u);
    }
}

TEST(LazyAttr, conversionsObserveOrderedMutationAndOwnTheirOutputs)
{
    const std::string before(300, 'a'), after(400, 'c');
    for (unsigned consumer = 0; consumer != 3; ++consumer) {
        SCOPED_TRACE(consumer);
        Attrs attrs;
        std::vector<std::string> calls;
        /* Owners outlive the call, including the replaced computation. */
        auto oldC = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
            calls.push_back("old-c");
            return uint64_t(0);
        }});
        auto newC = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
            calls.push_back("c");
            return after;
        }});
        auto b = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
            calls.push_back("b");
            attrs.insert_or_assign("a", std::string("changed"));
            attrs.insert_or_assign("c", LazyAttr(newC));
            return Explicit<bool>{true};
        }});
        attrs = {{"a", before}, {"b", LazyAttr(b)}, {"c", LazyAttr(oldC)}};
        nlohmann::json json;
        StringMap query;
        std::string encoding;
        if (consumer == 0)
            json = attrsToJSON(attrs);
        if (consumer == 1)
            query = attrsToQuery(attrs);
        if (consumer == 2)
            encoding = encodeAttrs(attrs);
        EXPECT_EQ(calls, (std::vector<std::string>{"b", "c"}));
        attrs.clear();
        if (consumer == 0)
            EXPECT_EQ(json, (nlohmann::json{{"a", before}, {"b", true}, {"c", after}}));
        if (consumer == 1)
            EXPECT_EQ(query, (StringMap{{"a", before}, {"b", "1"}, {"c", after}}));
        if (consumer == 2)
            EXPECT_EQ(encoding, "s1:a300:" + before + "b1:b1s1:c400:" + after);
    }
}

TEST(LazyAttr, replacingALazyAttrKeepsItsComputedResult)
{
    const std::string expected(300, 'x');
    for (unsigned consumer = 0; consumer != 3; ++consumer) {
        ResolvedAttr result;
        unsigned calls = 0;
        {
            Attrs attrs;
            attrs.emplace(
                "x", LazyAttr(make_ref<LazyAttr::element_type>(
                         LazyAttr::element_type{[&, hold = std::make_shared<int>(0)]() -> ResolvedAttr {
                             auto alive = std::weak_ptr(hold);
                             auto result = expected;
                             ++calls;
                             attrs.insert_or_assign("x", std::string("replacement"));
                             EXPECT_FALSE(alive.expired());
                             return result;
                         }})));
            if (consumer == 0)
                result = forceAttr(attrs.at("x"));
            if (consumer == 1)
                result = maybeGetStrAttr(attrs, "x").value();
            if (consumer == 2)
                result = getStrAttr(attrs, "x");
            EXPECT_EQ(std::get<std::string>(attrs.at("x")), "replacement");
            attrs.clear();
        }
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(result, ResolvedAttr(expected));
    }
}

TEST(LazyAttr, wrongStringDiagnosticsReevaluateInMapOrder)
{
    for (bool required : {false, true})
        for (bool diagnosticThrows : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "required=" << required << " diagnosticThrows=" << diagnosticThrows);
            unsigned xCalls = 0;
            std::vector<std::string> calls;
            auto x = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
                calls.push_back("x");
                if (++xCalls == 1)
                    return uint64_t(7);
                return std::string("second");
            }});
            auto a = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
                calls.push_back("a");
                if (diagnosticThrows)
                    throw Error("diagnostic failed");
                return uint64_t(1);
            }});
            auto z = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
                calls.push_back("z");
                return Explicit<bool>{true};
            }});
            Attrs attrs{{"a", LazyAttr(a)}, {"x", LazyAttr(x)}, {"z", LazyAttr(z)}};
            const Error expected =
                diagnosticThrows
                    ? Error("diagnostic failed")
                    : Error("input attribute '%s' is not a string %s", "x", R"({"a":1,"x":"second","z":true})");
            expectAttrsError(
                [&] {
                    if (required)
                        getStrAttr(attrs, "x");
                    else
                        maybeGetStrAttr(attrs, "x");
                },
                expected);
            EXPECT_EQ(xCalls, diagnosticThrows ? 1u : 2u);
            EXPECT_EQ(
                calls,
                diagnosticThrows ? (std::vector<std::string>{"x", "a"})
                                 : (std::vector<std::string>{"x", "a", "x", "z"}));
        }
}

TEST(LazyAttr, failedAttrConstructionPreservesItsObservableState)
{
    const auto tooLong = std::string{}.max_size() + 1;
    ASSERT_GT(tooLong, std::string{}.max_size());
    for (bool nullComputation : {false, true}) {
        SCOPED_TRACE(nullComputation);
        Attr broken = uint64_t(7);
        if (nullComputation) {
            EXPECT_THROW(broken.emplace<LazyAttr>(std::shared_ptr<LazyAttr::element_type>{}), std::invalid_argument);
        } else {
            EXPECT_THROW(broken.emplace<std::string>(tooLong, 'x'), std::length_error);
        }
        Attrs attrs{{"x", std::move(broken)}};
        EXPECT_FALSE(maybeGetLazyAttr(attrs, "x"));
        if (attrs.at("x").valueless_by_exception()) {
            for (auto consume : attrsConsumers)
                EXPECT_THROW(consume(attrs), std::bad_variant_access);
        } else {
            EXPECT_EQ(forceAttr(attrs.at("x")), ResolvedAttr(uint64_t(7)));
            EXPECT_EQ(getIntAttr(attrs, "x"), 7u);
            EXPECT_EQ(attrsToJSON(attrs), (nlohmann::json{{"x", 7}}));
            EXPECT_EQ(attrsToQuery(attrs), (StringMap{{"x", "7"}}));
            EXPECT_EQ(encodeAttrs(attrs), "i1:x7;");
        }
    }
}

TEST(LazyAttr, failedResolvedConstructionKeepsTypedErrorsAndReevaluation)
{
    const auto tooLong = std::string{}.max_size() + 1;
    ASSERT_GT(tooLong, std::string{}.max_size());
    ResolvedAttr failed = uint64_t(7);
    EXPECT_THROW(failed.emplace<std::string>(tooLong, 'x'), std::length_error);
    const bool valueless = failed.valueless_by_exception();
    if (!valueless)
        ASSERT_EQ(failed, ResolvedAttr(uint64_t(7)));
    unsigned calls = 0;
    auto computation = make_ref<LazyAttr::element_type>(LazyAttr::element_type{[&]() -> ResolvedAttr {
        if (++calls == 1)
            return failed;
        return std::string("diagnostic");
    }});
    Attrs attrs{{"x", LazyAttr(computation)}};
    auto result = forceAttr(attrs.at("x"));
    EXPECT_EQ(result.valueless_by_exception(), valueless);
    if (!valueless)
        EXPECT_EQ(result, ResolvedAttr(uint64_t(7)));
    EXPECT_EQ(calls, 1u);
    for (bool required : {false, true}) {
        calls = 0;
        auto integer = [&] {
            return required ? std::optional<uint64_t>(getIntAttr(attrs, "x")) : maybeGetIntAttr(attrs, "x");
        };
        if (valueless) {
            expectAttrsError(integer, Error("input attribute '%s' is not an integer", "x"));
        } else {
            EXPECT_EQ(integer(), 7u);
        }
        EXPECT_EQ(calls, 1u);
        calls = 0;
        expectAttrsError(
            [&] {
                if (required)
                    getBoolAttr(attrs, "x");
                else
                    maybeGetBoolAttr(attrs, "x");
            },
            Error("input attribute '%s' is not a Boolean", "x"));
        EXPECT_EQ(calls, 1u);
        calls = 0;
        expectAttrsError(
            [&] {
                if (required)
                    getStrAttr(attrs, "x");
                else
                    maybeGetStrAttr(attrs, "x");
            },
            Error("input attribute '%s' is not a string %s", "x", R"({"x":"diagnostic"})"));
        EXPECT_EQ(calls, 2u);
    }
    calls = 0;
    if (valueless) {
        EXPECT_THROW(encodeAttrs(attrs), std::bad_variant_access);
    } else {
        EXPECT_EQ(encodeAttrs(attrs), "i1:x7;");
    }
    EXPECT_EQ(calls, 1u);
    /* A valueless resolved result reaches unreachable() in JSON/query;
       this test does not invoke that existing terminal path. */
    if (!valueless) {
        calls = 0;
        EXPECT_EQ(attrsToJSON(attrs), (nlohmann::json{{"x", 7}}));
        EXPECT_EQ(calls, 1u);
        calls = 0;
        EXPECT_EQ(attrsToQuery(attrs), (StringMap{{"x", "7"}}));
        EXPECT_EQ(calls, 1u);
    }
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
        "x", LazyAttr(make_ref<LazyAttr::element_type>(LazyAttr::element_type{[]() -> ResolvedAttr {
            return uint64_t(99);
        }})));
    EXPECT_EQ(encodeAttrs(attrs), "i1:x99;");
    EXPECT_EQ(decodeAttrs(encodeAttrs(attrs)), (Attrs{{"x", uint64_t(99)}}));
}

/* Every byte, both where a decimal digit belongs and where its delimiter
   belongs. Lengths use small payloads so their accepted forms are observable. */
TEST(AttrsCodec, decimalByteAlphabet)
{
    for (unsigned byte = 0; byte <= std::numeric_limits<unsigned char>::max(); ++byte) {
        SCOPED_TRACE(byte);
        std::string token(1, static_cast<char>(byte));
        bool digit = byte >= '0' && byte <= '9';
        for (const std::string & prefix : {std::string{}, std::string("1")}) {
            // Two payload bytes make an early ':' leave malformed trailing input.
            auto value = digit ? (prefix.empty() ? 0 : 10) + byte - '0' : 2;
            auto integer = "i0:" + prefix + token + ";";
            auto length = "s0:" + prefix + token + ":" + std::string(value, 'x');
            if (digit) {
                EXPECT_EQ(decodeAttrs(integer), (Attrs{{"", uint64_t(value)}}));
                EXPECT_EQ(decodeAttrs(length), (Attrs{{"", std::string(value, 'x')}}));
            } else {
                EXPECT_THROW(decodeAttrs(integer), Error);
                EXPECT_THROW(decodeAttrs(length), Error);
            }
        }
        if (byte == ';')
            EXPECT_EQ(decodeAttrs("i0:0" + token), (Attrs{{"", uint64_t(0)}}));
        else
            EXPECT_THROW(decodeAttrs("i0:0" + token), Error);
        if (byte == ':')
            EXPECT_EQ(decodeAttrs("s0:0" + token), (Attrs{{"", std::string{}}}));
        else
            EXPECT_THROW(decodeAttrs("s0:0" + token), Error);
    }
}

TEST(AttrsCodec, decimalBoundariesAndTruncations)
{
    for (auto value : {uint64_t(0), uint64_t(9), uint64_t(10), std::numeric_limits<uint64_t>::max()}) {
        auto whole = "i0:" + std::to_string(value) + ";";
        EXPECT_EQ(decodeAttrs(whole), (Attrs{{"", value}}));
        for (size_t n = 1; n < whole.size(); ++n)
            EXPECT_THROW(decodeAttrs(std::string_view(whole).substr(0, n)), Error) << n;
    }
    for (auto value : {size_t(0), size_t(9), size_t(10), size_t(20)}) {
        auto whole = "s0:" + std::to_string(value) + ":" + std::string(value, 'x');
        EXPECT_EQ(decodeAttrs(whole), (Attrs{{"", std::string(value, 'x')}}));
        for (size_t n = 1; n < whole.size(); ++n)
            EXPECT_THROW(decodeAttrs(std::string_view(whole).substr(0, n)), Error) << n;
    }
    for (const std::string & decimal :
         {std::string{},
          std::string("00"),
          std::string("01"),
          std::string("+0"),
          std::string("-0"),
          std::string("18446744073709551616"),
          std::string("99999999999999999999"),
          std::string("100000000000000000000"),
          std::string(256, '9'),
          std::string(256, '0')}) {
        SCOPED_TRACE(decimal);
        EXPECT_THROW(decodeAttrs("i0:" + decimal + ";"), Error);
        EXPECT_THROW(decodeAttrs("s0:" + decimal + ":"), Error);
    }
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
