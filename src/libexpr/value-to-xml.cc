#include "nix/expr/value-to-xml.hh"
#include "nix/util/finally.hh"
#include "nix/util/xml-writer.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/util/signals.hh"

#include <cstdlib>

namespace nix {

/**
 * Automatic parallel pre-forcing for strict whole-structure XML
 * serialization. This mirrors the `parallelForceDeep` used for
 * `builtins.toJSON` / `nix eval --json` (see `value-to-json.cc`).
 *
 * It force-evaluates `v` and then fans out per-child deep-force work
 * onto the evaluator's `Executor`, so that the (serial) serialization
 * pass in `printValueAsXML` finds values already in WHNF (or being
 * computed concurrently, in which case it blocks on the normal forcing
 * protocol) instead of forcing them one at a time.
 *
 * Only the *forcing* is parallelized; serialization stays strictly
 * serial, which is what keeps the `<derivation>`/`drvsSeen` handling,
 * the attribute ordering (`lexicographicOrder`), the error traces and
 * the output bytes identical to the serial path. Work is
 * fire-and-forget (no `Completion`): any exception thrown while
 * force-evaluating here is swallowed, and the serial pass re-forces the
 * value and surfaces the real diagnostic.
 *
 * Fan-out stops at derivations / `__toString`-coercible attrsets (leaf
 * nodes). The serial XML pass does descend into a derivation's
 * attributes (`showAttrs`), so stopping here only means the serial pass
 * forces those itself: the set of values force-evaluated here is a
 * subset of those the serial pass forces, which keeps the output
 * byte-identical. (Lists are forced shallowly only; like the toJSON
 * port we do not fan out list elements -- the serial pass forces them.)
 * The recursion is expressed via spawned work items rather than C++
 * recursion, so each call's own stack use is shallow.
 *
 * Runs only off a worker thread (`!amWorkerThread`); nested
 * `xmlParallelForceDeep` invocations (which execute *on* worker threads)
 * keep spawning child work directly. (Named distinctly from the toJSON
 * `parallelForceDeep` so the two internal-linkage helpers do not collide
 * when a unity build groups `value-to-json.cc` and `value-to-xml.cc` into
 * one translation unit.)
 */
static void xmlParallelForceDeep(EvalState & state, Value & v, PosIdx pos)
{
    state.forceValue(v, pos);

    Executor::WorkItems work;

    /* Only attrsets fan out; everything else (including lists) is left
       to the serial pass. We deliberately avoid a `switch` on
       `v.type()` here so we don't have to enumerate every `ValueType`
       (`-Werror=switch-enum`). */
    if (v.type() == nAttrs
        /* Leaf: `__toString`-coercible attrset. */
        && !v.attrs()->get(state.s.toString)
        /* Leaf: derivation-like attrset carrying `outPath`. */
        && !v.attrs()->get(state.s.outPath)) {
        for (auto & a : *v.attrs())
            work.emplace_back(
                [rv(allocRootValue(a.value)), pos(a.pos), &state]() { xmlParallelForceDeep(state, **rv, pos); },
                uint8_t(0));
    }

    state.executor->spawn(std::move(work));
}

static XMLAttrs singletonAttrs(const std::string & name, std::string_view value)
{
    XMLAttrs attrs;
    attrs[name] = value;
    return attrs;
}

static void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen,
    const PosIdx pos);

/* Whether the current render unforces each element (see the public
   `printValueAsXML`). */
static thread_local bool unforceElements = false;

static void posToXML(EvalState & state, XMLAttrs & xmlAttrs, const Pos & pos)
{
    if (auto path = std::get_if<SourcePath>(&pos.origin))
        xmlAttrs["path"] = path->path.abs();
    xmlAttrs["line"] = fmt("%1%", pos.line);
    xmlAttrs["column"] = fmt("%1%", pos.column);
}

static void showAttrs(
    EvalState & state,
    bool strict,
    bool location,
    const Bindings & attrs,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen)
{
    StringSet names;

    UnforceBatch batch(state, unforceElements);
    for (auto & a : attrs.lexicographicOrder(state.symbols)) {
        XMLAttrs xmlAttrs;
        xmlAttrs["name"] = state.symbols[a->name];
        if (location && a->pos)
            posToXML(state, xmlAttrs, state.positions[a->pos]);

        XMLOpenElement _(doc, "attr", xmlAttrs);
        UnforceBatch::Element element(batch);
        printValueAsXML(state, strict, location, *a->value, doc, context, drvsSeen, a->pos);
    }
}

static void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen,
    const PosIdx pos)
{
    checkInterrupt();

    auto _level = state.addCallDepth(pos);

    if (strict)
        state.forceValue(v, pos);

    switch (v.type()) {

    case nInt:
        doc.writeEmptyElement("int", singletonAttrs("value", fmt("%1%", v.integer())));
        break;

    case nBool:
        doc.writeEmptyElement("bool", singletonAttrs("value", v.boolean() ? "true" : "false"));
        break;

    case nString:
        /* !!! show the context? */
        copyContext(v, context);
        doc.writeEmptyElement("string", singletonAttrs("value", v.string_view()));
        break;

    case nPath:
        doc.writeEmptyElement("path", singletonAttrs("value", v.path().to_string()));
        break;

    case nNull:
        doc.writeEmptyElement("null");
        break;

    case nAttrs:
        if (state.isDerivation(v)) {
            XMLAttrs xmlAttrs;

            std::string drvPath;
            if (auto a = v.attrs()->get(state.s.drvPath)) {
                if (strict)
                    state.forceValue(*a->value, a->pos);
                if (a->value->type() == nString)
                    xmlAttrs["drvPath"] = drvPath = a->value->string_view();
            }

            if (auto a = v.attrs()->get(state.s.outPath)) {
                if (strict)
                    state.forceValue(*a->value, a->pos);
                if (a->value->type() == nString)
                    xmlAttrs["outPath"] = a->value->string_view();
            }

            XMLOpenElement _(doc, "derivation", xmlAttrs);

            if (drvPath != "" && drvsSeen.insert(drvPath).second)
                showAttrs(state, strict, location, *v.attrs(), doc, context, drvsSeen);
            else
                doc.writeEmptyElement("repeated");
        }

        else {
            XMLOpenElement _(doc, "attrs");
            showAttrs(state, strict, location, *v.attrs(), doc, context, drvsSeen);
        }

        break;

    case nList: {
        XMLOpenElement _(doc, "list");
        UnforceBatch batch(state, unforceElements);
        for (auto v2 : v.listView()) {
            UnforceBatch::Element element(batch);
            printValueAsXML(state, strict, location, *v2, doc, context, drvsSeen, pos);
        }
        break;
    }

    case nFunction: {
        if (!v.isLambda()) {
            // FIXME: Serialize primops and primopapps
            doc.writeEmptyElement("unevaluated");
            break;
        }
        XMLAttrs xmlAttrs;
        if (location)
            posToXML(state, xmlAttrs, state.positions[v.lambda().fun->pos]);
        XMLOpenElement _(doc, "function", xmlAttrs);

        if (auto formals = v.lambda().fun->getFormals()) {
            XMLAttrs attrs;
            if (v.lambda().fun->arg)
                attrs["name"] = state.symbols[v.lambda().fun->arg];
            if (formals->ellipsis)
                attrs["ellipsis"] = "1";
            XMLOpenElement _(doc, "attrspat", attrs);
            for (auto & i : formals->lexicographicOrder(state.symbols))
                doc.writeEmptyElement("attr", singletonAttrs("name", state.symbols[i.name]));
        } else
            doc.writeEmptyElement("varpat", singletonAttrs("name", state.symbols[v.lambda().fun->arg]));

        break;
    }

    case nExternal:
        v.external()->printValueAsXML(state, strict, location, doc, context, drvsSeen, pos);
        break;

    case nFloat:
        doc.writeEmptyElement("float", singletonAttrs("value", fmt("%1%", v.fpoint())));
        break;

    case nThunk:
    // Historically, a tried and then ignored value (e.g. through tryEval) was
    // reverted to the original thunk.
    case nFailed:
        doc.writeEmptyElement("unevaluated");
        break;
    }
}

void ExternalValueBase::printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen,
    const PosIdx pos) const
{
    doc.writeEmptyElement("unevaluated");
}

void printValueAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    std::ostream & out,
    NixStringContext & context,
    const PosIdx pos,
    bool unforce)
{
    /* Auto-parallel pre-force of the whole structure before the serial
       serialization pass. Guarded on strictness (lazy serialization
       must not force), the executor being enabled (eval-cores > 1 under
       the parallel-eval feature), and not already running on a worker
       thread. Byte-identity is preserved because only forcing is
       parallelized; serialization below remains serial. */
    if (strict && state.executor->enabled && !Executor::amWorkerThread)
        xmlParallelForceDeep(state, v, pos);

    auto savedUnforce = std::exchange(unforceElements, unforce);
    Finally restoreUnforce([&] { unforceElements = savedUnforce; });
    XMLWriter doc(true, out);
    XMLOpenElement root(doc, "expr");
    StringSet drvsSeen;
    printValueAsXML(state, strict, location, v, doc, context, drvsSeen, pos);
}

} // namespace nix
