#include "nix/cmd/command.hh"
#include "command-register.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"

namespace nix {

struct CmdOptimiseStore : StoreCommand
{
    std::string description() override
    {
        return "replace identical files in the store by hard links";
    }

    std::string doc() override
    {
        return
#include "optimise-store.md"
            ;
    }

    void run(ref<Store> store) override
    {
        store->optimiseStore();
    }
};

NIX_REGISTER_COMMAND(CmdOptimiseStore, "store", "optimise");

} // namespace nix
