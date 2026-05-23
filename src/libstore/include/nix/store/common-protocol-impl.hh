#pragma once
/**
 * @file
 *
 * Template implementations (as opposed to mere declarations).
 *
 * This file is an example of the "impl.hh" pattern. See the
 * contributing guide.
 */

#include "nix/store/common-protocol.hh"
#include "nix/store/length-prefixed-protocol-helper.hh"

NIX_DEFINE_LENGTH_PREFIX_SERIALISERS(CommonProto)

namespace nix {

/* protocol-specific templates */

} // namespace nix
