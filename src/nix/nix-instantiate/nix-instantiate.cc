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

/**
 * PROTOTYPE. `--strict` output with unforcing. The top-level elements are
 * forced deeply in `forceValueDeep`'s order and with its traces, so errors
 * are the same as without unforcing, and printed in batches: before a batch
 * ends (unforcing what its elements forced), its elements are printed, in
 * printing order, into buffers that are assembled at the end. Containers
 * printed in one batch are kept when it ends, with their addresses in `seen`
 * rewritten (`UnforceSeen`), so that one reached again in a later batch
 * prints as «repeated», as without unforcing (with the mimalloc backend it
 * prints in full instead).
 *
 * A top-level attribute set is forced, and so printed, in its Bindings
 * order, but printed out in lexicographic order. Which occurrence of a
 * shared container is printed in full depends on that order, so the
 * attributes' texts are recorded with where each container occurs
 * (`StrictAttrsAssembler`), and the choice is made again when they are
 * assembled.
 */
/**
 * PROTOTYPE. The parts of a printed value (`printStrictUnforced`'s
 * attributes), each printed in some order with one `seen` set, and
 * assembled in another as if printed in that order: every attribute set and
 * list that `seen` tracks is printed in full where it first occurs in the
 * assembled order and as «repeated» everywhere else, which is what
 * `printAmbiguous` on the whole value prints. Every container has exactly one
 * full rendering among the parts (the first in printing order), whose text
 * is reused wherever the assembled order needs it.
 *
 * Containers are identified by ids rather than addresses: `rekey` follows an
 * address rewritten by `UnforceSeen`, and `forget` drops every address when
 * `seen` is cleared (the mimalloc backend), since freed addresses are reused.
 */
struct StrictAttrsAssembler : PrintAmbiguousRecorder
{
    struct Node
    {
        size_t id;
        bool full;
        std::streamoff from, to;
        std::vector<size_t> children;
    };

    struct Part
    {
        std::string text;
        std::vector<Node> nodes;
        std::vector<size_t> roots;
    };

    std::vector<Part> parts;
    std::map<const void *, size_t> ids;
    /* Per id, its full rendering: (part, node). */
    std::vector<std::pair<size_t, size_t>> fullOf;
    std::vector<size_t> open_;
    std::optional<size_t> topId;

    size_t idOf(const void * key)
    {
        auto [i, fresh] = ids.try_emplace(key, fullOf.size());
        if (fresh)
            fullOf.emplace_back(SIZE_MAX, SIZE_MAX);
        return i->second;
    }

    void rekey(const void * from, const void * to)
    {
        auto i = ids.find(from);
        if (i == ids.end())
            return;
        auto id = i->second;
        ids.erase(i);
        ids[to] = id;
    }

    /* The top level, which `seen` always holds (it is never freed). */
    const void * topKey = nullptr;

    void forget()
    {
        ids.clear();
        if (topId)
            ids[topKey] = *topId;
    }

    void seedTop(const void * top)
    {
        if (top) {
            topKey = top;
            topId = idOf(top);
        }
    }

    size_t attach(Node && n)
    {
        auto & p = parts.back();
        p.nodes.push_back(std::move(n));
        auto i = p.nodes.size() - 1;
        (open_.empty() ? p.roots : p.nodes[open_.back()].children).push_back(i);
        return i;
    }

    void open(const void * key, std::streamoff at) override
    {
        auto id = idOf(key);
        auto i = attach({id, true, at, at, {}});
        fullOf[id] = {parts.size() - 1, i};
        open_.push_back(i);
    }

    void close(std::streamoff at) override
    {
        parts.back().nodes[open_.back()].to = at;
        open_.pop_back();
    }

    void repeated(const void * key, std::streamoff from, std::streamoff to) override
    {
        attach({idOf(key), false, from, to, {}});
    }

    /** Print `v` as a new part; returns its index. */
    size_t print(
        EvalState & state, Value & v, std::set<const void *> & seen, NixStringContext & context, size_t depth)
    {
        parts.emplace_back();
        std::ostringstream str;
        printAmbiguous(state, v, str, &seen, &context, depth, this);
        parts.back().text = str.str();
        return parts.size() - 1;
    }

    std::set<size_t> emitted;

    void emitSpan(std::ostream & out, const Part & p, std::streamoff from, std::streamoff to, const std::vector<size_t> & kids)
    {
        auto pos = from;
        for (auto k : kids) {
            auto & n = p.nodes[k];
            out.write(p.text.data() + pos, n.from - pos);
            emitNode(out, p, n);
            pos = n.to;
        }
        out.write(p.text.data() + pos, to - pos);
    }

    void emitNode(std::ostream & out, const Part & p, const Node & n)
    {
        if (!emitted.insert(n.id).second) {
            out << "«repeated»";
            return;
        }
        if (n.full) {
            emitSpan(out, p, n.from, n.to, n.children);
            return;
        }
        auto [pi, ni] = fullOf.at(n.id);
        assert(pi != SIZE_MAX);
        auto & fp = parts.at(pi);
        auto & fn = fp.nodes.at(ni);
        emitSpan(out, fp, fn.from, fn.to, fn.children);
    }

    /** Write part `i`; the parts must be written in the assembled order. */
    void emit(std::ostream & out, size_t i)
    {
        if (topId && emitted.empty())
            emitted.insert(*topId);
        auto & p = parts.at(i);
        emitSpan(out, p, 0, p.text.size(), p.roots);
    }
};

static void printStrictUnforced(EvalState & state, Value & v, std::ostream & out, NixStringContext & context)
{
    /* As `forceValueDeep`'s outermost level, so that call depth (and with it
       where a stack overflow is reported) is the same. */
    auto _level = state.addCallDepth(v.determinePos(noPos));
    state.forceValue(v, v.determinePos(noPos));
    if (v.type() != nAttrs && !v.isList()) {
        std::set<const void *> seen;
        printAmbiguous(state, v, out, &seen, &context);
        return;
    }

    /* As `printAmbiguous` on the whole value, which marks the top level as
       seen before printing its elements, so that an element referring back
       to it prints as «repeated». The top level was forced before any batch
       opened, so no batch frees it: it stays seen. */
    const void * top = v.type() == nAttrs ? (v.attrs()->empty() ? nullptr : (const void *) v.attrs())
                                          : (v.listSize() ? (const void *) &v : nullptr);
    std::set<const void *> seen;
    auto seedSeen = [&]() {
        if (top)
            seen.insert(top);
    };
    seedSeen();
    StrictAttrsAssembler assembler;
    assembler.seedTop(top);
    UnforceSeen keepSeen(seen);
    keepSeen.onRekey = [&](const void * from, const void * to) { assembler.rekey(from, to); };
    UnforceBatch batch(state);
    batch.onEnd = [&](bool freed) {
        if (freed && !unforceSeenExact()) {
            seen.clear();
            seedSeen();
            assembler.forget();
        }
    };

    if (v.type() == nAttrs) {
        auto sorted = v.attrs()->lexicographicOrder(state.symbols);
        std::map<const Attr *, size_t> part;
        std::vector<const Attr *> pending;
        auto printPending = [&]() {
            for (auto a : pending)
                part[a] = assembler.print(state, *a->value, seen, context, 1);
            pending.clear();
        };
        for (auto & i : *v.attrs()) {
            batch.open();
            try {
                state.forceValueDeep(*i.value);
            } catch (Error & e) {
                /* The trace `forceValueDeep` adds. */
                e.addTrace(
                    state.positions[i.pos], HintFmt("while evaluating the attribute '%1%'", state.symbols[i.name]));
                throw;
            }
            pending.push_back(&i);
            if (batch.full()) {
                printPending();
                batch.end();
            }
        }
        printPending();
        batch.end();
        out << "{ ";
        for (auto i : sorted) {
            out << state.symbols[i->name] << " = ";
            assembler.emit(out, part.at(i));
            out << "; ";
        }
        out << "}";
    } else {
        out << "[ ";
        size_t index = 0;
        for (auto v2 : v.listView()) {
            batch.open();
            try {
                state.forceValueDeep(*v2);
            } catch (Error & e) {
                e.addTrace(nullptr, HintFmt("while evaluating list element at index %1%", index));
                throw;
            }
            printAmbiguous(state, *v2, out, &seen, &context, 1);
            out << " ";
            index++;
            if (batch.full())
                batch.end();
        }
        batch.end();
        out << "]";
    }
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
                    /* It forces while printing: print nothing if forcing
                       fails, as `forceValueDeep` before printing does. */
                    std::ostringstream out;
                    printStrictUnforced(state, vRes, out, context);
                    std::cout << out.str();
                } else {
                    if (strict)
                        state.forceValueDeep(vRes);
                    std::set<const void *> seen;
                    printAmbiguous(state, vRes, std::cout, &seen, &context);
                }
                std::cout << std::endl;
            }
        } else {
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
