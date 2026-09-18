#include <nlohmann/json.hpp>

#include "nix/store/path-info.hh"
#include "nix/store/store-api.hh"
#include "nix/util/json-utils.hh"
#include "nix/util/comparator.hh"
#include "nix/util/strings.hh"

namespace nix {

void UnkeyedValidPathInfo::anchor() {}

void ValidPathInfo::anchor() {}

PathInfoJsonFormat parsePathInfoJsonFormat(uint64_t version)
{
    switch (version) {
    case 1:
        return PathInfoJsonFormat::V1;
    case 2:
        return PathInfoJsonFormat::V2;
    case 3:
        return PathInfoJsonFormat::V3;
    case 4:
        return PathInfoJsonFormat::V4;
    default:
        throw Error("unsupported path info JSON format version %d; supported versions are 1, 2, 3 and 4", version);
    }
}

UnkeyedValidPathInfo::UnkeyedValidPathInfo(const StoreDirConfig & store, std::optional<ObjectHash> objectHash)
    : UnkeyedValidPathInfo{store.storeDir, std::move(objectHash)}
{
}

GENERATE_CMP_EXT(
    ,
    std::weak_ordering,
    UnkeyedValidPathInfo,
    me->storeDir,
    me->deriver,
    me->objectHash,
    me->assertedNarHash,
    me->references,
    me->registrationTime,
    me->narSize,
    // me->id,
    me->ultimate,
    me->sigs,
    me->ca);

const ObjectHash & ValidPathInfo::requireObjectHash(const StoreDirConfig & store) const
{
    if (!objectHash)
        throw Error("store path '%s' has no object hash", store.printStorePath(path));
    return *objectHash;
}

std::string ValidPathInfo::fingerprint(const StoreDirConfig & store) const
{
    return "2;" + store.printStorePath(path) + ";" + requireObjectHash(store).render() + ";"
           + concatStringsSep(",", store.printStorePathSet(references));
}

std::string ValidPathInfo::fingerprintV1(const StoreDirConfig & store, const Hash & narHash) const
{
    if (narSize == 0)
        throw Error(
            "cannot calculate fingerprint of path '%s' because its size is not known", store.printStorePath(path));
    return "1;" + store.printStorePath(path) + ";" + narHash.to_string(HashFormat::Nix32, true) + ";"
           + std::to_string(narSize) + ";" + concatStringsSep(",", store.printStorePathSet(references));
}

void ValidPathInfo::sign(const Store & store, const Signer & signer)
{
    sigs.insert(signer.signDetached(fingerprint(store)));
}

void ValidPathInfo::sign(const Store & store, const std::vector<std::unique_ptr<Signer>> & signers)
{
    auto fingerprint = this->fingerprint(store);
    for (auto & signer : signers) {
        sigs.insert(signer->signDetached(fingerprint));
    }
}

void ValidPathInfo::signV1(const StoreDirConfig & store, const Hash & narHash, const Signer & signer)
{
    sigs.insert(signer.signDetached(fingerprintV1(store, narHash)));
}

std::optional<ContentAddressWithReferences> ValidPathInfo::contentAddressWithReferences() const
{
    if (!ca)
        return std::nullopt;

    switch (ca->method.raw) {
    case ContentAddressMethod::Raw::Text: {
        assert(references.count(path) == 0);
        return TextInfo{
            .hash = ca->hash,
            .references = references,
        };
    }

    case ContentAddressMethod::Raw::Flat:
    case ContentAddressMethod::Raw::NixArchive:
    case ContentAddressMethod::Raw::Git:
    default: {
        auto refs = references;
        bool hasSelfReference = false;
        if (refs.count(path)) {
            hasSelfReference = true;
            refs.erase(path);
        }
        return FixedOutputInfo{
            .method = ca->method.getFileIngestionMethod(),
            .hash = ca->hash,
            .references =
                {
                    .others = std::move(refs),
                    .self = hasSelfReference,
                },
        };
    }
    }
}

bool ValidPathInfo::isContentAddressed(const StoreDirConfig & store) const
{
    auto fullCaOpt = contentAddressWithReferences();

    if (!fullCaOpt)
        return false;

    auto caPath = store.makeFixedOutputPathFromCA(path.name(), *fullCaOpt);

    bool res = caPath == path;

    if (!res)
        printError("warning: path '%s' claims to be content-addressed but isn't", store.printStorePath(path));

    return res;
}

size_t ValidPathInfo::checkSignatures(
    const StoreDirConfig & store, const PublicKeys & publicKeys, std::optional<NarHashThunk> narHashFor) const
{
    if (isContentAddressed(store))
        return maxSigs;

    /* The caller's walk, once for every signature that needs it. */
    std::optional<Hash> walked;
    std::optional<NarHashThunk> once;
    if (narHashFor)
        once = NarHashThunk{[&]() -> Hash {
            if (!walked)
                walked = (*narHashFor)();
            return *walked;
        }};

    /* Keys, not signatures: one key may have signed both fingerprints. */
    std::set<std::string> keys;
    for (auto & sig : sigs)
        if (!keys.count(sig.keyName) && checkSignature(store, publicKeys, sig, once))
            keys.insert(sig.keyName);
    return keys.size();
}

bool ValidPathInfo::checkSignature(
    const StoreDirConfig & store,
    const PublicKeys & publicKeys,
    const Signature & sig,
    std::optional<NarHashThunk> narHashFor) const
{
    /* Over the version-2 fingerprint when the object hash is known; over
       the version-1 fingerprint when the NAR hash is: asserted by this
       description (the shim: a signature made before the object hash, on
       a description that still carries the NAR hash it signed), else
       given by the caller's walk -- paid only for a signature one of the
       trusted keys made, since a signature by any other key verifies
       under no fingerprint (`verifyDetached` finds the key by name). */
    if (objectHash && verifyDetached(fingerprint(store), sig, publicKeys))
        return true;
    if (narSize == 0)
        return false;
    if (assertedNarHash)
        return verifyDetached(fingerprintV1(store, *assertedNarHash), sig, publicKeys);
    if (narHashFor && publicKeys.count(sig.keyName))
        return verifyDetached(fingerprintV1(store, (*narHashFor)()), sig, publicKeys);
    return false;
}

Strings ValidPathInfo::shortRefs() const
{
    Strings refs;
    for (auto & r : references)
        refs.push_back(std::string(r.to_string()));
    return refs;
}

ValidPathInfo ValidPathInfo::makeFromCA(
    const StoreDirConfig & store,
    std::string_view name,
    ContentAddressWithReferences && ca,
    std::optional<ObjectHash> objectHash)
{
    ValidPathInfo res{
        store.makeFixedOutputPathFromCA(name, ca),
        UnkeyedValidPathInfo(store, std::move(objectHash)),
    };
    res.ca = ContentAddress{
        .method = ca.getMethod(),
        .hash = ca.getHash(),
    };
    res.references = std::visit(
        overloaded{
            [&](TextInfo && ti) { return std::move(ti.references); },
            [&](FixedOutputInfo && foi) {
                auto references = std::move(foi.references.others);
                if (foi.references.self)
                    references.insert(res.path);
                return references;
            },
        },
        std::move(ca).raw);
    return res;
}

nlohmann::json UnkeyedValidPathInfo::toJSON(
    const StoreDirConfig * store,
    bool includeImpureInfo,
    PathInfoJsonFormat format,
    std::optional<NarHashThunk> narHashFor) const
{
    using nlohmann::json;

    if (format == PathInfoJsonFormat::V1)
        assert(store);

    auto jsonObject = json::object();

    jsonObject["version"] = format;

    jsonObject["storeDir"] = storeDir;

    if (format == PathInfoJsonFormat::V4) {
        if (!objectHash)
            throw Error(
                "cannot render path info as JSON format 4: the object hash is not known (the description came from a peer that does not carry one; use a lower format)");
        jsonObject["objectHash"] = objectHash->render();
        if (assertedNarHash)
            jsonObject["narHash"] = *assertedNarHash;
    } else {
        /* Formats 1 to 3 require the NAR hash: the one asserted to us, else
           the shim's walk, else nothing to write. */
        auto narHash = [&]() -> Hash {
            if (assertedNarHash)
                return *assertedNarHash;
            if (narHashFor)
                return (*narHashFor)();
            throw Error(
                "cannot render path info as JSON format %d: it requires a NAR hash, and none is known (use format 4, or supply the shim)",
                static_cast<int>(format));
        }();
        jsonObject["narHash"] = format == PathInfoJsonFormat::V1
                                    ? static_cast<json>(narHash.to_string(HashFormat::SRI, true))
                                    : static_cast<json>(narHash);
    }

    jsonObject["narSize"] = narSize;

    {
        auto & jsonRefs = jsonObject["references"] = json::array();
        for (auto & ref : references)
            jsonRefs.emplace_back(
                format == PathInfoJsonFormat::V1 ? static_cast<json>(store->printStorePath(ref))
                                                 : static_cast<json>(ref));
    }

    if (format == PathInfoJsonFormat::V1)
        jsonObject["ca"] = ca ? static_cast<json>(renderContentAddress(*ca)) : static_cast<json>(nullptr);
    else
        jsonObject["ca"] = ca;

    if (includeImpureInfo) {
        if (format == PathInfoJsonFormat::V1) {
            jsonObject["deriver"] =
                deriver ? static_cast<json>(store->printStorePath(*deriver)) : static_cast<json>(nullptr);
        } else {
            jsonObject["deriver"] = deriver;
        }
        jsonObject["registrationTime"] = registrationTime ? std::optional{registrationTime} : std::nullopt;

        jsonObject["ultimate"] = ultimate;

        if (format >= PathInfoJsonFormat::V3) {
            /* Structured signatures from version 3 on; version 4 keeps version 3's shape. */
            jsonObject["signatures"] = sigs;
        } else {
            auto & sigsObj = jsonObject["signatures"] = json::array();
            for (auto & sig : sigs)
                sigsObj.push_back(sig.to_string());
        }
    }

    return jsonObject;
}

UnkeyedValidPathInfo UnkeyedValidPathInfo::fromJSON(const StoreDirConfig * store, const nlohmann::json & _json)
{
    auto & json = getObject(_json);

    PathInfoJsonFormat format = PathInfoJsonFormat::V1;
    if (auto * version = optionalValueAt(json, "version"))
        format = *version;

    if (format == PathInfoJsonFormat::V1)
        assert(store);

    UnkeyedValidPathInfo res{
        [&] {
            if (auto * rawStoreDir = optionalValueAt(json, "storeDir"))
                return getString(*rawStoreDir);
            else if (format == PathInfoJsonFormat::V1)
                return store->storeDir;
            else
                throw Error("'storeDir' field is required in path info JSON format version 2");
        }(),
        [&]() -> std::optional<ObjectHash> {
            if (format != PathInfoJsonFormat::V4)
                return std::nullopt;
            try {
                return ObjectHash::parseOrThrow(getString(valueAt(json, "objectHash")));
            } catch (Error & e) {
                e.addTrace({}, "while reading key 'objectHash'");
                throw;
            }
        }(),
    };

    try {
        if (format == PathInfoJsonFormat::V4) {
            if (auto * rawNarHash = optionalValueAt(json, "narHash"))
                if (auto * narHash = getNullable(*rawNarHash))
                    res.assertedNarHash = Hash(*narHash);
        } else {
            res.assertedNarHash = format == PathInfoJsonFormat::V1 ? Hash::parseSRI(getString(valueAt(json, "narHash")))
                                                                   : Hash(valueAt(json, "narHash"));
        }
    } catch (Error & e) {
        e.addTrace({}, "while reading key 'narHash'");
        throw;
    }

    res.narSize = getUnsigned(valueAt(json, "narSize"));

    try {
        auto & references = getArray(valueAt(json, "references"));
        for (auto & input : references)
            res.references.insert(
                format == PathInfoJsonFormat::V1 ? store->parseStorePath(getString(input))
                                                 : static_cast<StorePath>(input));
    } catch (Error & e) {
        e.addTrace({}, "while reading key 'references'");
        throw;
    }

    try {
        if (format == PathInfoJsonFormat::V1) {
            if (auto * rawCa = getNullable(valueAt(json, "ca")))
                res.ca = ContentAddress::parse(getString(*rawCa));
        } else {
            res.ca = ptrToOwned<ContentAddress>(getNullable(valueAt(json, "ca")));
        }
    } catch (Error & e) {
        e.addTrace({}, "while reading key 'ca'");
        throw;
    }

    if (auto * rawDeriver0 = optionalValueAt(json, "deriver")) {
        if (format == PathInfoJsonFormat::V1) {
            if (auto * rawDeriver = getNullable(*rawDeriver0))
                res.deriver = store->parseStorePath(getString(*rawDeriver));
        } else {
            res.deriver = ptrToOwned<StorePath>(getNullable(*rawDeriver0));
        }
    }

    if (auto * rawRegistrationTime0 = optionalValueAt(json, "registrationTime"))
        if (auto * rawRegistrationTime = getNullable(*rawRegistrationTime0))
            res.registrationTime = getInteger<time_t>(*rawRegistrationTime);

    if (auto * rawUltimate = optionalValueAt(json, "ultimate"))
        res.ultimate = getBoolean(*rawUltimate);

    if (auto * rawSignatures = optionalValueAt(json, "signatures"))
        res.sigs = *rawSignatures;

    return res;
}

} // namespace nix

namespace nlohmann {

nix::PathInfoJsonFormat adl_serializer<nix::PathInfoJsonFormat>::from_json(const json & json)
{
    return nix::parsePathInfoJsonFormat(nix::getUnsigned(json));
}

void adl_serializer<nix::PathInfoJsonFormat>::to_json(json & json, const nix::PathInfoJsonFormat & format)
{
    json = static_cast<int>(format);
}

nix::UnkeyedValidPathInfo adl_serializer<nix::UnkeyedValidPathInfo>::from_json(const json & json)
{
    return nix::UnkeyedValidPathInfo::fromJSON(nullptr, json);
}

void adl_serializer<nix::UnkeyedValidPathInfo>::to_json(json & json, const nix::UnkeyedValidPathInfo & c)
{
    json = c.toJSON(nullptr, true, nix::PathInfoJsonFormat::V4);
}

nix::ValidPathInfo adl_serializer<nix::ValidPathInfo>::from_json(const json & json0)
{
    using namespace nix;
    auto json = getObject(json0);

    return ValidPathInfo{
        valueAt(json, "path"),
        adl_serializer<UnkeyedValidPathInfo>::from_json(json0),
    };
}

void adl_serializer<nix::ValidPathInfo>::to_json(json & json, const nix::ValidPathInfo & v)
{
    using namespace nix;
    adl_serializer<UnkeyedValidPathInfo>::to_json(json, v);
    json["path"] = v.path;
}

} // namespace nlohmann
