#include <algorithm>
#include <gtest/gtest.h>

#include "nix/util/git.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/archive.hh"
#include "nix/util/terminal.hh"

#include "nix/util/tests/characterization.hh"

namespace nix {

/**
 * Test implementation of merkle::DirectorySink that captures entries.
 */
struct TestDirectorySink : merkle::DirectorySink
{
    git::Tree entries;

    void insertChild(std::string_view name, merkle::TreeEntry entry) override
    {
        auto name2 = std::string{name};
        if (entry.mode == merkle::Mode::Directory)
            name2 += '/';
        entries.insert_or_assign(name2, std::move(entry));
    }
};

class GitTest : public CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "git";

public:

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / std::string(testStem);
    }
};

TEST(GitMode, gitMode_directory)
{
    using namespace git;
    Mode m = Mode::Directory;
    RawMode r = 0040000;
    ASSERT_EQ(static_cast<RawMode>(m), r);
    ASSERT_EQ(decodeMode(r), std::optional{m});
};

TEST(GitMode, gitMode_executable)
{
    using namespace git;
    Mode m = Mode::Executable;
    RawMode r = 0100755;
    ASSERT_EQ(static_cast<RawMode>(m), r);
    ASSERT_EQ(decodeMode(r), std::optional{m});
};

TEST(GitMode, gitMode_regular)
{
    using namespace git;
    Mode m = Mode::Regular;
    RawMode r = 0100644;
    ASSERT_EQ(static_cast<RawMode>(m), r);
    ASSERT_EQ(decodeMode(r), std::optional{m});
};

TEST(GitMode, gitMode_symlink)
{
    using namespace git;
    Mode m = Mode::Symlink;
    RawMode r = 0120000;
    ASSERT_EQ(static_cast<RawMode>(m), r);
    ASSERT_EQ(decodeMode(r), std::optional{m});
};

TEST_F(GitTest, blob_read)
{
    using namespace git;
    readTest("hello-world-blob.bin", [&](const auto & encoded) {
        StringSource in{encoded};
        StringSink out;
        ASSERT_EQ(parseObjectType(in), ObjectType::Blob);
        auto size = parseBlob(in);
        in.drainInto(out, size);

        auto expected = readFile(goldenMaster("hello-world.bin"));

        ASSERT_EQ(out.s, expected);
    });
}

TEST_F(GitTest, blob_read_large_size)
{
    // Test that parseBlob handles sizes larger than INT_MAX (2^31 - 1)
    // This verifies that a bad overflowing number parser isn't used.
    uint64_t largeSize = 5000000000ULL; // ~5 GB, larger than INT_MAX
    std::string blobHeader = std::to_string(largeSize);
    blobHeader.push_back('\0'); // null terminator expected by parseBlob

    StringSource in{blobHeader};
    auto size = git::parseBlob(in);

    ASSERT_EQ(size, largeSize);
}

TEST_F(GitTest, blob_write)
{
    using namespace git;
    writeTest("hello-world-blob.bin", [&]() {
        auto decoded = readFile(goldenMaster("hello-world.bin"));
        StringSink s;
        merkle::feedHeader(s, "blob", decoded.size());
        s(decoded);
        return s.s;
    });
}

/**
 * This data is for "shallow" tree tests. However, we use "real" hashes
 * so that we can check our test data in a small shell script test test
 * (`src/libutil-tests/data/git/check-data.sh`).
 */
static const git::Tree treeSha1 = {
    {
        "Foo",
        {
            .mode = git::Mode::Regular,
            // hello world with special chars from above
            .hash = Hash::parseAny("63ddb340119baf8492d2da53af47e8c7cfcd5eb2", HashAlgorithm::SHA1),
        },
    },
    {
        "bAr",
        {
            .mode = git::Mode::Executable,
            // ditto
            .hash = Hash::parseAny("63ddb340119baf8492d2da53af47e8c7cfcd5eb2", HashAlgorithm::SHA1),
        },
    },
    {
        "baZ/",
        {
            .mode = git::Mode::Directory,
            // Empty directory hash
            .hash = Hash::parseAny("4b825dc642cb6eb9a060e54bf8d69288fbee4904", HashAlgorithm::SHA1),
        },
    },
    {
        "quuX",
        {
            .mode = git::Mode::Symlink,
            // hello world with special chars from above (symlink target
            // can be anything)
            .hash = Hash::parseAny("63ddb340119baf8492d2da53af47e8c7cfcd5eb2", HashAlgorithm::SHA1),
        },
    },
};

/**
 * Same conceptual object as `treeSha1`, just different hash algorithm.
 * See that one for details.
 */
static const git::Tree treeSha256 = {
    {
        "Foo",
        {
            .mode = git::Mode::Regular,
            .hash = Hash::parseAny(
                "ce60f5ad78a08ac24872ef74d78b078f077be212e7a246893a1a5d957dfbc8b1", HashAlgorithm::SHA256),
        },
    },
    {
        "bAr",
        {
            .mode = git::Mode::Executable,
            .hash = Hash::parseAny(
                "ce60f5ad78a08ac24872ef74d78b078f077be212e7a246893a1a5d957dfbc8b1", HashAlgorithm::SHA256),
        },
    },
    {
        "baZ/",
        {
            .mode = git::Mode::Directory,
            .hash = Hash::parseAny(
                "6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9b939dd74decc5321", HashAlgorithm::SHA256),
        },
    },
    {
        "quuX",
        {
            .mode = git::Mode::Symlink,
            .hash = Hash::parseAny(
                "ce60f5ad78a08ac24872ef74d78b078f077be212e7a246893a1a5d957dfbc8b1", HashAlgorithm::SHA256),
        },
    },
};

static auto mkTreeReadTest(HashAlgorithm hashAlgo, git::Tree tree)
{
    using namespace git;
    return [hashAlgo, tree](const auto & encoded) {
        StringSource in{encoded};
        TestDirectorySink out;
        ASSERT_EQ(parseObjectType(in), ObjectType::Tree);
        parseTree(out, in, hashAlgo);

        ASSERT_EQ(out.entries, tree);
    };
}

TEST_F(GitTest, tree_sha1_read)
{
    readTest("tree-sha1.bin", mkTreeReadTest(HashAlgorithm::SHA1, treeSha1));
}

TEST_F(GitTest, tree_sha256_read)
{
    readTest("tree-sha256.bin", mkTreeReadTest(HashAlgorithm::SHA256, treeSha256));
}

TEST_F(GitTest, tree_sha1_write)
{
    using namespace git;
    writeTest("tree-sha1.bin", [&]() {
        StringSink s;
        auto body = merkle::serialiseTree(treeSha1);
        merkle::feedHeader(s, "tree", body.size());
        s(body);
        return s.s;
    });
}

TEST_F(GitTest, tree_sha256_write)
{
    using namespace git;
    writeTest("tree-sha256.bin", [&]() {
        StringSink s;
        auto body = merkle::serialiseTree(treeSha256);
        merkle::feedHeader(s, "tree", body.size());
        s(body);
        return s.s;
    });
}

/* Tree digests have an algorithm-defined width even when Git does not
   support that algorithm. A source failure must precede its rejection. */
TEST_F(GitTest, tree_digest_read_boundaries)
{
    struct ChunkedSource : Source
    {
        std::string_view bytes;
        size_t pos = 0;

        explicit ChunkedSource(std::string_view bytes)
            : bytes(bytes)
        {
        }

        size_t read(char * data, size_t len) override
        {
            if (pos == bytes.size())
                throw EndOfFile("tree digest source exhausted");
            auto n = bytes.copy(data, std::min(len, size_t{3}), pos);
            pos += n;
            return n;
        }
    };

    struct Case
    {
        HashAlgorithm algorithm;
        size_t width;
        bool supported;
        const char * name;
    };

    const Case cases[] = {
        {HashAlgorithm::MD5, 16, false, "md5"},
        {HashAlgorithm::SHA1, 20, true, "sha1"},
        {HashAlgorithm::SHA256, 32, true, "sha256"},
        {HashAlgorithm::SHA512, 64, false, "sha512"},
        {HashAlgorithm::BLAKE3, 32, false, "blake3"},
    };
    size_t checked = 0;
    for (auto c : cases) {
        SCOPED_TRACE(c.name);
        {
            ChunkedSource source{std::string_view{"0\0", 2}};
            TestDirectorySink sink;
            EXPECT_NO_THROW(git::parseTree(sink, source, c.algorithm));
            EXPECT_EQ(source.pos, 2u);
            EXPECT_TRUE(sink.entries.empty());
            ++checked;
        }

        std::string digest(c.width, '\0');
        for (size_t i = 0; i < c.width; ++i)
            digest[i] = static_cast<char>(i * 37);
        const std::string prefix = std::to_string(13 + c.width) + '\0' + std::string("100644 child\0", 13);
        for (size_t available = 0; available <= c.width; ++available) {
            SCOPED_TRACE(available);
            auto wire = prefix + digest.substr(0, available);
            if (available == c.width)
                wire += '!'; // A complete entry must leave the next byte unread.
            ChunkedSource source{wire};
            TestDirectorySink sink;
            if (available < c.width) {
                EXPECT_THROW(git::parseTree(sink, source, c.algorithm), EndOfFile);
                EXPECT_EQ(source.pos, wire.size());
                EXPECT_TRUE(sink.entries.empty());
            } else if (c.supported) {
                git::parseTree(sink, source, c.algorithm);
                ASSERT_EQ(sink.entries.size(), 1u);
                const auto & entry = sink.entries.at("child");
                EXPECT_EQ(entry.mode, merkle::Mode::Regular);
                EXPECT_EQ(entry.hash.algo, c.algorithm);
                ASSERT_EQ(entry.hash.hashSize, c.width);
                EXPECT_EQ(std::string(reinterpret_cast<const char *>(entry.hash.hash), c.width), digest);
            } else {
                try {
                    git::parseTree(sink, source, c.algorithm);
                    FAIL() << "expected unsupported Git tree algorithm";
                } catch (const Error & e) {
                    EXPECT_EQ(
                        filterANSIEscapes(e.message(), true),
                        std::string("Unsupported hash algorithm for git trees: ") + c.name);
                }
                EXPECT_TRUE(sink.entries.empty());
            }
            EXPECT_EQ(source.pos, prefix.size() + available);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 174u); // Five empty, five complete, and every 164 short prefixes.
}

TEST_F(GitTest, tree_body_matches_canonical_encoding)
{
    const std::string_view modes[] = {"40000", "100644", "100755", "120000"};
    const std::string_view names[] = {"a", "a.", "a0"};
    size_t checked = 0;
    for (auto algorithm : {HashAlgorithm::SHA1, HashAlgorithm::SHA256}) {
        auto digest = hashString(algorithm, "tree body bytes");
        auto entry = [&](std::string_view mode, std::string_view name) {
            return std::string(mode) + ' ' + std::string(name) + '\0'
                   + std::string(reinterpret_cast<const char *>(digest.hash), digest.hashSize);
        };
        auto check = [&](const std::string & body) {
            SCOPED_TRACE(checked);
            /* The old store operation is an independent encoding oracle:
               parse permissively, then compare the map's canonical bytes. */
            auto framed = std::to_string(body.size()) + '\0' + body;
            StringSource source{framed};
            TestDirectorySink legacy;
            git::parseTree(legacy, source, algorithm);
            auto canonical = merkle::serialiseTree(legacy.entries) == body;
            auto before = body;
            TestDirectorySink actual;
            if (canonical)
                EXPECT_NO_THROW(git::parseTreeBody(actual, body, algorithm));
            else {
                try {
                    git::parseTreeBody(actual, body, algorithm);
                    FAIL() << "expected noncanonical tree body";
                } catch (const Error & e) {
                    EXPECT_EQ(filterANSIEscapes(e.message(), true), "tree body is not its canonical encoding");
                }
            }
            EXPECT_EQ(actual.entries, legacy.entries);
            EXPECT_EQ(body, before);
            ++checked;
        };
        for (auto mode : modes)
            for (bool leadingZero : {false, true})
                for (bool suffix : {false, true})
                    check(entry((leadingZero ? "0" : "") + std::string(mode) + (suffix ? "x" : ""), "a"));
        /* All mode pairs and equal/before/after name pairs, including the
           ordering reversal between directory a/ and ordinary a. or a0. */
        for (auto firstMode : modes)
            for (auto secondMode : modes)
                for (auto firstName : names)
                    for (auto secondName : names)
                        check(entry(firstMode, firstName) + entry(secondMode, secondName));
    }
    EXPECT_EQ(checked, 320u);
}

TEST_F(GitTest, tree_body_preserves_sink_and_truncation_errors)
{
    struct RejectSecond : merkle::DirectorySink
    {
        size_t calls = 0;

        void insertChild(std::string_view, merkle::TreeEntry) override
        {
            if (++calls == 2)
                throw Error("second entry rejected");
        }
    };

    size_t checked = 0;
    for (auto algorithm : {HashAlgorithm::SHA1, HashAlgorithm::SHA256}) {
        auto digest = hashString(algorithm, "tree body bytes");
        auto bytes = std::string(reinterpret_cast<const char *>(digest.hash), digest.hashSize);
        auto first = std::string("0100644 a\0", 10) + bytes;
        auto second = std::string("100644 b\0", 9) + bytes;
        for (size_t available = 0; available <= second.size(); ++available) {
            SCOPED_TRACE(available);
            auto body = first + second.substr(0, available);
            RejectSecond sink;
            if (available && available < second.size())
                EXPECT_THROW(git::parseTreeBody(sink, body, algorithm), EndOfFile);
            else {
                try {
                    git::parseTreeBody(sink, body, algorithm);
                    FAIL() << "expected tree body failure";
                } catch (const Error & e) {
                    EXPECT_EQ(
                        filterANSIEscapes(e.message(), true),
                        available ? "second entry rejected" : "tree body is not its canonical encoding");
                }
            }
            EXPECT_EQ(sink.calls, available == second.size() ? 2u : 1u);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 72u);
}

namespace memory_source_accessor {

extern ref<MemorySourceAccessor> exampleComplex();

}

TEST_F(GitTest, both_roundrip)
{
    using namespace git;
    auto files = memory_source_accessor::exampleComplex();

    {
        /* The hasher's algorithm; the reader takes it because git has two. */
        const auto hashAlgo = merkle::hashAlgo;
        std::map<Hash, std::string> cas;

        // Dump phase: the walk's hooks give each object as it completes;
        // the bytes are framed as the hasher framed them (`feedHeader`), so
        // the ids key what `parseObjectType` reads.
        auto blobObject = [](std::string_view bytes) {
            StringSink s;
            merkle::feedHeader(s, "blob", bytes.size());
            s(bytes);
            return s.s;
        };

        struct Collecting : HashingVisitor
        {
            std::map<Hash, std::string> & cas;
            MemorySourceAccessor & files;
            std::function<std::string(std::string_view)> blobObject;

            Collecting(
                std::map<Hash, std::string> & cas,
                MemorySourceAccessor & files,
                std::function<std::string(std::string_view)> blobObject)
                : cas(cas)
                , files(files)
                , blobObject(std::move(blobObject))
            {
            }

            HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
            {
                auto node = HashingVisitor::regular(path, std::move(read));
                cas.insert_or_assign(node.entry.hash, blobObject(files.readFile(path)));
                return node;
            }

            HashedNode symlink(const CanonPath & path, const std::string & target) override
            {
                auto node = HashingVisitor::symlink(path, target);
                cas.insert_or_assign(node.entry.hash, blobObject(target));
                return node;
            }

            HashedNode directory(const CanonPath & path, Children children) override
            {
                auto [node, body] = hashDirectory(path, std::move(children));
                StringSink s;
                merkle::feedHeader(s, "tree", body.size());
                s(body);
                cas.insert_or_assign(node.entry.hash, std::move(s.s));
                return node;
            }
        } collecting{cas, *files, blobObject};

        auto root = objectHashOf(*files, CanonPath::root, defaultPathFilter, collecting).root;
        ASSERT_EQ(root.hash.algo, hashAlgo);

        // Parse phase: deserialize git objects back to files
        auto files2 = make_ref<MemorySourceAccessor>();

        // Recursive function to parse a git object into a File
        std::function<MemorySourceAccessor::File(merkle::TreeEntry)> parseToFile;

        // DirectorySink that recursively parses children
        struct RecursiveDirSink : merkle::DirectorySink
        {
            std::function<MemorySourceAccessor::File(merkle::TreeEntry)> & parseToFile;
            MemorySourceAccessor::File::Directory dir;

            RecursiveDirSink(std::function<MemorySourceAccessor::File(merkle::TreeEntry)> & parseToFile)
                : parseToFile(parseToFile)
            {
            }

            void insertChild(std::string_view name, merkle::TreeEntry entry) override
            {
                dir.entries.insert_or_assign(std::string{name}, parseToFile(entry));
            }
        };

        parseToFile = [&](merkle::TreeEntry entry) -> MemorySourceAccessor::File {
            StringSource in{cas[entry.hash]};
            auto type = parseObjectType(in);

            switch (type) {
            case ObjectType::Blob: {
                StringSink content;
                auto size = parseBlob(in);
                in.drainInto(content, size);
                if (entry.mode == merkle::Mode::Symlink) {
                    return MemorySourceAccessor::File::Symlink{std::move(content.s)};
                } else {
                    return MemorySourceAccessor::File::Regular{
                        .executable = entry.mode == merkle::Mode::Executable,
                        .contents = std::move(content.s),
                    };
                }
            }
            case ObjectType::Tree: {
                RecursiveDirSink dirSink{parseToFile};
                parseTree(dirSink, in, hashAlgo);
                return std::move(dirSink.dir);
            }
            default:
                assert(false);
            }
        };

        files2->root = parseToFile(root);

        EXPECT_EQ(files->root, files2->root);
    }
}

TEST(GitLsRemote, parseSymrefLineWithReference)
{
    using namespace git;
    auto line = "ref: refs/head/main	HEAD";
    auto res = parseLsRemoteLine(line);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->kind, LsRemoteRefLine::Kind::Symbolic);
    ASSERT_EQ(res->target, "refs/head/main");
    ASSERT_EQ(res->reference, "HEAD");
}

TEST(GitLsRemote, parseSymrefLineWithNoReference)
{
    using namespace git;
    auto line = "ref: refs/head/main";
    auto res = parseLsRemoteLine(line);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->kind, LsRemoteRefLine::Kind::Symbolic);
    ASSERT_EQ(res->target, "refs/head/main");
    ASSERT_EQ(res->reference, std::nullopt);
}

TEST(GitLsRemote, parseObjectRefLine)
{
    using namespace git;
    auto line = "abc123	refs/head/main";
    auto res = parseLsRemoteLine(line);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->kind, LsRemoteRefLine::Kind::Object);
    ASSERT_EQ(res->target, "abc123");
    ASSERT_EQ(res->reference, "refs/head/main");
}

} // namespace nix

namespace nix {

/* The compositional (git) encoding is total over store content, `.git`
   included: `merkle::serialiseTree` writes and `parseTree` reads every entry
   name verbatim, so the only `.git` rejection anywhere is libgit2's
   `git_treebuilder` (tree.c:57-61,488), which this path never touches
   (doc/lazy-store/01-specification.md, section 2.4). */
TEST_F(GitTest, tree_encoding_round_trips_dot_git)
{
    using namespace git;
    Tree tree{
        {".git/", {.mode = Mode::Directory, .hash = hashString(HashAlgorithm::SHA1, "d")}},
        {"README", {.mode = Mode::Regular, .hash = hashString(HashAlgorithm::SHA1, "r")}},
        {"link", {.mode = Mode::Symlink, .hash = hashString(HashAlgorithm::SHA1, "l")}},
        {"run", {.mode = Mode::Executable, .hash = hashString(HashAlgorithm::SHA1, "x")}},
    };
    StringSink encoded;
    auto body = merkle::serialiseTree(tree);
    merkle::feedHeader(encoded, "tree", body.size());
    encoded(body);
    /* The name is on the wire verbatim. */
    ASSERT_NE(encoded.s.find(std::string(".git\0", 5)), std::string::npos);

    StringSource in{encoded.s};
    TestDirectorySink out;
    ASSERT_EQ(parseObjectType(in), ObjectType::Tree);
    parseTree(out, in, HashAlgorithm::SHA1);
    ASSERT_EQ(out.entries, tree);
}

/* The NAR half of the bijection over `.git`, and the exact edge of the
   name domain the parser enforces (archive.cc:275-282): `.git` is an
   ordinary name and round-trips byte-exactly; `.` and `..` are rejected. */
TEST_F(GitTest, nar_accepts_dot_git_and_rejects_dot_names)
{
    auto roundTrip = [](MemorySourceAccessor & src) {
        StringSink nar1;
        src.dumpPath(CanonPath::root, nar1);
        auto dst = make_ref<MemorySourceAccessor>();
        MemorySink sink{*dst};
        StringSource in{nar1.s};
        parseDump(sink, in);
        StringSink nar2;
        dst->dumpPath(CanonPath::root, nar2);
        return std::pair{nar1.s, nar2.s};
    };

    /* `.git` accepted, byte-exact. */
    {
        auto acc = make_ref<MemorySourceAccessor>();
        acc->addFile(CanonPath{"/.git/HEAD"}, "ref: refs/heads/main\n");
        acc->addFile(CanonPath{"/README"}, "r");
        auto [a, b] = roundTrip(*acc);
        ASSERT_EQ(a, b);
        ASSERT_NE(a.find(".git"), std::string::npos);
    }

    /* `.` and `..` rejected. Substitute the name bytes of a same-length
       entry whose letter (`z`) occurs nowhere else in the NAR, so the
       length-prefixed framing is untouched and only the name changes. */
    auto rejects = [](std::string entryName, std::string badName) {
        ASSERT_EQ(entryName.size(), badName.size());
        auto acc = make_ref<MemorySourceAccessor>();
        acc->addFile(CanonPath{"/" + entryName}, "");
        StringSink nar;
        acc->dumpPath(CanonPath::root, nar);
        auto pos = nar.s.find(entryName);
        ASSERT_NE(pos, std::string::npos);
        ASSERT_EQ(nar.s.find(entryName, pos + 1), std::string::npos); /* exactly one occurrence */
        nar.s.replace(pos, badName.size(), badName);
        auto dst = make_ref<MemorySourceAccessor>();
        MemorySink sink{*dst};
        StringSource in{nar.s};
        EXPECT_THROW(parseDump(sink, in), Error);
    };
    rejects("z", ".");
    rejects("zz", "..");
}

} // namespace nix
