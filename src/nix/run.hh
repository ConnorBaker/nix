#pragma once
///@file

#include <string_view>

#include "nix/store/store-api.hh"

namespace nix {

#ifndef _WIN32
inline constexpr std::string_view chrootHelperName = "__run_in_chroot";
#endif

enum struct UseLookupPath { Use, DontUse };

void execProgramInStore(
    ref<Store> store,
    UseLookupPath useLookupPath,
    const std::string & program,
    const Strings & args,
    std::optional<std::string_view> system = std::nullopt,
    std::optional<StringMap> env = std::nullopt);

} // namespace nix
