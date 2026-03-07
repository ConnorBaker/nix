#pragma once

#include <sstream>
#include <string>

#include <toml.hpp>

namespace nix::eval_trace {

/**
 * Canonical string form of a TOML scalar value for hashing.
 * Single implementation shared between record-time (fromTOML.cc) and
 * verify-time (trace-verify-deps.cc).
 */
inline std::string tomlCanonical(const toml::value & v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

} // namespace nix::eval_trace
