#pragma once
///@file
///
/// Small mixin for ordinary move-only types.
///
/// Use this for capabilities/handles where copying is forbidden but dropping is
/// normal. Do not use it for must-consume typestate transitions; those should
/// use `Linear<Derived>`.

namespace nix {

class MoveOnly
{
protected:
    MoveOnly() = default;
    MoveOnly(MoveOnly &&) noexcept = default;
    MoveOnly & operator=(MoveOnly &&) noexcept = default;
    ~MoveOnly() = default;

public:
    MoveOnly(const MoveOnly &) = delete;
    MoveOnly & operator=(const MoveOnly &) = delete;
};

} // namespace nix
