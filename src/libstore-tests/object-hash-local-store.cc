/**
 * The local store's column under the object hash (doc/lazy-store/04-
 * derivation.md section 1.9, "The local database"; 01 section 9.11 "The
 * database"): `ValidPaths.hash` holds `objectHash.render()`; a schema-10
 * row's `sha256:` value is read as `assertedNarHash` and migrated on the
 * first `queryPathInfo` unless the store is read-only; `migratePathInfo`
 * does the same on demand; `verifyStore --check-contents` compares the
 * recomputed object hash with the row; schema 11, and a newer schema is
 * refused.
 *
 * A `LocalStore` in a temporary directory (`TempLocalStoreTest`,
 * nix/store/tests/temp-local-store.hh, shared with local-store.cc); the
 * old row is planted by that header's `plantSchema10Row`.
 */
#include <gtest/gtest.h>

#include "nix/store/local-store.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/store/tests/temp-local-store.hh"

#include "nix/util/archive.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/strings.hh"
// Needed for template specialisations. This is not good! When we
// overhaul how store configs work, this should be fixed.
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

#include <optional>

#ifndef _WIN32

namespace nix {

class ObjectHashLocalStore : public TempLocalStoreTest
{
protected:
    std::filesystem::path schemaPath() const
    {
        return config->stateDir.get() / "db" / "schema";
    }

    /* The column as the database holds it (temp-local-store.hh). */
    std::string hashColumn(const StorePath & path)
    {
        return readHashColumn(*store, path);
    }

    /* A schema-10 row for `path`: the NAR's hash in the old rendering, the
       NAR's size unless told otherwise (temp-local-store.hh; drops the
       store's path-info cache). */
    void plantOldRow(const StorePath & path, const std::string & nar, std::optional<uint64_t> narSize = std::nullopt)
    {
        plantSchema10Row(*store, path, hashString(HashAlgorithm::SHA256, nar), narSize.value_or(nar.size()));
    }

    /* The rendering the old row holds. */
    static std::string oldRow(const Hash & narHash)
    {
        return narHash.to_string(HashFormat::Base16, true);
    }

    /* A small tree: two files (one executable), a nested directory, a
       symlink. */
    static std::string smallNar()
    {
        auto acc = make_ref<MemorySourceAccessor>();
        acc->addFile(CanonPath{"/a"}, "one\n");
        acc->addFile(CanonPath{"/d/b"}, "two\n");
        acc->addFile(CanonPath{"/d/x"}, "#!/bin/sh\nexit 0\n");
        std::get<MemorySourceAccessor::File::Regular>(acc->open(CanonPath{"/d/x"}, std::nullopt)->raw).executable =
            true;
        acc->open(CanonPath{"/d/l"}, MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{.target = "b"}});
        StringSink nar;
        acc->dumpPath(CanonPath::root, nar);
        return std::move(nar.s);
    }

    StorePath addNar(std::string_view name, const std::string & nar)
    {
        StringSource source{nar};
        return store->addToStoreFromDump(
            source,
            name,
            FileSerialisationMethod::NixArchive,
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256,
            {},
            NoRepair);
    }

    /* The independent computation of the object hash: the walk, without
       hooks, over the path as written. */
    ObjectHash objectHashOnDisk(const StorePath & path)
    {
        return ObjectHash::of(objectHashOf(*makeFSSourceAccessor(store->toRealPath(path)), CanonPath::root).root);
    }
};

/* (a) A path added from a NAR: the row holds its object hash, which is the
   walk's; the NAR size is the NAR's length; nothing is asserted. */
TEST_F(ObjectHashLocalStore, addToStoreFromDump_sets_objectHash_and_narSize)
{
    auto nar = smallNar();
    auto p = addNar("small", nar);
    auto info = store->queryPathInfo(p);

    ASSERT_TRUE(info->objectHash.has_value());
    EXPECT_EQ(*info->objectHash, objectHashOnDisk(p));
    EXPECT_EQ(info->narSize, nar.size());
    EXPECT_FALSE(info->assertedNarHash.has_value()) << "an assertion is verified and never stored";

    /* The column is the rendering. */
    EXPECT_EQ(hashColumn(p), info->objectHash->render());

    /* And the stream's own object hash agrees: route independence. */
    StringSource in{nar};
    auto viaNar = objectHashOfNar(in);
    EXPECT_EQ(ObjectHash::of(viaNar.root), *info->objectHash);
    EXPECT_EQ(viaNar.narSize, info->narSize);
}

/* (c) Read-only: the object hash is produced, the column is left alone. */
TEST_F(ObjectHashLocalStore, read_only_store_does_not_write_the_migration)
{
    auto nar = smallNar();
    auto p = addNar("ro", nar);
    auto expectedObjectHash = *store->queryPathInfo(p)->objectHash;
    auto narHash = hashString(HashAlgorithm::SHA256, nar);

    /* A read-only store opens the database immutable (local-store.cc,
       `openMode`), which does not read the WAL: plant the row, then close
       the read-write connection -- the last connection's close checkpoints
       the WAL, so the old row is in the main file. */
    plantOldRow(p, nar);
    ASSERT_EQ(hashColumn(p), oldRow(narHash));
    store.reset();

    EnableExperimentalFeature readOnlyLocalStore{"read-only-local-store"};
    auto roConfig = std::make_shared<LocalStoreConfig>(tempStoreDir.path(), StoreConfig::Params{{"read-only", "true"}});
    ASSERT_TRUE(roConfig->readOnly);
    auto ro = std::make_shared<LocalStore>(ref{roConfig});

    auto info = ro->queryPathInfo(p);
    ASSERT_TRUE(info->objectHash.has_value());
    EXPECT_EQ(*info->objectHash, expectedObjectHash);
    ASSERT_TRUE(info->assertedNarHash.has_value());
    EXPECT_EQ(*info->assertedNarHash, narHash);

    ro.reset();

    /* Reopened read-write: the column is unchanged, then the first query
       migrates it. */
    store = std::make_shared<LocalStore>(ref{config});
    EXPECT_EQ(hashColumn(p), oldRow(narHash)) << "read-only: the column is unchanged";
    EXPECT_EQ(*store->queryPathInfo(p)->objectHash, expectedObjectHash);
    EXPECT_EQ(hashColumn(p), expectedObjectHash.render());
}

/* (d) `migratePathInfo` on an old row migrates it; on a new row it is a
   no-op. */
TEST_F(ObjectHashLocalStore, migratePathInfo_migrates_an_old_row)
{
    auto nar = smallNar();
    auto p = addNar("migrate", nar);
    auto expectedObjectHash = *store->queryPathInfo(p)->objectHash;

    /* The old row with no size, as a store older still may have left it
       (local-store.cc, "narSize = NULL yields 0"). */
    plantOldRow(p, nar, /*narSize=*/0);
    ASSERT_EQ(store->queryPathInfo(p)->narSize, nar.size()) << "the query fills the size from the walk";
    plantOldRow(p, nar, /*narSize=*/0);
    store->migratePathInfo(p);
    EXPECT_EQ(hashColumn(p), expectedObjectHash.render());

    store->clearPathInfoCache();
    auto info = store->queryPathInfo(p);
    EXPECT_EQ(info->objectHash, expectedObjectHash);
    EXPECT_FALSE(info->assertedNarHash.has_value());
    EXPECT_EQ(info->narSize, nar.size()) << "the migration fills the size";

    /* Already migrated: unchanged. */
    store->migratePathInfo(p);
    EXPECT_EQ(hashColumn(p), expectedObjectHash.render());

    /* An invalid path is refused. */
    EXPECT_THROW(store->migratePathInfo(StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-nope"}), InvalidPath);
}

/* (e) `verifyStore(checkContents)`: clean is false; a modified file is
   reported, true. */
TEST_F(ObjectHashLocalStore, verifyStore_checkContents_reports_a_modified_path)
{
    auto nar = smallNar();
    auto p = addNar("verify", nar);

    EXPECT_FALSE(store->verifyStore(true, NoRepair)) << "a clean store has no errors";

    auto file = store->toRealPath(p) / "a";
    std::filesystem::permissions(file, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    writeFile(file, "one\ncorrupt\n");

    EXPECT_TRUE(store->verifyStore(true, NoRepair)) << "the modified path is an error";

    /* The check is against the object hash: the row is untouched by the
       corruption and still differs from the walk. */
    EXPECT_NE(ObjectHash::parseOrThrow(hashColumn(p)), objectHashOnDisk(p));
}

/* (f) Schema 11; a store at a newer schema is refused. */
TEST_F(ObjectHashLocalStore, schema_is_11_and_newer_is_refused)
{
    EXPECT_EQ(trim(readFile(schemaPath())), "11");

    store.reset();
    writeFile(schemaPath(), "12");
    EXPECT_THROW(std::make_shared<LocalStore>(ref{config}), Error);

    /* Back to 11, the store opens again. */
    writeFile(schemaPath(), "11");
    EXPECT_NO_THROW(store = std::make_shared<LocalStore>(ref{config}));
}

} // namespace nix

#endif
