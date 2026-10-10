#include <algorithm>
#include <charconv>
#include <iterator>
#include <stdexcept>

#include "nix/util/merkle-hash.hh"
#include "nix/util/archive.hh"
#include "nix/util/error.hh"
#include "nix/util/util.hh"

namespace nix::merkle {

void DirectorySink::anchor() {}

void feedHeader(Sink & sink, std::string_view kind, uint64_t size)
{
    assert(kind == "blob" || kind == "tree");
    /* Four kind bytes, a space, at most 20 uint64_t digits, and NUL. */
    char header[26];
    auto * end = header + sizeof(header);
    auto * begin = end;
    *--begin = '\0';
    do {
        *--begin = static_cast<char>('0' + size % 10);
        size /= 10;
    } while (size);
    *--begin = ' ';
    begin -= 4;
    std::copy_n(kind.begin(), 4, begin);
    sink({begin, static_cast<size_t>(end - begin)});
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
    std::string body;
    for (auto & [key, entry] : tree) {
        std::string_view name = key;
        if (entry.mode == Mode::Directory) {
            assert(!name.empty());
            assert(name.back() == '/');
            name.remove_suffix(1);
        }
        char mode[12];
        auto [end, ec] = std::to_chars(mode, mode + 11, static_cast<RawMode>(entry.mode), 8);
        assert(ec == std::errc{});
        *end++ = ' ';
        const auto hash = entry.hash.bytes();
        const auto modeSize = static_cast<size_t>(end - mode);
        const auto previousSize = body.size();
        const auto suffixSize = modeSize + 1 + hash.size();
        if (name.size() > body.max_size() - suffixSize || previousSize > body.max_size() - name.size() - suffixSize)
            throw std::length_error("tree body exceeds string capacity");
        body.resize_and_overwrite(previousSize + name.size() + suffixSize, [&](char * begin, size_t) {
            auto * out = std::char_traits<char>::copy(begin + previousSize, mode, modeSize) + modeSize;
            out = std::char_traits<char>::copy(out, name.data(), name.size()) + name.size();
            *out++ = '\0';
            out = std::char_traits<char>::copy(out, hash.data(), hash.size()) + hash.size();
            return static_cast<size_t>(out - begin);
        });
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
    const auto isDir = entry.mode == Mode::Directory;
    if (isDir) {
        if (tree.contains(name))
            throw Error("duplicate tree entry name '%s'", name);
        name += '/';
    } else if (tree.contains(name + '/'))
        throw Error("duplicate tree entry name '%s'", name);
    const auto size = tree.size();
    const auto i = tree.try_emplace(tree.end(), std::move(name), entry);
    if (tree.size() == size)
        throw Error("duplicate tree entry name '%s'", std::string_view(i->first).substr(0, i->first.size() - isDir));
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
        body.append(root.hash.bytes());
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

void Directory::add(uint64_t nameSize, uint64_t nodeSize) noexcept
{
    entries += tag("entry") + tag("(") + tag("name") + stringSize(nameSize) + tag("node") + nodeSize + tag(")");
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
