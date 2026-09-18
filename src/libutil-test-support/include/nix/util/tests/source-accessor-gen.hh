#pragma once
///@file

#include <rapidcheck.h>

#include "nix/util/memory-source-accessor.hh"
#include "nix/util/merkle-hash.hh"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace nix {

/**
 * What `genFile` draws from: the names a directory's children may take
 * (each present or absent independently), and the generators of a regular
 * file's contents and of a symlink's target.
 */
struct FileGenSpec
{
    std::vector<std::string> names;
    rc::Gen<std::string> contents;
    rc::Gen<std::string> targets;
};

/**
 * A random in-memory file system object of at most `depth` directory
 * levels: at depth 0 a regular file (executable or not) or a symlink;
 * above it also -- twice as likely as either -- a directory whose children
 * are drawn recursively under `spec.names`.
 */
rc::Gen<MemorySourceAccessor::File> genFile(int depth, const FileGenSpec & spec);

/**
 * The NAR `dumpPath` serialises `path` of `accessor` to, under `filter`.
 */
std::string
narOf(SourceAccessor & accessor, const CanonPath & path = CanonPath::root, PathFilter & filter = defaultPathFilter);

/**
 * An in-memory tree of regular files: each pair is a path below the root
 * and the file's contents.
 */
ref<MemorySourceAccessor> memoryTree(std::initializer_list<std::pair<const char *, const char *>> files);

/**
 * The object hash's reference: a recursion over the hasher core alone --
 * blob ids from bytes, tree ids from entries inserted under the "/" key --
 * with `filter` applied as `dumpPath` applies it, to the unhacked name
 * under the path walked.  Independent of `ObjectHashSink`, of `objectHashOf`
 * and of `traverse`, so those are checked against it.
 */
merkle::TreeEntry
referenceObjectEntry(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter = defaultPathFilter);

/**
 * A leaf that names every subtree by the hash of its NAR, as a git leaf
 * names subtrees by object identifier: the leaf of the naming-law tests
 * (doc/lazy-store/01-specification.md, section 8), where a memory tree
 * stands in for the git tree.
 */
struct ContentNamedAccessor : SourceAccessor
{
private:
    void anchor() override {}

public:
    ref<SourceAccessor> inner;

    ContentNamedAccessor(ref<SourceAccessor> inner);

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;
    std::optional<Stat> maybeLstat(const CanonPath & path) override;
    DirEntries readDirectory(const CanonPath & path) override;
    std::string readLink(const CanonPath & path) override;
    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;
};

} // namespace nix
