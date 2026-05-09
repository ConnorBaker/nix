#pragma once
///@file

#include "nix/util/sync.hh"
#include "nix/expr/eval.hh"

#include <functional>

namespace nix::eval_trace {

using RootLoader = std::function<Value *()>;

using EvalState = nix::EvalState;

class RootHandle
{
public:
    explicit RootHandle(EvalState & state, RootLoader rootLoader);

    Value * getRealRoot();
    void reset(RootLoader rootLoader);

private:
    RootLoader rootLoader;
    RootValue realRoot;
};

} // namespace nix::eval_trace
