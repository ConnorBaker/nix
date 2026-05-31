#include "nix/store/source-content-id.hh"
#include "nix/store/path.hh"

namespace nix {

using namespace std::string_view_literals;

SourceContentId SourceContentId::compute(
    std::string_view sourceFingerprint,
    const Hash & shapeHash,
    ContentAddressMethod method,
    const StoreReferences & refs)
{
    /* Canonical encoding. Order matters; field separators must not
       collide with field contents. We use NUL bytes — none of the
       field encodings legitimately contain NULs (fingerprints are
       printable, hashes are hex, methods are short identifiers,
       refs are store paths which are also printable). */
    std::string buf;
    buf += "src-content-id-v1\0"sv; // schema tag for future migration
    buf += sourceFingerprint;
    buf.push_back('\0');
    buf += shapeHash.to_string(HashFormat::Base16, false);
    buf.push_back('\0');
    buf += method.render();
    buf.push_back('\0');

    /* refs are sorted (StorePathSet is std::set) so iteration is
       deterministic. */
    if (refs.self)
        buf += "self";
    buf.push_back('\0');
    for (auto & r : refs.others) {
        buf += r.to_string();
        buf.push_back('\0');
    }

    return SourceContentId{hashString(HashAlgorithm::SHA256, buf)};
}

std::string SourceContentId::to_string() const
{
    return hash.to_string(HashFormat::Base16, false);
}

} // namespace nix
