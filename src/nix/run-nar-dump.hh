#pragma once
///@file

#include "nix/util/archive.hh"
#include "nix/util/error.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/serialise.hh"
#include "nix/util/terminal.hh"

namespace nix {

/**
 * Stream a NAR (Nix Archive) to stdout via `dump`.
 *
 * `dump` is a callable taking a `Sink &` that writes the NAR. The
 * three CLI entry points that share this scaffold are
 * `nix store dump-path`, `nix nar pack`, and the legacy
 * `nix-store --dump`:
 *
 *   - `nix store dump-path` and `nix nar pack` refuse to write a NAR
 *     when stdout is a TTY (the user almost certainly meant to pipe);
 *   - `nix-store --dump` skips the check, preserving the historical
 *     behaviour of the legacy CLI.
 *
 * Toggle the guard via `checkTTY`.
 */
template<typename Dump>
void runNarDump(Dump dump, bool checkTTY)
{
    auto fd = getStandardOutput();
    if (checkTTY && isTTY(fd))
        throw UsageError("refusing to write NAR to a terminal");
    FdSink sink(std::move(fd));
    dump(sink);
    sink.flush();
}

} // namespace nix
