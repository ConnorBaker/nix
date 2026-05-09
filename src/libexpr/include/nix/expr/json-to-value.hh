#pragma once
///@file

#include "nix/util/error.hh"

#include <string>

namespace nix {

class EvalState;
struct Value;
struct DepSource;

MakeError(JSONParseError, Error);

void parseJSON(EvalState & state, const std::string_view & s, Value & v);

/**
 * Parse JSON into lazy traced data: objects/arrays become thunks that record
 * StructuredContent deps on scalar leaf access. Requires active DepRecordingContext.
 * Falls back to parseJSON for scalar root values.
 */
void parseTracedJSON(EvalState & state, const std::string_view & s, Value & v,
                     const DepSource & depSource, const std::string & depKey);

} // namespace nix
