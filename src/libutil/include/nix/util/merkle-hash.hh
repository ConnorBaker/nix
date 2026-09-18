#pragma once
///@file
/**
 * The hasher core of the object hash (01 §9.11): git's object framing, a
 * tree's serialisation, the object hash from a root entry, and a NAR's
 * length by arithmetic.  Ungated; the one tree walker (`objectHashOf`) and
 * the fetchers' tree writer are over these, so what they write is what
 * these hash.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "nix/util/merkle-files.hh"
#include "nix/util/hash.hh"
#include "nix/util/serialise.hh"

namespace nix::merkle {

/**
 * A directory's entries, keyed by name with a trailing "/" for a directory.
 * That key is git's sort order: git orders entries as if a directory's
 * name ended in "/".  See https://github.com/mirage/irmin/issues/352.
 */
using Tree = std::map<std::string, TreeEntry>;

/**
 * The algorithm of the object hash, and the one algorithm the git
 * content-address method admits (01 section 9.9, "The algorithm";
 * section 10, *The git method is SHA-256 only*).  Every identifier
 * below is computed under it.
 */
constexpr HashAlgorithm hashAlgo = HashAlgorithm::SHA256;

/**
 * Writes git's object header, `<kind> <size>\0` (`kind` is `blob` or
 * `tree`), to the sink.
 */
void feedHeader(Sink & sink, std::string_view kind, uint64_t size);

/**
 * Incremental blob identifier: `BlobHasher h{size}; h(chunk)...; h.finish()`.
 * The size is git's, so it is needed before the first byte.  A `Sink`, so
 * that `SourceAccessor::readFile` can write into one.
 */
struct BlobHasher : Sink
{
    explicit BlobHasher(uint64_t size);

    void operator()(std::string_view chunk) override;

    Hash finish();

private:
    HashSink sink;
};

Hash blobId(std::string_view bytes);

/**
 * git's tree body: for each entry in the order of the map,
 * `<octal mode> <name>\0` followed by the raw hash bytes, directory keys
 * without their trailing "/".  Every name a NAR admits is written as it
 * is.  `feedHeader(sink, "tree", body.size())` gives the header that
 * precedes it.
 */
std::string serialiseTree(const Tree & tree);

/**
 * The hash of `tree <size>\0` followed by the body.
 */
Hash treeId(std::string_view body);

/**
 * Insert under the "/" convention: the key is the name, with "/" appended
 * when the entry is a directory.
 *
 * @throws Error if the name is empty, "." or "..", or contains '/' or NUL,
 * or if the tree already has an entry of that name — each is a bug in the
 * caller, since no directory yields such a name or a name twice.
 */
void insertEntry(Tree & tree, std::string_view name, const TreeEntry & entry);

/**
 * The object hash of a store object from its root entry (01 §9.11, which
 * shows it total and injective): the tree id of a directory, the blob id
 * of a plain file, and for an executable file or a symlink the id of the
 * one-entry tree `{"." -> entry}`.
 */
Hash objectHash(const TreeEntry & root);

/**
 * The body of the one-entry tree `{"." -> root}` when the root is an
 * executable file or a symlink — what an object store writes for such a
 * root so that its blob is reachable through a tree — and nullopt for a
 * directory or a plain file, whose object hash is their own id.
 */
std::optional<std::string> syntheticRootTree(const TreeEntry & root);

/**
 * The length of a NAR, from names and sizes alone: the serialisation of
 * `archive.cc`'s `dumpPath` without producing it.  Each function mirrors
 * one node as `dumpPath` writes it, every tag and string through
 * `writeString` — an 8-byte little-endian length, the bytes, and zero
 * padding to the next multiple of 8.
 */
namespace nar {

/**
 * One regular-file node: `( type regular [executable ""] contents <bytes> )`.
 * `dumpPath` omits the `executable` pair for a non-executable file.
 */
uint64_t regular(uint64_t contentsSize, bool executable);

/**
 * One symlink node: `( type symlink target <target> )`.
 */
uint64_t symlink(uint64_t targetLen);

/**
 * One directory node: `( type directory { entry ( name <name> node <node> ) } )`.
 * Call `add` once per entry the serialisation admits, then `finish`.
 */
struct Directory
{
    /**
     * The length so far of the entry records added, not their number.
     */
    uint64_t entries = 0;

    void add(std::string_view name, uint64_t nodeSize);

    uint64_t finish() const;
};

/**
 * The whole archive: the magic `nix-archive-1` and the root node.
 */
uint64_t root(uint64_t nodeSize);

} // namespace nar

} // namespace nix::merkle
