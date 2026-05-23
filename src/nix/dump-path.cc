#include "nix/cmd/command.hh"
#include "command-register.hh"
#include "run-nar-dump.hh"
#include "nix/store/store-api.hh"

namespace nix {

struct CmdDumpPath : StorePathCommand
{
    std::string description() override
    {
        return "serialise a store path to stdout in NAR format";
    }

    std::string doc() override
    {
        return
#include "store-dump-path.md"
            ;
    }

    void run(ref<Store> store, const StorePath & storePath) override
    {
        runNarDump([&](Sink & sink) { store->narFromPath(storePath, sink); }, /* checkTTY = */ true);
    }
};

NIX_REGISTER_COMMAND(CmdDumpPath, "store", "dump-path");

struct CmdDumpPath2 : Command
{
    std::filesystem::path path;

    CmdDumpPath2()
    {
        expectArgs({.label = "path", .handler = {&path}, .completer = completePath});
    }

    std::string description() override
    {
        return "serialise a path to stdout in NAR format";
    }

    std::string doc() override
    {
        return
#include "nar-dump-path.md"
            ;
    }

    void run() override
    {
        runNarDump([&](Sink & sink) { dumpPath(path, sink); }, /* checkTTY = */ true);
    }
};

struct CmdNarDumpPath : CmdDumpPath2
{
    void run() override
    {
        warn("'nix nar dump-path' is a deprecated alias for 'nix nar pack'");
        CmdDumpPath2::run();
    }
};

NIX_REGISTER_COMMAND(CmdDumpPath2, "nar", "pack");
NIX_REGISTER_COMMAND(CmdNarDumpPath, "nar", "dump-path");

} // namespace nix
