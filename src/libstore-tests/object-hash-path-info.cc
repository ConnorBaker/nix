/**
 * Path info under the object hash (doc/lazy-store/04-derivation.md section
 * 1.9, one address; 01 section 9.11 "Signatures" and "JSON
 * and text"): fingerprint version 2 over the object hash, version 1 kept
 * for a row that still asserts a NAR hash, `checkSignatures` deciding
 * between them, and path-info JSON version 4 with `objectHash` required
 * and `narHash` optional, versions 1 to 3 written through the NAR-hash
 * provider.
 */
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "nix/store/path-info.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/signature/local-keys.hh"
#include "nix/util/signature/signer.hh"

#include "nix/util/tests/characterization.hh"
#include "nix/store/tests/libstore.hh"
#include "object-hash-fixtures.hh"

namespace nix {

using nlohmann::json;
using namespace object_hash_fixtures;

namespace {

const SecretKey objectHashTestSecretKey{
    "test-key:tU7tTvLcScf8pmz/eTV0BEtLmRsPpZfKaRcd0nCN+pysBZPHSeg61/u2oc7mIOewfuAY1V1BiX32homTaDJ2Jw=="};

PublicKeys testPublicKeys()
{
    auto pk = objectHashTestSecretKey.toPublicKey();
    PublicKeys keys;
    keys.insert_or_assign(pk.name, pk);
    return keys;
}

/* A fixed input-addressed info: a path, the object hash, two references. */
ValidPathInfo fixedInfo(const StoreDirConfig & store)
{
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-foo"},
        UnkeyedValidPathInfo{store, objectHash()},
    };
    info.narSize = 34878;
    info.references = {
        StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar"},
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-foo"},
    };
    return info;
}

} // namespace

class ObjectHashPathInfo : public LibStoreTest
{
protected:
    /* `StoreDirConfig` holds a reference to the string: keep it alive. */
    std::string storeDirStr = "/nix/store";
    StoreDirConfig cfg{storeDirStr};
};

/* Fingerprint 2, computed by hand: `2;<path>;<objectHash.render()>;<refs>`,
   the references full paths, sorted, comma-separated, no size field. */
TEST_F(ObjectHashPathInfo, fingerprint_v2_literal)
{
    auto info = fixedInfo(cfg);
    EXPECT_EQ(
        info.fingerprint(cfg),
        "2;/nix/store/n5wkd9frr45pa74if5gpz9j7mifg27fh-foo;git:sha256:" + objectHashHex
            + ";/nix/store/g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar,/nix/store/n5wkd9frr45pa74if5gpz9j7mifg27fh-foo");
}

/* No object hash, no fingerprint 2: an info from an old peer cannot be
   signed under version 2. */
TEST_F(ObjectHashPathInfo, fingerprint_throws_without_objectHash)
{
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-foo"},
        UnkeyedValidPathInfo{cfg, std::nullopt},
    };
    info.narSize = 34878;
    info.assertedNarHash = narHash();
    EXPECT_THROW(info.fingerprint(cfg), Error);
}

/* Fingerprint 1 is the old form byte for byte: `1;<path>;sha256:<nix32>;
   <narSize>;<refs>`.  The Nix32 digits are computed outside this code
   base from the same 32 bytes. */
TEST_F(ObjectHashPathInfo, fingerprint_v1_literal)
{
    auto info = fixedInfo(cfg);
    EXPECT_EQ(
        info.fingerprintV1(cfg, narHash()),
        "1;/nix/store/n5wkd9frr45pa74if5gpz9j7mifg27fh-foo;sha256:09ymwqf5i9q7d4dm7x4pjjcqqj0qrcp5lnznbh42gfsci5hcbqqm;34878;"
        "/nix/store/g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar,/nix/store/n5wkd9frr45pa74if5gpz9j7mifg27fh-foo");
    /* And it does not depend on the object hash: the same string without one. */
    ValidPathInfo old{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-foo"},
        UnkeyedValidPathInfo{cfg, std::nullopt},
    };
    old.narSize = info.narSize;
    old.references = info.references;
    EXPECT_EQ(old.fingerprintV1(cfg, narHash()), info.fingerprintV1(cfg, narHash()));
}

/* A version-2 signature verifies. */
TEST_F(ObjectHashPathInfo, checkSignatures_accepts_v2)
{
    auto info = fixedInfo(cfg);
    LocalSigner signer(SecretKey{objectHashTestSecretKey});
    info.sigs.insert(signer.signDetached(info.fingerprint(cfg)));
    EXPECT_EQ(info.checkSignatures(cfg, testPublicKeys()), 1u);
}

/* A version-1 signature verifies only while the info asserts a NAR hash
   (the shim: a row still holding one, an old peer's reply). */
TEST_F(ObjectHashPathInfo, checkSignatures_accepts_v1_only_with_assertedNarHash)
{
    LocalSigner signer(SecretKey{objectHashTestSecretKey});

    auto withAssertion = fixedInfo(cfg);
    withAssertion.assertedNarHash = narHash();
    withAssertion.sigs.insert(signer.signDetached(withAssertion.fingerprintV1(cfg, narHash())));
    EXPECT_EQ(withAssertion.checkSignatures(cfg, testPublicKeys()), 1u);

    auto without = fixedInfo(cfg);
    without.sigs = withAssertion.sigs;
    ASSERT_FALSE(without.assertedNarHash.has_value());
    EXPECT_EQ(without.checkSignatures(cfg, testPublicKeys()), 0u);

    /* A wrong assertion is a wrong fingerprint: the signature does not verify. */
    auto wrongAssertion = fixedInfo(cfg);
    wrongAssertion.assertedNarHash = hashString(HashAlgorithm::SHA256, "not the NAR");
    wrongAssertion.sigs = withAssertion.sigs;
    EXPECT_EQ(wrongAssertion.checkSignatures(cfg, testPublicKeys()), 0u);
}

/* `signV1` signs the version-1 fingerprint, the form every Nix before the
   object hash verifies, and deduplicates as `sign` does; the store itself
   accepts it through the shim (an asserted NAR hash) and not otherwise. */
TEST_F(ObjectHashPathInfo, signV1_verifies_under_fingerprintV1)
{
    auto info = fixedInfo(cfg);
    LocalSigner signer(SecretKey{objectHashTestSecretKey});
    info.signV1(cfg, narHash(), signer);
    ASSERT_EQ(info.sigs.size(), 1u);
    EXPECT_TRUE(verifyDetached(info.fingerprintV1(cfg, narHash()), *info.sigs.begin(), testPublicKeys()));
    EXPECT_FALSE(verifyDetached(info.fingerprint(cfg), *info.sigs.begin(), testPublicKeys()));
    info.signV1(cfg, narHash(), signer);
    EXPECT_EQ(info.sigs.size(), 1u);

    EXPECT_EQ(info.checkSignatures(cfg, testPublicKeys()), 0u);
    info.assertedNarHash = narHash();
    EXPECT_EQ(info.checkSignatures(cfg, testPublicKeys()), 1u);
}

/* A description without an object hash -- a cache written by an older Nix,
   an old daemon's reply -- carries only the NAR hash it asserted, and can
   be signed over the version-1 fingerprint alone: `signV1` needs no object
   hash, only the size.  `nix store sign` signs such an info that way and
   does not ask for the version-2 fingerprint. */
TEST_F(ObjectHashPathInfo, signV1_without_objectHash_does_not_throw)
{
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-foo"},
        UnkeyedValidPathInfo{cfg, std::nullopt},
    };
    info.narSize = 34878;
    info.assertedNarHash = narHash();
    LocalSigner signer(SecretKey{objectHashTestSecretKey});
    EXPECT_NO_THROW(info.signV1(cfg, *info.assertedNarHash, signer));
    ASSERT_EQ(info.sigs.size(), 1u);
    EXPECT_EQ(info.checkSignatures(cfg, testPublicKeys()), 1u);
    /* The version-2 fingerprint is what such an info cannot give. */
    EXPECT_THROW(info.sign(*store, signer), Error);
}

/* One key, both fingerprints signed (what `nix store sign` and the binary
   cache's upload leave): one key, so `checkSignatures` -- and with it
   `nix store verify --sigs-needed` -- counts one. */
TEST_F(ObjectHashPathInfo, checkSignatures_counts_a_key_once_across_both_fingerprints)
{
    auto info = fixedInfo(cfg);
    info.assertedNarHash = narHash();
    LocalSigner signer(SecretKey{objectHashTestSecretKey});
    info.sigs.insert(signer.signDetached(info.fingerprint(cfg)));
    info.signV1(cfg, narHash(), signer);
    ASSERT_EQ(info.sigs.size(), 2u);
    EXPECT_EQ(info.checkSignatures(cfg, testPublicKeys()), 1u);
    /* A second key's signature is a second key. */
    LocalSigner other(SecretKey::generate("other-key"));
    info.sigs.insert(other.signDetached(info.fingerprint(cfg)));
    auto keys = testPublicKeys();
    auto otherPk = other.getPublicKey();
    keys.insert_or_assign(otherPk.name, otherPk);
    EXPECT_EQ(info.checkSignatures(cfg, keys), 2u);
}

/* --- JSON version 4 ----------------------------------------------------- */

/* The fixtures under data/path-info/json-4 mirror json-3's set with
   `objectHash` in place of `narHash`, plus `pure_asserted` carrying both.
   Written by hand from the specification; `_NIX_TEST_ACCEPT=1` regenerates
   them as for the other versions. */
class ObjectHashPathInfoJsonV4 : public CharacterizationTest, public LibStoreTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "path-info" / "json-4";

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / (testStem + ".json");
    }
};

static UnkeyedValidPathInfo makeEmptyV4()
{
    return {"/nix/store", objectHash()};
}

static ValidPathInfo makeFullKeyedV4(const Store & store, bool includeImpureInfo)
{
    auto info = ValidPathInfo::makeFromCA(
        store,
        "foo",
        FixedOutputInfo{
            .method = FileIngestionMethod::NixArchive,
            .hash = hashString(HashAlgorithm::SHA256, "(...)"),
            .references =
                {
                    .others =
                        {
                            StorePath{
                                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar",
                            },
                        },
                    .self = true,
                },
        },
        objectHash());
    info.narSize = 34878;
    if (includeImpureInfo) {
        info.deriver = StorePath{
            "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
        };
        info.registrationTime = 23423;
        info.ultimate = true;
        info.sigs = {
            Signature{.keyName = "asdf", .sig = std::string(64, '\0')},
            Signature{.keyName = "qwer", .sig = std::string(64, '\0')},
        };
    }
    return info;
}

static UnkeyedValidPathInfo makeFullV4(const Store & store, bool includeImpureInfo)
{
    return makeFullKeyedV4(store, includeImpureInfo);
}

static UnkeyedValidPathInfo makeAssertedV4(const Store & store)
{
    auto info = makeFullV4(store, false);
    info.assertedNarHash = narHash();
    return info;
}

#define JSON_READ_TEST_V4(STEM, OBJ)                                                     \
    TEST_F(ObjectHashPathInfoJsonV4, PathInfo_##STEM##_from_json)                        \
    {                                                                                    \
        readTest(#STEM, [&](const auto & encoded_) {                                     \
            auto encoded = json::parse(encoded_);                                        \
            UnkeyedValidPathInfo got = UnkeyedValidPathInfo::fromJSON(nullptr, encoded); \
            auto expected = OBJ;                                                         \
            ASSERT_EQ(got, expected);                                                    \
        });                                                                              \
    }

#define JSON_WRITE_TEST_V4(STEM, OBJ, PURE)                                                           \
    TEST_F(ObjectHashPathInfoJsonV4, PathInfo_##STEM##_to_json)                                       \
    {                                                                                                 \
        writeTest(                                                                                    \
            #STEM,                                                                                    \
            [&]() -> json { return OBJ.toJSON(nullptr, PURE, PathInfoJsonFormat::V4); },              \
            [](const auto & file) { return json::parse(readFile(file)); },                            \
            [](const auto & file, const auto & got) { return writeFile(file, got.dump(2) + "\n"); }); \
    }

#define JSON_TEST_V4(STEM, OBJ, PURE) \
    JSON_READ_TEST_V4(STEM, OBJ)      \
    JSON_WRITE_TEST_V4(STEM, OBJ, PURE)

JSON_TEST_V4(empty_pure, makeEmptyV4(), false)
JSON_TEST_V4(empty_impure, makeEmptyV4(), true)
JSON_TEST_V4(pure, makeFullV4(*store, false), false)
JSON_TEST_V4(impure, makeFullV4(*store, true), true)
JSON_TEST_V4(pure_asserted, makeAssertedV4(*store), false)

#undef JSON_TEST_V4
#undef JSON_READ_TEST_V4
#undef JSON_WRITE_TEST_V4

/* Version 4 without `objectHash` is not version 4. */
TEST_F(ObjectHashPathInfoJsonV4, objectHash_is_required)
{
    auto j = makeFullV4(*store, false).toJSON(nullptr, false, PathInfoJsonFormat::V4);
    j.erase("objectHash");
    EXPECT_THROW(UnkeyedValidPathInfo::fromJSON(nullptr, j), Error);
    /* And a NAR hash in its place does not stand in for it. */
    j["narHash"] = narHashSRI;
    EXPECT_THROW(UnkeyedValidPathInfo::fromJSON(nullptr, j), Error);
}

/* A version-4 `objectHash` that is not a rendering is refused. */
TEST_F(ObjectHashPathInfoJsonV4, objectHash_must_be_a_rendering)
{
    auto j = makeFullV4(*store, false).toJSON(nullptr, false, PathInfoJsonFormat::V4);
    j["objectHash"] = "sha256:" + objectHashHex;
    EXPECT_THROW(UnkeyedValidPathInfo::fromJSON(nullptr, j), Error);
}

/* Writing version 2 for an info without an asserted NAR hash goes through
   the provider: the `narHash` written is what the provider returned. */
TEST_F(ObjectHashPathInfoJsonV4, older_versions_written_through_the_provider)
{
    auto info = makeFullV4(*store, false);
    ASSERT_FALSE(info.assertedNarHash.has_value());
    auto provided = hashString(HashAlgorithm::SHA256, "the NAR the shim would produce");
    size_t calls = 0;
    /* The provider is `toJSON`'s trailing `std::optional<NarHashThunk>`,
       `NarHashThunk = std::function<Hash()>` (path-info.hh). */
    auto j = info.toJSON(nullptr, false, PathInfoJsonFormat::V2, [&]() {
        calls++;
        return provided;
    });
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(j.at("version").get<uint64_t>(), 2u);
    EXPECT_EQ(Hash(j.at("narHash")), provided);
    EXPECT_FALSE(j.contains("objectHash")) << "version 2's shape is unchanged";
    /* The same for version 1, whose hash is an SRI string. */
    auto j1 = info.toJSON(&*store, false, PathInfoJsonFormat::V1, [&]() { return provided; });
    EXPECT_EQ(Hash::parseSRI(j1.at("narHash").get<std::string>()), provided);
    EXPECT_FALSE(j1.contains("objectHash"));
}

/* The constructors and `makeFromCA` take the object hash, possibly absent. */
TEST_F(ObjectHashPathInfoJsonV4, constructors_take_optional_objectHash)
{
    UnkeyedValidPathInfo a{"/nix/store", objectHash()};
    UnkeyedValidPathInfo b{"/nix/store", std::nullopt};
    std::string storeDirStr = "/nix/store";
    StoreDirConfig cfg{storeDirStr};
    UnkeyedValidPathInfo c{cfg, objectHash()};
    UnkeyedValidPathInfo d{cfg, std::nullopt};
    EXPECT_EQ(a.objectHash, objectHash());
    EXPECT_FALSE(b.objectHash.has_value());
    EXPECT_EQ(c.objectHash, objectHash());
    EXPECT_FALSE(d.objectHash.has_value());
    EXPECT_FALSE(a.assertedNarHash.has_value());
    EXPECT_EQ(a, c);
    EXPECT_EQ(b, d);
    EXPECT_NE(a, b);
    /* Equality sees the asserted hash too. */
    auto e = a;
    e.assertedNarHash = narHash();
    EXPECT_NE(a, e);
}

} // namespace nix
