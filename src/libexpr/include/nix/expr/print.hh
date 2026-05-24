#pragma once
/**
 * @file
 * @brief Common printing functions for the Nix language
 *
 * While most types come with their own methods for printing, they share some
 * functions that are placed here.
 */

#include <iostream>

#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/util/fmt.hh"
#include "nix/expr/value/context.hh"
#include "nix/expr/print-options.hh"

namespace nix {

class Bindings;
class EvalState;
struct Value;

/**
 * A pointer-keyed "already seen" set used by recursive value/attrset
 * walkers to break cycles and de-duplicate visits.
 *
 * `TypedSeenSet<T>` pins the element pointer type so each call site
 * declares which pointer kind it visits. `SeenSet` is the
 * compatibility alias for walkers that legitimately mix two pointer
 * kinds in the same set (the value-printer family visits attrsets
 * through `v.attrs()` and lists through `&v`); it is `TypedSeenSet<void>`.
 *
 *   - `TypedSeenSet<Value>` for walkers that visit only `Value` nodes
 *     (e.g. `EvalState::forceValueDeep`).
 *   - `TypedSeenSet<Bindings>` for walkers that visit only attrset
 *     bodies (e.g. `getDerivations`'s `Done` alias).
 *   - `SeenSet` (alias for `TypedSeenSet<void>`) for walkers that mix
 *     the two pointer kinds in one set.
 *
 * Backed by `boost::unordered_flat_set` for amortised-O(1) insert/lookup
 * and locality-friendly memory layout. Iteration order is unspecified
 * — every in-tree consumer uses the set only as a membership probe,
 * never iterates it.
 */
template<class T>
using TypedSeenSet = boost::unordered_flat_set<const T *>;

using SeenSet = TypedSeenSet<void>;

/**
 * Try to record `p` in `seen`. Returns `true` if `p` was newly inserted,
 * `false` if it was already present. The template parameter follows
 * `seen`'s element type so callers can `dedupe(seen, &v)` without
 * having to spell out the pointer kind.
 */
template<class T>
inline bool dedupe(TypedSeenSet<T> & seen, const T * p)
{
    return seen.insert(p).second;
}

/* The void-typed compatibility overload accepts any pointer kind via
   implicit conversion to `const void *`, matching the original API. */
inline bool dedupe(SeenSet & seen, const void * p)
{
    return seen.insert(p).second;
}

/**
 * Print a string as a Nix string literal.
 *
 * Quotes and fairly minimal escaping are added.
 *
 * @param o The output stream to print to
 * @param s The logical string
 */
std::ostream & printLiteralString(std::ostream & o, std::string_view s);

inline std::ostream & printLiteralString(std::ostream & o, const char * s)
{
    return printLiteralString(o, std::string_view(s));
}

inline std::ostream & printLiteralString(std::ostream & o, const std::string & s)
{
    return printLiteralString(o, std::string_view(s));
}

/** Print `true` or `false`. */
std::ostream & printLiteralBool(std::ostream & o, bool b);

/**
 * Print a string as an attribute name in the Nix expression language syntax.
 *
 * Prints a quoted string if necessary.
 */
std::ostream & printAttributeName(std::ostream & o, std::string_view s);

/**
 * Returns `true' is a string is a reserved keyword which requires quotation
 * when printing attribute set field names.
 */
bool isReservedKeyword(const std::string_view str);

/**
 * Returns `true` if `s` is a valid Nix identifier (matching the lexer's `ID`
 * regex `[a-zA-Z_][a-zA-Z0-9_'-]*`) and is not a reserved keyword.
 *
 * Used by attribute-name and identifier printing to decide whether the input
 * can be emitted bare or must be string-quoted.
 *
 * Invariant: this predicate must accept the same set of strings that the
 * lexer's `ID` rule accepts. If the lexer regex in `lexer.l` ever changes,
 * this implementation must change with it (and the corresponding pinning
 * comment in `lexer.l` should be updated).
 */
bool isVarName(std::string_view s);

/**
 * Print a string as an identifier in the Nix expression language syntax.
 *
 * FIXME: "identifier" is ambiguous. Identifiers do not have a single
 *        textual representation. They can be used in variable references,
 *        let bindings, left-hand sides or attribute names in a select
 *        expression, or something else entirely, like JSON. Use one of the
 *        `print*` functions instead.
 */
std::ostream & printIdentifier(std::ostream & o, std::string_view s);

void printValue(
    EvalState & state,
    std::ostream & str,
    Value & v,
    PrintOptions options = PrintOptions{},
    NixStringContext * context = nullptr);

/**
 * A partially-applied form of `printValue` which can be formatted using `<<`
 * without allocating an intermediate string.
 */
class ValuePrinter
{
    friend std::ostream & operator<<(std::ostream & output, const ValuePrinter & printer);
private:
    EvalState & state;
    Value & value;
    PrintOptions options;
    NixStringContext * context;

public:
    ValuePrinter(
        EvalState & state, Value & value, PrintOptions options = PrintOptions{}, NixStringContext * context = nullptr)
        : state(state)
        , value(value)
        , options(options)
        , context(context)
    {
    }
};

std::ostream & operator<<(std::ostream & output, const ValuePrinter & printer);

/**
 * `ValuePrinter` does its own ANSI formatting, so we don't color it
 * magenta.
 */
template<>
HintFmt & HintFmt::operator%(const ValuePrinter & value);

} // namespace nix
