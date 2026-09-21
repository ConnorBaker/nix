#include "nix/util/canon-path.hh"

#include <algorithm>
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
