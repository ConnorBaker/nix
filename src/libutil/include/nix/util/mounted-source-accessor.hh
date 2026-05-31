#pragma once

#include "nix/util/switch-source-accessor.hh"

namespace nix {

/**
 * The **mutable** face of `Switch` (`switch-source-accessor.hh`): a
 * longest-prefix-match combinator whose mount set grows at runtime via
 * `mount()`. All read/resolve/strip behaviour is inherited from
 * `SwitchSourceAccessor`; this interface adds the runtime mutators.
 *
 * Used for `EvalState::storeFS`, where one mount per lazily-materialised
 * input/source is added during evaluation. For a fixed mount set known
 * at construction, use `makeSwitch` (the immutable face) instead.
 */
struct MountedSourceAccessor : SwitchSourceAccessor
{
    virtual void mount(CanonPath mountPoint, ref<SourceAccessor> accessor) = 0;

    /**
     * Return the accessor mounted on `mountPoint`, or `nullptr` if
     * there is no such mount point.
     */
    std::shared_ptr<SourceAccessor> getMount(CanonPath mountPoint) override = 0;
};

ref<MountedSourceAccessor> makeMountedSourceAccessor(std::map<CanonPath, ref<SourceAccessor>> mounts);

} // namespace nix
