#pragma once
///@file

#include "nix/util/error.hh"

#include <string>

namespace nix {

class EvalState;
struct Value;

MakeError(JSONParseError, Error);

void parseJSON(EvalState & state, const std::string_view & s, Value & v);

/**
 * Parse JSON into lazy traced data: objects/arrays become thunks that record
 * StructuredContent deps on scalar leaf access. Requires active DependencyTracker.
 * Falls back to parseJSON for scalar root values.
 */
void parseTracedJSON(EvalState & state, const std::string_view & s, Value & v,
                     const std::string & depSource, const std::string & depKey);

} // namespace nix
