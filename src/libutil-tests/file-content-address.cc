#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "nix/util/file-content-address.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

/* ----------------------------------------------------------------------------
 * parseFileSerialisationMethod, renderFileSerialisationMethod
 * --------------------------------------------------------------------------*/

TEST(FileSerialisationMethod, testRoundTripPrintParse_1)
{
    for (const FileSerialisationMethod fim : {
             FileSerialisationMethod::Flat,
             FileSerialisationMethod::NixArchive,
         }) {
        EXPECT_EQ(parseFileSerialisationMethod(renderFileSerialisationMethod(fim)), fim);
    }
}

TEST(FileSerialisationMethod, testRoundTripPrintParse_2)
{
    for (const std::string_view fimS : {
             "flat",
             "nar",
         }) {
        EXPECT_EQ(renderFileSerialisationMethod(parseFileSerialisationMethod(fimS)), fimS);
    }
}

TEST(FileSerialisationMethod, testParseFileSerialisationMethodOptException)
{
    EXPECT_THAT(
        []() { parseFileSerialisationMethod("narwhal"); },
        ::testing::ThrowsMessage<UsageError>(::testing::HasSubstr("narwhal")));
}

/* ----------------------------------------------------------------------------
 * parseFileIngestionMethod, renderFileIngestionMethod
 * --------------------------------------------------------------------------*/

TEST(FileIngestionMethod, testRoundTripPrintParse_1)
{
    for (const FileIngestionMethod fim : {
             FileIngestionMethod::Flat,
             FileIngestionMethod::NixArchive,
             FileIngestionMethod::Git,
         }) {
        EXPECT_EQ(parseFileIngestionMethod(renderFileIngestionMethod(fim)), fim);
    }
}

TEST(FileIngestionMethod, testRoundTripPrintParse_2)
{
    for (const std::string_view fimS : {
             "flat",
             "nar",
             "git",
         }) {
        EXPECT_EQ(renderFileIngestionMethod(parseFileIngestionMethod(fimS)), fimS);
    }
}

TEST(FileIngestionMethod, testParseFileIngestionMethodOptException)
{
    EXPECT_THAT(
        []() { parseFileIngestionMethod("narwhal"); },
        ::testing::ThrowsMessage<UsageError>(::testing::HasSubstr("narwhal")));
}

/* ----------------------------------------------------------------------------
 * hashPath: the git method admits SHA-256 only
 * --------------------------------------------------------------------------*/

/* The object hash is SHA-256 and no other is computed (01 section 9.9,
   "The algorithm"; section 10, *The git method is SHA-256 only*), so
   `hashPath` under the git
   method refuses every other algorithm, naming the remedy; the other
   methods admit them as before. */
TEST(FileIngestionMethod, gitMethodHashesUnderSha256Only)
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->addFile(CanonPath{"/hello"}, "Hello World\n");
    SourcePath path{acc};

    EXPECT_NO_THROW(hashPath(path, FileIngestionMethod::Git, HashAlgorithm::SHA256));
    /* BLAKE3 is behind its own experimental feature, refused earlier. */
    for (auto algo : {HashAlgorithm::SHA1, HashAlgorithm::MD5, HashAlgorithm::SHA512})
        EXPECT_THAT(
            [&]() { hashPath(path, FileIngestionMethod::Git, algo); },
            ::testing::ThrowsMessage<Error>(::testing::HasSubstr("the git content-address method admits SHA-256 only")))
            << printHashAlgo(algo);

    EXPECT_NO_THROW(hashPath(path, FileIngestionMethod::NixArchive, HashAlgorithm::SHA1));
    EXPECT_NO_THROW(hashPath(path / "hello", FileIngestionMethod::Flat, HashAlgorithm::MD5));
}

} // namespace nix
