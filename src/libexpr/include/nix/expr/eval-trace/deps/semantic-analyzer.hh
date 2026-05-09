#pragma once
///@file
/// Runtime-owned semantic producer caches for eval-trace.

#include "nix/util/pos-idx.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"

#include <unordered_map>

namespace nix::eval_trace {

class NixSemanticAnalyzer
{
    std::unordered_map<PosIdx, NixBindingEntry> nixBindings;

public:
    void clear()
    {
        nixBindings.clear();
    }

    void rememberBinding(PosIdx pos, NixBindingEntry entry)
    {
        nixBindings.insert_or_assign(pos, std::move(entry));
    }

    const NixBindingEntry * lookupBinding(PosIdx pos) const
    {
        auto it = nixBindings.find(pos);
        if (it == nixBindings.end())
            return nullptr;
        return &it->second;
    }
};

} // namespace nix::eval_trace
