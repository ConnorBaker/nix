#include "nix/cmd/command.hh"
#include "command-register.hh"

namespace nix {

struct CmdNar : NixMultiCommand
{
    CmdNar()
        : NixMultiCommand("nar", RegisterCommand::getCommandsFor({"nar"}))
    {
    }

    std::string description() override
    {
        return "create or inspect NAR files";
    }

    std::string doc() override
    {
        return
#include "nar.md"
            ;
    }

    Category category() override
    {
        return catUtility;
    }
};

NIX_REGISTER_COMMAND(CmdNar, "nar");

} // namespace nix
