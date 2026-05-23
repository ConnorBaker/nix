#pragma once
///@file

#include "nix/cmd/command.hh"

/**
 * Register a `nix` subcommand at namespace scope.
 *
 * Wraps `nix::RegisterCommand` so callers do not have to invent a unique
 * variable name, write the lambda factory, or pick between
 * `registerCommand` (single-segment) and `registerCommand2`
 * (multi-segment). The variadic argument list is the command path:
 *
 *     NIX_REGISTER_COMMAND(CmdBuild, "build");
 *     NIX_REGISTER_COMMAND(CmdStoreGC, "store", "gc");
 *
 * The generated registrar variable is `nixRegisterCommand_##CLASS`,
 * which is unique within a translation unit because every command class
 * is declared exactly once per file.
 */
#define NIX_REGISTER_COMMAND(CLASS, ...)                                                            \
    static const ::nix::RegisterCommand nixRegisterCommand_##CLASS{                                 \
        std::vector<std::string>{__VA_ARGS__}, []() { return ::nix::make_ref<CLASS>(); }}
