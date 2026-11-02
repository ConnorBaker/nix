#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <string>
#include <map>

namespace nix {

/**
 * `traversal`: `v` is the root of the traversal (`UnforceJobs`) that the
 * caller opened for this call: each of its attributes or elements is
 * printed in a job.
 */
void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    std::ostream & out,
    NixStringContext & context,
    const PosIdx pos,
    bool traversal = false);

}
