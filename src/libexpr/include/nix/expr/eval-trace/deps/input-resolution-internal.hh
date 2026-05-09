#pragma once
///@file
/// Internal dep-key encoding helpers for the interning/persistence substrate.

#include "nix/expr/eval-trace/deps/input-resolution.hh"

#include <vector>

namespace nix {

/// Shape-parameterised encoded blob wrapper.  The phantom tag is
/// `singleton::Tag<SimpleKeyShape::X>`, so each shape stays
/// non-interconvertible with the others at compile time.
template<SimpleKeyShape S>
using EncodedSimpleDepKeyBlobFor = Tagged<singleton::Tag<S>, std::vector<uint8_t>>;

using EncodedDerivedStorePathDepKeyBlob      = EncodedSimpleDepKeyBlobFor<SimpleKeyShape::DerivedStorePath>;
using EncodedStorePathAvailabilityDepKeyBlob = EncodedSimpleDepKeyBlobFor<SimpleKeyShape::StorePathAvailability>;
using EncodedRuntimeFetchIdentityDepKeyBlob  = EncodedSimpleDepKeyBlobFor<SimpleKeyShape::RuntimeFetchIdentity>;

EncodedDerivedStorePathDepKeyBlob encodeDerivedStorePathDepKey(const DerivedStorePathDepKey & key);
EncodedStorePathAvailabilityDepKeyBlob encodeStorePathAvailabilityDepKey(const StorePathAvailabilityDepKey & key);
EncodedRuntimeFetchIdentityDepKeyBlob encodeRuntimeFetchIdentityDepKey(const RuntimeFetchIdentityDepKey & key);

} // namespace nix
