#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/flake/lockfile.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/util/error.hh"
#include "nix/util/fmt.hh"
#include "nix/util/hash.hh"

namespace nix {

/* The lock file at version 8 (01 section 9.11; 04 section 1.9, the fetchers'
   and the language's representation): a locked input carries `treeHash`, the hash the store
   names its tree by; `narHash` is an older assertion, read from a version-7
   lock (or from a version-8 node not yet refetched) and verified when the
   input is fetched; versions 5 to 8 are read, 8 is written, 9 is refused. */

namespace {

/* A lock with one git input, locked by its revision, carrying `hashAttr`. */
std::string lockWith(int version, std::string_view hashAttr, std::string_view hashValue)
{
    return fmt(
        R"({
  "nodes": {
    "dependency": {
      "locked": {
        "lastModified": 1746721011,
        "%s": "%s",
        "ref": "refs/heads/master",
        "rev": "432058dbfc82b0369bc9cce440e4af2aece52b54",
        "revCount": 1,
        "type": "git",
        "url": "file:///no-such-path"
      },
      "original": {
        "type": "git",
        "url": "file:///no-such-path"
      }
    },
    "root": {
      "inputs": {
        "dependency": "dependency"
      }
    }
  },
  "root": "root",
  "version": %d
})",
        hashAttr,
        hashValue,
        version);
}

/* The SHA-256 git id of the empty tree, as SRI. */
const std::string emptyTreeHash = "sha256-bvGbQSJcU2nxwQTUXY2F76mwV7U7FLS5uTnddN7MUyE=";
/* Some NAR hash, as SRI. */
const std::string someNarHash = "sha256-9aIDvIdyHAfQyvT5SwPgYxUUhf1GwQVAWq+qa5LcEQE=";

std::shared_ptr<flake::LockedNode> theDependency(flake::LockFile & lock)
{
    auto node = lock.findInput({"dependency"});
    return node ? std::dynamic_pointer_cast<flake::LockedNode>(node) : nullptr;
}

} // namespace

TEST(LockFile, version8RoundTripsWithTreeHash)
{
    EnableExperimentalFeature enableFlakes("flakes");
    fetchers::Settings fetchSettings;

    flake::LockFile lock(fetchSettings, lockWith(8, "treeHash", emptyTreeHash), "<v8>");
    auto dep = theDependency(lock);
    ASSERT_TRUE(dep);
    EXPECT_EQ(dep->lockedRef.input.getTreeHash(), std::optional<Hash>(Hash::parseSRI(emptyTreeHash)));
    EXPECT_FALSE(dep->lockedRef.input.getNarHash());

    auto [json, keys] = lock.toJSON();
    EXPECT_EQ(json["version"].get<int>(), 8);
    EXPECT_EQ(json["nodes"]["dependency"]["locked"]["treeHash"].get<std::string>(), emptyTreeHash);
    EXPECT_FALSE(json["nodes"]["dependency"]["locked"].contains("narHash"));

    /* Written and read again, the same lock. */
    flake::LockFile again(fetchSettings, lock.to_string().first, "<v8-again>");
    EXPECT_EQ(again, lock);
    EXPECT_EQ(theDependency(again)->lockedRef.input.getTreeHash(), std::optional<Hash>(Hash::parseSRI(emptyTreeHash)));
}

TEST(LockFile, version7IsReadWithItsNarHashAsAnAssertion)
{
    EnableExperimentalFeature enableFlakes("flakes");
    fetchers::Settings fetchSettings;

    flake::LockFile lock(fetchSettings, lockWith(7, "narHash", someNarHash), "<v7>");
    auto dep = theDependency(lock);
    ASSERT_TRUE(dep);
    /* The node's input has the assertion and no name yet. */
    EXPECT_EQ(dep->lockedRef.input.getNarHash(), std::optional<Hash>(Hash::parseSRI(someNarHash)));
    EXPECT_FALSE(dep->lockedRef.input.getTreeHash());

    /* Written, a lock is version 8 whatever it was read as; a node that was
       not fetched keeps its assertion until a fetch replaces it. */
    auto [json, keys] = lock.toJSON();
    EXPECT_EQ(json["version"].get<int>(), 8);
    EXPECT_EQ(json["nodes"]["dependency"]["locked"]["narHash"].get<std::string>(), someNarHash);
}

TEST(LockFile, version8AcceptsANodeWithOnlyANarHash)
{
    EnableExperimentalFeature enableFlakes("flakes");
    fetchers::Settings fetchSettings;

    /* Every reader accepts either attribute: a version-8 lock whose node
       was never refetched still carries the old assertion. */
    flake::LockFile lock(fetchSettings, lockWith(8, "narHash", someNarHash), "<v8-nar>");
    auto dep = theDependency(lock);
    ASSERT_TRUE(dep);
    EXPECT_EQ(dep->lockedRef.input.getNarHash(), std::optional<Hash>(Hash::parseSRI(someNarHash)));
    EXPECT_FALSE(dep->lockedRef.input.getTreeHash());
}

TEST(LockFile, version9IsRefused)
{
    EnableExperimentalFeature enableFlakes("flakes");
    fetchers::Settings fetchSettings;

    EXPECT_THROW(flake::LockFile(fetchSettings, lockWith(9, "treeHash", emptyTreeHash), "<v9>"), Error);
}

} // namespace nix
