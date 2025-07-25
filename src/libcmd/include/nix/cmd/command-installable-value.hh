#pragma once
///@file

#include "nix/cmd/installable-value.hh"
#include "nix/cmd/command.hh"

namespace nix {

/**
 * An InstallableCommand where the single positional argument must be an
 * InstallableValue in particular.
 */
struct InstallableValueCommand : InstallableCommand
{
    /**
     * Entry point to this command
     */
    virtual void run(ref<Store> store, ref<InstallableValue> installable) = 0;

    void run(ref<Store> store, ref<Installable> installable) override;
};

} // namespace nix
