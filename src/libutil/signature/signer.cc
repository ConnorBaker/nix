#include "nix/util/signature/signer.hh"

#include <sodium.h>

namespace nix {

LocalSigner::LocalSigner(SecretKey && privateKey)
    : privateKey(std::move(privateKey))
    // Derive from the member (already initialised above in declaration
    // order), not the moved-from parameter.
    , publicKey(this->privateKey.toPublicKey())
{
}

Signature LocalSigner::signDetached(std::string_view s) const
{
    return privateKey.signDetached(s);
}

const PublicKey & LocalSigner::getPublicKey()
{
    return publicKey;
}

} // namespace nix
