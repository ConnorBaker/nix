#include "dotgraph.hh"
#include "closure-walk.hh"
#include "nix/store/store-api.hh"

#include <iostream>

using std::cout;

namespace nix {

static std::string dotQuote(std::string_view s)
{
    return "\"" + std::string(s) + "\"";
}

static const std::string & nextColour()
{
    static int n = 0;
    static std::vector<std::string> colours{"black", "red", "green", "blue", "magenta", "burlywood"};
    return colours[n++ % colours.size()];
}

static std::string makeEdge(std::string_view src, std::string_view dst)
{
    return fmt("%1% -> %2% [color = %3%];\n", dotQuote(src), dotQuote(dst), dotQuote(nextColour()));
}

static std::string makeNode(std::string_view id, std::string_view label, std::string_view colour)
{
    return fmt(
        "%1% [label = %2%, shape = box, "
        "style = filled, fillcolor = %3%];\n",
        dotQuote(id),
        dotQuote(label),
        dotQuote(colour));
}

void printDotGraph(ref<Store> store, StorePathSet && roots)
{
    cout << "digraph G {\n";

    walkClosure(
        store,
        std::move(roots),
        [&](const StorePath & path, const ValidPathInfo &) {
            cout << makeNode(std::string(path.to_string()), path.name(), "#ff0000");
        },
        // Edges in the dot output are reversed: the reference points at the path that holds it.
        [&](const StorePath & path, const StorePath & reference) {
            cout << makeEdge(std::string(reference.to_string()), std::string(path.to_string()));
        });

    cout << "}\n";
}

} // namespace nix
