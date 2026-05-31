#include "nix/util/fingerprint.hh"
#include "nix/util/error.hh"

#include <cassert>

namespace nix {

/* Suffixes are `;<tag>` or `;<tag>=<value>`.  Splitting on `;` first,
   then on the first `=` within each suffix, separates tag from value.
   Two stacks of wrappers that produce the same set of suffixes must
   produce the same string — alphabetic order on tag does it.

   The root tag (everything up to the first `;`) is preserved verbatim;
   only the suffix run is canonicalised. */

static std::string_view tagOf(std::string_view suffix)
{
    /* `suffix` is e.g. `;a=H` or `;e`; tag is the chars after the `;`
       up to `=` or end. */
    assert(!suffix.empty() && suffix.front() == ';');
    auto rest = suffix.substr(1);
    auto eq = rest.find('=');
    return eq == std::string_view::npos ? rest : rest.substr(0, eq);
}

std::string mergeFingerprintSuffix(std::string_view fingerprint, std::string_view suffix)
{
    if (suffix.empty())
        return std::string(fingerprint);
    if (suffix.front() != ';')
        throw Error("fingerprint suffix '%s' must begin with ';'", suffix);

    /* Split fingerprint into root + sorted suffix list. */
    auto rootEnd = fingerprint.find(';');
    std::string_view root = fingerprint.substr(0, rootEnd);

    std::vector<std::string_view> suffixes;
    if (rootEnd != std::string_view::npos) {
        std::string_view rest = fingerprint.substr(rootEnd);
        while (!rest.empty()) {
            assert(rest.front() == ';');
            auto next = rest.find(';', 1);
            auto piece = rest.substr(0, next);
            suffixes.push_back(piece);
            rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next);
        }
    }

    auto newTag = tagOf(suffix);

    /* Refuse to merge two suffixes with the same tag. */
    for (auto & existing : suffixes)
        if (tagOf(existing) == newTag)
            throw Error("fingerprint suffix '%s' clashes with existing '%s' (same tag '%s')", suffix, existing, newTag);

    /* Insert in alphabetical order by tag. */
    auto pos = suffixes.begin();
    while (pos != suffixes.end() && tagOf(*pos) < newTag)
        ++pos;
    suffixes.insert(pos, suffix);

    std::string result(root);
    for (auto & s : suffixes)
        result += s;
    return result;
}

std::string blobFingerprint(std::string_view oidHex, std::string_view modeOctal)
{
    std::string r = "blob:";
    r += oidHex;
    r += ";m=";
    r += modeOctal;
    return r;
}

std::string treeFingerprint(std::string_view oidHex)
{
    std::string r = "tree:";
    r += oidHex;
    return r;
}

std::optional<Hash> bareTreeOid(std::string_view fingerprint)
{
    constexpr std::string_view prefix = "tree:";
    if (!fingerprint.starts_with(prefix))
        return std::nullopt;
    auto after = fingerprint.substr(prefix.size());
    /* Any suffix means the NAR differs from a vanilla tree dump — not
       bridge-eligible. (This is also why we check before parsing.) */
    if (after.find(';') != std::string_view::npos)
        return std::nullopt;
    try {
        return Hash::parseAny(std::string(after), HashAlgorithm::SHA1);
    } catch (const Error &) {
        /* Malformed OID hex — treat as non-bare rather than throwing, so
           every caller observes the same "not bridge-eligible" answer. */
        return std::nullopt;
    }
}

} // namespace nix
