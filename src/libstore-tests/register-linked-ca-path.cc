#include <gtest/gtest.h>

#include <sys/stat.h>

#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/source-path.hh"

// Needed for template specialisations (see local-store.cc).
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

namespace nix {

/* copy-once-link-N primitive (Perf #1): `LocalStore::registerLinkedCAPath`
   materialises a sibling CA path by hardlinking the first's file tree
   rather than re-copying. These tests use a per-process temp local store
   and a small on-disk DIRECTORY tree (so linkOrCopyTree's recursion +
   leaf-linking is exercised, not just a flat file). */
class RegisterLinkedCAPathTest : public ::testing::Test
{
protected:
    std::filesystem::path storeRoot;
    ref<LocalStore> store;

    RegisterLinkedCAPathTest()
        : storeRoot(createTempDir())
        , store([&] {
            auto s = openStore("local?root=" + storeRoot.string());
            auto local = s.dynamic_pointer_cast<LocalStore>();
            assert(local);
            return ref<LocalStore>(local);
        }())
    {
    }

    /* Build a small directory tree on disk and add it as a NixArchive CA
       path under `name`. Content is fixed so two different names share
       one NAR (the NAR does not encode the store-path name). */
    StorePath addTree(std::string_view name)
    {
        auto dir = createTempDir();
        createDirs(dir / "sub");
        writeFile(dir / "a.txt", "alpha");
        writeFile(dir / "sub" / "b.txt", "beta");
        auto accessor = makeFSSourceAccessor(dir);
        /* Call through the Store interface: LocalStore declares a
           different addToStore overload that hides the base
           name+SourcePath one. */
        return static_cast<Store &>(*store).addToStore(
            name, SourcePath{accessor, CanonPath::root}, ContentAddressMethod::Raw::NixArchive);
    }
};

TEST_F(RegisterLinkedCAPathTest, LinksSiblingValidAndContentEqual)
{
    auto a = addTree("src-a");
    ASSERT_TRUE(store->isValidPath(a));

    auto infoA = store->queryPathInfo(a);

    /* Build the sibling info: same content (narHash/narSize/method), a
       DIFFERENT name. No self-reference (the premise of linking). */
    auto desc = ContentAddressWithReferences::fromParts(
        ContentAddressMethod::Raw::NixArchive, infoA->narHash, StoreReferences{.others = {}, .self = false});
    auto bInfo = ValidPathInfo::makeFromCA(*store, "src-b", std::move(desc), infoA->narHash);
    bInfo.narSize = infoA->narSize;

    /* The sibling path must differ from A (different name ⇒ different
       hash-part), else the test proves nothing. */
    ASSERT_NE(bInfo.path, a);
    ASSERT_FALSE(store->isValidPath(bInfo.path));

    store->registerLinkedCAPath(a, bInfo);

    /* B is now a first-class valid path. */
    EXPECT_TRUE(store->isValidPath(bInfo.path));

    /* Content equality: the registered narHash/narSize match A's. */
    auto infoB = store->queryPathInfo(bInfo.path);
    EXPECT_EQ(infoB->narHash, infoA->narHash);
    EXPECT_EQ(infoB->narSize, infoA->narSize);

    /* The file bytes are reachable and correct under B. */
    EXPECT_EQ(readFile(store->toRealPath(bInfo.path) / "a.txt"), "alpha");
    EXPECT_EQ(readFile(store->toRealPath(bInfo.path) / "sub" / "b.txt"), "beta");

    /* Inode sharing: leaves under B are hardlinked from A (same inode,
       link count > 1). */
    struct stat stA, stB;
    ASSERT_EQ(::lstat((store->toRealPath(a) / "a.txt").c_str(), &stA), 0);
    ASSERT_EQ(::lstat((store->toRealPath(bInfo.path) / "a.txt").c_str(), &stB), 0);
    EXPECT_EQ(stA.st_ino, stB.st_ino) << "sibling leaf was copied, not hardlinked";
    EXPECT_GT(stB.st_nlink, 1u) << "link count should reflect the shared inode";
}

TEST_F(RegisterLinkedCAPathTest, IdempotentWhenAlreadyValid)
{
    auto a = addTree("src-a");
    auto infoA = store->queryPathInfo(a);
    auto desc = ContentAddressWithReferences::fromParts(
        ContentAddressMethod::Raw::NixArchive, infoA->narHash, StoreReferences{.others = {}, .self = false});
    auto bInfo = ValidPathInfo::makeFromCA(*store, "src-b", std::move(desc), infoA->narHash);
    bInfo.narSize = infoA->narSize;

    store->registerLinkedCAPath(a, bInfo);
    ASSERT_TRUE(store->isValidPath(bInfo.path));
    /* Second call is a no-op (already valid), must not throw. */
    EXPECT_NO_THROW(store->registerLinkedCAPath(a, bInfo));
    EXPECT_TRUE(store->isValidPath(bInfo.path));
}

} // namespace nix
