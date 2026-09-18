#include <cerrno>
#include <algorithm>
#include <charconv>
#include <regex>
#include <strings.h> // for strcasecmp

#include "nix/util/signals.hh"
#include "nix/util/hash.hh"

#include "nix/util/git.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/serialise.hh"
#include "nix/util/util.hh"

namespace nix::git {

static std::string getStringUntil(Source & source, char byte)
{
    std::string s;
    char n[1] = {0};
    source(std::string_view{n, 1});
    while (*n != byte) {
        s += *n;
        source(std::string_view{n, 1});
    }
    return s;
}

static std::string getString(Source & source, int n)
{
    std::string v;
    v.resize(n);
    source(v);
    return v;
}

uint64_t parseBlob(Source & source)
{
    auto sizeStr = getStringUntil(source, 0);
    auto size = string2Int<uint64_t>(sizeStr);
    if (!size)
        throw Error("invalid blob size '%s'", sizeStr);
    return *size;
}

void parseTree(merkle::DirectorySink & sink, Source & source, HashAlgorithm hashAlgo)
{
    auto sizeStr = getStringUntil(source, 0);
    auto leftOpt = string2Int<uint64_t>(sizeStr);
    if (!leftOpt)
        throw Error("invalid tree size '%s'", sizeStr);
    auto left = *leftOpt;

    while (left) {
        std::string perms = getStringUntil(source, ' ');
        left -= perms.size();
        left -= 1;

        /* Not `string2Int`, which is decimal-only; these are octal. */
        RawMode rawMode;
        auto [ptr, ec] = std::from_chars(perms.data(), perms.data() + perms.size(), rawMode, 8);
        if (ec != std::errc{})
            throw Error("invalid Git permission: %s", perms);
        auto modeOpt = decodeMode(rawMode);
        if (!modeOpt)
            throw Error("unknown Git permission: %o", rawMode);
        auto mode = std::move(*modeOpt);

        std::string name = getStringUntil(source, '\0');
        left -= name.size();
        left -= 1;

        const auto hashSize = regularHashSize(hashAlgo);
        std::string hashs = getString(source, hashSize);
        left -= hashSize;

        if (!(hashAlgo == HashAlgorithm::SHA1 || hashAlgo == HashAlgorithm::SHA256)) {
            throw Error("Unsupported hash algorithm for git trees: %s", printHashAlgo(hashAlgo));
        }

        Hash hash(hashAlgo);
        std::copy(hashs.begin(), hashs.end(), hash.hash);

        sink.insertChild(name, TreeEntry{.mode = mode, .hash = hash});
    }
}

ObjectType parseObjectType(Source & source)
{
    auto type = getString(source, 5);

    if (type == "blob ") {
        return ObjectType::Blob;
    } else if (type == "tree ") {
        return ObjectType::Tree;
    } else
        throw Error("input doesn't look like a Git object");
}

/* The readers above (`parseObjectType`, `parseBlob`, `parseTree`) only
   read; hashing lives in `objectHashOf` (`object-hash-sink.cc`) over
   `merkle-hash.hh`, under SHA-256. */

std::optional<LsRemoteRefLine> parseLsRemoteLine(std::string_view line)
{
    static const std::regex line_regex("^(ref: *)?([^\\s]+)(?:\\t+(.*))?$");
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_match(line.cbegin(), line.cend(), match, line_regex))
        return std::nullopt;

    return LsRemoteRefLine{
        .kind = match[1].length() == 0 ? LsRemoteRefLine::Kind::Object : LsRemoteRefLine::Kind::Symbolic,
        .target = match[2],
        .reference = match[3].length() == 0 ? std::nullopt : std::optional<std::string>{match[3]}};
}

} // namespace nix::git
