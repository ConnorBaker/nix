/**
 * `.narinfo` under the object hash (doc/lazy-store/01-specification.md
 * section 9.11, "Binary caches"; 04 section 1.9): `ObjectHash:` is read
 * into `objectHash`, `NarHash:` into `assertedNarHash`, at least one of
 * the two is required, and `to_string` writes each that is present -- so
 * an old client reads a new cache and a new client reads an old one.
 *
 * The fixtures under data/nar-info/text are the three admissible files and
 * the one refused, written by hand.
 */
#include <gtest/gtest.h>

#include "nix/store/nar-info.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/object-hash.hh"

#include "nix/util/tests/test-data.hh"
#include "nix/store/tests/libstore.hh"
#include "object-hash-fixtures.hh"

namespace nix {

using namespace object_hash_fixtures;

namespace {

/* The same 32 bytes as `narHashSRI`, in the Nix32 rendering the file
   carries. */
const std::string narHashNix32 = "sha256:09ymwqf5i9q7d4dm7x4pjjcqqj0qrcp5lnznbh42gfsci5hcbqqm";

std::string fixture(std::string_view name)
{
    return readFile(getUnitTestData() / "nar-info" / "text" / (std::string{name} + ".narinfo"));
}

size_t countLines(const std::string & text, std::string_view field)
{
    size_t n = 0;
    size_t pos = 0;
    auto needle = std::string{field} + ": ";
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string_view line{text.data() + pos, eol - pos};
        if (line.starts_with(needle))
            n++;
        pos = eol + 1;
    }
    return n;
}

} // namespace

class ObjectHashNarInfo : public LibStoreTest
{
protected:
    /* `StoreDirConfig` holds a reference to the string: keep it alive. */
    std::string storeDirStr = "/nix/store";
    StoreDirConfig cfg{storeDirStr};

    void checkCommonFields(const NarInfo & info)
    {
        EXPECT_EQ(cfg.printStorePath(info.path), "/nix/store/n5wkd9frr45pa74if5gpz9j7mifg27fh-foo");
        EXPECT_EQ(info.url, "nar/1w1fff338fvdw53sqgamddn1b2xgds473pv6y13gizdbqjv4i5p3.nar.xz");
        EXPECT_EQ(info.compression, CompressionAlgo::xz);
        ASSERT_TRUE(info.fileHash.has_value());
        EXPECT_EQ(*info.fileHash, Hash::parseSRI(narHashSRI));
        EXPECT_EQ(info.fileSize, 4029176u);
        EXPECT_EQ(info.narSize, 34878u);
        EXPECT_EQ(
            info.references,
            (StorePathSet{
                StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar"},
                StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-foo"},
            }));
        ASSERT_TRUE(info.deriver.has_value());
        EXPECT_EQ(info.deriver->to_string(), "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv");
        EXPECT_EQ(info.sigs.size(), 1u);
        EXPECT_EQ(info.sigs.begin()->keyName, "asdf");
    }
};

/* A new cache's file: `ObjectHash:` alone. */
TEST_F(ObjectHashNarInfo, parses_ObjectHash_only)
{
    NarInfo info{cfg, fixture("object-hash-only"), "object-hash-only.narinfo"};
    checkCommonFields(info);
    ASSERT_TRUE(info.objectHash.has_value());
    EXPECT_EQ(info.objectHash->render(), objectHashRendered);
    EXPECT_FALSE(info.assertedNarHash.has_value());
}

/* What the upload writes: both. */
TEST_F(ObjectHashNarInfo, parses_both)
{
    NarInfo info{cfg, fixture("both"), "both.narinfo"};
    checkCommonFields(info);
    ASSERT_TRUE(info.objectHash.has_value());
    EXPECT_EQ(info.objectHash->render(), objectHashRendered);
    ASSERT_TRUE(info.assertedNarHash.has_value());
    EXPECT_EQ(*info.assertedNarHash, Hash::parseSRI(narHashSRI));
}

/* An old cache's file: `NarHash:` alone -- an assertion to verify on the
   stream, no object hash until the NAR arrives. */
TEST_F(ObjectHashNarInfo, parses_NarHash_only)
{
    NarInfo info{cfg, fixture("nar-hash-only"), "nar-hash-only.narinfo"};
    checkCommonFields(info);
    EXPECT_FALSE(info.objectHash.has_value());
    ASSERT_TRUE(info.assertedNarHash.has_value());
    EXPECT_EQ(*info.assertedNarHash, Hash::parseSRI(narHashSRI));
}

/* Neither is not a description of anything. */
TEST_F(ObjectHashNarInfo, rejects_neither)
{
    EXPECT_THROW((NarInfo{cfg, fixture("neither"), "neither.narinfo"}), Error);
}

/* An `ObjectHash:` value that is not a rendering is corrupt, not silently
   an old row. */
TEST_F(ObjectHashNarInfo, rejects_ObjectHash_that_is_not_a_rendering)
{
    auto text = fixture("object-hash-only");
    auto pos = text.find("ObjectHash: git:sha256:");
    ASSERT_NE(pos, std::string::npos);
    text.replace(pos, std::string_view{"ObjectHash: git:sha256:"}.size(), "ObjectHash: sha256:");
    EXPECT_THROW((NarInfo{cfg, text, "bad.narinfo"}), Error);
}

/* `to_string` of an info with both writes both lines, once each, and what
   it writes parses back to the same info. */
TEST_F(ObjectHashNarInfo, to_string_writes_both)
{
    NarInfo info{cfg, fixture("both"), "both.narinfo"};
    auto text = info.to_string(cfg);

    EXPECT_EQ(countLines(text, "ObjectHash"), 1u);
    EXPECT_EQ(countLines(text, "NarHash"), 1u);
    EXPECT_NE(text.find("ObjectHash: " + objectHashRendered + "\n"), std::string::npos) << text;
    EXPECT_NE(text.find("NarHash: " + narHashNix32 + "\n"), std::string::npos) << text;

    NarInfo back{cfg, text, "round-trip"};
    EXPECT_EQ(back, info);
}

/* `to_string` writes only what is present: no `NarHash:` line without an
   assertion, no `ObjectHash:` line without an object hash. */
TEST_F(ObjectHashNarInfo, to_string_writes_each_when_present)
{
    NarInfo onlyObject{cfg, fixture("object-hash-only"), "object-hash-only.narinfo"};
    auto textObject = onlyObject.to_string(cfg);
    EXPECT_EQ(countLines(textObject, "ObjectHash"), 1u);
    EXPECT_EQ(countLines(textObject, "NarHash"), 0u);
    EXPECT_EQ((NarInfo{cfg, textObject, "round-trip"}), onlyObject);

    NarInfo onlyNar{cfg, fixture("nar-hash-only"), "nar-hash-only.narinfo"};
    auto textNar = onlyNar.to_string(cfg);
    EXPECT_EQ(countLines(textNar, "ObjectHash"), 0u);
    EXPECT_EQ(countLines(textNar, "NarHash"), 1u);
    EXPECT_EQ((NarInfo{cfg, textNar, "round-trip"}), onlyNar);
}

} // namespace nix
