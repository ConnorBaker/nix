#include "nix/cmd/command.hh"
#include "command-register.hh"

namespace nix {

struct CmdDerivation : NixMultiCommand
{
    CmdDerivation()
        : NixMultiCommand("derivation", RegisterCommand::getCommandsFor({"derivation"}))
    {
    }

    std::string description() override
    {
        return "Work with derivations, Nix's notion of a build plan.";
    }

    Category category() override
    {
        return catUtility;
    }
};

NIX_REGISTER_COMMAND(CmdDerivation, "derivation");

} // namespace nix
