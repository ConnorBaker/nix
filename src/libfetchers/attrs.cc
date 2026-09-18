#include "nix/fetchers/attrs.hh"

#include <nlohmann/json.hpp>

#include <limits>

namespace nix::fetchers {

ResolvedAttr forceAttr(const Attr & attr)
{
    return std::visit(
        overloaded{
            [](const LazyAttr & lazy) -> ResolvedAttr { return lazy->compute(); },
            [](const std::string & v) -> ResolvedAttr { return v; },
            [](uint64_t v) -> ResolvedAttr { return v; },
            [](const Explicit<bool> & v) -> ResolvedAttr { return v; },
        },
        attr);
}

Attrs jsonToAttrs(const nlohmann::json & json)
{
    Attrs attrs;

    for (auto & i : json.items()) {
        if (i.value().is_number())
            attrs.emplace(i.key(), i.value().get<uint64_t>());
        else if (i.value().is_string())
            attrs.emplace(i.key(), i.value().get<std::string>());
        else if (i.value().is_boolean())
            attrs.emplace(i.key(), Explicit<bool>{i.value().get<bool>()});
        else
            throw Error("unsupported input attribute type in lock file");
    }

    return attrs;
}

nlohmann::json attrsToJSON(const Attrs & attrs)
{
    nlohmann::json json;
    for (auto & attr : attrs) {
        auto resolved = forceAttr(attr.second);
        if (auto v = std::get_if<uint64_t>(&resolved)) {
            json[attr.first] = *v;
        } else if (auto v = std::get_if<std::string>(&resolved)) {
            json[attr.first] = *v;
        } else if (auto v = std::get_if<Explicit<bool>>(&resolved)) {
            json[attr.first] = v->t;
        } else
            unreachable();
    }
    return json;
}

static void appendLengthPrefixed(std::string & out, std::string_view bytes)
{
    out += std::to_string(bytes.size());
    out += ':';
    out += bytes;
}

std::string encodeAttrs(const Attrs & attrs)
{
    std::string out;
    for (auto & [name, attr] : attrs) {
        auto resolved = forceAttr(attr);
        std::visit(
            overloaded{
                [&](const std::string & v) {
                    out += 's';
                    appendLengthPrefixed(out, name);
                    appendLengthPrefixed(out, v);
                },
                [&](uint64_t v) {
                    out += 'i';
                    appendLengthPrefixed(out, name);
                    out += std::to_string(v);
                    out += ';';
                },
                [&](const Explicit<bool> & v) {
                    out += 'b';
                    appendLengthPrefixed(out, name);
                    out += v.t ? '1' : '0';
                },
            },
            resolved);
    }
    return out;
}

namespace {

/**
 * A cursor over an encoding; every read checks the end first.
 */
struct AttrsDecoder
{
    std::string_view rest;

    [[noreturn]] void fail(const char * what)
    {
        throw Error("malformed attribute encoding: %s", what);
    }

    char byte()
    {
        if (rest.empty())
            fail("unexpected end");
        char c = rest.front();
        rest.remove_prefix(1);
        return c;
    }

    /**
     * A canonical decimal number up to and including `terminator`: at
     * least one digit, no leading zero unless the number is zero, and
     * fitting a `uint64_t`.
     */
    uint64_t decimal(char terminator)
    {
        uint64_t value = 0;
        size_t digits = 0;
        for (;;) {
            char c = byte();
            if (c == terminator)
                break;
            if (c < '0' || c > '9')
                fail("expected a digit");
            if (digits == 1 && value == 0)
                fail("a number with a leading zero");
            uint64_t digit = c - '0';
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
                fail("a number too large");
            value = value * 10 + digit;
            digits++;
        }
        if (digits == 0)
            fail("expected a number");
        return value;
    }

    std::string_view bytes(uint64_t length)
    {
        if (length > rest.size())
            fail("a length past the end");
        auto result = rest.substr(0, length);
        rest.remove_prefix(length);
        return result;
    }

    std::string_view lengthPrefixed()
    {
        return bytes(decimal(':'));
    }
};

} // namespace

Attrs decodeAttrs(std::string_view encoded)
{
    AttrsDecoder in{encoded};
    Attrs attrs;
    std::optional<std::string_view> previousName;
    while (!in.rest.empty()) {
        char tag = in.byte();
        auto name = in.lengthPrefixed();
        if (previousName && !(*previousName < name))
            in.fail("attribute names out of order");
        Attr value;
        switch (tag) {
        case 's':
            value = std::string(in.lengthPrefixed());
            break;
        case 'i':
            value = in.decimal(';');
            break;
        case 'b': {
            char c = in.byte();
            if (c == '0')
                value = Explicit<bool>{false};
            else if (c == '1')
                value = Explicit<bool>{true};
            else
                in.fail("expected '0' or '1' for a Boolean");
            break;
        }
        default:
            in.fail("unknown type tag");
        }
        attrs.emplace_hint(attrs.end(), std::string(name), std::move(value));
        previousName = name;
    }
    return attrs;
}

std::optional<LazyAttr> maybeGetLazyAttr(const Attrs & attrs, const std::string & name)
{
    auto i = attrs.find(name);
    if (i == attrs.end())
        return {};
    if (auto v = std::get_if<LazyAttr>(&i->second))
        return *v;
    return {};
}

std::optional<std::string> maybeGetStrAttr(const Attrs & attrs, const std::string & name)
{
    auto i = attrs.find(name);
    if (i == attrs.end())
        return {};
    auto resolved = forceAttr(i->second);
    if (auto v = std::get_if<std::string>(&resolved))
        return *v;
    throw Error("input attribute '%s' is not a string %s", name, attrsToJSON(attrs).dump());
}

std::string getStrAttr(const Attrs & attrs, const std::string & name)
{
    auto s = maybeGetStrAttr(attrs, name);
    if (!s)
        throw Error("input attribute '%s' is missing", name);
    return *s;
}

std::optional<uint64_t> maybeGetIntAttr(const Attrs & attrs, const std::string & name)
{
    auto i = attrs.find(name);
    if (i == attrs.end())
        return {};
    auto resolved = forceAttr(i->second);
    if (auto v = std::get_if<uint64_t>(&resolved))
        return *v;
    throw Error("input attribute '%s' is not an integer", name);
}

uint64_t getIntAttr(const Attrs & attrs, const std::string & name)
{
    auto s = maybeGetIntAttr(attrs, name);
    if (!s)
        throw Error("input attribute '%s' is missing", name);
    return *s;
}

std::optional<bool> maybeGetBoolAttr(const Attrs & attrs, const std::string & name)
{
    auto i = attrs.find(name);
    if (i == attrs.end())
        return {};
    auto resolved = forceAttr(i->second);
    if (auto v = std::get_if<Explicit<bool>>(&resolved))
        return v->t;
    throw Error("input attribute '%s' is not a Boolean", name);
}

bool getBoolAttr(const Attrs & attrs, const std::string & name)
{
    auto s = maybeGetBoolAttr(attrs, name);
    if (!s)
        throw Error("input attribute '%s' is missing", name);
    return *s;
}

StringMap attrsToQuery(const Attrs & attrs)
{
    StringMap query;
    for (auto & attr : attrs) {
        auto resolved = forceAttr(attr.second);
        if (auto v = std::get_if<uint64_t>(&resolved)) {
            query.insert_or_assign(attr.first, fmt("%d", *v));
        } else if (auto v = std::get_if<std::string>(&resolved)) {
            query.insert_or_assign(attr.first, *v);
        } else if (auto v = std::get_if<Explicit<bool>>(&resolved)) {
            query.insert_or_assign(attr.first, v->t ? "1" : "0");
        } else
            unreachable();
    }
    return query;
}

Hash getRevAttr(const Attrs & attrs, const std::string & name)
{
    return Hash::parseAny(getStrAttr(attrs, name), HashAlgorithm::SHA1);
}

} // namespace nix::fetchers
