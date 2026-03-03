#pragma once

#include "nix/expr/eval-trace/store/trace-store.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <memory>
#include <optional>

namespace nix {
class EvalState;
}

namespace nix::eval_trace {

struct VerificationScope;

struct VerificationScopeDeleter {
    void operator()(VerificationScope * p) const noexcept;
};

using VerificationScopePtr = std::unique_ptr<VerificationScope, VerificationScopeDeleter>;

VerificationScopePtr createVerificationScope();

std::optional<DepHashValue> computeCurrentHash(
    EvalState & state, const TraceStore::ResolvedDep & dep,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    VerificationScope & scope,
    const boost::unordered_flat_map<std::string, std::string> & dirSets);

} // namespace nix::eval_trace
