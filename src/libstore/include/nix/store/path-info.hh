#pragma once
///@file

#include "nix/util/signature/signer.hh"
#include "nix/store/path.hh"
#include "nix/util/hash.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/fun.hh"
#include "nix/store/content-address.hh"

#include <functional>
#include <string>
#include <optional>

namespace nix {

class Store;
struct StoreDirConfig;

/**
 * JSON format version for path info output.
 */
enum class PathInfoJsonFormat {
    /// Legacy format with string hashes and full store paths
    V1 = 1,
    /// New format with structured hashes and store path base names
    V2 = 2,
    /// New format with structured signatures
    V3 = 3,
    /// Format carrying the object hash (`objectHash`, required) with
    /// the NAR hash (`narHash`) optional
    V4 = 4,
};

/**
 * Convert an integer version number to PathInfoJsonFormat.
 * Throws Error if the version is not supported.
 */
PathInfoJsonFormat parsePathInfoJsonFormat(uint64_t version);

struct SubstitutablePathInfo
{
    std::optional<StorePath> deriver;
    StorePathSet references;
    /**
     * 0 = unknown or inapplicable
     */
    uint64_t downloadSize;
    /**
     * 0 = unknown
     */
    uint64_t narSize;
};

using SubstitutablePathInfos = std::map<StorePath, SubstitutablePathInfo>;

/**
 * Information about a store object.
 *
 * See `store/store-object` and `protocols/json/store-object-info` in
 * the Nix manual
 */
struct UnkeyedValidPathInfo
{
    /**
     * The store directory this store object belongs to.
     *
     * This supports relocatable store objects where different objects
     * may have different store directories.
     */
    std::string storeDir;

    /**
     * Path to derivation that produced this store object, if known.
     */
    std::optional<StorePath> deriver;

    /**
     * The store object's content hash (01 §9.11).  Present for every info
     * the local store returns or registers; absent for a substituter's or
     * an old peer's description, which carries `assertedNarHash` alone.
     */
    std::optional<ObjectHash> objectHash;

    /**
     * A NAR hash a sender asserted -- an old peer on the wire, a
     * `.narinfo`'s `NarHash:`, `--load-db`, path-info JSON before
     * version 4.  Verified on the receiving tee, never stored.
     */
    std::optional<Hash> assertedNarHash;

    /**
     * Other store objects this store object refers to.
     */
    StorePathSet references;

    /**
     * When this store object was registered in the store that contains
     * it, if known.
     */
    time_t registrationTime = 0;

    /**
     * 0 = unknown
     */
    uint64_t narSize = 0;

    /**
     * Whether the path is ultimately trusted, that is, it's a
     * derivation output that was built locally.
     */
    bool ultimate = false;

    std::set<Signature> sigs;

    /**
     * If non-empty, an assertion that the path is content-addressed,
     * i.e., that the store path is computed from a cryptographic hash
     * of the contents of the path, plus some other bits of data like
     * the "name" part of the path. Such a path doesn't need
     * signatures, since we don't have to trust anybody's claim that
     * the path is the output of a particular derivation. (In the
     * extensional store model, we have to trust that the *contents*
     * of an output path of a derivation were actually produced by
     * that derivation. In the intensional model, we have to trust
     * that a particular output path was produced by a derivation; the
     * path then implies the contents.)
     *
     * Ideally, the content-addressability assertion would just be a Boolean,
     * and the store path would be computed from the name component, 'narHash'
     * and 'references'. However, we support many types of content addresses.
     */
    std::optional<ContentAddress> ca;

    /**
     * A thunk producing the NAR hash of this object: for the JSON formats
     * before 4, whose `narHash` is required (`toJSON`), and for
     * `lazyNarHash` below.
     */
    using NarHashThunk = fun<Hash()>;

    /**
     * The NAR hash an old form may need, computed only when one asks
     * (01 §9.11: the cost falls on the old peer).  Not part of the
     * description -- neither compared nor serialised -- and forced by
     * `CommonProto::writePathInfoHashes` alone, in the old form when
     * `assertedNarHash` is absent; the new form's slot stays empty.
     */
    std::optional<NarHashThunk> lazyNarHash;

    UnkeyedValidPathInfo(const UnkeyedValidPathInfo & other) = default;

    UnkeyedValidPathInfo(const StoreDirConfig & store, std::optional<ObjectHash> objectHash);

    UnkeyedValidPathInfo(std::string storeDir, std::optional<ObjectHash> objectHash)
        : storeDir(std::move(storeDir))
        , objectHash(std::move(objectHash))
    {
    }

    bool operator==(const UnkeyedValidPathInfo &) const noexcept;

    /**
     * @todo return `std::strong_ordering` once `id` is removed
     */
    std::weak_ordering operator<=>(const UnkeyedValidPathInfo &) const noexcept;

    virtual ~UnkeyedValidPathInfo() {}

    /**
     * @param store If non-null, store paths are rendered as full paths.
     *              If null, store paths are rendered as base names.
     * @param includeImpureInfo If true, variable elements such as the
     *                          registration time are included.
     * @param format JSON format version. Version 1 uses string hashes and
     *               string content addresses. Version 2 uses structured
     *               hashes and structured content addresses. Version 4
     *               carries `objectHash` (required) and `narHash` only
     *               when `assertedNarHash` is present.
     * @param narHashFor For formats 1 to 3, which require `narHash`:
     *               used when `assertedNarHash` is absent; without
     *               either, those formats throw.
     */
    virtual nlohmann::json toJSON(
        const StoreDirConfig * store,
        bool includeImpureInfo,
        PathInfoJsonFormat format,
        std::optional<NarHashThunk> narHashFor = std::nullopt) const;
    static UnkeyedValidPathInfo fromJSON(const StoreDirConfig * store, const nlohmann::json & json);

private:
    /* VTable anchor to avoid weak linkage of the vtable - it breaks
       dynamic_cast across shared libraries on Darwin. */
    virtual void anchor();
};

struct ValidPathInfo : virtual UnkeyedValidPathInfo
{
    StorePath path;

    bool operator==(const ValidPathInfo &) const = default;
    auto operator<=>(const ValidPathInfo &) const = default;

    /**
     * Return a fingerprint of the store path to be used in binary
     * cache signatures, version 2: `2;<path>;<object hash>;<references>`
     * (`doc/lazy-store/01-specification.md` section 9.11).
     *
     * @throws Error if the object hash is not known.
     */
    std::string fingerprint(const StoreDirConfig & store) const;

    /**
     * The version-1 fingerprint, `1;<path>;<NAR hash>;<NAR size>;<references>`,
     * over a NAR hash the caller supplies -- the shim under which a
     * signature made before the object hash is still verified.
     *
     * @throws Error if `narSize` is 0.
     */
    std::string fingerprintV1(const StoreDirConfig & store, const Hash & narHash) const;

    /**
     * The object hash, which every info the local store holds carries.
     *
     * @throws Error("store path '%s' has no object hash") otherwise: a
     * description from an old peer or cache that has not yet been
     * verified against the NAR it names.
     */
    const ObjectHash & requireObjectHash(const StoreDirConfig & store) const;

    /**
     * Sign over the version-2 fingerprint; a signature present is not
     * added twice.  Signing at registration does this and nothing more
     * (01 §9.11, "Signatures": no walk per output); `signV1` is for the
     * places that hold or are asked to walk for the NAR hash.
     */
    void sign(const Store & store, const Signer & signer);
    void sign(const Store & store, const std::vector<std::unique_ptr<Signer>> & signers);

    /**
     * Sign over the version-1 fingerprint, for clients that verify only
     * that form (every Nix before the object hash), given the NAR hash the
     * caller computed or was asserted.  Deduplicated as `sign` is.
     *
     * @throws Error if `narSize` is 0 (`fingerprintV1`).
     */
    void signV1(const StoreDirConfig & store, const Hash & narHash, const Signer & signer);

    /**
     * @return The `ContentAddressWithReferences` that determines the
     * store path for a content-addressed store object, `std::nullopt`
     * for an input-addressed store object.
     */
    std::optional<ContentAddressWithReferences> contentAddressWithReferences() const;

    /**
     * @return true iff the path is verifiably content-addressed.
     */
    bool isContentAddressed(const StoreDirConfig & store) const;

    static const size_t maxSigs = std::numeric_limits<size_t>::max();

    /**
     * Return the number of distinct keys among `publicKeys` that signed
     * this path, or maxSigs if the path is content-addressed.  A key
     * counts once if any of its signatures verifies, over either
     * fingerprint (01 §9.11, "Signatures").  The NAR hash for version 1
     * is `assertedNarHash`, else `narHashFor`, called at most once and
     * only for a signature by one of `publicKeys` that does not verify
     * under version 2; without it such a signature verifies under no
     * fingerprint.
     */
    size_t checkSignatures(
        const StoreDirConfig & store,
        const PublicKeys & publicKeys,
        std::optional<NarHashThunk> narHashFor = std::nullopt) const;

    /**
     * Verify a single signature, under the same rule; `narHashFor` is
     * called when the version-1 check needs it and nothing is asserted.
     */
    bool checkSignature(
        const StoreDirConfig & store,
        const PublicKeys & publicKeys,
        const Signature & sig,
        std::optional<NarHashThunk> narHashFor = std::nullopt) const;

    /**
     * References as store path basenames, including a self reference if it has one.
     */
    Strings shortRefs() const;

    ValidPathInfo(StorePath && path, UnkeyedValidPathInfo info)
        : UnkeyedValidPathInfo(info)
        , path(std::move(path))
    {
    }

    ValidPathInfo(const StorePath & path, UnkeyedValidPathInfo info)
        : ValidPathInfo(StorePath{path}, std::move(info))
    {
    }

    static ValidPathInfo makeFromCA(
        const StoreDirConfig & store,
        std::string_view name,
        ContentAddressWithReferences && ca,
        std::optional<ObjectHash> objectHash);

private:
    void anchor() override;
};

static_assert(std::is_move_assignable_v<ValidPathInfo>);
static_assert(std::is_copy_assignable_v<ValidPathInfo>);
static_assert(std::is_copy_constructible_v<ValidPathInfo>);
static_assert(std::is_move_constructible_v<ValidPathInfo>);

using ValidPathInfos = std::map<StorePath, ValidPathInfo>;

} // namespace nix

JSON_IMPL(nix::PathInfoJsonFormat)
JSON_IMPL(nix::UnkeyedValidPathInfo)
JSON_IMPL(nix::ValidPathInfo)
