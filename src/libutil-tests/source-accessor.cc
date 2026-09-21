#include "nix/util/fs-sink.hh"
#include "nix/util/file-system.hh"
#include "nix/util/processes.hh"
#include "nix/util/tests/gmock-matchers.hh"

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <rapidcheck/gtest.h>

namespace nix {

class FSSourceAccessorTest : public ::testing::Test
{
protected:
    std::filesystem::path tmpDir;
    std::unique_ptr<nix::AutoDelete> delTmpDir;

    void SetUp() override
    {
        tmpDir = nix::createTempDir();
        delTmpDir = std::make_unique<nix::AutoDelete>(tmpDir, true);
    }

    void TearDown() override
    {
        delTmpDir.reset();
    }
};

TEST_F(FSSourceAccessorTest, works)
{
#ifdef _WIN32
    GTEST_SKIP() << "Broken on Windows";
#endif
    {
        RestoreSink sink(false);
        sink.dstPath = tmpDir;
#ifndef _WIN32
        sink.dirFd = openDirectory(tmpDir, FinalSymlink::Follow);
#endif
        sink.createDirectory(CanonPath("subdir"));
        sink.createRegularFile(CanonPath("file1"), [](CreateRegularFileSink & crf) { crf("content1"); });
        sink.createRegularFile(CanonPath("subdir/file2"), [](CreateRegularFileSink & crf) { crf("content2"); });
        sink.createSymlink(CanonPath("rootlink"), "target");
        sink.createDirectory(CanonPath("a"));
        sink.createSymlink(CanonPath("a/dirlink"), "../subdir");
    }

    EXPECT_THAT(makeFSSourceAccessor(tmpDir / "file1"), testing::HasContents(CanonPath::root, "content1"));
    EXPECT_THAT(makeFSSourceAccessor(tmpDir / "rootlink"), testing::HasSymlink(CanonPath::root, "target"));
    EXPECT_THAT(
        makeFSSourceAccessor(tmpDir),
        testing::HasDirectory(CanonPath::root, std::set<std::string>{"file1", "subdir", "rootlink", "a"}));
    EXPECT_THAT(
        makeFSSourceAccessor(tmpDir / "subdir"),
        testing::HasDirectory(CanonPath::root, std::set<std::string>{"file2"}));

    {
        auto accessor = makeFSSourceAccessor(tmpDir);
        EXPECT_THAT(accessor, testing::HasContents(CanonPath("file1"), "content1"));
        EXPECT_THAT(accessor, testing::HasContents(CanonPath("subdir/file2"), "content2"));

        EXPECT_TRUE(accessor->pathExists(CanonPath("file1")));
        EXPECT_FALSE(accessor->pathExists(CanonPath("nonexistent")));

        EXPECT_THROW(accessor->readFile(CanonPath("a/dirlink/file2")), SymlinkNotAllowed);
        EXPECT_THROW(accessor->maybeLstat(CanonPath("a/dirlink/file2")), SymlinkNotAllowed);
        EXPECT_THROW(accessor->readDirectory(CanonPath("a/dirlink")), SymlinkNotAllowed);
        EXPECT_THROW(accessor->pathExists(CanonPath("a/dirlink/file2")), SymlinkNotAllowed);
    }

    {
        EXPECT_THROW(makeFSSourceAccessor(tmpDir / "nonexistent"), SystemError);
    }

    {
        auto accessor = makeFSSourceAccessor(tmpDir, true);
        EXPECT_EQ(accessor->getLastModified(), 0);
        accessor->maybeLstat(CanonPath("file1"));
        EXPECT_GT(accessor->getLastModified(), 0);
    }
}

TEST_F(FSSourceAccessorTest, listingAndScopeUseTheSameOpenedDirectory)
{
#ifdef _WIN32
    GTEST_SKIP() << "fd-based accessor is Unix-only";
#endif
    createDirs(tmpDir / "old");
    writeFile(tmpDir / "old" / "file", "observed");
    createSymlink("file", tmpDir / "old" / "link");
    auto accessor = makeFSSourceAccessor(tmpDir, true);
    accessor->readDirectoryWithEntries(
        CanonPath("old"),
        [&](SourceAccessor & scoped, const CanonPath & root, const SourceAccessor::DirEntries & entries) {
            ASSERT_EQ(entries.size(), 2u);
            std::filesystem::rename(tmpDir / "old", tmpDir / "moved");
            createDirs(tmpDir / "old");
            writeFile(tmpDir / "old" / "file", "replacement");
            EXPECT_EQ(scoped.readFile(root / "file"), "observed");
            EXPECT_EQ(scoped.readLink(root / "link"), "file");
            EXPECT_THROW(
                scoped.readDirectoryWithEntries(root / "link", [](auto &, auto &, auto &) {}), SymlinkNotAllowed);
        });
    EXPECT_GT(accessor->getLastModified(), 0);
}

TEST_F(FSSourceAccessorTest, invalidateCacheDropsStaleDirFds)
{
#ifdef _WIN32
    GTEST_SKIP() << "fd-based accessor is Unix-only";
#endif
    auto accessor = makeFSSourceAccessor(tmpDir);

    createDirs(tmpDir / "a" / "b");
    writeFile(tmpDir / "a" / "b" / "f", "old");

    EXPECT_TRUE(accessor->pathExists(CanonPath("a/b/f")));

    deletePath(tmpDir / "a" / "b");
    createDirs(tmpDir / "a" / "b");
    writeFile(tmpDir / "a" / "b" / "g", "new");
    createSymlink("g", tmpDir / "a" / "b" / "l");

    accessor->invalidateCache();

    EXPECT_FALSE(accessor->pathExists(CanonPath("a/b/f")));
    EXPECT_TRUE(accessor->pathExists(CanonPath("a/b/g")));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("a/b/g"), "new"));
    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath("a/b"), (std::set<std::string>{"g", "l"})));
    EXPECT_THAT(accessor, testing::HasSymlink(CanonPath("a/b/l"), "g"));
}

/* ----------------------------------------------------------------------------
 * The destination computed for `CanonPath::root`
 * --------------------------------------------------------------------------*/

/* A root that is not a directory is restored *at* `dstPath`, so the
   destination computed for `CanonPath::root` must be the target exactly:
   `std::filesystem::path / ""` appends a separator, and a destination
   ending in one has an empty `filename()`, which is not a name any `*at`
   call can take.  `append` (fs-sink.cc) is where the guard is; it is
   file-local, so the three root kinds are restored here and the text of
   the error that names the destination is checked, since that text is the
   computed path itself.  The restore of a one-file NAR at its destination
   is exercised inside `LocalStoreObjectsTest.restoreThroughObjects`. */
TEST_F(FSSourceAccessorTest, singleFileRootDestinationHasNoTrailingSlash)
{
#ifdef _WIN32
    GTEST_SKIP() << "Broken on Windows";
#endif
    /* A regular file. */
    auto file = tmpDir / "one-file";
    {
        RestoreSink sink(/*startFsync=*/false);
        sink.dstPath = file;
        sink.createRegularFile(CanonPath::root, [](CreateRegularFileSink & crf) { crf("bytes"); });
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::symlink_status(file)))
        << "the file is at the destination, not below it";
    EXPECT_EQ(readFile(file), "bytes");

    /* A symlink. */
    auto link = tmpDir / "one-link";
    {
        RestoreSink sink(/*startFsync=*/false);
        sink.dstPath = link;
        sink.createSymlink(CanonPath::root, "somewhere");
    }
    EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
    EXPECT_EQ(std::filesystem::read_symlink(link), std::filesystem::path("somewhere"));

    /* A directory, whose own `dirFd` is opened from the same computed
       destination. */
    auto dir = tmpDir / "one-dir";
    {
        RestoreSink sink(/*startFsync=*/false);
        sink.dstPath = dir;
        sink.createDirectory(CanonPath::root);
        EXPECT_TRUE(sink.dirFd);
    }
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::symlink_status(dir)));

    /* And the destination as the error names it: `PathFmt` puts it between
       quotes, so a separator after it would show. */
    auto missing = tmpDir / "no-such-directory" / "x";
    RestoreSink bad(/*startFsync=*/false);
    bad.dstPath = missing;
    EXPECT_THAT(
        [&] { bad.createRegularFile(CanonPath::root, [](CreateRegularFileSink &) {}); },
        ::testing::ThrowsMessage<SysError>(testing::HasSubstrIgnoreANSIMatcher("\"" + missing.string() + "\"")));
}

/* ----------------------------------------------------------------------------
 * RestoreSink non-directory at root (no dirFd)
 * --------------------------------------------------------------------------*/

TEST_F(FSSourceAccessorTest, RestoreSinkRegularFileAtRoot)
{
    auto filePath = tmpDir / "rootfile";
    {
        RestoreSink sink(false);
        sink.dstPath = filePath;
        // No dirFd set - this tests the !dirFd path
        sink.createRegularFile(CanonPath::root, [](CreateRegularFileSink & crf) { crf("root content"); });
    }

    EXPECT_THAT(makeFSSourceAccessor(filePath), testing::HasContents(CanonPath::root, "root content"));
}

TEST_F(FSSourceAccessorTest, RestoreSinkSymlinkAtRoot)
{
#ifdef _WIN32
    GTEST_SKIP() << "symlinks have some problems under Wine";
#endif
    auto linkPath = tmpDir / "rootlink";
    {
        RestoreSink sink(false);
        sink.dstPath = linkPath;
        // No dirFd set - this tests the !dirFd path
        sink.createSymlink(CanonPath::root, "symlink_target");
    }

    EXPECT_THAT(makeFSSourceAccessor(linkPath), testing::HasSymlink(CanonPath::root, "symlink_target"));
}

} // namespace nix
