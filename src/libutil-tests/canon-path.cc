#include "nix/util/canon-path.hh"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>

namespace nix {

TEST(CanonPath, basic)
{
    {
        CanonPath p("/");
        ASSERT_EQ(p.abs(), "/");
        ASSERT_EQ(p.rel(), "");
        ASSERT_EQ(p.baseName(), std::nullopt);
        ASSERT_EQ(p.dirOf(), std::nullopt);
        ASSERT_FALSE(p.parent());
    }

    {
        CanonPath p("/foo//");
        ASSERT_EQ(p.abs(), "/foo");
        ASSERT_EQ(p.rel(), "foo");
        ASSERT_EQ(*p.baseName(), "foo");
        ASSERT_EQ(*p.dirOf(), ""); // FIXME: do we want this?
        ASSERT_EQ(p.parent()->abs(), "/");
    }

    {
        CanonPath p("foo/bar");
        ASSERT_EQ(p.abs(), "/foo/bar");
        ASSERT_EQ(p.rel(), "foo/bar");
        ASSERT_EQ(*p.baseName(), "bar");
        ASSERT_EQ(*p.dirOf(), "/foo");
        ASSERT_EQ(p.parent()->abs(), "/foo");
    }

    {
        CanonPath p("foo//bar/");
        ASSERT_EQ(p.abs(), "/foo/bar");
        ASSERT_EQ(p.rel(), "foo/bar");
        ASSERT_EQ(*p.baseName(), "bar");
        ASSERT_EQ(*p.dirOf(), "/foo");
    }
}

TEST(CanonPath, nullBytes)
{
    std::string s = "/hello/world";
    s[8] = '\0';
    ASSERT_THROW(CanonPath("/").push(std::string(1, '\0')), BadCanonPath);
    ASSERT_THROW(CanonPath(std::string_view(s)), BadCanonPath);
    ASSERT_THROW(CanonPath(s, CanonPath::root), BadCanonPath);
}

TEST(CanonPath, fromFilename)
{
    ASSERT_THROW(CanonPath::fromFilename("."), BadCanonPath);
    ASSERT_THROW(CanonPath::fromFilename(".."), BadCanonPath);
    ASSERT_THROW(CanonPath::fromFilename(""), BadCanonPath);
    ASSERT_THROW(CanonPath::fromFilename("/.."), BadCanonPath);
    ASSERT_THROW(CanonPath::fromFilename("/."), BadCanonPath);
    ASSERT_THROW(CanonPath::fromFilename("/abc"), BadCanonPath);
    ASSERT_THROW(CanonPath::fromFilename("abc/d"), BadCanonPath);
    ASSERT_THROW(CanonPath::fromFilename("/abc/d"), BadCanonPath);
    ASSERT_EQ(CanonPath::fromFilename("abc").rel(), "abc");
}

TEST(CanonPath, fromFilenameMatchesTheCompleteShortByteDomain)
{
    size_t cases = 0, accepted = 0;
    auto check = [&](std::string_view word) {
        ++cases;
        SCOPED_TRACE(::testing::Message() << "case=" << cases << " length=" << word.size());
        bool hasNull = std::any_of(word.begin(), word.end(), [](char c) { return c == '\0'; });
        bool permitted = !word.empty() && word != "." && word != ".."
                         && std::all_of(word.begin(), word.end(), [](char c) { return c != '\0' && c != '/'; });
        accepted += permitted;
        try {
            auto result = CanonPath::fromFilename(word);
            if (!permitted) {
                ADD_FAILURE() << "accepted an invalid filename";
                return;
            }
            EXPECT_EQ(result.abs(), "/" + std::string(word));
            EXPECT_EQ(result.rel(), word);
            EXPECT_EQ(result.baseName(), std::optional{word});
            EXPECT_EQ(result.parent(), CanonPath::root);
            EXPECT_EQ(std::ranges::distance(result), 1);
            EXPECT_EQ(CanonPath(result.abs()), result);
        } catch (const BadCanonPath & error) {
            EXPECT_FALSE(permitted);
            EXPECT_NE(
                std::string(error.what()).find(hasNull ? "must not contain null" : "invalid filename"),
                std::string::npos);
        } catch (const std::exception & error) {
            ADD_FAILURE() << "wrong exception class: " << error.what();
        }
    };

    check("");
    for (unsigned int first = 0; first < 256; ++first) {
        std::string word(1, static_cast<char>(first));
        check(word);
        for (unsigned int second = 0; second < 256; ++second)
            check(word + static_cast<char>(second));
    }
    EXPECT_EQ(cases, 65793u);
    EXPECT_EQ(accepted, 64768u);

    std::string filenameBytes;
    for (unsigned int byte = 1; byte < 256; ++byte)
        if (byte != '/')
            filenameBytes += static_cast<char>(byte);
    for (size_t length : {3u, 15u, 16u, 255u, 256u, 257u, 513u}) {
        /* Include longer dot-only names, every other legal byte, the old
           reserve boundary, and each invalid byte at every position. */
        for (char byte : filenameBytes)
            check(std::string(length, byte));
        std::string word(length, '\0');
        for (size_t i = 0; i < length; ++i)
            word[i] = filenameBytes[i % filenameBytes.size()];
        check(word);
        for (size_t i = 0; i < length; ++i) {
            char previous = word[i];
            for (char invalid : {'\0', '/'}) {
                word[i] = invalid;
                check(word);
            }
            word[i] = previous;
        }
    }
    EXPECT_EQ(cases, 70208u);
    EXPECT_EQ(accepted, 66553u);
}

TEST(CanonPath, from_existing)
{
    CanonPath p0("foo//bar/");
    {
        CanonPath p("/baz//quux/", p0);
        ASSERT_EQ(p.abs(), "/baz/quux");
        ASSERT_EQ(p.rel(), "baz/quux");
        ASSERT_EQ(*p.baseName(), "quux");
        ASSERT_EQ(*p.dirOf(), "/baz");
    }
    {
        CanonPath p("baz//quux/", p0);
        ASSERT_EQ(p.abs(), "/foo/bar/baz/quux");
        ASSERT_EQ(p.rel(), "foo/bar/baz/quux");
        ASSERT_EQ(*p.baseName(), "quux");
        ASSERT_EQ(*p.dirOf(), "/foo/bar/baz");
    }
}

TEST(CanonPath, pop)
{
    CanonPath p("foo/bar/x");
    ASSERT_EQ(p.abs(), "/foo/bar/x");
    p.pop();
    ASSERT_EQ(p.abs(), "/foo/bar");
    p.pop();
    ASSERT_EQ(p.abs(), "/foo");
    p.pop();
    ASSERT_EQ(p.abs(), "/");
}

TEST(CanonPath, removePrefix)
{
    CanonPath p1("foo/bar");
    CanonPath p2("foo/bar/a/b/c");
    ASSERT_EQ(p2.removePrefix(p1).abs(), "/a/b/c");
    ASSERT_EQ(p1.removePrefix(p1).abs(), "/");
    ASSERT_EQ(p1.removePrefix(CanonPath("/")).abs(), "/foo/bar");
}

TEST(CanonPath, iter)
{
    {
        CanonPath p("a//foo/bar//");
        std::vector<std::string_view> ss;
        for (auto & c : p)
            ss.push_back(c);
        ASSERT_EQ(ss, std::vector<std::string_view>({"a", "foo", "bar"}));
    }

    {
        CanonPath p("/");
        std::vector<std::string_view> ss;
        for (auto & c : p)
            ss.push_back(c);
        ASSERT_EQ(ss, std::vector<std::string_view>());
    }
}

TEST(CanonPath, concat)
{
    {
        CanonPath p1("a//foo/bar//");
        CanonPath p2("xyzzy/bla");
        ASSERT_EQ((p1 / p2).abs(), "/a/foo/bar/xyzzy/bla");
    }

    {
        CanonPath p1("/");
        CanonPath p2("/a/b");
        ASSERT_EQ((p1 / p2).abs(), "/a/b");
    }

    {
        CanonPath p1("/a/b");
        CanonPath p2("/");
        ASSERT_EQ((p1 / p2).abs(), "/a/b");
    }

    {
        CanonPath p("/foo/bar");
        ASSERT_EQ((p / "x").abs(), "/foo/bar/x");
    }

    {
        CanonPath p("/");
        ASSERT_EQ((p / "foo" / "bar").abs(), "/foo/bar");
    }

    const std::vector<std::vector<std::string>> components{
        {},
        {"a"},
        {"nested", "leaf"},
        {std::string(13, 'x')},
        {std::string(14, 'x')},
        {std::string(15, 'x')},
        {std::string(16, 'x')},
    };
    for (const auto & leftComponents : components)
        for (const auto & rightComponents : components) {
            const CanonPath left(leftComponents), right(rightComponents);
            auto combined = leftComponents;
            combined.insert(combined.end(), rightComponents.begin(), rightComponents.end());
            const CanonPath expected(combined);
            SCOPED_TRACE(::testing::Message() << left.abs() << " / " << right.abs());
            EXPECT_EQ((left / right).abs(), expected.abs());
            if (&leftComponents == &rightComponents)
                EXPECT_EQ((left / left).abs(), expected.abs());
            EXPECT_EQ(left.abs(), CanonPath(leftComponents).abs());
            EXPECT_EQ(right.abs(), CanonPath(rightComponents).abs());
        }
}

TEST(CanonPath, componentConcatPreservesBytesAndAliases)
{
    const std::array parents{CanonPath{"/"}, CanonPath{"/" + std::string(40, 'p')}};
    const std::array prefixes{std::string{"/"}, parents[1].abs() + "/"};
    size_t cases = 0, nulls = 0;
    auto check = [&](std::string_view component) {
        // Slash and exact dot/dotdot are unchanged assertion preconditions.
        if (component.find('/') != component.npos || component == "." || component == "..")
            return;
        ++cases;
        bool hasNull = component.find('\0') != component.npos;
        nulls += hasNull;
        for (size_t i = 0; i < parents.size(); ++i) {
            const auto before = parents[i].abs();
            if (hasNull)
                EXPECT_THROW((void) (parents[i] / component), BadCanonPath);
            else
                EXPECT_EQ((parents[i] / component).abs(), prefixes[i] + std::string(component));
            EXPECT_EQ(parents[i].abs(), before);
        }
    };
    check("");
    for (unsigned first = 0; first < 256; ++first) {
        std::string word(1, static_cast<char>(first));
        check(word);
        for (unsigned second = 0; second < 256; ++second)
            check(word + static_cast<char>(second));
    }
    EXPECT_EQ(cases, 65279u);
    EXPECT_EQ(nulls, 510u);
    for (size_t length = 1; length <= 64; ++length) {
        const std::string name(length, 'q');
        check(name);
        const CanonPath parent{"/" + name};
        EXPECT_EQ((parent / "").abs(), "/" + name + "/");
        EXPECT_EQ((parent / "x").abs(), "/" + name + "/x");
    }
    for (const auto & parent : parents) {
        auto name = std::string_view(parent.abs()).substr(1);
        check(name);
        if (!name.empty())
            check(name.substr(1));
    }
    EXPECT_EQ((CanonPath(CanonPath::unchecked_t{}, "") / "x").abs(), "x");
    EXPECT_EQ((CanonPath(CanonPath::unchecked_t{}, "q") / "x").abs(), "qx");
    EXPECT_EQ((CanonPath(CanonPath::unchecked_t{}, std::string("a\0b", 3)) / "x").abs(), std::string("a\0b/x", 5));
}

TEST(CanonPath, within)
{
    ASSERT_TRUE(CanonPath("foo").isWithin(CanonPath("foo")));
    ASSERT_FALSE(CanonPath("foo").isWithin(CanonPath("bar")));
    ASSERT_FALSE(CanonPath("foo").isWithin(CanonPath("fo")));
    ASSERT_TRUE(CanonPath("foo/bar").isWithin(CanonPath("foo")));
    ASSERT_FALSE(CanonPath("foo").isWithin(CanonPath("foo/bar")));
    ASSERT_TRUE(CanonPath("/foo/bar/default.nix").isWithin(CanonPath("/")));
    ASSERT_TRUE(CanonPath("/").isWithin(CanonPath("/")));
}

TEST(CanonPath, sort)
{
    ASSERT_FALSE(CanonPath("foo") < CanonPath("foo"));
    ASSERT_TRUE(CanonPath("foo") < CanonPath("foo/bar"));
    ASSERT_TRUE(CanonPath("foo/bar") < CanonPath("foo!"));
    ASSERT_FALSE(CanonPath("foo!") < CanonPath("foo"));
    ASSERT_TRUE(CanonPath("foo") < CanonPath("foo!"));
}

TEST(CanonPath, allowed)
{
    std::set<CanonPath> allowed{
        CanonPath("foo/bar"),
        CanonPath("foo!"),
        CanonPath("xyzzy"),
        CanonPath("a/b/c"),
    };

    ASSERT_TRUE(CanonPath("foo/bar").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("foo/bar/bla").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("foo").isAllowed(allowed));
    ASSERT_FALSE(CanonPath("bar").isAllowed(allowed));
    ASSERT_FALSE(CanonPath("bar/a").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("a").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("a/b").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("a/b/c").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("a/b/c/d").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("a/b/c/d/e").isAllowed(allowed));
    ASSERT_FALSE(CanonPath("a/b/a").isAllowed(allowed));
    ASSERT_FALSE(CanonPath("a/b/d").isAllowed(allowed));
    ASSERT_FALSE(CanonPath("aaa").isAllowed(allowed));
    ASSERT_FALSE(CanonPath("zzz").isAllowed(allowed));
    ASSERT_TRUE(CanonPath("/").isAllowed(allowed));
}

TEST(CanonPath, separatorsSortBeforeEveryFilenameByte)
{
    const CanonPath parent("a");
    const CanonPath child("a/child");
    EXPECT_LT(parent, child);
    /* NUL and '/' cannot occur within a filename. Every other byte can,
       including UTF-8 bytes that plain char makes negative on some hosts. */
    for (unsigned int byte = 1; byte <= 255; ++byte) {
        if (byte == '/')
            continue;
        SCOPED_TRACE(byte);
        const CanonPath neighbour(std::string("a") + static_cast<char>(byte));
        EXPECT_LT(child, neighbour);
        EXPECT_TRUE(parent.isAllowed({child, neighbour}));
    }
}

TEST(CanonPath, orderingAcrossWordBoundaries)
{
    /* Every possible first differing filename byte at every offset across
       two word boundaries. The oppositely ordered next byte also rejects
       ordering by a whole integer word. The suffix keeps every component
       valid and leaves a complete word containing the differing byte. */
    for (size_t offset = 1; offset <= 18; ++offset) {
        const std::string prefix(offset - 1, 'q');
        std::vector<std::pair<unsigned int, CanonPath>> paths;
        for (unsigned int byte = 1; byte <= 255; ++byte)
            if (byte != '/')
                paths.emplace_back(
                    byte,
                    CanonPath(
                        "/" + prefix + static_cast<char>(byte) + static_cast<char>(256 - byte) + "suffix-of-name"));
        for (auto & [a, x] : paths)
            for (auto & [b, y] : paths)
                if ((x <=> y) != (a <=> b))
                    FAIL() << "offset " << offset << ", bytes " << a << ", " << b;
    }

    /* A separator is below every admitted byte, including control bytes
       below '/' in ordinary byte order. Neither side is normalised away. */
    for (size_t offset = 2; offset <= 18; ++offset) {
        const std::string prefix(offset - 1, 'q');
        const CanonPath parent("/" + prefix);
        const CanonPath child("/" + prefix + "/child-suffix");
        ASSERT_LT(parent, child);
        for (unsigned int byte = 1; byte <= 255; ++byte) {
            if (byte == '/')
                continue;
            const CanonPath neighbour("/" + prefix + static_cast<char>(byte) + "suffix-of-name");
            if (!(child < neighbour && neighbour > child && parent < neighbour))
                FAIL() << "separator offset " << offset << ", byte " << byte;
        }
    }

    /* Empty root, equal paths, and either exhausted side at every length
       through three word boundaries, including incomplete final words. */
    std::vector<CanonPath> prefixes;
    for (size_t length = 0; length <= 24; ++length)
        prefixes.emplace_back("/" + std::string(length, 'q'));
    for (size_t a = 0; a < prefixes.size(); ++a)
        for (size_t b = 0; b < prefixes.size(); ++b)
            if ((prefixes[a] <=> prefixes[b]) != (a <=> b))
                FAIL() << "prefix lengths " << a << ", " << b;
}

TEST(CanonPath, allowedMatchesComponentPrefixesForEverySubset)
{
    using Components = std::vector<std::string>;
    const std::vector<Components> nodes{
        {},
        {"a"},
        {"a", "b"},
        {"a", "b", "c"},
        {"a", "c"},
        {"a!"},
        {"a!", "b"},
        {"a."},
        {"a0"},
        {"aa"},
        {"a\xc3\xa9"},
        {"b"},
    };
    auto queries = nodes;
    queries.insert(queries.end(), {{"a", "missing"}, {"a!", "missing"}, {"b", "nested"}, {"missing"}});

    auto prefixOf = [](const Components & prefix, const Components & path) {
        return prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
    };
    std::vector<CanonPath> paths;
    for (auto & node : nodes)
        paths.emplace_back(node);
    std::vector<CanonPath> queryPaths;
    for (auto & query : queries)
        queryPaths.emplace_back(query);

    /* All 4096 sets, including empty/root-containing sets, nested and
       disjoint prefixes, and neighbours on either side of a separator. */
    for (size_t subset = 0; subset < (size_t{1} << nodes.size()); ++subset) {
        std::set<CanonPath> allowed;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (subset & (size_t{1} << i))
                allowed.insert(paths[i]);
        for (size_t q = 0; q < queries.size(); ++q) {
            bool expected = false;
            for (size_t i = 0; i < nodes.size(); ++i)
                if (subset & (size_t{1} << i))
                    expected |= prefixOf(nodes[i], queries[q]) || prefixOf(queries[q], nodes[i]);
            EXPECT_EQ(queryPaths[q].isAllowed(allowed), expected) << "subset " << subset << ", path " << queryPaths[q];
        }
    }
}

TEST(CanonPath, makeRelative)
{
    CanonPath d("/foo/bar");
    ASSERT_EQ(d.makeRelative(CanonPath("/foo/bar")), ".");
    ASSERT_EQ(d.makeRelative(CanonPath("/foo")), "..");
    ASSERT_EQ(d.makeRelative(CanonPath("/")), "../..");
    ASSERT_EQ(d.makeRelative(CanonPath("/foo/bar/xyzzy")), "xyzzy");
    ASSERT_EQ(d.makeRelative(CanonPath("/foo/bar/xyzzy/bla")), "xyzzy/bla");
    ASSERT_EQ(d.makeRelative(CanonPath("/foo/xyzzy/bla")), "../xyzzy/bla");
    ASSERT_EQ(d.makeRelative(CanonPath("/xyzzy/bla")), "../../xyzzy/bla");
}
} // namespace nix
