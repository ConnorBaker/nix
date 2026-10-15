#pragma once

#include "nix/expr/value.hh"
#include "nix/expr/symbol-table.hh"

namespace nix {

class EvalState;

/**
 * Print a value in the deprecated format used by `nix-instantiate --eval` and
 * `nix-env` (for manifests).
 *
 * This output can't be changed because it's part of the `nix-instantiate` API,
 * but it produces ambiguous output; unevaluated thunks and lambdas (and a few
 * other types) are printed as Nix path syntax like `<CODE>`.
 *
 * See: https://github.com/NixOS/nix/issues/9730
 */
/**
 * PROTOTYPE (unforce). Told where `printAmbiguous` printed each attribute
 * set and list that `seen` tracks, as `str.tellp()` offsets: in full (`open`
 * at its first character, after `seen` marked `key`, then `close` after its
 * last), or as «repeated» (`repeated`, with that text's span). A caller
 * that prints parts of one value out of order can then re-decide which
 * occurrence is printed in full (`printStrictUnforced`).
 */
struct PrintAmbiguousRecorder
{
    virtual ~PrintAmbiguousRecorder() = default;
    virtual void open(const void * key, std::streamoff at) = 0;
    virtual void close(std::streamoff at) = 0;
    virtual void repeated(const void * key, std::streamoff from, std::streamoff to) = 0;
};

void printAmbiguous(
    EvalState & state,
    Value & v,
    std::ostream & str,
    std::set<const void *> * seen,
    NixStringContext * context = nullptr,
    size_t depth = 0,
    PrintAmbiguousRecorder * recorder = nullptr);

} // namespace nix
