#include "nix/fetchers/attrs.hh"

#include <nlohmann/json.hpp>

#include <charconv>
#include <limits>
#include <type_traits>
#include <utility>

namespace nix::fetchers {

/* Borrow an eager variant, or own a lazy result through the continuation. */
template<typename Consumer>
static decltype(auto) withAttr(const Attr & attr, Consumer && consume)
{
    auto lazy = std::visit(
        overloaded{
            [](const LazyAttr & lazy) { return &lazy; },
            [](const auto &) -> const LazyAttr * { return nullptr; },
        },
        attr);
    if (lazy)
        return consume((*lazy)->compute());
    return consume(attr);
}

ResolvedAttr forceAttr(const Attr & attr)
{
    return std::visit(
        overloaded{
            [](const LazyAttr & lazy) -> ResolvedAttr { return lazy->compute(); },
            [](const auto & value) -> ResolvedAttr { return value; },
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
    for (auto & attr : attrs)
        json[attr.first] = withAttr(attr.second, [](auto && resolved) -> nlohmann::json {
            if (auto v = std::get_if<uint64_t>(&resolved))
                return *v;
            else if (auto v = std::get_if<std::string>(&resolved))
                return std::move(*v);
            else if (auto v = std::get_if<Explicit<bool>>(&resolved))
                return v->t;
            else
                unreachable();
        });
    return json;
}

/* A decimal number appended without a string of its own. */
static void appendDecimal(std::string & out, uint64_t n)
{
    char buf[20];
    auto end = std::to_chars(buf, buf + sizeof buf, n).ptr;
    out.append(buf, end - buf);
}

static void appendLengthPrefixed(std::string & out, std::string_view bytes)
{
    appendDecimal(out, bytes.size());
    out += ':';
    out += bytes;
}

std::string encodeAttrs(const Attrs & attrs)
{
    std::string out;
    /* Room for the whole encoding before a byte of it is written: the
       memo's key and row are encoded once per node of a naming walk, and a
       string that grows from nothing reallocates four times over the
       hundred bytes a key takes (`05` section 6).  The estimate is an upper
       bound for strings and a generous one for the rest; a wrong guess
       costs only what the growth would have cost anyway. */
    size_t room = 0;
    for (auto & [name, attr] : attrs) {
        room += name.size() + 24;
        if (auto s = std::get_if<std::string>(&attr))
            room += s->size();
    }
    out.reserve(room);
    for (auto & [name, attr] : attrs)
        withAttr(attr, [&](auto && resolved) {
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
                        appendDecimal(out, v);
                        out += ';';
                    },
                    [&](const Explicit<bool> & v) {
                        out += 'b';
                        appendLengthPrefixed(out, name);
                        out += v.t ? '1' : '0';
                    },
                    [](const LazyAttr &) { unreachable(); },
                },
                resolved);
        });
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

template<typename T>
static std::optional<T> maybeGetAttr(const Attrs & attrs, const std::string & name)
{
    auto i = attrs.find(name);
    if (i == attrs.end())
        return {};
    return withAttr(i->second, [&](auto && resolved) -> std::optional<T> {
        if (auto value = std::get_if<T>(&resolved))
            return std::move(*value);
        if constexpr (std::is_same_v<T, std::string>)
            throw Error("input attribute '%s' is not a string %s", name, attrsToJSON(attrs).dump());
        else
            throw Error(
                std::is_same_v<T, uint64_t> ? "input attribute '%s' is not an integer"
                                            : "input attribute '%s' is not a Boolean",
                name);
    });
}

template<typename T>
static T requireAttr(std::optional<T> value, const std::string & name)
{
    if (!value)
        throw Error("input attribute '%s' is missing", name);
    return std::move(*value);
}

std::optional<std::string> maybeGetStrAttr(const Attrs & attrs, const std::string & name)
{
    return maybeGetAttr<std::string>(attrs, name);
}

std::string getStrAttr(const Attrs & attrs, const std::string & name)
{
    return requireAttr(maybeGetStrAttr(attrs, name), name);
}

std::optional<uint64_t> maybeGetIntAttr(const Attrs & attrs, const std::string & name)
{
    return maybeGetAttr<uint64_t>(attrs, name);
}

uint64_t getIntAttr(const Attrs & attrs, const std::string & name)
{
    return requireAttr(maybeGetIntAttr(attrs, name), name);
}

std::optional<bool> maybeGetBoolAttr(const Attrs & attrs, const std::string & name)
{
    return maybeGetAttr<Explicit<bool>>(attrs, name).transform([](auto value) { return value.t; });
}

bool getBoolAttr(const Attrs & attrs, const std::string & name)
{
    return requireAttr(maybeGetBoolAttr(attrs, name), name);
}

StringMap attrsToQuery(const Attrs & attrs)
{
    StringMap query;
    for (auto & attr : attrs)
        query.insert_or_assign(attr.first, withAttr(attr.second, [](auto && resolved) -> std::string {
                                   if (auto v = std::get_if<uint64_t>(&resolved))
                                       return fmt("%d", *v);
                                   else if (auto v = std::get_if<std::string>(&resolved))
                                       return std::move(*v);
                                   else if (auto v = std::get_if<Explicit<bool>>(&resolved))
                                       return v->t ? "1" : "0";
                                   else
                                       unreachable();
                               }));
    return query;
}

Hash getRevAttr(const Attrs & attrs, const std::string & name)
{
    return Hash::parseAny(getStrAttr(attrs, name), HashAlgorithm::SHA1);
}

} // namespace nix::fetchers
