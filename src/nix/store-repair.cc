#include "nix/cmd/command.hh"
#include "command-register.hh"
#include "nix/store/store-api.hh"

namespace nix {

struct CmdStoreRepair : StorePathsCommand
{
    std::string description() override
    {
        return "repair store paths";
    }

    std::string doc() override
    {
        return
#include "store-repair.md"
            ;
    }

    void run(ref<Store> store, StorePaths && storePaths) override
    {
        for (auto & path : storePaths)
            store->repairPath(path);
    }
};

NIX_REGISTER_COMMAND(CmdStoreRepair, "store", "repair");

} // namespace nix
