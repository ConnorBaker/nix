#pragma once
///@file
///
/// Data gated by a GDP proof.
///
/// ProofGuarded<T, Tag> wraps data accessible only when the caller
/// provides a const Proof<Tag> &. Generalizes StrandLocal<T, Tag>
/// (strand-local.hh:46-59).

#include "nix/util/gdp/proof.hh"

#include <utility>

namespace nix::gdp {

template<typename T, typename Tag>
class ProofGuarded {
    T data_;

public:
    ProofGuarded() = default;

    template<typename... Args>
        requires (sizeof...(Args) > 0)
    explicit ProofGuarded(Args &&... args)
        : data_(std::forward<Args>(args)...) {}

    ProofGuarded(const ProofGuarded &) = delete;
    ProofGuarded & operator=(const ProofGuarded &) = delete;

    T & access(const Proof<Tag> &) { return data_; }
    const T & access(const Proof<Tag> &) const { return data_; }
};

} // namespace nix::gdp
