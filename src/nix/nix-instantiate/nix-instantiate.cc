#include "nix/store/globals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/main/shared.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/value-to-xml.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/store/store-open.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/cmd/legacy.hh"
#include "man-pages.hh"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>

namespace nix {

std::filesystem::path gcRoot;
static int rootNr = 0;

enum OutputKind { okPlain, okRaw, okXML, okJSON };

/* `printStrictUnforced` where walked cells cannot be kept: each batch's
   elements are printed before it ends (see below). */
[[gnu::noinline]] static void printStrictUnforcedBuffered(
    EvalState & state, Value & v, std::ostream & out, NixStringContext & context, std::set<const void *> & walked)
{
    /* Walk element `e` (with `forceValueDeep`'s trace for it). */
    auto forceElement = [&](Value & e, auto && trace) {
        try {
            state.forceValueDeep(e, walked);
        } catch (Error & err) {
            trace(err);
            throw;
        }
    };

    /* As `printAmbiguous` on the whole value, which marks the top level as
       seen before printing its elements, so that an element referring back
       to it prints as «repeated». The top level was forced before any batch
       opened, so no batch frees it: it stays seen. */
    std::ostringstream buffer;
    const void * top = v.type() == nAttrs ? (v.attrs()->empty() ? nullptr : (const void *) v.attrs())
                                          : (v.listSize() ? (const void *) &v : nullptr);
    std::set<const void *> seen;
    auto seedSeen = [&]() {
        if (top)
            seen.insert(top);
    };
    seedSeen();
    UnforceBatch batch(state);
    batch.onEnd = [&](bool freed) {
        if (freed) {
            seen.clear();
            seedSeen();
            walked.clear();
            walked.insert(&v);
        }
    };

    if (v.type() == nAttrs) {
        auto sorted = v.attrs()->lexicographicOrder(state.symbols);
        std::map<const Attr *, size_t> rank;
        for (auto [n, a] : enumerate(sorted))
            rank[a] = n;
        std::map<const Attr *, std::string> printed;
        std::vector<const Attr *> pending;
        auto printPending = [&]() {
            std::sort(pending.begin(), pending.end(), [&](auto a, auto b) { return rank[a] < rank[b]; });
            for (auto a : pending) {
                std::ostringstream str;
                printAmbiguous(state, *a->value, str, &seen, &context, 1);
                printed[a] = str.str();
            }
            pending.clear();
        };
        for (auto & i : *v.attrs()) {
            batch.open();
            forceElement(*i.value, [&](Error & e) {
                e.addTrace(
                    state.positions[i.pos], HintFmt("while evaluating the attribute '%1%'", state.symbols[i.name]));
            });
            pending.push_back(&i);
            if (batch.full()) {
                printPending();
                batch.end();
            }
        }
        printPending();
        batch.end();
        buffer << "{ ";
        for (auto i : sorted)
            buffer << state.symbols[i->name] << " = " << printed[i] << "; ";
        buffer << "}";
    } else {
        buffer << "[ ";
        size_t index = 0;
        for (auto v2 : v.listView()) {
            batch.open();
            forceElement(*v2, [&](Error & e) {
                e.addTrace(nullptr, HintFmt("while evaluating list element at index %1%", index));
            });
            printAmbiguous(state, *v2, buffer, &seen, &context, 1);
            buffer << " ";
            index++;
            if (batch.full())
                batch.end();
        }
        batch.end();
        buffer << "]";
    }
    out << buffer.str();
}

/* The traces `forceValueDeep` adds to an error in an element, out of line:
   their formatters are large, and `printStrictUnforced`'s frame is part of
   the stack prefix (STACK-PARITY.md, R4). */
[[gnu::noinline, gnu::cold]] static void addAttrTrace(EvalState & state, Error & e, const Attr & i)
{
    e.addTrace(state.positions[i.pos], HintFmt("while evaluating the attribute '%1%'", state.symbols[i.name]));
}

[[gnu::noinline, gnu::cold]] static void addListElemTrace(Error & e, size_t index)
{
    e.addTrace(nullptr, HintFmt("while evaluating list element at index %1%", index));
}

/**
 * PROTOTYPE. `--strict` output with unforcing. As without unforcing, the
 * value is forced deeply (`forceValueDeep`'s order, traces and depth: its
 * top-level elements are walked one by one, sharing one set of walked
 * cells, so that a cell is walked once, as one `forceValueDeep` of the whole
 * value walks it) and then printed once, so nothing is printed if forcing
 * fails. The elements are forced in batches, each unforcing what its
 * elements forced except what they reach: the walked cells are kept forced
 * and their addresses valid (`UnforceSeen`), so the value printed at the end
 * is the value forced, with its sharing, and «repeated» is decided by
 * `printAmbiguous` exactly as without unforcing. Peak memory is what the
 * printed value needs (as without unforcing) plus a batch.
 *
 * Where walked cells cannot be kept (the mimalloc backend, or
 * NIX_UNFORCE_SEEN=0), each batch's elements are printed before it ends, into
 * buffers assembled at the end in printing order; a value shared between
 * batches then prints in full instead of as «repeated»
 * (`printStrictUnforcedBuffered`).
 */
[[gnu::noinline]] static void
printStrictUnforced(EvalState & state, Value & v, std::ostream & out, NixStringContext & context)
{
    /* As `forceValueDeep`'s outermost level, so that call depth (and with it
       where a stack overflow is reported) is the same. */
    auto _level = state.addCallDepth(v.determinePos(noPos));
    /* On the heap, as everything else of this function's: it is part of the
       stack prefix below every level of the deep walk, which may be no
       larger than base's (STACK-PARITY.md, R4). */
    auto walked = std::make_unique<std::set<const void *>>();
    walked->insert(&v);
    state.forceValue(v, v.determinePos(noPos));
    if ((v.type() == nAttrs || v.isList()) && !unforceSeenExact()) {
        printStrictUnforcedBuffered(state, v, out, context, *walked);
        return;
    }
    if (v.type() == nAttrs || v.isList()) {
        auto keepWalked = std::make_unique<UnforceSeen>(*walked);
        auto batch = std::make_unique<UnforceBatch>(state);
        if (v.type() == nAttrs)
            for (auto & i : *v.attrs()) {
                batch->open();
                try {
                    state.forceValueDeep(*i.value, *walked);
                } catch (Error & e) {
                    addAttrTrace(state, e, i);
                    throw;
                }
                if (batch->full())
                    batch->end();
            }
        else {
            size_t index = 0;
            for (auto v2 : v.listView()) {
                batch->open();
                try {
                    state.forceValueDeep(*v2, *walked);
                } catch (Error & e) {
                    addListElemTrace(e, index);
                    throw;
                }
                index++;
                if (batch->full())
                    batch->end();
            }
        }
        batch->end();
        batch.reset();
        keepWalked.reset();
    }
    /* As `forceValueDeep`'s set is gone before base prints. */
    walked.reset();
    std::set<const void *> seen;
    printAmbiguous(state, v, out, &seen, &context);
}

/* The derivations' paths: out of line, so that its locals are not part of
   processExpr's frame, below every level of the evaluation (STACK-PARITY.md,
   R4). */
[[gnu::noinline]] static void printDerivationPaths(EvalState & state, Value & v, const Bindings & autoArgs)
{
    /* PROTOTYPE: unforce each job once its .drv is written. */
    bool unforce = unforceEnabled(state);
    std::set<std::pair<StorePath, std::string>> seen;
    forEachDerivation(
        state,
        v,
        "",
        autoArgs,
        [&](PackageInfo & i) {
            auto drvPath = i.requireDrvPath();
            if (unforce && !seen.insert({drvPath, i.queryOutputName()}).second)
                return;
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
        },
        unforce,
        false);
}

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

    Value vRoot;
    state.eval(e, vRoot);

    for (auto & i : attrPaths) {
        Value & v(*findAlongAttrPath(state, i, autoArgs, vRoot).first);
        state.forceValue(v, v.determinePos(noPos));

        NixStringContext context;
        if (evalOnly) {
            Value vRes;
            if (autoArgs.empty())
                vRes = v;
            else
                state.autoCallFunction(autoArgs, v, vRes);
            if (output == okRaw)
                std::cout << *state.coerceToString(noPos, vRes, context, "while generating the nix-instantiate output");
            // We intentionally don't output a newline here. The default PS1 for Bash in NixOS starts with a newline
            // and other interactive shells like Zsh are smart enough to print a missing newline before the prompt.
            else if (output == okXML)
                printValueAsXML(state, strict, location, vRes, std::cout, context, noPos, strict);
            else if (output == okJSON) {
                printValueAsJSON(state, strict, vRes, v.determinePos(noPos), std::cout, context, true, strict);
                std::cout << std::endl;
            } else {
                if (strict && unforceEnabled(state)) {
                    /* It prints nothing if forcing fails, as
                       `forceValueDeep` before printing does. */
                    printStrictUnforced(state, vRes, std::cout, context);
                } else {
                    if (strict)
                        state.forceValueDeep(vRes);
                    std::set<const void *> seen;
                    printAmbiguous(state, vRes, std::cout, &seen, &context);
                }
                std::cout << std::endl;
            }
        } else {
            printDerivationPaths(state, v, autoArgs);
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
