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

namespace nix {

/* protocol-agnostic templates */

USE_LENGTH_PREFIX_SERIALISERS(CommonProto)

/* protocol-specific templates */

} // namespace nix
