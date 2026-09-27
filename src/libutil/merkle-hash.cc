#include <algorithm>
#include <iterator>

#include "nix/util/merkle-hash.hh"
#include "nix/util/archive.hh"
#include "nix/util/error.hh"
#include "nix/util/util.hh"

namespace nix::merkle {

void DirectorySink::anchor() {}

/* Append a non-negative integer in base `base` without generic formatting. */
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

ObjectHasher::ObjectHasher(std::string_view kind, uint64_t size)
{
    static_assert(hashAlgo == HashAlgorithm::SHA256);
    SHA256_Init(&context);
    feedHeader(*this, kind, size);
}

ObjectHasher::~ObjectHasher() = default;

void ObjectHasher::operator()(std::string_view chunk)
{
    SHA256_Update(&context, chunk.data(), chunk.size());
}

Hash ObjectHasher::finish()
{
    Hash hash(hashAlgo);
    SHA256_Final(hash.hash, &context);
    return hash;
}

static Hash hashObject(std::string_view kind, std::string_view body)
{
    ObjectHasher hasher{kind, body.size()};
    hasher(body);
    return hasher.finish();
}

Hash blobId(std::string_view bytes)
{
    return hashObject("blob", bytes);
}

std::string serialiseTree(const Tree & tree)
{
    using namespace std::string_view_literals;
    std::string body;

    for (auto & [key, entry] : tree) {
        std::string_view name = key;
        switch (entry.mode) {
        case Mode::Directory:
            assert(!name.empty());
            assert(name.back() == '/');
            name.remove_suffix(1);
            body += "40000 "sv;
            break;
        case Mode::Regular:
            body += "100644 "sv;
            break;
        case Mode::Executable:
            body += "100755 "sv;
            break;
        case Mode::Symlink:
            body += "120000 "sv;
            break;
        default:
            appendDigits<8>(body, static_cast<RawMode>(entry.mode));
            body += ' ';
        }
        body += name;
        body += '\0';
        body.append(reinterpret_cast<const char *>(entry.hash.hash), entry.hash.hashSize);
    }

    return body;
}

Hash treeId(std::string_view body)
{
    return hashObject("tree", body);
}

void insertEntry(Tree & tree, std::string name, const TreeEntry & entry)
{
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos
        || name.find('\0') != std::string_view::npos || name.find(caseHackSuffix) != std::string_view::npos)
        throw Error("invalid tree entry name '%s'", name);
    /* The trailing slash orders directories; it does not make a second
       child name. Reject either encoding of an existing logical name. */
    if (entry.mode == Mode::Directory) {
        if (tree.contains(name))
            throw Error("duplicate tree entry name '%s'", name);
        name += '/';
    } else if (tree.contains(name + '/'))
        throw Error("duplicate tree entry name '%s'", name);
    if (!tree.try_emplace(std::move(name), entry).second) {
        if (entry.mode == Mode::Directory)
            name.pop_back();
        throw Error("duplicate tree entry name '%s'", name);
    }
}

std::optional<std::string> syntheticRootTree(const TreeEntry & root)
{
    switch (root.mode) {
    case Mode::Directory:
    case Mode::Regular:
        return std::nullopt;
    case Mode::Executable:
    case Mode::Symlink: {
        /* The one-entry tree {"." -> root}: six octal mode bytes, a space,
           the name ".", and NUL, followed by the active hash bytes. */
        std::string body(root.mode == Mode::Executable ? "100755 .\0" : "120000 .\0", 9);
        body.append(reinterpret_cast<const char *>(root.hash.hash), root.hash.hashSize);
        return body;
    }
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
