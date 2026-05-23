#include "nix/cmd/command.hh"
#include "command-register.hh"

namespace nix {

struct CmdStore : NixMultiCommand
{
    CmdStore()
        : NixMultiCommand("store", RegisterCommand::getCommandsFor({"store"}))
    {
        aliases = {
            {"ping", {AliasStatus::Deprecated, {"info"}}},
        };
    }

    std::string description() override
    {
        return "manipulate a Nix store";
    }

    Category category() override
    {
        return catUtility;
    }
};

NIX_REGISTER_COMMAND(CmdStore, "store");

} // namespace nix
