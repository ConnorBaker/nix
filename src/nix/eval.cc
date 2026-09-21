#include "nix/cmd/command-installable-value.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/print.hh"

#include <nlohmann/json.hpp>
#include <sstream>

namespace nix {

struct CmdEval : MixJSON, InstallableValueCommand, MixReadOnlyOption
{
    bool raw = false;
    std::optional<std::string> apply;
    std::optional<std::filesystem::path> writeTo;

    CmdEval()
        : InstallableValueCommand()
    {
        addFlag({
            .longName = "raw",
            .description = "Print strings without quotes or escaping.",
            .handler = {&raw, true},
        });

        addFlag({
            .longName = "apply",
            .description = "Apply the function *expr* to each argument.",
            .labels = {"expr"},
            .handler = {&apply},
        });

        addFlag({
            .longName = "write-to",
            .description = "Write a string or attrset of strings to *path*.",
            .labels = {"path"},
            .handler = {&writeTo},
        });
    }

    std::string description() override
    {
        return "evaluate a Nix expression";
    }

    std::string doc() override
    {
        return
#include "eval.md"
            ;
    }

    Category category() override
    {
        return catSecondary;
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        if (raw && json)
            throw UsageError("--raw and --json are mutually exclusive");

        auto state = getEvalState();

        auto [v, pos] = installable->toValue(*state, AutoCall::No);
        NixStringContext context;

        if (apply) {
            auto vApply = state->allocValue();
            state->eval(state->parseExprFromString(*apply, state->rootPath(".")), *vApply);
            auto vRes = state->allocValue();
            state->callFunction(*vApply, *v, *vRes, noPos);
            v = vRes;
        }

        if (writeTo) {
            logger->stop();

            if (pathExists(*writeTo))
                throw Error("path '%s' already exists", writeTo->string());

            [&](this const auto & recurse, Value & v, const PosIdx pos, const std::filesystem::path & path) -> void {
                state->forceValue(v, pos);
                if (v.type() == nString) {
                    auto bytes = state->emit(v, &context);
                    writeFile(path, bytes.view());
                } else if (v.type() == nAttrs) {
                    [[maybe_unused]] bool directoryCreated = std::filesystem::create_directory(path);
                    // Directory should not already exist
                    assert(directoryCreated);
                    for (auto & attr : *v.attrs()) {
                        std::string_view name = state->symbols[attr.name];
                        try {
                            if (name == "." || name == "..")
                                throw Error("invalid file name '%s'", name);
                            recurse(*attr.value, attr.pos, path / name);
                        } catch (Error & e) {
                            e.addTrace(
                                state->positions[attr.pos], HintFmt("while evaluating the attribute '%s'", name));
                            throw;
                        }
                    }
                } else
                    state->error<TypeError>("value at '%s' is not a string or an attribute set", state->positions[pos])
                        .debugThrow();
            }(*v, pos, *writeTo);
        }

        else if (raw) {
            logger->stop();
            auto string = state->coerceAndEmit(noPos, *v, context, "while generating the eval command output");
            writeFull(getStandardOutput(), string.view());
        }

        else if (json) {
            /* As `printJSON`, with the bytes passing through the realising accessor. */
            auto j = printValueAsJSON(*state, true, *v, pos, context, false);
            auto suspension = logger->suspend();
            logger->writeToStdout(state->emit(outputPretty ? j.dump(2) : j.dump(), context).view());
        }

        else {
            /* Rendered first, the printer accumulating the provenance of what
               it prints; the rendering then leaves through `emit`, which
               copies the inputs it names before any of it is shown. */
            std::ostringstream out;
            printValue(*state, out, *v, PrintOptions{.force = true, .derivationPaths = true}, &context);
            logger->cout("%s", state->emit(out.str(), context).view());
        }
    }
};

static auto rCmdEval = registerCommand<CmdEval>("eval");

} // namespace nix
