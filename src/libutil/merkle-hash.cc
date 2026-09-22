#include <algorithm>
#include <iterator>

#include "nix/util/merkle-hash.hh"
#include "nix/util/archive.hh"
#include "nix/util/error.hh"
#include "nix/util/util.hh"

namespace nix::merkle {

void DirectorySink::anchor() {}

/* A non-negative integer in base `base`, appended.  `nix::fmt` is
   `boost::format`, which costs some thousands of instructions a call; the
   header of every blob and the mode of every tree entry pass through here
   once per node per walk, which made it a fifth of the object store's own
   instructions on a first entry. */
template<unsigned base>
static void appendDigits(std::string & out, uint64_t value)
{
    char buf[64];
    size_t i = sizeof(buf);
    do {
        buf[--i] = char('0' + value % base);
        value /= base;
    } while (value);
    out.append(buf + i, sizeof(buf) - i);
}

void feedHeader(Sink & sink, std::string_view kind, uint64_t size)
{
    std::string header;
    header += kind;
    header += ' ';
    appendDigits<10>(header, size);
    header += '\0';
    sink(header);
}

BlobHasher::BlobHasher(uint64_t size)
    : sink(hashAlgo)
{
    /* Header and body use the same unbuffered stream, leaving HashSink's
       buffer empty for the whole blob. */
    feedHeader(*this, "blob", size);
}

void BlobHasher::operator()(std::string_view chunk)
{
    sink.writeUnbuffered(chunk);
}

Hash BlobHasher::finish()
{
    return sink.finish().hash;
}

Hash blobId(std::string_view bytes)
{
    BlobHasher h{bytes.size()};
    h(bytes);
    return h.finish();
}

std::string serialiseTree(const Tree & tree)
{
    std::string body;

    for (auto & [key, entry] : tree) {
        std::string_view name = key;
        if (entry.mode == Mode::Directory) {
            assert(!name.empty());
            assert(name.back() == '/');
            name.remove_suffix(1);
        }
        appendDigits<8>(body, static_cast<RawMode>(entry.mode));
        body += ' ';
        body += name;
        body += '\0';
        body.append(reinterpret_cast<const char *>(entry.hash.hash), entry.hash.hashSize);
    }

    return body;
}

Hash treeId(std::string_view body)
{
    HashSink sink(hashAlgo);
    LambdaSink direct{[&](std::string_view chunk) { sink.writeUnbuffered(chunk); }};
    feedHeader(direct, "tree", body.size());
    direct(body);
    return sink.finish().hash;
}

void insertEntry(Tree & tree, std::string_view name, const TreeEntry & entry)
{
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos
        || name.find('\0') != std::string_view::npos)
        throw Error("invalid tree entry name '%s'", name);
    std::string key(name);
    if (entry.mode == Mode::Directory)
        key += '/';
    if (!tree.emplace(std::move(key), entry).second)
        throw Error("duplicate tree entry name '%s'", name);
}

/* The one-entry tree an executable or a symlink root is addressed by.  Its
   key is "." itself: neither mode is a directory, so no "/" follows. */
static Tree syntheticTree(const TreeEntry & root)
{
    return Tree{{".", root}};
}

std::optional<std::string> syntheticRootTree(const TreeEntry & root)
{
    switch (root.mode) {
    case Mode::Directory:
    case Mode::Regular:
        return std::nullopt;
    case Mode::Executable:
    case Mode::Symlink:
        return serialiseTree(syntheticTree(root));
    }
    unreachable();
}

Hash objectHash(const TreeEntry & root)
{
    auto body = syntheticRootTree(root);
    return body ? treeId(*body) : root.hash;
}

namespace nar {

/* `writeString`'s length for a string of `len` bytes: an 8-byte length,
   the bytes, and zero padding to the next multiple of 8. */
static constexpr uint64_t stringSize(uint64_t len)
{
    return 8 + len + (len % 8 ? 8 - len % 8 : 0);
}

/* The same, for a tag `dumpPath` writes literally. */
static constexpr uint64_t tag(std::string_view s)
{
    return stringSize(s.size());
}

uint64_t regular(uint64_t contentsSize, bool executable)
{
    return tag("(") + tag("type") + tag("regular") + (executable ? tag("executable") + tag("") : 0) + tag("contents")
           + stringSize(contentsSize) + tag(")");
}

uint64_t symlink(uint64_t targetLen)
{
    return tag("(") + tag("type") + tag("symlink") + tag("target") + stringSize(targetLen) + tag(")");
}

void Directory::add(std::string_view name, uint64_t nodeSize)
{
    entries += tag("entry") + tag("(") + tag("name") + stringSize(name.size()) + tag("node") + nodeSize + tag(")");
}

uint64_t Directory::finish() const
{
    return tag("(") + tag("type") + tag("directory") + entries + tag(")");
}

uint64_t root(uint64_t nodeSize)
{
    return tag(narVersionMagic1) + nodeSize;
}

} // namespace nar

} // namespace nix::merkle
