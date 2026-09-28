#pragma once
///@file

#include "nix/store/store-api.hh"

namespace nix {

enum struct UseLookupPath { Use, DontUse };

/** Replace the process image, using a chroot for a diverted store.
 * Callers persist evaluator caches before handoff; a successful exec runs no destructors. */
void execProgramInStore(
    ref<Store> store,
    UseLookupPath useLookupPath,
    const std::string & program,
    const Strings & args,
    std::optional<std::string_view> system = std::nullopt,
    std::optional<StringMap> env = std::nullopt);

} // namespace nix
