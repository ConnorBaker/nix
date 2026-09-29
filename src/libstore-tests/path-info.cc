#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "nix/store/path-info.hh"

#include "nix/util/tests/characterization.hh"
#include "nix/store/tests/libstore.hh"

namespace nix {

using nlohmann::json;

TEST(PathInfoDeathTest, contentAddressConversionPreservesOwnedData)
{
    const StorePath path{"n5wkd9frr45pa74if5gpz9j7mifg27fh-foo"};
    const StorePath other{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar"};
    ValidPathInfo info{path, UnkeyedValidPathInfo{"/nix/store", std::nullopt}};
    info.references = {path, other};
    const auto withoutCa = info;
    EXPECT_FALSE(info.contentAddressWithReferences());
    EXPECT_EQ(info, withoutCa);

    using Method = ContentAddressMethod::Raw;
    for (auto [method, ingestion] : {
             std::pair{Method::Text, FileIngestionMethod::Flat},
             std::pair{Method::Flat, FileIngestionMethod::Flat},
             std::pair{Method::NixArchive, FileIngestionMethod::NixArchive},
             std::pair{Method::Git, FileIngestionMethod::Git},
         }) {
        for (auto algo : {HashAlgorithm::SHA256, HashAlgorithm::SHA1}) {
            if (algo == HashAlgorithm::SHA1 && method != Method::Git)
                continue;
            for (bool hasOther : {false, true}) {
                for (bool hasSelf : {false, true}) {
                    info.ca = ContentAddress{method, hashString(algo, "content")};
                    info.references = hasOther ? StorePathSet{other} : StorePathSet{};
                    if (hasSelf)
                        info.references.insert(path);
                    SCOPED_TRACE(info.ca->printMethodAlgo());
                    SCOPED_TRACE(::testing::Message() << "other=" << hasOther << " self=" << hasSelf);
                    const auto before = info;
#ifndef NDEBUG
                    if (method == Method::Text && hasSelf) {
                        EXPECT_DEATH_IF_SUPPORTED((void) info.contentAddressWithReferences(), "references.count");
                        EXPECT_EQ(info, before);
                        continue;
                    }
#endif
                    using Result = ContentAddressWithReferences::Raw;
                    Result expected = method == Method::Text
                                          ? Result{TextInfo{before.ca->hash, before.references}}
                                          : Result{FixedOutputInfo{
                                                ingestion,
                                                before.ca->hash,
                                                {hasOther ? StorePathSet{other} : StorePathSet{}, hasSelf},
                                            }};
                    auto actual = info.contentAddressWithReferences();
                    ASSERT_TRUE(actual);
                    EXPECT_EQ(info, before);
                    EXPECT_EQ(info.ca->hash.algo, algo);
                    info.references.clear();
                    info.ca.reset();
                    EXPECT_EQ(actual->raw, expected);
                    EXPECT_EQ(actual->getHash().algo, algo);
                }
            }
        }
    }
}

class PathInfoTestV1 : public CharacterizationTest, public LibStoreTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "path-info" / "json-1";

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / (testStem + ".json");
    }
};

class PathInfoTestV2 : public CharacterizationTest, public LibStoreTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "path-info" / "json-2";

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / (testStem + ".json");
    }
};

class PathInfoTestV3 : public CharacterizationTest, public LibStoreTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "path-info" / "json-3";

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / (testStem + ".json");
    }
};

/* The json-1..3 fixtures are the old forms: they carry a NAR hash and no
   object hash, which reads as `assertedNarHash` (04 section 1.9). */
static UnkeyedValidPathInfo makeEmpty()
{
    UnkeyedValidPathInfo info{"/nix/store", std::nullopt};
    info.assertedNarHash = Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc=");
    return info;
}

static ValidPathInfo makeFullKeyed(const Store & store, bool includeImpureInfo)
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
        std::nullopt);
    info.narSize = 34878;
    info.assertedNarHash = Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc=");
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

static UnkeyedValidPathInfo makeFull(const Store & store, bool includeImpureInfo)
{
    return makeFullKeyed(store, includeImpureInfo);
}

#define JSON_READ_TEST_V1(STEM, OBJ)                                                     \
    TEST_F(PathInfoTestV1, PathInfo_##STEM##_from_json)                                  \
    {                                                                                    \
        readTest(#STEM, [&](const auto & encoded_) {                                     \
            auto encoded = json::parse(encoded_);                                        \
            UnkeyedValidPathInfo got = UnkeyedValidPathInfo::fromJSON(&*store, encoded); \
            auto expected = OBJ;                                                         \
            ASSERT_EQ(got, expected);                                                    \
        });                                                                              \
    }

#define JSON_WRITE_TEST_V1(STEM, OBJ, PURE)                                                           \
    TEST_F(PathInfoTestV1, PathInfo_##STEM##_to_json)                                                 \
    {                                                                                                 \
        writeTest(                                                                                    \
            #STEM,                                                                                    \
            [&]() -> json { return OBJ.toJSON(&*store, PURE, PathInfoJsonFormat::V1); },              \
            [](const auto & file) { return json::parse(readFile(file)); },                            \
            [](const auto & file, const auto & got) { return writeFile(file, got.dump(2) + "\n"); }); \
    }

#define JSON_TEST_V1(STEM, OBJ, PURE) \
    JSON_READ_TEST_V1(STEM, OBJ)      \
    JSON_WRITE_TEST_V1(STEM, OBJ, PURE)

#define JSON_READ_TEST_V2(STEM, OBJ)                                                     \
    TEST_F(PathInfoTestV2, PathInfo_##STEM##_from_json)                                  \
    {                                                                                    \
        readTest(#STEM, [&](const auto & encoded_) {                                     \
            auto encoded = json::parse(encoded_);                                        \
            UnkeyedValidPathInfo got = UnkeyedValidPathInfo::fromJSON(nullptr, encoded); \
            auto expected = OBJ;                                                         \
            ASSERT_EQ(got, expected);                                                    \
        });                                                                              \
    }

#define JSON_WRITE_TEST_V2(STEM, OBJ, PURE)                                                           \
    TEST_F(PathInfoTestV2, PathInfo_##STEM##_to_json)                                                 \
    {                                                                                                 \
        writeTest(                                                                                    \
            #STEM,                                                                                    \
            [&]() -> json { return OBJ.toJSON(nullptr, PURE, PathInfoJsonFormat::V2); },              \
            [](const auto & file) { return json::parse(readFile(file)); },                            \
            [](const auto & file, const auto & got) { return writeFile(file, got.dump(2) + "\n"); }); \
    }

#define JSON_TEST_V2(STEM, OBJ, PURE) \
    JSON_READ_TEST_V2(STEM, OBJ)      \
    JSON_WRITE_TEST_V2(STEM, OBJ, PURE)

#define JSON_READ_TEST_V3(STEM, OBJ)                                                     \
    TEST_F(PathInfoTestV3, PathInfo_##STEM##_from_json)                                  \
    {                                                                                    \
        readTest(#STEM, [&](const auto & encoded_) {                                     \
            auto encoded = json::parse(encoded_);                                        \
            UnkeyedValidPathInfo got = UnkeyedValidPathInfo::fromJSON(nullptr, encoded); \
            auto expected = OBJ;                                                         \
            ASSERT_EQ(got, expected);                                                    \
        });                                                                              \
    }

#define JSON_WRITE_TEST_V3(STEM, OBJ, PURE)                                                           \
    TEST_F(PathInfoTestV3, PathInfo_##STEM##_to_json)                                                 \
    {                                                                                                 \
        writeTest(                                                                                    \
            #STEM,                                                                                    \
            [&]() -> json { return OBJ.toJSON(nullptr, PURE, PathInfoJsonFormat::V3); },              \
            [](const auto & file) { return json::parse(readFile(file)); },                            \
            [](const auto & file, const auto & got) { return writeFile(file, got.dump(2) + "\n"); }); \
    }

#define JSON_TEST_V3(STEM, OBJ, PURE) \
    JSON_READ_TEST_V3(STEM, OBJ)      \
    JSON_WRITE_TEST_V3(STEM, OBJ, PURE)

JSON_TEST_V1(empty_pure, makeEmpty(), false)
JSON_TEST_V1(empty_impure, makeEmpty(), true)
JSON_TEST_V1(pure, makeFull(*store, false), false)
JSON_TEST_V1(impure, makeFull(*store, true), true)

// Test that JSON without explicit version field parses as V1
JSON_READ_TEST_V1(pure_noversion, makeFull(*store, false))

JSON_TEST_V2(empty_pure, makeEmpty(), false)
JSON_TEST_V2(empty_impure, makeEmpty(), true)
JSON_TEST_V2(pure, makeFull(*store, false), false)
JSON_TEST_V2(impure, makeFull(*store, true), true)

JSON_TEST_V3(empty_pure, makeEmpty(), false)
JSON_TEST_V3(empty_impure, makeEmpty(), true)
JSON_TEST_V3(pure, makeFull(*store, false), false)
JSON_TEST_V3(impure, makeFull(*store, true), true)

#undef JSON_TEST_V1
#undef JSON_READ_TEST_V1
#undef JSON_WRITE_TEST_V1

#undef JSON_TEST_V2
#undef JSON_READ_TEST_V2
#undef JSON_WRITE_TEST_V2

#undef JSON_TEST_V3
#undef JSON_READ_TEST_V3
#undef JSON_WRITE_TEST_V3

TEST_F(PathInfoTestV2, PathInfo_full_shortRefs)
{
    ValidPathInfo it = makeFullKeyed(*store, true);
    // it.references = unkeyed.references;
    auto refs = it.shortRefs();
    ASSERT_EQ(refs.size(), 2u);
    ASSERT_EQ(*refs.begin(), "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar");
    ASSERT_EQ(*++refs.begin(), "n5wkd9frr45pa74if5gpz9j7mifg27fh-foo");
}

} // namespace nix
