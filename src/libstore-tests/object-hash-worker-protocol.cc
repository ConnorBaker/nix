/**
 * The path-info slot on the wire under the object hash (doc/lazy-store/04-
 * derivation.md section 1.9, "The forms in this step"): with the worker
 * feature `object-hash` the slot carries `objectHash.render()` or "" and a
 * second string, the asserted NAR hash in base16 or ""; without it the slot
 * is the NAR hash as an old peer sends it, read into `assertedNarHash`.  The
 * serve protocol takes the two-string form from one minor version up.
 *
 * Plain round trips through a `StringSink`/`StringSource`, no golden files.
 */
#include <gtest/gtest.h>

#include "nix/store/worker-protocol.hh"
#include "nix/store/worker-protocol-impl.hh"
#include "nix/store/serve-protocol.hh"
#include "nix/store/serve-protocol-impl.hh"
#include "nix/store/path-info.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/object-hash.hh"
#include "object-hash-fixtures.hh"

namespace nix {

using namespace object_hash_fixtures;

namespace {

UnkeyedValidPathInfo baseInfo(const StoreDirConfig & store, std::optional<ObjectHash> oh)
{
    UnkeyedValidPathInfo info{store, oh};
    info.deriver = StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv"};
    info.references = {
        StorePath{"g1w7hyyyy1w7hy3qg1w7hy3qgqqqqy3q-foo.drv"},
        StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar"},
    };
    info.registrationTime = 23423;
    info.narSize = 34878;
    info.ultimate = true;
    info.sigs = {
        Signature{.keyName = "asdf", .sig = std::string(64, '\0')},
    };
    info.ca = ContentAddress{
        .method = ContentAddressMethod::Raw::NixArchive,
        .hash = hashString(HashAlgorithm::SHA256, "(...)"),
    };
    return info;
}

} // namespace

class ObjectHashWorkerProto : public ::testing::Test
{
protected:
    /* `StoreDirConfig` holds a reference to the string: keep it alive. */
    std::string storeDirStr = "/nix/store";
    StoreDirConfig store{storeDirStr};

    WorkerProto::Version withFeature{
        .number = WorkerProto::latest.number,
        .features = {std::string{WorkerProto::featureObjectHash}},
    };

    WorkerProto::Version withoutFeature{
        .number = WorkerProto::latest.number,
        .features = {},
    };

    std::string write(const WorkerProto::Version & version, const UnkeyedValidPathInfo & info)
    {
        StringSink to;
        WorkerProto::Serialise<UnkeyedValidPathInfo>::write(
            store, WorkerProto::WriteConn{.to = to, .version = version}, info);
        return std::move(to.s);
    }

    UnkeyedValidPathInfo read(const WorkerProto::Version & version, const std::string & bytes)
    {
        StringSource from{bytes};
        return WorkerProto::Serialise<UnkeyedValidPathInfo>::read(
            store, WorkerProto::ReadConn{.from = from, .version = version});
    }

    /* The old wire form of the slot, as a peer without the feature writes
       it (worker-protocol.cc before this step): deriver, the hash as one
       base16 string, references, registrationTime, narSize, ultimate,
       sigs, ca. */
    std::string oldForm(const UnkeyedValidPathInfo & info, std::string_view hashSlot)
    {
        StringSink to;
        to << (info.deriver ? store.printStorePath(*info.deriver) : "");
        to << hashSlot;
        to << (uint64_t) info.references.size();
        for (auto & r : info.references)
            to << store.printStorePath(r);
        to << (uint64_t) info.registrationTime << info.narSize;
        to << (uint64_t) info.ultimate;
        to << (uint64_t) info.sigs.size();
        for (auto & s : info.sigs)
            to << s.to_string();
        to << renderContentAddress(info.ca);
        return std::move(to.s);
    }
};

/* The feature is named as the spec names it. */
TEST_F(ObjectHashWorkerProto, feature_name)
{
    EXPECT_EQ(WorkerProto::featureObjectHash, "object-hash");
    EXPECT_TRUE(WorkerProto::latest.features.contains(std::string{WorkerProto::featureObjectHash}))
        << "the latest version offers the feature";
}

/* With the feature: object hash set, no assertion. */
TEST_F(ObjectHashWorkerProto, round_trip_objectHash_only_with_feature)
{
    auto info = baseInfo(store, objectHash());
    ASSERT_FALSE(info.assertedNarHash.has_value());
    auto got = read(withFeature, write(withFeature, info));
    EXPECT_EQ(got, info);
    ASSERT_TRUE(got.objectHash.has_value());
    EXPECT_EQ(*got.objectHash, objectHash());
    EXPECT_FALSE(got.assertedNarHash.has_value());
}

/* With the feature: both set. */
TEST_F(ObjectHashWorkerProto, round_trip_both_with_feature)
{
    auto info = baseInfo(store, objectHash());
    info.assertedNarHash = narHash();
    auto got = read(withFeature, write(withFeature, info));
    EXPECT_EQ(got, info);
    ASSERT_TRUE(got.assertedNarHash.has_value());
    EXPECT_EQ(*got.assertedNarHash, narHash());
}

/* With the feature: neither set, both strings empty -- allowed on the wire
   (a substituter's description before the NAR arrives). */
TEST_F(ObjectHashWorkerProto, round_trip_neither_with_feature)
{
    auto info = baseInfo(store, std::nullopt);
    auto got = read(withFeature, write(withFeature, info));
    EXPECT_EQ(got, info);
    EXPECT_FALSE(got.objectHash.has_value());
    EXPECT_FALSE(got.assertedNarHash.has_value());
}

/* With the feature, the bytes of the slot are the two strings stated: the
   rendering, then the base16 NAR hash or "". */
TEST_F(ObjectHashWorkerProto, wire_form_with_feature_is_two_strings)
{
    auto info = baseInfo(store, objectHash());
    info.assertedNarHash = narHash();
    auto bytes = write(withFeature, info);
    StringSource from{bytes};
    EXPECT_EQ(readString(from), store.printStorePath(*info.deriver));
    EXPECT_EQ(readString(from), objectHashRendered);
    EXPECT_EQ(readString(from), narHashBase16);

    auto none = baseInfo(store, std::nullopt);
    StringSource fromNone{write(withFeature, none)};
    readString(fromNone);
    EXPECT_EQ(readString(fromNone), "");
    EXPECT_EQ(readString(fromNone), "");
}

/* Without the feature, an info that has no NAR hash to send cannot be
   written: the caller must supply one (`narHashOf`) before reaching the
   serialiser. */
TEST_F(ObjectHashWorkerProto, write_without_feature_throws_without_assertedNarHash)
{
    auto info = baseInfo(store, objectHash());
    ASSERT_FALSE(info.assertedNarHash.has_value());
    EXPECT_THROW(write(withoutFeature, info), Error);
}

/* Without the feature, an info with no asserted NAR hash but a lazy one is
   written in the old form with the thunk's value, forced here and nowhere
   else; with the feature the slot stays empty and the thunk is never
   forced -- the wire of two new peers does not change. */
TEST_F(ObjectHashWorkerProto, lazy_nar_hash_is_forced_by_the_old_form_alone)
{
    auto info = baseInfo(store, objectHash());
    unsigned forced = 0;
    info.lazyNarHash = [&] {
        forced++;
        return narHash();
    };
    EXPECT_EQ(write(withoutFeature, info), oldForm(info, narHashBase16));
    EXPECT_EQ(forced, 1u);

    auto plain = baseInfo(store, objectHash());
    EXPECT_EQ(write(withFeature, info), write(withFeature, plain));
    EXPECT_EQ(forced, 1u);
}

/* Without the feature, an info with an asserted NAR hash is written in the
   old form, and read back as an old peer's reply: the assertion set, no
   object hash. */
TEST_F(ObjectHashWorkerProto, write_without_feature_sends_the_nar_hash)
{
    auto info = baseInfo(store, objectHash());
    info.assertedNarHash = narHash();
    auto bytes = write(withoutFeature, info);
    EXPECT_EQ(bytes, oldForm(info, narHashBase16));

    auto got = read(withoutFeature, bytes);
    EXPECT_FALSE(got.objectHash.has_value());
    ASSERT_TRUE(got.assertedNarHash.has_value());
    EXPECT_EQ(*got.assertedNarHash, narHash());
    EXPECT_EQ(got.narSize, info.narSize);
    EXPECT_EQ(got.references, info.references);
    EXPECT_EQ(got.deriver, info.deriver);
    EXPECT_EQ(got.sigs, info.sigs);
    EXPECT_EQ(got.ca, info.ca);
}

/* Without the feature, the old reader's slot is a NAR hash: a rendering
   there is not one. */
TEST_F(ObjectHashWorkerProto, read_without_feature_rejects_a_rendering_in_the_slot)
{
    auto info = baseInfo(store, std::nullopt);
    EXPECT_THROW(read(withoutFeature, oldForm(info, objectHashRendered)), Error);
}

/* --- serve protocol ----------------------------------------------------- */

class ObjectHashServeProto : public ::testing::Test
{
protected:
    /* `StoreDirConfig` holds a reference to the string: keep it alive. */
    std::string storeDirStr = "/nix/store";
    StoreDirConfig store{storeDirStr};

    /* The two-string slot begins at 2.9 (serve-protocol.hh, `latest`'s
       comment), and 2.9 is `ServeProto::latest`. */
    ServeProto::Version withObjectHash{.major = 2, .minor = 9};
    ServeProto::Version before{.major = 2, .minor = 8};

    std::string write(ServeProto::Version version, const UnkeyedValidPathInfo & info)
    {
        StringSink to;
        ServeProto::Serialise<UnkeyedValidPathInfo>::write(
            store, ServeProto::WriteConn{.to = to, .version = version}, info);
        return std::move(to.s);
    }

    UnkeyedValidPathInfo read(ServeProto::Version version, const std::string & bytes)
    {
        StringSource from{bytes};
        return ServeProto::Serialise<UnkeyedValidPathInfo>::read(
            store, ServeProto::ReadConn{.from = from, .version = version});
    }
};

TEST_F(ObjectHashServeProto, latest_is_one_minor_up)
{
    EXPECT_EQ(ServeProto::latest, withObjectHash);
}

TEST_F(ObjectHashServeProto, round_trip_from_the_new_version)
{
    /* The serve protocol never carried `registrationTime` or `ultimate`
       (serve-protocol.cc's writer), so the read form has neither. */
    auto serveInfo = [&](std::optional<ObjectHash> oh, std::optional<Hash> asserted) {
        auto i = baseInfo(store, oh);
        i.assertedNarHash = asserted;
        i.registrationTime = 0;
        i.ultimate = false;
        return i;
    };
    for (auto & info :
         {serveInfo(objectHash(), std::nullopt),
          serveInfo(std::nullopt, std::nullopt),
          serveInfo(objectHash(), narHash())}) {
        auto got = read(withObjectHash, write(withObjectHash, info));
        EXPECT_EQ(got, info);
    }
}

/* Before the new version an info without a NAR hash cannot be written, and
   one with it is read back as an assertion only. */
TEST_F(ObjectHashServeProto, before_the_new_version_is_the_nar_hash)
{
    auto info = baseInfo(store, objectHash());
    EXPECT_THROW(write(before, info), Error);

    info.assertedNarHash = narHash();
    auto got = read(before, write(before, info));
    EXPECT_FALSE(got.objectHash.has_value());
    ASSERT_TRUE(got.assertedNarHash.has_value());
    EXPECT_EQ(*got.assertedNarHash, narHash());
    EXPECT_EQ(got.narSize, info.narSize);
    EXPECT_EQ(got.references, info.references);
}

} // namespace nix
