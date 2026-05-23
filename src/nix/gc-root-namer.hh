#pragma once
///@file

#include <filesystem>
#include <optional>
#include <string_view>

namespace nix {

/**
 * Tracks state shared by `nix-store --realise` and `nix-instantiate`
 * when the user passes `--add-root <path>` to register a permanent GC
 * root. Both tools build successive root names of the form
 * `baseRoot[-N][-<outputName>]`:
 *
 *  - the `-N` suffix is appended for `N > 1`, so the first root is
 *    `baseRoot`, the second `baseRoot-2`, and so on;
 *  - the `-<outputName>` suffix is appended only when an output name
 *    is supplied and is not the default `"out"`. This second suffix
 *    is used by `nix-store`'s derivation realisation path, where one
 *    counter value is reused across all outputs of one derivation.
 *
 * Wrapping `baseRoot` and `counter` together also normalises the
 * accidental linkage difference between the two call sites: the file-
 * scope variables were `static` in `nix-store/nix-store.cc` but had
 * external linkage in `nix-instantiate/nix-instantiate.cc`. With the
 * helper owning the state, both tools declare a single
 * `static GcRootNamer` and the inconsistency disappears.
 */
class GcRootNamer
{
public:
    /**
     * Absolute path supplied to `--add-root`. Empty means the user
     * did not request a GC root; callers should print a GC warning
     * and skip `addPermRoot`.
     */
    std::filesystem::path baseRoot;

    bool empty() const
    {
        return baseRoot.empty();
    }

    /**
     * Increment the per-root counter. Call once per root the caller
     * intends to register. The counter is monotonically increasing;
     * the type pins the invariant and there is no `set` accessor.
     *
     * Bump scope is intentionally caller-driven: `nix-store`'s
     * derivation branch bumps once per derivation and reuses the
     * value across all outputs (so the per-output suffix in
     * `nameForCurrent(outputName)` differentiates them); the other
     * call sites bump per call.
     */
    void bump()
    {
        ++counter;
    }

    /**
     * Build the GC-root path for the *current* counter value, without
     * bumping it. Appends `-<counter>` when `counter > 1` and, if
     * `outputName` is supplied and not `"out"`, also appends
     * `-<outputName>`.
     */
    std::filesystem::path nameForCurrent(std::optional<std::string_view> outputName = std::nullopt) const
    {
        std::filesystem::path rootName = baseRoot;
        if (counter > 1)
            rootName += "-" + std::to_string(counter);
        if (outputName && *outputName != "out")
            rootName += "-" + std::string(*outputName);
        return rootName;
    }

private:
    int counter = 0;
};

} // namespace nix
