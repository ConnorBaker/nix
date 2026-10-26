#include "nix/expr/value-to-xml.hh"
#include "nix/util/xml-writer.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/unforce.hh"
#include "nix/util/signals.hh"

#include <cstdlib>

namespace nix {

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

static void posToXML(EvalState & state, XMLAttrs & xmlAttrs, const Pos & pos)
{
    if (auto path = std::get_if<SourcePath>(&pos.origin))
        xmlAttrs["path"] = path->path.abs();
    xmlAttrs["line"] = std::to_string(pos.line);
    xmlAttrs["column"] = std::to_string(pos.column);
}

/* Out of line: the tag's attributes are not needed while the value is printed. */
[[gnu::noinline]] static void openAttrElement(EvalState & state, bool location, const Attr & a, XMLWriter & doc)
{
    XMLAttrs xmlAttrs;
    xmlAttrs["name"] = state.symbols[a.name];
    if (location && a.pos)
        posToXML(state, xmlAttrs, state.positions[a.pos]);
    doc.openElement("attr", xmlAttrs);
}

namespace {

/* Closes the element that `openAttrElement` opened. */
struct CloseAttrElement
{
    XMLWriter & doc;

    ~CloseAttrElement()
    {
        doc.closeElement();
    }
};

} // namespace

static void showAttrs(
    EvalState & state,
    bool strict,
    bool location,
    const Bindings & attrs,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen)
{
    for (auto & a : attrs.lexicographicOrder(state.symbols)) {
        openAttrElement(state, location, *a, doc);
        CloseAttrElement _{doc};
        printValueAsXML(state, strict, location, *a->value, doc, context, drvsSeen, a->pos);
    }
}

/* A derivation's element: its paths, and its attributes the first time it is seen. */
[[gnu::noinline]] static void printDerivationAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen)
{
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

/* The root of a traversal (`UnforceJobs`), each element of it in a job of
   its own. Out of line, so that the levels below have no room for it. */
[[gnu::noinline]] static void printElementsAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen,
    const PosIdx pos)
{
    if (v.type() == nAttrs) {
        XMLOpenElement _(doc, "attrs");
        for (auto & a : v.attrs()->lexicographicOrder(state.symbols)) {
            unforceElement(state, v);
            openAttrElement(state, location, *a, doc);
            CloseAttrElement _{doc};
            printValueAsXML(state, strict, location, *a->value, doc, context, drvsSeen, a->pos);
        }
    } else {
        XMLOpenElement _(doc, "list");
        for (auto v2 : v.listView()) {
            unforceElement(state, v);
            printValueAsXML(state, strict, location, *v2, doc, context, drvsSeen, pos);
        }
    }
}

/* printValueAsXML of a value that is not a list or an attribute set. */
[[gnu::noinline]] static void printLeafAsXML(
    EvalState & state,
    bool strict,
    bool location,
    Value & v,
    XMLWriter & doc,
    NixStringContext & context,
    StringSet & drvsSeen,
    const PosIdx pos)
{
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
    case nList:
        unreachable();

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

    case nAttrs:
        if (state.isDerivation(v))
            printDerivationAsXML(state, strict, location, v, doc, context, drvsSeen);
        else if (state.unforceRoot == &v) [[unlikely]]
            printElementsAsXML(state, strict, location, v, doc, context, drvsSeen, pos);
        else {
            XMLOpenElement _(doc, "attrs");
            showAttrs(state, strict, location, *v.attrs(), doc, context, drvsSeen);
        }
        break;

    case nList: {
        if (state.unforceRoot == &v) [[unlikely]] {
            printElementsAsXML(state, strict, location, v, doc, context, drvsSeen, pos);
            break;
        }
        XMLOpenElement _(doc, "list");
        for (auto v2 : v.listView())
            printValueAsXML(state, strict, location, *v2, doc, context, drvsSeen, pos);
        break;
    }

    case nThunk:
    case nFailed:
    case nInt:
    case nFloat:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nFunction:
    case nExternal:
        printLeafAsXML(state, strict, location, v, doc, context, drvsSeen, pos);
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
    const PosIdx pos)
{
    XMLWriter doc(true, out);
    XMLOpenElement root(doc, "expr");
    StringSet drvsSeen;
    printValueAsXML(state, strict, location, v, doc, context, drvsSeen, pos);
}

} // namespace nix
