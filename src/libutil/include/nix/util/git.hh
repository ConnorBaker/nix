#pragma once
///@file
/**
 * Readers of git's object encoding (`parseObjectType`, `parseBlob`,
 * `parseTree`) and the `git ls-remote` line parser.  No hashing or
 * writing lives here: the one tree walker is `objectHashOf`
 * (`object-hash-sink.hh`), under SHA-256, over the hasher core of
 * `merkle-hash.hh`.  The readers take the algorithm because git's format
 * has two, and a tree object is read under the one its repository uses.
 */

#include <string>
#include <string_view>
#include <optional>

#include "nix/util/types.hh"
#include "nix/util/serialise.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/merkle-files.hh"
#include "nix/util/merkle-hash.hh"

namespace nix::git {

enum struct ObjectType {
    Blob,
    Tree,
    // Commit,
    // Tag,
};

// Backwards compatibility aliases
using merkle::Mode;
using merkle::TreeEntry;
using nix::RawMode;

/**
 * A Git tree object, fully decoded and stored in memory.
 *
 * Directory names must end in a `/` for sake of sorting. See
 * https://github.com/mirage/irmin/issues/352
 */
using merkle::Tree;

inline std::optional<Mode> decodeMode(RawMode m)
{
    return merkle::decodeMode(m);
}

/**
 * Parse the "blob " or "tree " prefix.
 *
 * @throws if prefix not recognized
 */
ObjectType parseObjectType(Source & source);

/**
 * Read the size of the blob
 *
 * The caller should then call `Source::drainInto` or similar with that
 * size.
 */
uint64_t parseBlob(Source & source);

/**
 * @param hashAlgo the repository's: `HashAlgorithm::SHA1` or
 * `HashAlgorithm::SHA256`, the two git has.  The store's own trees are
 * read under `merkle::hashAlgo`.
 */
void parseTree(merkle::DirectorySink & sink, Source & source, HashAlgorithm hashAlgo);

/**
 * A line from the output of `git ls-remote --symref`.
 *
 * These can be of two kinds:
 *
 * - Symbolic references of the form
 *
 *   ```
 *   ref: {target} {reference}
 *   ```
 *   where {target} is itself a reference and {reference} is optional
 *
 * - Object references of the form
 *
 *   ```
 *   {target}  {reference}
 *   ```
 *   where {target} is a commit id and {reference} is mandatory
 */
struct LsRemoteRefLine
{
    enum struct Kind { Symbolic, Object };
    Kind kind;
    std::string target;
    std::optional<std::string> reference;
};

/**
 * Parse an `LsRemoteRefLine`
 */
std::optional<LsRemoteRefLine> parseLsRemoteLine(std::string_view line);

} // namespace nix::git
