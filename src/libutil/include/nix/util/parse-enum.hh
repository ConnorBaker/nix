#pragma once
///@file

#include "nix/util/configuration.hh"
#include "nix/util/error.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/suggestions.hh"
#include "nix/util/types.hh"

#include <initializer_list>
#include <optional>
#include <string_view>

namespace nix {

/**
 * One row of a named-enum lookup table for `parseEnumOpt` /
 * `parseEnumOrThrow`. The optional `onMatch` callback runs after a
 * successful name match and can be used to gate on experimental
 * features (e.g. `xpSettings.require(Xp::BLAKE3Hashes)`) or to emit a
 * deprecation warning for an alias spelling.
 */
template<typename E>
struct EnumName
{
    std::string_view name;
    E value;
    void (*onMatch)(const ExperimentalFeatureSettings &) = nullptr;
};

/**
 * A constant table of `EnumName<E>` rows. Stored as
 * `std::initializer_list` so call sites can write `static constexpr`
 * arrays of brace-enclosed initializers without needing to spell out
 * a length.
 */
template<typename E>
using EnumNames = std::initializer_list<EnumName<E>>;

/**
 * Look up `input` in `table`. Returns the matched value, or
 * `std::nullopt` if no row's `name` equals `input`. If a row has an
 * `onMatch` callback, it is invoked before returning (so xp-feature
 * gates and deprecation warnings still fire on the optional path).
 */
template<typename E>
std::optional<E> parseEnumOpt(
    std::string_view input,
    EnumNames<E> table,
    const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)
{
    for (const auto & entry : table) {
        if (entry.name == input) {
            if (entry.onMatch)
                entry.onMatch(xpSettings);
            return entry.value;
        }
    }
    return std::nullopt;
}

/**
 * Collect the set of names from `table` for use with
 * `Suggestions::bestMatches`. Independent helper because some call
 * sites (e.g. `parseCompressionAlgo`) need to attach the suggestions
 * to a custom exception type rather than the `UsageError` that
 * `parseEnumOrThrow` produces.
 */
template<typename E>
StringSet enumNames(EnumNames<E> table)
{
    StringSet res;
    for (const auto & entry : table)
        res.emplace(entry.name);
    return res;
}

/**
 * Render the allowed-name list in the form
 * `'a'`, `'a' or 'b'`, or `'a', 'b', or 'c'` (Oxford comma for
 * three or more). Used by `parseEnumOrThrow` to produce a helpful
 * "expect …" tail in the error message.
 */
template<typename E>
std::string formatEnumNameList(EnumNames<E> table)
{
    std::string res;
    auto it = table.begin();
    auto end = table.end();
    auto remaining = static_cast<size_t>(end - it);
    for (size_t i = 0; it != end; ++it, ++i) {
        if (i > 0) {
            if (remaining == 2 && i == remaining - 1)
                res += " or ";
            else if (i == remaining - 1)
                res += ", or ";
            else
                res += ", ";
        }
        res += '\'';
        res += it->name;
        res += '\'';
    }
    return res;
}

/**
 * Like `parseEnumOpt` but throws `UsageError` on no match. The
 * thrown error carries best-match suggestions derived from the
 * table's name set, quotes `settingName` in the message so the
 * user can tell which option they typo'd, and lists the accepted
 * names so they don't have to consult the manual.
 */
template<typename E>
E parseEnumOrThrow(
    std::string_view input,
    EnumNames<E> table,
    std::string_view settingName,
    const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)
{
    if (auto v = parseEnumOpt<E>(input, table, xpSettings))
        return *v;
    throw UsageError(
        Suggestions::bestMatches(enumNames<E>(table), input),
        "unknown %s '%s', expect %s",
        settingName,
        input,
        formatEnumNameList<E>(table));
}

} // namespace nix
