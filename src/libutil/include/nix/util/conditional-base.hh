#pragma once
///@file
///
/// Conditional EBO base for parameterized class templates.
///
/// `Absent<T>` is an empty struct keyed on `T`, used as a placeholder base
/// when a conditional field bundle is inactive. Since each `Absent<T>` is a
/// distinct type (for distinct `T`), EBO applies independently to each
/// inactive base within the same class — no manual slot numbering needed.
///
/// `ConditionalBase<Cond, T>` selects `T` when `Cond` is true, `Absent<T>`
/// when false. Members of `T` are accessible on the derived class when the
/// base is active; accessing them when inactive is a compile error because
/// `Absent<T>` has no such members.

#include <type_traits>

namespace nix {

/// Empty placeholder base for a physically-absent field bundle.
template <typename T>
struct Absent {};

/// Selects T as a base when Cond is true, Absent<T> when false.
template <bool Cond, typename T>
using ConditionalBase = std::conditional_t<Cond, T, Absent<T>>;

} // namespace nix
