#include <gtest/gtest.h>

#include "nix/store/derivations.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#ifndef _WIN32

#  include <filesystem>
#  include <fstream>

namespace nix {

namespace {

/* Create a LocalStore backed by a temporary directory, plus a cleanup
   guard that removes the directory when the test finishes. */
struct TempLocalStore
{
    std::filesystem::path root;
    ref<Store> store;

    static TempLocalStore make()
    {
        auto root = createTempDir();
        std::filesystem::create_directories(root / "nix/store");
        return TempLocalStore{root, openStore(fmt("local?root=%s", root.string()))};
    }

    LocalStore & local()
    {
        /* openStore returns a ref<Store>; unwrap through the shared_ptr
           to dynamic-cast to the concrete LocalStore. */
        auto sp = std::shared_ptr<Store>(store.get_ptr());
        auto lp = std::dynamic_pointer_cast<LocalStore>(sp);
        if (!lp)
            throw Error("expected LocalStore for local?root=...");
        return *lp;
    }

    ~TempLocalStore()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

/* Register a trivial derivation with no outputs and no content so we
   have a valid StorePath we can later invalidate. */
StorePath addTrivialPath(LocalStore & store, const std::string & name)
{
    auto path = StorePath::random(name);
    std::ofstream out(store.config->realStoreDir.get() / std::string(path.to_string()), std::ios::binary);
    out << "placeholder contents for " << name;
    out.close();

    ValidPathInfo info{path, UnkeyedValidPathInfo(store, Hash::dummy)};
    info.narSize = 42;

    ValidPathInfos infos;
    infos.emplace(path, std::move(info));
    store.registerValidPaths(infos);
    return path;
}

} // namespace

TEST(InvalidatePathsChecked, empty)
{
    auto t = TempLocalStore::make();
    /* An empty batch is a no-op and must not crash. */
    EXPECT_NO_THROW(t.local().invalidatePathsChecked({}));
}

TEST(InvalidatePathsChecked, singleDisjoint)
{
    auto t = TempLocalStore::make();

    auto p1 = addTrivialPath(t.local(), "invalid-single");
    EXPECT_TRUE(t.local().isValidPath(p1));

    t.local().invalidatePathsChecked({p1});

    EXPECT_FALSE(t.local().isValidPath(p1));
}

TEST(InvalidatePathsChecked, manyDisjoint)
{
    auto t = TempLocalStore::make();

    /* Register many paths and invalidate them all in one batch. None
       reference each other, so the happy path exercises the whole
       transaction + deferred-cache flow. */
    std::vector<StorePath> paths;
    paths.reserve(50);
    for (int i = 0; i < 50; ++i)
        paths.push_back(addTrivialPath(t.local(), fmt("invalid-many-%d", i)));

    for (auto & p : paths)
        EXPECT_TRUE(t.local().isValidPath(p));

    t.local().invalidatePathsChecked(paths);

    for (auto & p : paths)
        EXPECT_FALSE(t.local().isValidPath(p));
}

TEST(InvalidatePathsChecked, idempotentOnAlreadyInvalid)
{
    auto t = TempLocalStore::make();
    auto p = addTrivialPath(t.local(), "invalid-idem");

    t.local().invalidatePathsChecked({p});
    EXPECT_FALSE(t.local().isValidPath(p));

    /* Running a second time must be a no-op, not an error. */
    EXPECT_NO_THROW(t.local().invalidatePathsChecked({p}));
    EXPECT_FALSE(t.local().isValidPath(p));
}

TEST(InvalidatePathsChecked, mixedValidInvalid)
{
    auto t = TempLocalStore::make();

    auto keep = addTrivialPath(t.local(), "invalid-mix-keep");
    auto drop1 = addTrivialPath(t.local(), "invalid-mix-drop1");
    auto drop2 = addTrivialPath(t.local(), "invalid-mix-drop2");

    /* First invalidate drop1 alone. */
    t.local().invalidatePathsChecked({drop1});
    EXPECT_FALSE(t.local().isValidPath(drop1));
    EXPECT_TRUE(t.local().isValidPath(keep));
    EXPECT_TRUE(t.local().isValidPath(drop2));

    /* Now a batch including the already-invalid drop1. It must skip
       drop1 (isValidPath_ returns false) and invalidate drop2. */
    t.local().invalidatePathsChecked({drop1, drop2});
    EXPECT_FALSE(t.local().isValidPath(drop1));
    EXPECT_FALSE(t.local().isValidPath(drop2));
    EXPECT_TRUE(t.local().isValidPath(keep));
}

} // namespace nix

#endif
