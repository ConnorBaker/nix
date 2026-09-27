#include "nix/util/fs-sink.hh"
#include "nix/util/archive.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/processes.hh"
#include "nix/util/tests/gmock-matchers.hh"

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <cerrno>

#if !defined(_WIN32) && !defined(__CYGWIN__)
#  include "nix/util/unix-domain-socket.hh"
#endif

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

/* The sentinel stops an unfixed implementation before it can return into a
   receiver destroyed by its own callback. It is distinct from the intended
   callback exception, so an early ownership failure cannot satisfy the test. */
TEST_F(FSSourceAccessorTest, scopedDirectoryCallbacksRetainTheirReceiver)
{
#ifdef _WIN32
    GTEST_SKIP() << "POSIX directory receiver lifetime";
#else
    struct ReceiverReleased
    {};

    struct CallbackException
    {};
    enum class Ending { Return, Exception, Symlink };
    const std::string contents = "scoped bytes";
    auto base = tmpDir / "callback-source";
    createDirs(base / "sub");
    writeFile(base / "leaf", contents);
    writeFile(base / "sub" / "leaf", contents);
    unsigned cells = 0, allBorrowedCalls = 0;
    for (bool withEntries : {false, true})
        for (bool nested : {false, true})
            for (auto ending : {Ending::Return, Ending::Exception, Ending::Symlink})
                for (bool releaseOwner : {false, true})
                    for (bool track : {false, true}) {
                        ++cells;
                        SCOPED_TRACE(
                            ::testing::Message() << withEntries << ' ' << nested << ' ' << int(ending) << ' '
                                                 << releaseOwner << ' ' << track);
                        std::shared_ptr<SourceAccessor> owner = makeFSSourceAccessor(base, track).get_ptr();
                        std::weak_ptr<SourceAccessor> weakOwner = owner;
                        auto * accessor = owner.get();
                        accessor->setPathDisplay("outer-display");
                        const CanonPath path(nested ? "/sub" : "/");
                        unsigned calls = 0, borrowedCalls = 0;
                        auto callback = [&](SourceAccessor & scoped, const CanonPath & relative) {
                            ++calls;
                            if (releaseOwner)
                                owner.reset();
                            bool alive = !weakOwner.expired();
                            EXPECT_TRUE(alive);
                            if (!alive)
                                throw ReceiverReleased{};
                            EXPECT_EQ(relative, CanonPath::root);
                            EXPECT_TRUE(scoped.weak_from_this().expired());
                            EXPECT_EQ(scoped.readFile(relative / "leaf"), contents);
                            EXPECT_EQ(scoped.getLastModified().has_value(), track);
                            /* Both POSIX directory operations must also accept the borrowed
                               stack receivers they themselves supply to callbacks. */
                            scoped.readDirectory(relative, [&](SourceAccessor & inner, const CanonPath & root) {
                                ++borrowedCalls;
                                EXPECT_TRUE(inner.weak_from_this().expired());
                                EXPECT_EQ(inner.readFile(root / "leaf"), contents);
                            });
                            scoped.readDirectoryWithEntries(
                                relative,
                                [&](SourceAccessor & inner,
                                    const CanonPath & root,
                                    SourceAccessor::DirectoryListing && listing) {
                                    const auto & entries = listing.entries();
                                    ++borrowedCalls;
                                    EXPECT_TRUE(inner.weak_from_this().expired());
                                    EXPECT_EQ(entries.at("leaf"), SourceAccessor::tRegular);
                                    EXPECT_EQ(inner.readFile(root / "leaf"), contents);
                                });
                            if (ending == Ending::Exception)
                                throw CallbackException{};
                            if (ending == Ending::Symlink)
                                throw SymlinkNotAllowed(CanonPath("/link"), "callback-marker");
                        };
                        auto run = [&] {
                            if (withEntries)
                                accessor->readDirectoryWithEntries(
                                    path,
                                    [&](SourceAccessor & scoped,
                                        const CanonPath & relative,
                                        SourceAccessor::DirectoryListing && listing) {
                                        const auto & entries = listing.entries();
                                        EXPECT_EQ(entries.at("leaf"), SourceAccessor::tRegular);
                                        callback(scoped, relative);
                                    });
                            else
                                accessor->readDirectory(path, callback);
                        };
                        if (ending == Ending::Exception)
                            EXPECT_THROW(run(), CallbackException);
                        else if (ending == Ending::Symlink) {
                            try {
                                run();
                                ADD_FAILURE() << "expected callback symlink exception";
                            } catch (SymlinkNotAllowed & error) {
                                EXPECT_EQ(error.path, CanonPath("/link"));
                                if (withEntries) {
                                    EXPECT_THAT(error.what(), ::testing::HasSubstr("outer-display/link"));
                                    EXPECT_THAT(error.what(), ::testing::Not(::testing::HasSubstr("callback-marker")));
                                } else
                                    EXPECT_THAT(error.what(), ::testing::HasSubstr("callback-marker"));
                            } catch (...) {
                                ADD_FAILURE() << "unexpected callback exception";
                            }
                        } else
                            EXPECT_NO_THROW(run());
                        EXPECT_EQ(calls, 1u);
                        EXPECT_EQ(borrowedCalls, 2u);
                        allBorrowedCalls += borrowedCalls;
                        EXPECT_EQ(weakOwner.expired(), releaseOwner);
                        owner.reset();
                        EXPECT_TRUE(weakOwner.expired());
                    }
    EXPECT_EQ(cells, 48u);
    EXPECT_EQ(allBorrowedCalls, 96u);
    RecordProperty("directory_cells", cells);
    RecordProperty("borrowed_scope_calls", allBorrowedCalls);
#endif
}

TEST_F(FSSourceAccessorTest, fileCallbacksPreserveTheOpenedFileAfterOwnerRelease)
{
#ifdef _WIN32
    GTEST_SKIP() << "POSIX file receiver lifetime";
#else
    struct ReceiverReleased
    {};

    struct CallbackException
    {};
    enum class Point { Size, Sink };
    enum class Kind { Directory, File, Memory };
    /* More than one generic copy buffer: closing the root-file descriptor at
       the first sink callback must not be hidden by already-buffered bytes. */
    const std::string contents(64 * 1024 + 7, 'x');
    writeFile(tmpDir / "file", contents);
    unsigned cells = 0;
    for (auto kind : {Kind::Directory, Kind::File, Kind::Memory})
        for (auto point : {Point::Size, Point::Sink})
            for (bool throwing : {false, true})
                for (bool releaseOwner : {false, true}) {
                    ++cells;
                    SCOPED_TRACE(
                        ::testing::Message()
                        << int(kind) << ' ' << int(point) << ' ' << throwing << ' ' << releaseOwner);
                    std::shared_ptr<SourceAccessor> owner;
                    if (kind == Kind::Memory) {
                        auto memory = std::make_shared<MemorySourceAccessor>();
                        memory->root = MemorySourceAccessor::File::Regular{.contents = contents};
                        owner = std::move(memory);
                    } else
                        owner = makeFSSourceAccessor(kind == Kind::File ? tmpDir / "file" : tmpDir).get_ptr();
                    std::weak_ptr<SourceAccessor> weakOwner = owner;
                    auto * accessor = owner.get();
                    unsigned callbackCalls = 0, writes = 0;
                    bool announced = false;
                    std::string output;
                    auto release = [&] {
                        ++callbackCalls;
                        if (releaseOwner)
                            owner.reset();
                        /* Directory reads already own an independent local fd.
                           File-root reads borrow a member fd; memory reads borrow contents. */
                        if (kind != Kind::Directory) {
                            bool alive = !weakOwner.expired();
                            EXPECT_TRUE(alive);
                            if (!alive)
                                throw ReceiverReleased{};
                        }
                        if (throwing)
                            throw CallbackException{};
                    };
                    LambdaSink sink([&](std::string_view bytes) {
                        EXPECT_TRUE(announced);
                        ++writes;
                        if (point == Point::Sink && writes == 1)
                            release();
                        output += bytes;
                    });
                    auto run = [&] {
                        accessor->readFile(
                            CanonPath(kind == Kind::Directory ? "/file" : "/"), sink, [&](uint64_t size) {
                                EXPECT_EQ(size, contents.size());
                                EXPECT_FALSE(announced);
                                announced = true;
                                if (point == Point::Size)
                                    release();
                            });
                    };
                    if (throwing)
                        EXPECT_THROW(run(), CallbackException);
                    else {
                        EXPECT_NO_THROW(run());
                        EXPECT_EQ(output, contents);
                    }
                    EXPECT_TRUE(announced);
                    EXPECT_EQ(callbackCalls, 1u);
                    EXPECT_EQ(weakOwner.expired(), releaseOwner);
                    owner.reset();
                    EXPECT_TRUE(weakOwner.expired());
                }
    EXPECT_EQ(cells, 24u);
    RecordProperty("file_cells", cells);
#endif
}

/* Fourteen reads and twenty-seven exclusive creates. Reads cover seven
   terminal kinds through immediate/nested parents; creation also covers
   FIFO/socket entries and a root destination with a separately opened parent. */
TEST_F(FSSourceAccessorTest, terminalFileOperationsPreserveKindsAndExclusiveCreation)
{
#if defined(_WIN32) || defined(__CYGWIN__)
    GTEST_SKIP() << "POSIX terminal-open matrix excludes Windows and Cygwin";
#else
    enum class Layout { Immediate, Nested, RootDestination };
    enum class Kind { Absent, Regular, Executable, Directory, Symlink, Dangling, Loop, Fifo, Socket };
    const std::string original("before\0after", 12);
    const std::string created("created\0bytes", 13);
    for (auto layout : {Layout::Immediate, Layout::Nested, Layout::RootDestination})
        for (auto kind :
             {Kind::Absent,
              Kind::Regular,
              Kind::Executable,
              Kind::Directory,
              Kind::Symlink,
              Kind::Dangling,
              Kind::Loop,
              Kind::Fifo,
              Kind::Socket}) {
            SCOPED_TRACE(::testing::Message() << "layout=" << int(layout) << " kind=" << int(kind));
            auto base = tmpDir / (std::to_string(int(layout)) + "-" + std::to_string(int(kind)));
            const CanonPath path(layout == Layout::Nested ? "nested/leaf" : "leaf");
            auto terminal = base / path.rel();
            auto parent = terminal.parent_path();
            createDirs(parent);
            writeFile(parent / "target", "target bytes");
            AutoCloseFD socket;
            switch (kind) {
            case Kind::Absent:
                break;
            case Kind::Regular:
            case Kind::Executable:
                writeFile(terminal, original);
                ASSERT_EQ(::chmod(terminal.c_str(), kind == Kind::Executable ? 0755 : 0644), 0);
                break;
            case Kind::Directory:
                createDirs(terminal);
                writeFile(terminal / "marker", original);
                break;
            case Kind::Symlink:
                createSymlink("target", terminal);
                break;
            case Kind::Dangling:
                createSymlink("missing", terminal);
                break;
            case Kind::Loop:
                createSymlink("leaf", terminal);
                break;
            case Kind::Fifo:
                ASSERT_EQ(::mkfifo(terminal.c_str(), 0600), 0);
                break;
            case Kind::Socket:
                socket = createUnixDomainSocket(terminal, 0600);
                break;
            }
            auto before = nix::maybeLstat(terminal);
            ASSERT_EQ(before.has_value(), kind != Kind::Absent);
            if (kind == Kind::Fifo)
                ASSERT_TRUE(S_ISFIFO(before->st_mode));
            if (kind == Kind::Socket)
                ASSERT_TRUE(S_ISSOCK(before->st_mode));

            if (layout != Layout::RootDestination && kind != Kind::Fifo && kind != Kind::Socket) {
                auto accessor = makeFSSourceAccessor(base);
                size_t sizes = 0, chunks = 0;
                std::string received;
                LambdaSink sink([&](std::string_view bytes) {
                    EXPECT_EQ(sizes, 1u);
                    ++chunks;
                    received.append(bytes);
                });
                auto read = [&] {
                    accessor->readFile(path, sink, [&](uint64_t size) {
                        EXPECT_EQ(sizes, 0u);
                        EXPECT_EQ(chunks, 0u);
                        ++sizes;
                        EXPECT_EQ(size, original.size());
                    });
                };
                if (kind == Kind::Regular || kind == Kind::Executable) {
                    EXPECT_NO_THROW(read());
                    EXPECT_EQ(sizes, 1u);
                    EXPECT_EQ(received, original);
                } else {
                    if (kind == Kind::Absent)
                        EXPECT_THAT(
                            read,
                            ::testing::ThrowsMessage<FileNotFound>(
                                testing::HasSubstrIgnoreANSIMatcher(terminal.string())));
                    else if (kind == Kind::Directory)
                        EXPECT_THAT(
                            read,
                            ::testing::ThrowsMessage<NotARegularFile>(
                                testing::HasSubstrIgnoreANSIMatcher(terminal.string())));
                    else {
                        try {
                            read();
                            ADD_FAILURE() << "reading a terminal symlink succeeded";
                        } catch (const SymlinkNotAllowed & error) {
                            EXPECT_EQ(error.path, path);
                            EXPECT_THAT(error.what(), testing::HasSubstrIgnoreANSIMatcher(terminal.string()));
                        }
                    }
                    EXPECT_EQ(sizes, 0u);
                    EXPECT_EQ(chunks, 0u);
                    EXPECT_TRUE(received.empty());
                }
            }

            RestoreSink destination(/*startFsync=*/false);
            destination.dstPath = layout == Layout::RootDestination ? terminal : base;
            if (layout != Layout::RootDestination)
                destination.dirFd = openDirectory(base, FinalSymlink::Follow);
            auto destinationPath = layout == Layout::RootDestination ? CanonPath::root : path;
            bool called = false;
            auto create = [&] {
                destination.createRegularFile(destinationPath, [&](CreateRegularFileSink & file) {
                    called = true;
                    file(created);
                });
            };
            if (kind == Kind::Absent) {
                EXPECT_NO_THROW(create());
                EXPECT_TRUE(called);
                EXPECT_TRUE(S_ISREG(nix::lstat(terminal).st_mode));
                EXPECT_EQ(nix::readFile(terminal), created);
            } else {
                try {
                    create();
                    ADD_FAILURE() << "exclusive creation replaced an existing entry";
                } catch (const SysError & error) {
                    EXPECT_EQ(error.errNo, EEXIST);
                    EXPECT_THAT(error.what(), testing::HasSubstrIgnoreANSIMatcher(terminal.string()));
                }
                EXPECT_FALSE(called);
                auto after = nix::lstat(terminal);
                EXPECT_EQ(after.st_dev, before->st_dev);
                EXPECT_EQ(after.st_ino, before->st_ino);
                EXPECT_EQ(after.st_mode, before->st_mode);
                if (kind == Kind::Regular || kind == Kind::Executable)
                    EXPECT_EQ(nix::readFile(terminal), original);
                else if (kind == Kind::Directory)
                    EXPECT_EQ(nix::readFile(terminal / "marker"), original);
                else if (kind == Kind::Symlink || kind == Kind::Dangling || kind == Kind::Loop)
                    EXPECT_EQ(
                        std::filesystem::read_symlink(terminal),
                        std::filesystem::path(
                            kind == Kind::Symlink    ? "target"
                            : kind == Kind::Dangling ? "missing"
                                                     : "leaf"));
            }
            EXPECT_EQ(nix::readFile(parent / "target"), "target bytes");
            EXPECT_FALSE(nix::pathExists(parent / "missing"));
        }
#endif
}

TEST_F(FSSourceAccessorTest, readFileRejectsTerminalProcFdMagicLink)
{
#ifndef __linux__
    GTEST_SKIP() << "procfs magic links are Linux-specific";
#else
    if (!std::filesystem::is_directory("/proc/self/fd"))
        GTEST_SKIP() << "procfs fd directory is unavailable";
    auto target = tmpDir / "target";
    writeFile(target, "target bytes");
    auto fd = openFileReadonly(target);
    auto accessor = makeFSSourceAccessor("/proc/self/fd");
    const auto path = CanonPath::fromFilename(std::to_string(fd.get()));
    bool sizeCalled = false, dataCalled = false;
    LambdaSink sink([&](std::string_view) { dataCalled = true; });
    try {
        accessor->readFile(path, sink, [&](uint64_t) { sizeCalled = true; });
        ADD_FAILURE() << "reading a terminal procfs magic link succeeded";
    } catch (const SymlinkNotAllowed & error) {
        EXPECT_EQ(error.path, path);
        EXPECT_THAT(
            error.what(), testing::HasSubstrIgnoreANSIMatcher(std::string("/proc/self/fd/") + std::string(path.rel())));
    }
    EXPECT_FALSE(sizeCalled);
    EXPECT_FALSE(dataCalled);
    EXPECT_EQ(nix::readFile(target), "target bytes");
#endif
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
        [&](SourceAccessor & scoped, const CanonPath & root, SourceAccessor::DirectoryListing && listing) {
            const auto & entries = listing.entries();
            ASSERT_EQ(entries.size(), 2u);
            std::filesystem::rename(tmpDir / "old", tmpDir / "moved");
            createDirs(tmpDir / "old");
            writeFile(tmpDir / "old" / "file", "replacement");
            EXPECT_EQ(scoped.readFile(root / "file"), "observed");
            EXPECT_EQ(scoped.readLink(root / "link"), "file");
            EXPECT_THROW(
                scoped.readDirectoryWithEntries(root / "link", [](auto &, auto &, const auto &) {}), SymlinkNotAllowed);
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

/* Both parent ownership routes retain the selected directory across rename.
   Once creation returns, the writer no longer needs the parent descriptor. */
TEST_F(FSSourceAccessorTest, regularFileAtRetainsItsSelectedParentAndOwnsItsFile)
{
#ifdef _WIN32
    GTEST_SKIP() << "descriptor-relative creation is Unix-only";
#else
    const std::string contents("before\0after", 12);
    for (bool borrowed : {false, true})
        for (bool executable : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "borrowed=" << borrowed << " executable=" << executable);
            auto base = tmpDir / (std::to_string(borrowed) + "-" + std::to_string(executable));
            auto parent = base / "parent";
            auto moved = base / "moved";
            createDirs(parent);
            /* An independent creation in the same directory observes the
               current umask/default ACL without changing process-wide state. */
            writeFile(parent / "reference", contents);
            auto expectedMode = (nix::lstat(parent / "reference").st_mode & 07777) | (executable ? 0111 : 0);

            RestoreSink sink(/*startFsync=*/false);
            sink.dstPath = borrowed ? parent : parent / "leaf";
            if (borrowed)
                sink.dirFd = openDirectory(parent, FinalSymlink::Follow);
            auto path = borrowed ? CanonPath("leaf") : CanonPath::root;
            auto [owned, parentFd, name] = sink.parentOf(path);
            ASSERT_EQ(bool(owned), !borrowed);
            std::filesystem::rename(parent, moved);
            createDirs(parent);

            auto writer = sink.beginRegularFileAt(path, parentFd, name);
            owned.close();
            sink.dirFd.close();
            if (executable)
                writer->isExecutable();
            (*writer)(contents);
            writer->finish();
            writer.reset();

            auto result = nix::lstat(moved / "leaf");
            EXPECT_TRUE(S_ISREG(result.st_mode));
            EXPECT_EQ(result.st_mode & 07777, expectedMode);
            EXPECT_EQ(nix::readFile(moved / "leaf"), contents);
            EXPECT_EQ(nix::readFile(moved / "reference"), contents);
            EXPECT_FALSE(nix::pathExists(parent / "leaf"));
        }
#endif
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

namespace {

/* Each probe copies its callback before invoking user code and does not
   access itself afterward. Thus an ownership failure is reported through
   weak pointers without continuing through a destroyed probe or cache. */
struct CachingOwnershipProbe : SourceAccessor
{
    std::function<void()> onCall;
    bool absent = false;

    void anchor() override {}

    void call()
    {
        auto callback = onCall;
        callback();
    }

    void readFile(const CanonPath &, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        sizeCallback(5);
        sink("bytes");
    }

    std::optional<Stat> maybeLstat(const CanonPath &) override
    {
        auto missing = absent;
        call();
        if (missing)
            return std::nullopt;
        return Stat{.type = tRegular, .fileSize = 5};
    }

    Stat lstat(const CanonPath &) override
    {
        call();
        return {.type = tRegular, .fileSize = 5};
    }

    DirEntries readDirectory(const CanonPath &) override
    {
        call();
        return {{"entry", tRegular}};
    }

    void
    readDirectory(const CanonPath & path, std::function<void(SourceAccessor &, const CanonPath &)> callback) override
    {
        callback(*this, path);
    }

    void readDirectoryWithEntries(
        const CanonPath & path,
        std::function<void(SourceAccessor &, const CanonPath &, DirectoryListing &&)> callback) override
    {
        DirEntries entries{{"entry", tRegular}};
        callback(*this, path, DirectoryListing(std::move(entries)));
    }

    std::string readLink(const CanonPath &) override
    {
        call();
        return "target";
    }

    std::string showPath(const CanonPath &) override
    {
        call();
        return "shown";
    }

    void invalidateCache() override
    {
        call();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath &) override
    {
        call();
        return std::filesystem::path("physical");
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        call();
        return std::pair{path, std::string("fingerprint")};
    }

    std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath &) override
    {
        call();
        return merkle::TreeEntry{merkle::Mode::Regular, hashString(HashAlgorithm::SHA256, "bytes")};
    }
};

struct CachingCallbackException
{};

struct CachingOwnershipLost
{};

struct CachingObservationProbe : CachingOwnershipProbe
{
    unsigned observations = 0;
    unsigned value = 1;
    std::optional<std::string> name = "first";
    std::function<void()> onInvalidate;

    template<typename T>
    T observe(T result)
    {
        ++observations;
        auto callback = onCall;
        if (callback)
            callback();
        return result;
    }

    std::optional<Stat> maybeLstat(const CanonPath &) override
    {
        return observe(absent ? std::nullopt : std::optional{Stat{.type = tRegular, .fileSize = value}});
    }

    Stat lstat(const CanonPath &) override
    {
        return observe(Stat{.type = tRegular, .fileSize = value});
    }

    std::string readLink(const CanonPath &) override
    {
        return observe(fmt("target-%d", value));
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        return observe(name ? std::optional{std::pair{path, *name}} : std::nullopt);
    }

    void invalidateCache() override
    {
        auto callback = onInvalidate;
        if (callback)
            callback();
    }
};

} // namespace

TEST(CachingSourceAccessor, optionalNamesAreObservationsOfTheCurrentView)
{
    const std::optional<std::string> names[]{std::nullopt, "", "named"};
    for (const auto & path : {CanonPath::root, CanonPath("sub/file")})
        for (const auto & before : names)
            for (const auto & after : names) {
                SCOPED_TRACE(
                    ::testing::Message() << "path=" << path << " before=" << before.value_or("<none>")
                                         << " after=" << after.value_or("<none>"));
                auto child = make_ref<CachingObservationProbe>();
                auto cached = makeCachingSourceAccessor(child);
                auto expected = [&](const auto & name) {
                    return name ? std::optional{std::pair{path, *name}} : std::nullopt;
                };
                child->name = before;
                EXPECT_EQ(cached->getFingerprint(path), expected(before));
                EXPECT_EQ(cached->getFingerprint(path), expected(before));
                EXPECT_EQ(child->observations, 1u);

                child->name = after;
                cached->invalidateCache();
                EXPECT_EQ(cached->getFingerprint(path), expected(after));
                EXPECT_EQ(cached->getFingerprint(path), expected(after));
                EXPECT_EQ(child->observations, 2u);
            }
}

TEST(CachingSourceAccessor, anUnnamedNodeIsNotAnAbsentNode)
{
    auto child = make_ref<CachingObservationProbe>();
    child->name = std::nullopt;
    child->absent = true;
    auto cached = makeCachingSourceAccessor(child);
    CanonPath path("entry");

    EXPECT_FALSE(cached->getFingerprint(path));
    EXPECT_FALSE(cached->getFingerprint(path));
    EXPECT_EQ(child->observations, 1u);
    EXPECT_FALSE(cached->maybeLstat(path));
    EXPECT_FALSE(cached->maybeLstat(path));
    EXPECT_EQ(child->observations, 3u);

    /* Positive lookup caching permits previously absent nodes to appear. */
    child->absent = false;
    EXPECT_EQ(cached->maybeLstat(path)->fileSize, 1u);
    EXPECT_EQ(cached->maybeLstat(path)->fileSize, 1u);
    EXPECT_FALSE(cached->getFingerprint(path));
    EXPECT_EQ(child->observations, 4u);
}

TEST(CachingSourceAccessor, invalidationDoesNotResurrectAnInFlightObservation)
{
    enum class Point { MaybeStat, MissingStat, Stat, Link, Named, Unnamed, EmptyName };
    constexpr Point points[]{
        Point::MaybeStat, Point::MissingStat, Point::Stat, Point::Link, Point::Named, Point::Unnamed, Point::EmptyName};

    /* Seven successful result shapes × three invalidation schedules ×
       normal/exceptional completion of the original observation. */
    for (auto point : points)
        for (unsigned schedule = 0; schedule != 3; ++schedule)
            for (bool throwing : {false, true}) {
                SCOPED_TRACE(
                    ::testing::Message() << "point=" << static_cast<int>(point) << " schedule=" << schedule
                                         << " throwing=" << throwing);
                auto child = make_ref<CachingObservationProbe>();
                child->absent = point == Point::MissingStat;
                child->name = point == Point::Unnamed     ? std::nullopt
                              : point == Point::EmptyName ? std::optional<std::string>("")
                                                          : std::optional<std::string>("first");
                auto cached = makeCachingSourceAccessor(child);
                CanonPath path("entry");
                auto read = [&]() -> std::string {
                    switch (point) {
                    case Point::MaybeStat:
                    case Point::MissingStat:
                        if (auto st = cached->maybeLstat(path))
                            return std::to_string(*st->fileSize);
                        return "missing";
                    case Point::Stat:
                        return std::to_string(*cached->lstat(path).fileSize);
                    case Point::Link:
                        return cached->readLink(path);
                    case Point::Named:
                    case Point::Unnamed:
                    case Point::EmptyName:
                        if (auto identity = cached->getFingerprint(path)) {
                            EXPECT_EQ(identity->first, path);
                            return "named:" + identity->second;
                        }
                        return "unnamed";
                    }
                    std::abort();
                };
                auto expected = [&](unsigned value) {
                    if (point == Point::Link)
                        return fmt("target-%d", value);
                    if (point == Point::Named || point == Point::Unnamed || point == Point::EmptyName)
                        return fmt("named:version-%d", value);
                    return std::to_string(value);
                };

                child->onCall = [&] {
                    /* Clear the callback before any nested observation. */
                    child->onCall = {};
                    child->value = 2;
                    child->name = "version-2";
                    child->absent = false;
                    if (schedule) {
                        child->onInvalidate = [&] {
                            child->onInvalidate = {};
                            if (schedule == 2)
                                EXPECT_EQ(read(), expected(2));
                            child->value = 3;
                            child->name = "version-3";
                            cached->invalidateCache();
                        };
                    }
                    cached->invalidateCache();
                    if (throwing)
                        throw CachingCallbackException{};
                };

                if (throwing)
                    EXPECT_THROW(read(), CachingCallbackException);
                else
                    EXPECT_NO_THROW(read());
                EXPECT_EQ(read(), expected(schedule ? 3 : 2));
                EXPECT_EQ(read(), expected(schedule ? 3 : 2));
                EXPECT_EQ(child->observations, schedule == 2 ? 3u : 2u);
            }
}

TEST(CachingSourceAccessor, invalidationPublishesAfterTheChildDropsItsStaleFacts)
{
    enum class Point { MaybeStat, Stat, Link, Named, Unnamed, EmptyName };
    constexpr Point points[]{
        Point::MaybeStat, Point::Stat, Point::Link, Point::Named, Point::Unnamed, Point::EmptyName};
    for (auto point : points)
        for (bool throwing : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "point=" << static_cast<int>(point) << " throwing=" << throwing);
            auto child = make_ref<CachingObservationProbe>();
            if (point == Point::Unnamed)
                child->name = std::nullopt;
            else if (point == Point::EmptyName)
                child->name = "";
            auto cached = makeCachingSourceAccessor(child);
            auto read = [&](const CanonPath & path) -> std::string {
                switch (point) {
                case Point::MaybeStat:
                    return std::to_string(*cached->maybeLstat(path)->fileSize);
                case Point::Stat:
                    return std::to_string(*cached->lstat(path).fileSize);
                case Point::Link:
                    return cached->readLink(path);
                case Point::Named:
                case Point::Unnamed:
                case Point::EmptyName:
                    if (auto identity = cached->getFingerprint(path)) {
                        EXPECT_EQ(identity->first, path);
                        return "named:" + identity->second;
                    }
                    return "unnamed";
                }
                std::abort();
            };
            CanonPath existing("existing"), fresh("fresh");
            auto before = read(existing);
            child->onInvalidate = [&] {
                /* Both an existing cache hit and a fresh observation can
                   still see stale child data during its invalidation. */
                EXPECT_EQ(read(existing), before);
                EXPECT_EQ(read(fresh), before);
                child->value = 2;
                child->name = "second";
                if (throwing)
                    throw CachingCallbackException{};
            };
            if (throwing)
                EXPECT_THROW(cached->invalidateCache(), CachingCallbackException);
            else
                EXPECT_NO_THROW(cached->invalidateCache());
            auto after = point == Point::Link                                ? "target-2"
                         : point == Point::MaybeStat || point == Point::Stat ? "2"
                                                                             : "named:second";
            for (const auto & path : {existing, fresh}) {
                EXPECT_EQ(read(path), after);
                EXPECT_EQ(read(path), after);
            }
            EXPECT_EQ(child->observations, 4u);
        }
}

TEST(CachingSourceAccessor, delegatedCallbacksKeepSelectedOwnersAlive)
{
    enum class Point {
        Size,
        Sink,
        MaybeStat,
        MissingStat,
        Stat,
        Directory,
        DirectoryScope,
        DirectoryEntries,
        Link,
        Display,
        Invalidate,
        Physical,
        Fingerprint,
        KnownEntry,
    };
    constexpr Point points[]{
        Point::Size,
        Point::Sink,
        Point::MaybeStat,
        Point::MissingStat,
        Point::Stat,
        Point::Directory,
        Point::DirectoryScope,
        Point::DirectoryEntries,
        Point::Link,
        Point::Display,
        Point::Invalidate,
        Point::Physical,
        Point::Fingerprint,
        Point::KnownEntry,
    };

    /* Fourteen callback points × caller retains/releases its owner ×
       normal/exceptional return. A cache miss also needs the wrapper to
       survive its child call; a tail delegation only needs the child. */
    for (auto point : points)
        for (bool release : {false, true})
            for (bool throwing : {false, true}) {
                SCOPED_TRACE(
                    ::testing::Message() << "point=" << static_cast<int>(point) << " release=" << release
                                         << " throwing=" << throwing);
                auto child = std::make_shared<CachingOwnershipProbe>();
                child->absent = point == Point::MissingStat;
                std::weak_ptr<SourceAccessor> weakChild = child;
                auto childNumber = child->number;
                std::shared_ptr<SourceAccessor> owner = makeCachingSourceAccessor(ref<SourceAccessor>(child)).get_ptr();
                std::weak_ptr<SourceAccessor> weakOwner = owner;
                auto * accessor = owner.get();
                unsigned calls = 0;
                auto callback = [&] {
                    ++calls;
                    if (release)
                        owner.reset();
                    auto childAlive = !weakChild.expired();
                    bool needsWrapper = point == Point::MaybeStat || point == Point::MissingStat || point == Point::Stat
                                        || point == Point::Link || point == Point::Fingerprint
                                        || point == Point::Invalidate;
                    auto ownerAlive = !weakOwner.expired();
                    EXPECT_TRUE(childAlive);
                    if (needsWrapper)
                        EXPECT_TRUE(ownerAlive);
                    /* Stop before returning into a dead wrapper in the
                       negative control, even if the intended result was
                       a normal callback return. */
                    if (!childAlive || (needsWrapper && !ownerAlive))
                        throw CachingOwnershipLost{};
                    if (throwing)
                        throw CachingCallbackException{};
                };
                child->onCall = callback;
                child.reset();
                CanonPath path("entry");

                auto run = [&] {
                    switch (point) {
                    case Point::Size:
                    case Point::Sink: {
                        std::string bytes;
                        LambdaSink sink([&](std::string_view data) {
                            if (point == Point::Sink)
                                callback();
                            bytes += data;
                        });
                        accessor->readFile(path, sink, [&](uint64_t size) {
                            EXPECT_EQ(size, 5u);
                            if (point == Point::Size)
                                callback();
                        });
                        EXPECT_EQ(bytes, "bytes");
                        break;
                    }
                    case Point::MaybeStat:
                        EXPECT_EQ(accessor->maybeLstat(path)->fileSize, 5u);
                        break;
                    case Point::MissingStat:
                        EXPECT_FALSE(accessor->maybeLstat(path));
                        break;
                    case Point::Stat:
                        EXPECT_EQ(accessor->lstat(path).fileSize, 5u);
                        break;
                    case Point::Directory:
                        EXPECT_EQ(
                            accessor->readDirectory(path),
                            (SourceAccessor::DirEntries{{"entry", SourceAccessor::tRegular}}));
                        break;
                    case Point::DirectoryScope:
                        accessor->readDirectory(path, [&](SourceAccessor & scoped, const CanonPath & relative) {
                            callback();
                            EXPECT_EQ(scoped.number, childNumber);
                            EXPECT_EQ(relative, path);
                        });
                        break;
                    case Point::DirectoryEntries:
                        accessor->readDirectoryWithEntries(
                            path,
                            [&](SourceAccessor & scoped,
                                const CanonPath & relative,
                                SourceAccessor::DirectoryListing && listing) {
                                const auto & entries = listing.entries();
                                callback();
                                EXPECT_EQ(scoped.number, childNumber);
                                EXPECT_EQ(relative, path);
                                EXPECT_EQ(entries, (SourceAccessor::DirEntries{{"entry", SourceAccessor::tRegular}}));
                            });
                        break;
                    case Point::Link:
                        EXPECT_EQ(accessor->readLink(path), "target");
                        break;
                    case Point::Display:
                        EXPECT_EQ(accessor->showPath(path), "shown");
                        break;
                    case Point::Invalidate:
                        accessor->invalidateCache();
                        break;
                    case Point::Physical:
                        EXPECT_EQ(accessor->getPhysicalPath(path), std::filesystem::path("physical"));
                        break;
                    case Point::Fingerprint:
                        EXPECT_EQ(accessor->getFingerprint(path), (std::pair{path, std::string("fingerprint")}));
                        break;
                    case Point::KnownEntry:
                        EXPECT_EQ(
                            accessor->getKnownEntry(path),
                            (merkle::TreeEntry{merkle::Mode::Regular, hashString(HashAlgorithm::SHA256, "bytes")}));
                        break;
                    }
                };
                if (throwing)
                    EXPECT_THROW(run(), CachingCallbackException);
                else
                    EXPECT_NO_THROW(run());
                EXPECT_EQ(calls, 1u);
                EXPECT_EQ(weakOwner.expired(), release);
                EXPECT_EQ(weakChild.expired(), release);
                owner.reset();
                EXPECT_TRUE(weakOwner.expired());
                EXPECT_TRUE(weakChild.expired());
            }
}

TEST(SourceAccessor, compoundDefaultsOwnTheirCallbackContinuations)
{
    enum class Event { Stat, Directory, Read, Link, Display, Scope, Sink, Filter };
    enum class Operation { Stat, Scope, Entries, Resolve, Dump };
    enum class Ownership { Borrowed, Retained, Released };

    struct CallbackStopped
    {};

    struct OwnershipLost
    {};

    struct Probe : MemorySourceAccessor
    {
        std::function<void(Event)> observe;

        void notify(Event event)
        {
            auto callback = observe;
            callback(event);
        }

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            auto result = MemorySourceAccessor::maybeLstat(path);
            notify(Event::Stat);
            return result;
        }

        /* Traversal gets a primitive stat with no nested lifetime guard;
           the stat case below invokes the base composition explicitly. */
        Stat lstat(const CanonPath & path) override
        {
            auto result = MemorySourceAccessor::maybeLstat(path).value();
            notify(Event::Stat);
            return result;
        }

        DirEntries readDirectory(const CanonPath & path) override
        {
            auto result = MemorySourceAccessor::readDirectory(path);
            notify(Event::Directory);
            return result;
        }

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            StringSink result;
            MemorySourceAccessor::readFile(path, result, [](uint64_t) {});
            notify(Event::Read);
            sizeCallback(result.s.size());
            sink(result.s);
        }

        std::string readLink(const CanonPath & path) override
        {
            auto result = MemorySourceAccessor::readLink(path);
            notify(Event::Link);
            return result;
        }

        std::string showPath(const CanonPath & path) override
        {
            auto result = "shown:" + path.abs();
            notify(Event::Display);
            return result;
        }
    };

    auto reference = make_ref<MemorySourceAccessor>();
    reference->addFile(CanonPath("plain"), "bytes");
    reference->addFile(CanonPath("dir/file"), "bytes");
    MemorySink memory{*reference};
    memory.createSymlink(CanonPath("dir/link"), "file");
    memory.createSymlink(CanonPath("relative"), "dir");
    memory.createSymlink(CanonPath("absolute"), "/dir");
    auto dumpedTree = *reference->open(CanonPath("dir"), std::nullopt);

    struct Case
    {
        Operation operation;
        const char * path;
        const char * expected;
        SymlinkResolution mode = SymlinkResolution::Full;
    };

    const Case cases[]{
        {Operation::Stat, "plain", "regular"},
        {Operation::Stat, "missing", "missing"},
        {Operation::Scope, "dir", "scope"},
        {Operation::Entries, "dir", "file,link,"},
        {Operation::Resolve, "dir/file", "/dir/file"},
        {Operation::Resolve, "missing/file", "/missing/file"},
        {Operation::Resolve, "relative/file", "/dir/file"},
        {Operation::Resolve, "absolute/file", "/dir/file"},
        {Operation::Resolve, "relative", "/dir"},
        {Operation::Resolve, "relative/file", "/dir/file", SymlinkResolution::Ancestors},
        {Operation::Resolve, "relative", "/relative", SymlinkResolution::Ancestors},
        {Operation::Dump, "dir", "dumped"},
    };
    for (const auto & test : cases) {
        CanonPath path(test.path);
        auto run = [&](std::optional<size_t> at, Ownership ownership, bool stop) {
            std::vector<Event> observed;
            std::shared_ptr<Probe> owner;
            std::unique_ptr<Probe> borrowed;
            if (ownership == Ownership::Borrowed)
                borrowed = std::make_unique<Probe>();
            else
                owner = std::make_shared<Probe>();
            std::weak_ptr<SourceAccessor> weakOwner = owner;
            SourceAccessor * accessor = owner ? owner.get() : borrowed.get();
            auto * probe = owner ? owner.get() : borrowed.get();
            probe->root = reference->root;
            auto observe = [&](Event event) {
                observed.push_back(event);
                if (at != observed.size() - 1)
                    return;
                if (ownership == Ownership::Released)
                    owner.reset();
                if (ownership != Ownership::Borrowed) {
                    auto alive = !weakOwner.expired();
                    EXPECT_TRUE(alive);
                    /* The probe copied all results and its callback before
                       notification. Abort before a removed base guard could
                       resume through a destroyed accessor. */
                    if (!alive)
                        throw OwnershipLost{};
                }
                if (stop)
                    throw CallbackStopped{};
            };
            probe->observe = observe;
            auto invoke = [&]() -> std::string {
                switch (test.operation) {
                case Operation::Stat:
                    try {
                        return accessor->SourceAccessor::lstat(path).typeString();
                    } catch (FileNotFound &) {
                        return "missing";
                    }
                case Operation::Scope:
                    accessor->readDirectory(path, [&](SourceAccessor & scoped, const CanonPath & relative) {
                        observe(Event::Scope);
                        EXPECT_EQ(&scoped, accessor);
                        EXPECT_EQ(relative, path);
                    });
                    return "scope";
                case Operation::Entries: {
                    std::string names;
                    accessor->readDirectoryWithEntries(
                        path,
                        [&](SourceAccessor & scoped,
                            const CanonPath & relative,
                            SourceAccessor::DirectoryListing && listing) {
                            const auto & entries = listing.entries();
                            observe(Event::Scope);
                            EXPECT_EQ(&scoped, accessor);
                            EXPECT_EQ(relative, path);
                            for (const auto & [name, type] : entries)
                                names += name + ",";
                        });
                    return names;
                }
                case Operation::Resolve:
                    return accessor->resolveSymlinks(path, test.mode).abs();
                case Operation::Dump: {
                    std::string nar;
                    LambdaSink sink{[&](std::string_view bytes) {
                        observe(Event::Sink);
                        nar += bytes;
                    }};
                    PathFilter filter = [&](const std::string &) {
                        observe(Event::Filter);
                        return true;
                    };
                    accessor->dumpPath(path, sink, filter);
                    MemorySourceAccessor restored;
                    MemorySink destination{restored};
                    StringSource source{nar};
                    parseDump(destination, source);
                    EXPECT_EQ(restored.root, dumpedTree);
                    return "dumped";
                }
                }
                throw Error("unhandled compound accessor operation");
            };
            if (stop)
                EXPECT_THROW(invoke(), CallbackStopped);
            else
                EXPECT_NO_THROW(EXPECT_EQ(invoke(), test.expected));
            if (ownership != Ownership::Borrowed)
                EXPECT_EQ(weakOwner.expired(), ownership == Ownership::Released && at.has_value());
            owner.reset();
            EXPECT_TRUE(weakOwner.expired());
            return observed;
        };
        /* Results come from the explicit tree above, including an independent
           NAR parse. The baseline counts callback boundaries only: exercise
           every boundary with shared or borrowed lifetime and both outcomes. */
        auto baseline = run(std::nullopt, Ownership::Retained, false);
        for (size_t at = 0; at < baseline.size(); ++at)
            for (auto ownership : {Ownership::Borrowed, Ownership::Retained, Ownership::Released})
                for (bool stop : {false, true}) {
                    SCOPED_TRACE(
                        ::testing::Message() << "operation=" << int(test.operation) << " path=" << path.abs()
                                             << " at=" << at << " ownership=" << int(ownership) << " stop=" << stop);
                    auto observed = run(at, ownership, stop);
                    auto expected = baseline;
                    if (stop)
                        expected.resize(at + 1);
                    EXPECT_EQ(observed, expected);
                }
    }
}

/* MemorySourceAccessor remains a mutable source. A sink may overwrite the
   source while consuming the bytes already delivered to this callback. */
TEST(MemorySourceAccessor, reentrantSinkConsumesDeliveredBytesBeforeLaterMutation)
{
    for (size_t length : {1u, 8192u}) {
        SCOPED_TRACE(length);
        auto accessor = make_ref<MemorySourceAccessor>();
        const CanonPath file("file");
        accessor->addFile(file, std::string(length, 'A'));
        std::string received;
        LambdaSink sink([&](std::string_view bytes) {
            auto & current = std::get<MemorySourceAccessor::File::Regular>(accessor->open(file, std::nullopt)->raw);
            std::fill(current.contents.begin(), current.contents.end(), 'B');
            received.append(bytes);
        });
        accessor->readFile(file, sink, [&](uint64_t size) { EXPECT_EQ(size, length); });
        EXPECT_EQ(received, std::string(length, 'A'));
        EXPECT_EQ(accessor->readFile(file), std::string(length, 'B'));
    }
}

} // namespace nix
