#include "nix/util/object-hash.hh"
#include "nix/util/merkle-hash.hh"

namespace nix {

static constexpr std::string_view prefix = "git:sha256:";

ObjectHash ObjectHash::of(const merkle::TreeEntry & root)
{
    return ObjectHash{.hash = merkle::objectHash(root)};
}

std::string ObjectHash::render() const
{
    return std::string{prefix} + hash.to_string(HashFormat::Base16, false);
}

std::optional<ObjectHash> ObjectHash::parse(std::string_view s)
{
    if (!s.starts_with(prefix))
        return std::nullopt;
    auto rest = s.substr(prefix.size());
    /* Exactly the rendering: base16 of a SHA-256 digest, lower case, nothing else. */
    if (rest.size() != 2 * regularHashSize(HashAlgorithm::SHA256))
        return std::nullopt;
    for (char c : rest)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return std::nullopt;
    /* Cannot throw: the length and the alphabet, the two things base16
       parsing checks, were checked above. */
    return ObjectHash{.hash = Hash::parseNonSRIUnprefixed(rest, HashAlgorithm::SHA256)};
}

ObjectHash ObjectHash::parseOrThrow(std::string_view s)
{
    if (auto h = parse(s))
        return *h;
    throw BadHash("'%s' is not an object hash (expected 'git:sha256:<64 hex digits>')", s);
}

} // namespace nix
