#pragma once
///@file
/**
 * A `LocalStore` in a temporary directory: the fixture the local-store
 * unit tests share (local-store.cc, object-hash-local-store.cc), and the
 * planting of a schema-10 row for the tests of its migration
 * (doc/lazy-store/01-specification.md, section 9.11, "The database").
 */
#include <gtest/gtest.h>

#include "nix/store/local-store.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"

#include <memory>
#include <string>

namespace nix {

/**
 * A fresh `LocalStore` under a temporary directory for each test, torn
 * down store first (the connection closes and checkpoints), directory
 * after.  Derived fixtures add their own `SetUp`/`TearDown` around a
 * call to these.
 */
struct TempLocalStoreTest : ::testing::Test
{
    AutoDelete tempStoreDir;
    std::shared_ptr<LocalStoreConfig> config;
    std::shared_ptr<LocalStore> store;

    void SetUp() override;
    void TearDown() override;
};

/**
 * Rewrite the row of `path` to the form a schema-10 Nix wrote:
 * `ValidPaths.hash` becomes `<algo>:<base16>` of `narHash` (master's
 * `local-store.cc` stored `narHash.to_string(HashFormat::Base16, true)`),
 * and `ValidPaths.narSize` becomes `narSize`, NULL when 0 as that code
 * left it.  Goes through a second connection to the store's SQLite, then
 * drops the store's in-memory path-info cache so the next query reads the
 * row.  Throws if the row does not exist.
 */
void plantSchema10Row(LocalStore & store, const StorePath & path, const Hash & narHash, uint64_t narSize);

/**
 * Rewrite `ValidPaths.ca` of `path` to `ca` as it is, so that a test can
 * plant a content address no Nix it can drive writes (the SHA-1 form of
 * the git method).  Same connection and cache rule as `plantSchema10Row`.
 * Throws if the row does not exist.
 */
void plantCaColumn(LocalStore & store, const StorePath & path, std::string_view ca);

/**
 * `ValidPaths.hash` of `path` as the database holds it, read through a
 * second connection.  Throws if the row does not exist.
 */
std::string readHashColumn(LocalStore & store, const StorePath & path);

} // namespace nix
