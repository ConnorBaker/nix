#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <string>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace nix {

/**
 * `traversal`: `v` is the root of the traversal (`UnforceJobs`) that the
 * caller opened for this call: each of its attributes or elements is
 * printed in a job.
 */
nlohmann::json printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    NixStringContext & context,
    bool copyToStore = true,
    bool traversal = false);

void printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    std::ostream & str,
    NixStringContext & context,
    bool copyToStore = true,
    bool traversal = false);

MakeError(JSONSerializationError, Error);

} // namespace nix
