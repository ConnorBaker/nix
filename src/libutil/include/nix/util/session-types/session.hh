#pragma once
///@file
///
/// Master Session concept: a protocol is a valid session type iff it is
/// well-scoped, has a dual, and is actionable.
///

#include <type_traits>

#include "nix/util/session-types/protocol.hh"
#include "nix/util/session-types/dual.hh"
#include "nix/util/session-types/scoped.hh"
#include "nix/util/session-types/subst.hh"
#include "nix/util/session-types/lift.hh"
#include "nix/util/session-types/then.hh"
#include "nix/util/session-types/actionable.hh"
#include "nix/util/session-types/select.hh"

namespace nix::session {

namespace detail {

template<typename P, typename = void>
struct HasDual : std::false_type {};

template<typename P>
struct HasDual<P, std::void_t<Dual_t<P>>> : std::true_type {};

} // namespace detail

template<typename P>
concept Session = Scoped<P>
              && detail::HasDual<P>::value
              && Actionable<P>;

} // namespace nix::session
