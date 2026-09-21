#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/util/merkle-hash.hh"
#include "nix/util/git.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/archive.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/util.hh"

#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/tests/setting-scopes.hh"
#include "nix/util/tests/source-accessor-gen.hh"

namespace nix {

using merkle::Mode;
using merkle::TreeEntry;
using File = MemorySourceAccessor::File;

static Hash treeIdOf(const merkle::Tree & tree)
{
    return merkle::treeId(merkle::serialiseTree(tree));
}

static ref<MemorySourceAccessor> accessorOf(File file)
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->root = std::move(file);
    return acc;
}

static File regular(std::string contents, bool executable = false)
{
    return File{File::Regular{.executable = executable, .contents = std::move(contents)}};
}

static File symlink(std::string target)
{
    return File{File::Symlink{.target = std::move(target)}};
}

static File directory(std::initializer_list<std::pair<const std::string, File>> entries)
{
    File::Directory d;
    for (auto & [name, file] : entries)
        d.entries.emplace(name, file);
    return File{std::move(d)};
}

/* The fixed tree of the id tests. */
static File exampleTree()
{
    return directory({
        {"a", regular("hello\n")},
        {"b", regular("#!/bin/sh\nexit 0\n", true)},
        {"d", directory({{"c", regular("")}, {"l", symlink("../a")}})},
        {"e", directory({})},
    });
}

/* The NAR's node length by the arithmetic alone: no bytes produced. */
static uint64_t narNodeSize(SourceAccessor & acc, const CanonPath & p)
{
    auto st = acc.lstat(p);
    switch (st.type) {
    case SourceAccessor::tRegular:
        return merkle::nar::regular(*st.fileSize, st.isExecutable);
    case SourceAccessor::tSymlink:
        return merkle::nar::symlink(acc.readLink(p).size());
    case SourceAccessor::tDirectory: {
        merkle::nar::Directory d;
        for (auto & [name, actual] : unhackedEntries(acc, p))
            d.add(name, narNodeSize(acc, p / actual));
        return d.finish();
    }
    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
        unreachable();
    }
    unreachable();
}

TEST(MerkleHash, blobHeaderAndIncrementalHasher)
{
    StringSink header;
    merkle::feedHeader(header, "blob", 6);
    EXPECT_EQ(header.s, std::string("blob 6\0", 7));

    StringSink treeHeader;
    merkle::feedHeader(treeHeader, "tree", 0);
    EXPECT_EQ(treeHeader.s, std::string("tree 0\0", 7));

    /* git's ids of the empty blob and the empty tree under SHA-256, to pin
       the framing to git's: `git hash-object --stdin < /dev/null` and
       `git hash-object -t tree --stdin < /dev/null` in a repository made
       with `--object-format=sha256` (git 2.54). */
    merkle::BlobHasher empty{0};
    EXPECT_EQ(
        empty.finish().to_string(HashFormat::Base16, false),
        "473a0f4c3be8a93681a267e3b1e9a7dcda1185436fe141f7749120a303721813");
    EXPECT_EQ(
        merkle::treeId(std::string_view{}).to_string(HashFormat::Base16, false),
        "6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9b939dd74decc5321");

    /* Incremental equals whole, whatever the chunking. */
    std::string bytes = "hello\nworld\n";
    merkle::BlobHasher h{bytes.size()};
    h(std::string_view(bytes).substr(0, 3));
    h(std::string_view(bytes).substr(3, 5));
    h(std::string_view(bytes).substr(8));
    EXPECT_EQ(h.finish(), merkle::blobId(bytes));
    EXPECT_EQ(merkle::blobId(bytes).algo, HashAlgorithm::SHA256);
}

TEST(MerkleHash, serialiseTreeIsGitsBody)
{
    auto hA = merkle::blobId("a");
    auto hD = treeIdOf(merkle::Tree{});
    merkle::Tree t;
    merkle::insertEntry(t, "d", {.mode = Mode::Directory, .hash = hD});
    merkle::insertEntry(t, "a", {.mode = Mode::Regular, .hash = hA});
    merkle::insertEntry(t, "x", {.mode = Mode::Executable, .hash = hA});
    merkle::insertEntry(t, "l", {.mode = Mode::Symlink, .hash = hA});
    ASSERT_EQ(t.size(), 4u);
    EXPECT_TRUE(t.contains("d/"));

    std::string expected;
    auto raw = [](const Hash & h) { return std::string(reinterpret_cast<const char *>(h.hash), h.hashSize); };
    expected += std::string("100644 a\0", 9) + raw(hA);
    expected += std::string("40000 d\0", 8) + raw(hD);
    expected += std::string("120000 l\0", 9) + raw(hA);
    expected += std::string("100755 x\0", 9) + raw(hA);
    EXPECT_EQ(merkle::serialiseTree(t), expected);

    EXPECT_THROW(merkle::insertEntry(t, "", {.mode = Mode::Regular, .hash = hA}), Error);
    EXPECT_THROW(merkle::insertEntry(t, "a/b", {.mode = Mode::Regular, .hash = hA}), Error);
    EXPECT_THROW(merkle::insertEntry(t, std::string_view("a\0", 2), {.mode = Mode::Regular, .hash = hA}), Error);
    EXPECT_THROW(merkle::insertEntry(t, "a", {.mode = Mode::Regular, .hash = hA}), Error);
    /* "." and "..", which no directory has: the injectivity of `objectHash`
       rests on no real tree holding the synthetic root's one entry. */
    EXPECT_THROW(merkle::insertEntry(t, ".", {.mode = Mode::Regular, .hash = hA}), Error);
    EXPECT_THROW(merkle::insertEntry(t, "..", {.mode = Mode::Regular, .hash = hA}), Error);
    EXPECT_THROW(merkle::insertEntry(t, ".", {.mode = Mode::Directory, .hash = hD}), Error);
    EXPECT_EQ(t.size(), 4u);
    /* Names that merely begin with a dot are names. */
    EXPECT_NO_THROW(merkle::insertEntry(t, ".git", {.mode = Mode::Directory, .hash = hD}));
    EXPECT_NO_THROW(merkle::insertEntry(t, "...", {.mode = Mode::Regular, .hash = hA}));
}

/* (2) The object hash: a directory's tree id, a plain file's blob id, and
   for the two bare roots the id of `{"." -> entry}`, the body of which
   parses back as git's tree with the mode and hash on the entry. */
TEST(MerkleHash, objectHashCases)
{
    auto eDir = objectHashOf(*accessorOf(exampleTree()), CanonPath::root).root;
    auto ePlain = objectHashOf(*accessorOf(regular("hello\n")), CanonPath::root).root;
    auto eExec = objectHashOf(*accessorOf(regular("hello\n", true)), CanonPath::root).root;
    auto eLink = objectHashOf(*accessorOf(symlink("hello\n")), CanonPath::root).root;

    EXPECT_EQ(merkle::objectHash(eDir), eDir.hash);
    EXPECT_EQ(merkle::syntheticRootTree(eDir), std::nullopt);

    EXPECT_EQ(merkle::objectHash(ePlain), ePlain.hash);
    EXPECT_EQ(merkle::objectHash(ePlain), merkle::blobId("hello\n"));
    EXPECT_EQ(merkle::syntheticRootTree(ePlain), std::nullopt);

    /* One blob, three roots, three object hashes. */
    EXPECT_EQ(ePlain.hash, eExec.hash);
    EXPECT_EQ(ePlain.hash, eLink.hash);
    EXPECT_EQ(merkle::objectHash(eExec), treeIdOf(merkle::Tree{{".", eExec}}));
    EXPECT_NE(merkle::objectHash(eExec), eExec.hash);
    EXPECT_NE(merkle::objectHash(eLink), eLink.hash);
    EXPECT_NE(merkle::objectHash(eExec), merkle::objectHash(eLink));
    EXPECT_NE(merkle::objectHash(eExec), merkle::objectHash(ePlain));
    EXPECT_NE(merkle::objectHash(eLink), merkle::objectHash(ePlain));

    struct Capture : merkle::DirectorySink
    {
        std::map<std::string, TreeEntry> entries;

        void insertChild(std::string_view name, TreeEntry entry) override
        {
            entries.emplace(name, entry);
        }
    };

    for (auto & e : {eExec, eLink}) {
        auto body = merkle::syntheticRootTree(e);
        ASSERT_TRUE(body);
        EXPECT_EQ(merkle::treeId(*body), merkle::objectHash(e));

        std::string object = "tree " + std::to_string(body->size()) + std::string("\0", 1) + *body;
        StringSource source{object};
        EXPECT_EQ(git::parseObjectType(source), git::ObjectType::Tree);
        Capture sink;
        git::parseTree(sink, source, HashAlgorithm::SHA256);
        ASSERT_EQ(sink.entries.size(), 1u);
        EXPECT_EQ(sink.entries.begin()->first, ".");
        EXPECT_EQ(sink.entries.begin()->second, e);
    }
}

/* The names the hash is over (01 §9.11): on-disk names with the case-hack
   suffix hash as the tree's own names, so a restored `Foo`/`foo` tree has
   the tree's id; and with the hack off nothing strips the suffix, so such
   a name is outside the NAR's domain (`parseDump` refuses it on every
   platform) and every dump-side reader refuses it instead of serialising
   or hashing a tree no parser accepts. */
TEST(MerkleHash, unhackNameAndUnhackedEntries)
{
    std::string hacked = std::string("foo") + std::string(caseHackSuffix) + "1";
    {
        WithCaseHack on{true};
        EXPECT_EQ(unhackName(hacked), "foo");
        EXPECT_EQ(unhackName("Foo"), "Foo");

        auto acc = accessorOf(directory({{"Foo", regular("1")}, {hacked, regular("2")}}));
        auto entries = unhackedEntries(*acc, CanonPath::root);
        EXPECT_EQ(entries, (StringMap{{"Foo", "Foo"}, {"foo", hacked}}));

        auto plain = accessorOf(directory({{"Foo", regular("1")}, {"foo", regular("2")}}));
        EXPECT_EQ(objectHashOf(*acc, CanonPath::root).root, objectHashOf(*plain, CanonPath::root).root);
        EXPECT_EQ(referenceObjectEntry(*acc, CanonPath::root), referenceObjectEntry(*plain, CanonPath::root));

        /* Two on-disk names that unhack to one are refused, as `dumpPath` refuses them. */
        auto colliding = accessorOf(directory({{"foo", regular("1")}, {hacked, regular("2")}}));
        EXPECT_THROW(unhackedEntries(*colliding, CanonPath::root), Error);
    }
    {
        WithCaseHack off{false};
        EXPECT_EQ(unhackName("Foo"), "Foo");
        EXPECT_THROW(unhackName(hacked), Error);

        auto acc = accessorOf(directory({{"Foo", regular("1")}, {hacked, regular("2")}}));
        EXPECT_THROW(unhackedEntries(*acc, CanonPath::root), Error);
        StringSink nar;
        ASSERT_THAT(
            [&]() { acc->dumpPath(CanonPath::root, nar); },
            ::testing::ThrowsMessage<Error>(::testing::AllOf(
                testing::HasSubstrIgnoreANSIMatcher("cannot serialise '"),
                testing::HasSubstrIgnoreANSIMatcher(
                    "entry '" + hacked + "' carries the case-hack suffix, which the NAR format reserves"))));
        EXPECT_THROW(filteredPaths(*acc, CanonPath::root, defaultPathFilter), Error);
        EXPECT_THROW(objectHashOf(*acc, CanonPath::root), Error);
        /* Below the root too, naming the directory that holds the entry. */
        auto nested = accessorOf(directory({{"d", directory({{hacked, regular("2")}})}}));
        ASSERT_THAT(
            [&]() { nested->dumpPath(CanonPath::root, nar); },
            ::testing::ThrowsMessage<Error>(testing::HasSubstrIgnoreANSIMatcher("d': entry '" + hacked + "'")));

        /* Without the suffix, `Foo` and `foo` are two names and the tree
           serialises and hashes as it is. */
        auto plain = accessorOf(directory({{"Foo", regular("1")}, {"foo", regular("2")}}));
        EXPECT_EQ(unhackedEntries(*plain, CanonPath::root), (StringMap{{"Foo", "Foo"}, {"foo", "foo"}}));
        EXPECT_NO_THROW(plain->dumpPath(CanonPath::root, nar));
        EXPECT_EQ(objectHashOf(*plain, CanonPath::root).root, referenceObjectEntry(*plain, CanonPath::root));
    }
}

/* The NAR's fixed arithmetic, against `dumpPath` itself where a node is
   small enough to write.  `writeString` is 8 bytes of length, the bytes,
   and zero padding to a multiple of 8: 8, 16, 16, 24 for 0, 1, 8, 9
   bytes, seen through the one string a symlink node holds. */
TEST(MerkleHash, narSizesOfFixedNodes)
{
    namespace nar = merkle::nar;
    EXPECT_EQ(nar::symlink(1) - nar::symlink(0), 8u);
    EXPECT_EQ(nar::symlink(8) - nar::symlink(0), 8u);
    EXPECT_EQ(nar::symlink(9) - nar::symlink(0), 16u);
    /* `( type symlink target <target> )` with an empty target: 16 + 16 + 16
       + 16 + 8 + 16. */
    EXPECT_EQ(nar::symlink(0), 88u);
    /* The executable marker is two strings, `executable` and ``: 24 + 8. */
    EXPECT_EQ(nar::regular(0, true) - nar::regular(0, false), 32u);

    for (auto target : {"", "x", "12345678", "123456789"}) {
        StringSink s;
        accessorOf(symlink(target))->dumpPath(CanonPath::root, s);
        EXPECT_EQ(nar::root(nar::symlink(std::string_view(target).size())), s.s.size()) << target;
    }

    /* `dumpString` is `dumpPath` of a plain file with these contents. */
    for (auto contents : {"", "x", "12345678", "123456789"}) {
        StringSink s;
        dumpString(contents, s);
        EXPECT_EQ(nar::root(nar::regular(std::string_view(contents).size(), false)), s.s.size()) << contents;
    }
}

/* Names distinct, none of "." ".." or containing '/' or NUL; `Foo`/`foo`
   so that the case hack's platform sees a colliding pair without a
   suffix; `.git`, which the NAR admits.  Arbitrary contents and targets. */
static FileGenSpec collidingNamesSpec()
{
    return {
        .names = {"a", "Foo", "foo", ".git"},
        .contents = rc::gen::arbitrary<std::string>(),
        .targets = rc::gen::arbitrary<std::string>(),
    };
}

/* (3) The NAR's length from names and sizes alone is the serialisation's. */
RC_GTEST_PROP(MerkleHash, narSizeEqualsDumpPathLength, ())
{
    auto acc = accessorOf(*genFile(3, collidingNamesSpec()));
    StringSink s;
    acc->dumpPath(CanonPath::root, s);
    RC_ASSERT(merkle::nar::root(narNodeSize(*acc, CanonPath::root)) == s.s.size());
}

} // namespace nix
