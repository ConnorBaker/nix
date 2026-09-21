/**
 * `ObjectHash` (doc/lazy-store/04-derivation.md section 1.9, one
 * address): the strong type over the object hash of a store object.
 * `render` is the one rendering every form carries, `parse` is its exact
 * inverse and nothing else's -- in particular not an old `sha256:` row's --
 * and `of(root)` is `merkle::objectHash(root)` (object-hash.cc), whose
 * cases `MerkleHash.objectHashCases` pins.
 */
#include <gtest/gtest.h>

#include "nix/util/object-hash.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/memory-source-accessor.hh"

#include <cctype>
#include <regex>

namespace nix {

namespace {

using File = MemorySourceAccessor::File;

/* git's id of the empty tree under SHA-256, from
   sha256("tree 0\0") -- an oracle outside this code base. */
constexpr std::string_view emptyTreeHex = "6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9b939dd74decc5321";

/* The four root kinds through the one walker. */
merkle::TreeEntry rootOf(ref<MemorySourceAccessor> acc)
{
    return objectHashOf(*acc, CanonPath::root).root;
}

ref<MemorySourceAccessor> directoryRoot()
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->addFile(CanonPath{"/a"}, "one\n");
    acc->addFile(CanonPath{"/d/b"}, "two\n");
    acc->open(CanonPath{"/d/l"}, File{File::Symlink{.target = "b"}});
    return acc;
}

ref<MemorySourceAccessor> plainRoot()
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->root = File{File::Regular{.executable = false, .contents = "hello\n"}};
    return acc;
}

ref<MemorySourceAccessor> executableRoot()
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->root = File{File::Regular{.executable = true, .contents = "hello\n"}};
    return acc;
}

ref<MemorySourceAccessor> symlinkRoot()
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->root = File{File::Symlink{.target = "hello\n"}};
    return acc;
}

ObjectHash someObjectHash()
{
    return ObjectHash::of(rootOf(directoryRoot()));
}

} // namespace

/* The rendering carries the hash's own digits. */
TEST(ObjectHash, render_carries_the_hash_base16)
{
    auto oh = someObjectHash();
    EXPECT_EQ(oh.render(), "git:sha256:" + oh.hash.to_string(HashFormat::Base16, false));
}

/* `parse` inverts `render`. */
TEST(ObjectHash, parse_inverts_render)
{
    for (auto & acc : {directoryRoot(), plainRoot(), executableRoot(), symlinkRoot()}) {
        auto oh = ObjectHash::of(rootOf(acc));
        auto back = ObjectHash::parse(oh.render());
        ASSERT_TRUE(back.has_value()) << oh.render();
        EXPECT_EQ(*back, oh);
        EXPECT_EQ(ObjectHash::parseOrThrow(oh.render()), oh);
    }
}

/* What `parse` refuses: everything that is not a rendering.  The first is
   the load-bearing one -- an old `ValidPaths.hash` row holding a NAR hash
   must come back nullopt at the one read site (04 section 1.9, "The local
   database"). */
TEST(ObjectHash, parse_rejects_non_renderings)
{
    std::string hex64(emptyTreeHex);
    std::string hex63 = hex64.substr(0, 63);
    std::string hex40 = hex64.substr(0, 40);
    std::string upper = hex64;
    for (auto & c : upper)
        c = toupper(c);

    EXPECT_FALSE(ObjectHash::parse("sha256:" + hex64).has_value()) << "an old NAR-hash row";
    EXPECT_FALSE(ObjectHash::parse("git:sha1:" + hex40).has_value()) << "SHA-1 is not the algorithm";
    EXPECT_FALSE(ObjectHash::parse("git:sha256:" + hex63).has_value()) << "63 digits";
    EXPECT_FALSE(ObjectHash::parse("git:sha256:" + hex64 + "0").has_value()) << "65 digits";
    EXPECT_FALSE(ObjectHash::parse("git:sha256:" + upper).has_value()) << "uppercase";
    EXPECT_FALSE(ObjectHash::parse("git:sha256:" + hex64 + " ").has_value()) << "trailing garbage";
    EXPECT_FALSE(ObjectHash::parse("git:sha256:" + hex64 + "\n").has_value()) << "trailing newline";
    EXPECT_FALSE(ObjectHash::parse(" git:sha256:" + hex64).has_value()) << "leading garbage";
    EXPECT_FALSE(ObjectHash::parse("").has_value()) << "empty";
    EXPECT_FALSE(ObjectHash::parse("git:sha256:").has_value()) << "prefix alone";
    EXPECT_FALSE(ObjectHash::parse(hex64).has_value()) << "bare hex";
    EXPECT_FALSE(ObjectHash::parse("git:sha256-" + hex64).has_value()) << "SRI-style separator";

    EXPECT_THROW(ObjectHash::parseOrThrow("sha256:" + hex64), Error);
    EXPECT_THROW(ObjectHash::parseOrThrow(""), Error);
}

/* Equality and ordering follow the hash bytes. */
TEST(ObjectHash, equality_and_ordering)
{
    auto a = ObjectHash::of(rootOf(plainRoot()));
    auto b = ObjectHash::of(rootOf(directoryRoot()));
    auto a2 = ObjectHash::parseOrThrow(a.render());

    EXPECT_EQ(a, a2);
    EXPECT_FALSE(a != a2);
    EXPECT_NE(a, b);
    EXPECT_TRUE((a < b) != (b < a));
    EXPECT_TRUE((a <=> b) != 0);
    EXPECT_TRUE((a <=> a2) == 0);
    /* The order agrees with the rendering's lexical order, so a sorted
       column and a sorted set of values agree. */
    EXPECT_EQ(a < b, a.render() < b.render());
}

} // namespace nix
