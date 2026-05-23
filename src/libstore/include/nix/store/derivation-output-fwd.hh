#pragma once
///@file

#include <map>
#include <string>

namespace nix {

struct DerivationOutput;

typedef std::map<std::string, DerivationOutput> DerivationOutputs;

} // namespace nix
