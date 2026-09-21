#pragma once
/**
 * @file
 *
 * A string body that may leave the evaluator: only `EvalState::realise` and
 * `emit` make one, after writing every pending store object (`WriteBuffer`),
 * so no reader holds a name with nothing behind it
 * (doc/lazy-store/01-specification.md, section 3).  Nothing is built.
 */

#include "nix/util/types.hh"

#include <ostream>
#include <string>
#include <string_view>

namespace nix {

class EvalState;

class RealisedString
{
    friend class EvalState;

    BackedStringView body;

    explicit RealisedString(BackedStringView && body)
        : body(std::move(body))
    {
    }

public:
    RealisedString(RealisedString &&) = default;
    RealisedString & operator=(RealisedString &&) = default;

    std::string_view view() const
    {
        return *body;
    }

    std::string toOwned() &&
    {
        return std::move(body).toOwned();
    }
};

inline std::ostream & operator<<(std::ostream & str, const RealisedString & s)
{
    return str << s.view();
}

} // namespace nix
