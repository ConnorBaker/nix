#pragma once
///@file
/**
 * The fixed hashes the object-hash tests share (object-hash-path-info.cc,
 * object-hash-worker-protocol.cc, object-hash-nar-info.cc): the NAR hash
 * the json-1..3 path-info fixtures carry, in its two renderings, and as
 * the object hash git's SHA-256 empty-tree id, a value any reader can
 * recompute (sha256("tree 0\0")).
 */
#include "nix/util/hash.hh"
#include "nix/util/object-hash.hh"

#include <string>

namespace nix::object_hash_fixtures {

inline const std::string narHashSRI = "sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc=";
inline const std::string narHashBase16 = "15e3c560894cbb27085cf65b5a2ecb18488c999497f4531b6907a7581ce6d527";
inline const std::string objectHashHex = "6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9b939dd74decc5321";
inline const std::string objectHashRendered = "git:sha256:" + objectHashHex;

inline Hash narHash()
{
    return Hash::parseSRI(narHashSRI);
}

inline ObjectHash objectHash()
{
    return ObjectHash::parseOrThrow(objectHashRendered);
}

} // namespace nix::object_hash_fixtures
