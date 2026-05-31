#include "nix/cmd/command-installable-value.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/value-to-json.hh"

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

        auto [v, pos] = installable->toValue(*state);
        NixStringContext context;

        if (apply) {
            auto vApply = state->allocValue();
            state->eval(state->parseExprFromString(*apply, state->rootPath(".")), *vApply);
            auto vRes = state->allocValue();
            state->callFunction(*vApply, *v, *vRes, noPos);
            v = vRes;
        }

        /* Boundary cover-fix (Item 1, audit per
           src/libexpr-tests/boundary-audit-*): every CLI output
           shape that serialises a `Value` to text must rewrite
           `SourceVirtual` placeholders to real storePaths *before*
           the bytes leave the process. The pattern:

             1. Build the text (collecting `SourceVirtual` elems
                into `context` along the way).
             2. `resolveSourceVirtual(context)` materialises each
                `SourceVirtual` placeholder via the
                `MaterialisationScheduler` and returns the rewrite
                map. We **do not** call `realiseContext` here:
                that would build any `Built` drv-path elements in
                the context (IFD), which `nix eval`/`nix-instantiate
                --eval` historically does not do.
             3. `ensureLazyPathsCopied(context)` covers any
                `Opaque` lazy-store paths left in the context.
             4. `rewriteStrings(text, rewrites)` substitutes the
                placeholder render text → real storePath in the
                serialised output.

           Without this rewrite, a `SourceVirtual`-bearing string
           leaks the placeholder render `/<base32>` into stdout
           verbatim. */

        if (writeTo) {
            logger->stop();

            if (pathExists(*writeTo))
                throw Error("path '%s' already exists", writeTo->string());

            /* For `--write-to`, each leaf string's body is written
               to a file. We need the rewrite map up front so each
               file write gets a rewritten body. The recursion
               accumulates context as it walks; we resolve once at
               the top after the walk completes. */
            std::vector<std::pair<std::filesystem::path, std::string>> pendingWrites;
            std::vector<std::filesystem::path> pendingDirs;
            [&](this const auto & recurse, Value & v, const PosIdx pos, const std::filesystem::path & path) -> void {
                state->forceValue(v, pos);
                if (v.type() == nString) {
                    copyContext(v, context);
                    pendingWrites.emplace_back(path, std::string(v.string_view()));
                } else if (v.type() == nAttrs) {
                    pendingDirs.push_back(path);
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

            auto rewrites = state->resolveSourceVirtualContext(context);
            state->ensureLazyPathsCopied(context);

            for (auto & dir : pendingDirs) {
                [[maybe_unused]] bool directoryCreated = std::filesystem::create_directory(dir);
                // Directory should not already exist
                assert(directoryCreated);
            }
            for (auto & [path, body] : pendingWrites) {
                writeFile(path, rewriteStrings(body, rewrites));
            }
        }

        else if (raw) {
            logger->stop();
            auto string = state->coerceToString(noPos, *v, context, "while generating the eval command output");
            auto rewrites = state->resolveSourceVirtualContext(context);
            state->ensureLazyPathsCopied(context);
            writeFull(getStandardOutput(), rewriteStrings(std::string(*string), rewrites));
        }

        else if (json) {
            auto j = printValueAsJSON(*state, true, *v, pos, context, false);
            auto rewrites = state->resolveSourceVirtualContext(context);
            state->ensureLazyPathsCopied(context);
            if (rewrites.empty()) {
                printJSON(j);
            } else {
                /* Dump-and-rewrite avoids parsing the JSON back. The
                   rewrite is safe on the JSON-serialised form because
                   placeholder render strings (`/<base32>`) cannot
                   accidentally collide with JSON syntax tokens or
                   non-string field bodies. */
                auto suspension = logger->suspend();
                logger->writeToStdout(rewriteStrings(j.dump(outputPretty ? 2 : -1), rewrites));
            }
        }

        else {
            std::ostringstream oss;
            {
                ValuePrinter printer(*state, *v, PrintOptions{.force = true, .derivationPaths = true}, &context);
                oss << printer;
            }
            auto rewrites = state->resolveSourceVirtualContext(context);
            state->ensureLazyPathsCopied(context);
            logger->cout("%s", rewriteStrings(oss.str(), rewrites));
        }
    }
};

static auto rCmdEval = registerCommand<CmdEval>("eval");

} // namespace nix
