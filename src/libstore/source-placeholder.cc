#include "nix/store/source-placeholder.hh"

namespace nix {

SourcePlaceholder SourcePlaceholder::make(const SourceContentId & contentId, std::string_view name)
{
    /* Content-determined: contentId is content-only, name is the
       per-output disambiguator. Two calls with same inputs anywhere
       produce identical placeholders. */
    auto clearText = "nix-source-output:" + contentId.to_string() + ":" + std::string(name);
    return SourcePlaceholder{hashString(HashAlgorithm::SHA256, clearText)};
}

std::string SourcePlaceholder::render() const
{
    /* Same shape as DownstreamPlaceholder::render — opaque,
       /<52 base32> (SHA-256 in Nix32). */
    return "/" + hash.to_string(HashFormat::Nix32, false);
}

std::optional<SourcePlaceholder> SourcePlaceholder::tryParse(std::string_view s)
{
    /* SHA-256 in Nix32 base32 is 52 chars. Render is "/<52chars>". */
    if (s.size() < 2 || s.front() != '/')
        return std::nullopt;
    auto body = s.substr(1);
    /* Reject anything containing '-' — that's the marker for a real
       store path's name suffix. Also reject paths longer than the
       expected hash render length (catches `/nix/store/HASH-NAME`
       cases). */
    for (char c : body)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')))
            return std::nullopt;
    try {
        auto h = Hash::parseNonSRIUnprefixed(std::string(body), HashAlgorithm::SHA256);
        return SourcePlaceholder{std::move(h)};
    } catch (Error &) {
        return std::nullopt;
    }
}

} // namespace nix
