#include "nix/store/globals.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/main/shared.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/value-to-xml.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/store/store-open.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/cmd/legacy.hh"
#include "man-pages.hh"

#include <iostream>
#include <optional>

namespace nix {

std::filesystem::path gcRoot;
static int rootNr = 0;

enum OutputKind { okPlain, okRaw, okXML, okJSON };

void processExpr(
    EvalState & state,
    const Strings & attrPaths,
    bool parseOnly,
    bool strict,
    const Bindings & autoArgs,
    bool evalOnly,
    OutputKind output,
    bool location,
    Expr * e)
{
    if (parseOnly) {
        e->show(state.symbols, std::cout);
        std::cout << "\n";
        return;
    }

    /* The root and each attribute path are evaluated in a job of their own,
       as a traversal's elements are. The job keeps all its memory: they are
       written outside any cell's force, where its end would not see them. */
    Value vRoot;
    {
        UnforceJobs jobs(state);
        jobs.element();
        unforceKeepJob();
        state.eval(e, vRoot);
    }

    for (auto & i : attrPaths) {
        Value * pv;
        {
            UnforceJobs jobs(state);
            jobs.element();
            unforceKeepJob();
            pv = findAlongAttrPath(state, i, autoArgs, vRoot).first;
            state.forceValue(*pv, pv->determinePos(noPos));
        }
        Value & v(*pv);

        NixStringContext context;
        if (evalOnly) {
            Value vRes;
            if (autoArgs.empty())
                vRes = v;
            else
                state.autoCallFunction(autoArgs, v, vRes);
            /* A traversal runs its elements in jobs only if it forces what
               it reads: without --strict, the printer reads cells as they
               are, which an earlier element's job end may have reverted. */
            std::optional<UnforceJobs> jobs;
            if (strict && (output == okXML || output == okJSON))
                jobs.emplace(state);
            if (output == okRaw)
                std::cout << *state.coerceToString(noPos, vRes, context, "while generating the nix-instantiate output");
            // We intentionally don't output a newline here. The default PS1 for Bash in NixOS starts with a newline
            // and other interactive shells like Zsh are smart enough to print a missing newline before the prompt.
            else if (output == okXML)
                printValueAsXML(state, strict, location, vRes, std::cout, context, noPos, jobs.has_value());
            else if (output == okJSON) {
                printValueAsJSON(
                    state, strict, vRes, v.determinePos(noPos), std::cout, context, true, jobs.has_value());
                std::cout << std::endl;
            } else {
                if (strict)
                    state.forceValueDeep(vRes, true);
                std::set<const void *> seen;
                printAmbiguous(state, vRes, std::cout, &seen, &context);
                std::cout << std::endl;
            }
        } else {
            PackageInfos drvs;
            getDerivations(state, v, "", autoArgs, drvs, false);
            UnforceJobs jobs(state);
            for (auto & i : drvs) {
                jobs.element();
                auto drvPath = i.requireDrvPath();
                auto drvPathS = state.store->printStorePath(drvPath);

                /* What output do we want? */
                std::string outputName = i.queryOutputName();
                if (outputName == "")
                    throw Error("derivation '%1%' lacks an 'outputName' attribute", drvPathS);

                if (gcRoot.empty())
                    printGCWarning();
                else {
                    auto rootName = absPath(gcRoot);
                    if (++rootNr > 1)
                        rootName += "-" + std::to_string(rootNr);
                    auto store2 = state.store.dynamic_pointer_cast<LocalFSStore>();
                    if (store2)
                        drvPathS = store2->addPermRoot(drvPath, rootName).string();
                }
                std::cout << fmt("%s%s\n", drvPathS, (outputName != "out" ? "!" + outputName : ""));
            }
        }

        state.ensureLazyPathsCopied(context);
    }
}

static int main_nix_instantiate(int argc, char ** argv)
{
    {
        Strings files;
        bool readStdin = false;
        bool fromArgs = false;
        bool findFile = false;
        bool evalOnly = false;
        bool parseOnly = false;
        OutputKind outputKind = okPlain;
        bool xmlOutputSourceLocation = true;
        bool strict = false;
        Strings attrPaths;
        bool wantsReadWrite = false;

        struct MyArgs : LegacyArgs, MixEvalArgs
        {
            using LegacyArgs::LegacyArgs;
        };

        MyArgs myArgs(std::string(baseNameOf(argv[0])), [&](Strings::iterator & arg, const Strings::iterator & end) {
            if (*arg == "--help")
                showManPage("nix-instantiate");
            else if (*arg == "--version")
                printVersion("nix-instantiate");
            else if (*arg == "-")
                readStdin = true;
            else if (*arg == "--expr" || *arg == "-E")
                fromArgs = true;
            else if (*arg == "--eval" || *arg == "--eval-only")
                evalOnly = true;
            else if (*arg == "--read-write-mode")
                wantsReadWrite = true;
            else if (*arg == "--parse" || *arg == "--parse-only")
                parseOnly = evalOnly = true;
            else if (*arg == "--find-file")
                findFile = true;
            else if (*arg == "--attr" || *arg == "-A")
                attrPaths.push_back(getArg(*arg, arg, end));
            else if (*arg == "--add-root")
                gcRoot = getArg(*arg, arg, end);
            else if (*arg == "--indirect")
                ;
            else if (*arg == "--raw")
                outputKind = okRaw;
            else if (*arg == "--xml")
                outputKind = okXML;
            else if (*arg == "--json")
                outputKind = okJSON;
            else if (*arg == "--no-location")
                xmlOutputSourceLocation = false;
            else if (*arg == "--strict")
                strict = true;
            else if (*arg == "--dry-run")
                settings.readOnlyMode = true;
            else if (*arg != "" && arg->at(0) == '-')
                return false;
            else
                files.push_back(*arg);
            return true;
        });

        myArgs.parseCmdline(argvToStrings(argc, argv));

        if (evalOnly && !wantsReadWrite)
            settings.readOnlyMode = true;

        auto store = openStore();
        auto evalStore = myArgs.evalStoreUrl ? openStore(StoreReference{*myArgs.evalStoreUrl}) : store;

        auto state = std::make_shared<EvalState>(myArgs.lookupPath, evalStore, fetchSettings, evalSettings, store);
        state->repair = myArgs.repair;

        const Bindings & autoArgs = *myArgs.getAutoArgs(*state);

        if (attrPaths.empty())
            attrPaths = {""};

        if (findFile) {
            for (auto & i : files) {
                auto p = state->findFile(i);
                if (auto fn = p.getPhysicalPath())
                    std::cout << fn->string() << std::endl;
                else
                    throw Error("'%s' has no physical path", p);
            }
            return 0;
        }

        if (readStdin) {
            Expr * e = state->parseStdin();
            processExpr(
                *state, attrPaths, parseOnly, strict, autoArgs, evalOnly, outputKind, xmlOutputSourceLocation, e);
        } else if (files.empty() && !fromArgs)
            files.push_back("./default.nix");

        for (auto & i : files) {
            Expr * e = fromArgs ? state->parseExprFromString(i, state->rootPath("."))
                                : state->parseExprFromFile(resolveExprPath(lookupFileArg(*state, i)));
            processExpr(
                *state, attrPaths, parseOnly, strict, autoArgs, evalOnly, outputKind, xmlOutputSourceLocation, e);
        }

        state->maybePrintStats();

        return 0;
    }
}

static RegisterLegacyCommand r_nix_instantiate("nix-instantiate", main_nix_instantiate);

} // namespace nix
