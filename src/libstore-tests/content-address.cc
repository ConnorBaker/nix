#include <gtest/gtest.h>

#include "nix/store/content-address.hh"
#include "nix/util/tests/json-characterization.hh"

namespace nix {

/* ----------------------------------------------------------------------------
 * ContentAddressMethod::parse, ContentAddressMethod::render
 * --------------------------------------------------------------------------*/

static auto methods = ::testing::Values(
    std::pair{ContentAddressMethod::Raw::Text, "text"},
    std::pair{ContentAddressMethod::Raw::Flat, "flat"},
    std::pair{ContentAddressMethod::Raw::NixArchive, "nar"},
    std::pair{ContentAddressMethod::Raw::Git, "git"});

struct ContentAddressMethodTest : ::testing::Test,
                                  ::testing::WithParamInterface<std::pair<ContentAddressMethod, std::string_view>>
{};

TEST_P(ContentAddressMethodTest, testRoundTripPrintParse_1)
{
    auto & [cam, _] = GetParam();
    EXPECT_EQ(ContentAddressMethod::parse(cam.render()), cam);
}

TEST_P(ContentAddressMethodTest, testRoundTripPrintParse_2)
{
    auto & [cam, camS] = GetParam();
    EXPECT_EQ(ContentAddressMethod::parse(camS).render(), camS);
}

INSTANTIATE_TEST_SUITE_P(ContentAddressMethod, ContentAddressMethodTest, methods);

TEST(ContentAddressMethod, testParseContentAddressMethodOptException)
{
    EXPECT_THROW(ContentAddressMethod::parse("narwhal"), UsageError);
}

/* The SHA-1 form of the git method is read as it is: a row, a narinfo, a
   derivation or a wire description an older Nix made under it parses, so
   that the collector and the verifier, which read every row they visit,
   run on a store holding one (01 section 10, *The git method is SHA-256
   only*).  Only creation
   refuses the form (`checkIngestionAlgorithm`). */
TEST(ContentAddress, sha1GitFormIsReadable)
{
    auto s = "fixed:git:sha1:" + hashString(HashAlgorithm::SHA1, "an old tree").to_string(HashFormat::Nix32, false);
    auto ca = ContentAddress::parse(s);
    EXPECT_EQ(ca.method, ContentAddressMethod::Raw::Git);
    EXPECT_EQ(ca.hash.algo, HashAlgorithm::SHA1);
    EXPECT_EQ(ca.render(), s);
    auto [method, algo] = ContentAddressMethod::parseWithAlgo("fixed:git:sha1");
    EXPECT_EQ(method, ContentAddressMethod::Raw::Git);
    EXPECT_EQ(algo, HashAlgorithm::SHA1);
}

/* ----------------------------------------------------------------------------
 * JSON
 * --------------------------------------------------------------------------*/

class ContentAddressTest : public virtual CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "content-address";

public:

    /**
     * We set these in tests rather than the regular globals so we don't have
     * to worry about race conditions if the tests run concurrently.
     */
    ExperimentalFeatureSettings mockXpSettings;

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / testStem;
    }
};

using nlohmann::json;

struct ContentAddressJsonTest : ContentAddressTest,
                                JsonCharacterizationTest<ContentAddress>,
                                ::testing::WithParamInterface<std::pair<std::string_view, ContentAddress>>
{};

TEST_P(ContentAddressJsonTest, from_json)
{
    auto & [name, expected] = GetParam();
    readJsonTest(name, expected);
}

TEST_P(ContentAddressJsonTest, to_json)
{
    auto & [name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(
    ContentAddressJSON,
    ContentAddressJsonTest,
    ::testing::Values(
        std::pair{
            "text",
            ContentAddress{
                .method = ContentAddressMethod::Raw::Text,
                .hash = hashString(HashAlgorithm::SHA256, "asdf"),
            },
        },
        std::pair{
            "nar",
            ContentAddress{
                .method = ContentAddressMethod::Raw::NixArchive,
                .hash = hashString(HashAlgorithm::SHA256, "qwer"),
            },
        }));

} // namespace nix
