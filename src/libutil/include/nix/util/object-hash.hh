#pragma once
///@file

#include "nix/util/hash.hh"
#include "nix/util/merkle-files.hh"

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace nix {

/**
 * The object hash of a store object (01 §9.11): `merkle::objectHash` of its
 * root entry under SHA-256, as a strong type so a NAR hash cannot stand in
 * for it.  Rendered `git:sha256:<base16>`; `parse` accepts exactly that
 * and returns nullopt for anything else, an old `sha256:` row included.
 */
struct ObjectHash
{
    Hash hash;

    /**
     * `merkle::objectHash(root)`.
     */
    static ObjectHash of(const merkle::TreeEntry & root);

    std::string render() const;

    static std::optional<ObjectHash> parse(std::string_view s);

    /**
     * @throws BadHash unless `s` is exactly the rendered form.
     */
    static ObjectHash parseOrThrow(std::string_view s);

    bool operator==(const ObjectHash &) const = default;
    auto operator<=>(const ObjectHash &) const = default;
};

} // namespace nix
