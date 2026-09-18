#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <rapidcheck/gtest.h>
#include <gtest/gtest.h>

#include "nix/store/derivation/aterm.hh"
#include "nix/store/tests/derivation.hh"
#include "nix/store/tests/libstore.hh"

namespace nix {

class DerivationAtermTest : public LibStoreTest
{};

// FIXME: `RC_GTEST_FIXTURE_PROP` isn't calling `SetUpTestSuite` because it is
// not a real fixture; a plain test in the suite makes it run.
TEST_F(DerivationAtermTest, force_init) {}

/* Whatever the ATerm writer produces, the reader accepts and the writer
   reproduces.  The first run of this property, against the generator
   carried from an earlier branch, found byte 0xFF taken for EOF by the
   reader (doc/lazy-store/04-derivation.md, section 4, defects of master found by this work). */
RC_GTEST_FIXTURE_PROP(DerivationAtermTest, prop_unparse_parse_round_trip, (const Derivation & drv))
{
    auto text = derivation::unparse(drv, *store);
    try {
        auto back = derivation::parse(*store, std::string(text), drv.name);
        RC_ASSERT(derivation::unparse(back, *store) == text);
    } catch (Error & e) {
        RC_FAIL(e.msg() + "\nwhile parsing:\n" + text);
    }
}

/* The platform is the one verbatim field with no escaped form, so it is
   refused rather than written into a derivation no reader accepts; on master
   `system = "a\"b"` was written and then failed to parse.  A store path, the
   other verbatim field, has an escaped form under a store directory that
   needs it (the `WindowsStoreDir` tests in `derivation/external-formats.cc`
   cover that field's two modes); under the verbatim mode a store directory
   that would need it is refused as the platform is (the test below). */
TEST_F(DerivationAtermTest, platformOutsideTheFormatIsRefused)
{
    Derivation drv;
    drv.name = "q";
    drv.builder = "/bin/sh";
    drv.outputs.insert_or_assign(
        "out",
        derivation::Output{
            derivation::Output::InputAddressed{.path = StorePath("00000000000000000000000000000000-q")}});
    for (auto platform : {std::string("a\"b"), std::string("a\\b")}) {
        drv.platform = platform;
        EXPECT_THROW(derivation::unparse(drv, *store), FormatError);
    }
    drv.platform = "x86_64-linux";
    EXPECT_NO_THROW(derivation::unparse(drv, *store));
}

/* The store directory under the verbatim mode: a double quote or a
   backslash in it would be written verbatim and could not be read back --
   master wrote such a derivation off Windows (`defaultSupportWindowsStoreDir`
   is the platform, not the directory's characters) -- so the writer refuses
   it, as it refuses the platform; under the escaped mode, the mode of a
   Windows store directory, the same directory round-trips. */
TEST_F(DerivationAtermTest, storeDirOutsideTheVerbatimFormIsRefused)
{
    for (auto storeDir : {std::string("/nix/st\"ore"), std::string("/nix/st\\ore")}) {
        StoreDirConfig quoted{storeDir};
        Derivation drv;
        drv.name = "q";
        drv.platform = "x86_64-linux";
        drv.builder = "/bin/sh";
        drv.outputs.insert_or_assign(
            "out",
            derivation::Output{
                derivation::Output::InputAddressed{.path = StorePath("00000000000000000000000000000000-q")}});
        EXPECT_THROW(derivation::unparse(drv, quoted, /*supportWindowsStoreDir=*/false), FormatError) << storeDir;
        auto text = derivation::unparse(drv, quoted, /*supportWindowsStoreDir=*/true);
        auto back = derivation::parse(quoted, std::string(text), drv.name, /*supportWindowsStoreDir=*/true);
        EXPECT_EQ(derivation::unparse(back, quoted, /*supportWindowsStoreDir=*/true), text) << storeDir;
    }
}

/* The byte the property found, pinned by name. */
TEST_F(DerivationAtermTest, aByteEqualToEofRoundTrips)
{
    Derivation drv;
    drv.name = "ff";
    drv.platform = "x86_64-linux";
    drv.builder = "/bin/sh";
    drv.env = {{"e", std::string("ab\xff") + "cd"}};
    drv.outputs.insert_or_assign(
        "out",
        derivation::Output{
            derivation::Output::InputAddressed{.path = StorePath("00000000000000000000000000000000-ff")}});
    auto text = derivation::unparse(drv, *store);
    auto back = derivation::parse(*store, std::string(text), drv.name);
    EXPECT_EQ(back.env.at("e"), drv.env.at("e"));
}

/* A fixed output under the SHA-1 form of the git method, which an older Nix
   could write, still reads: the reader computes the output's path to check
   the one written, and the collector reads every derivation it visits under
   `ca-derivations` (store-api.cc, `queryPartialDerivationOutputMap`), so the
   name function stays total over the form and only a build refuses it (01
   section 10, *The git method is SHA-256 only*). */
TEST_F(DerivationAtermTest, sha1GitFixedOutputReads)
{
    Derivation drv;
    drv.name = "old";
    drv.platform = "x86_64-linux";
    drv.builder = "/bin/sh";
    drv.outputs.insert_or_assign(
        "out",
        derivation::Output{derivation::Output::CAFixed{
            .ca =
                ContentAddress{
                    .method = ContentAddressMethod::Raw::Git,
                    .hash = hashString(HashAlgorithm::SHA1, "tree 0"),
                },
        }});
    auto text = derivation::unparse(drv, *store);
    EXPECT_NE(text.find("git:sha1"), std::string::npos);
    auto back = derivation::parse(*store, std::string(text), drv.name);
    EXPECT_EQ(derivation::unparse(back, *store), text);
}

} // namespace nix
