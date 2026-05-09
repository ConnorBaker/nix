#pragma once

#include "nix/util/error.hh"

namespace nix {

struct LookupPathMissError : Error
{
    using Error::Error;
};

} // namespace nix
