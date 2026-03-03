#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"

namespace nix::eval_trace {

// ── Cold verification helpers ────────────────────────────────────────

/**
 * Compare two derivations by their .drv contents and report which
 * environment variables, input derivations, or input sources differ.
 */
[[gnu::cold]]
static std::string diffDerivationInputs(
    EvalState & state,
    const StorePath & drvA, const StorePath & drvB)
{
    std::string msg;
    try {
        auto derivA = state.store->readDerivation(drvA);
        auto derivB = state.store->readDerivation(drvB);

        // Compare env vars
        std::vector<std::string> envDiffs;
        for (auto & [k, vA] : derivA.env) {
            auto it = derivB.env.find(k);
            if (it == derivB.env.end()) {
                envDiffs.push_back(fmt("  env '%s': present in A, missing in B", k));
            } else if (it->second != vA) {
                auto showVal = [](const std::string & s) -> std::string {
                    if (s.size() <= 120) return s;
                    return s.substr(0, 120) + "...";
                };
                envDiffs.push_back(fmt("  env '%s':\n    A: %s\n    B: %s",
                    k, showVal(vA), showVal(it->second)));
            }
        }
        for (auto & [k, vB] : derivB.env) {
            if (derivA.env.find(k) == derivA.env.end())
                envDiffs.push_back(fmt("  env '%s': missing in A, present in B", k));
        }
        if (!envDiffs.empty()) {
            msg += "\nenv diffs (" + std::to_string(envDiffs.size()) + "):";
            for (auto & d : envDiffs)
                msg += "\n" + d;
        }

        // Compare inputDrvs
        auto printInputDrv = [&](const auto & map) {
            std::set<std::string> paths;
            for (auto & [p, _] : map.map)
                paths.insert(state.store->printStorePath(p));
            return paths;
        };
        auto inputsA = printInputDrv(derivA.inputDrvs);
        auto inputsB = printInputDrv(derivB.inputDrvs);
        std::vector<std::string> onlyA, onlyB;
        for (auto & p : inputsA)
            if (!inputsB.count(p)) onlyA.push_back(p);
        for (auto & p : inputsB)
            if (!inputsA.count(p)) onlyB.push_back(p);
        if (!onlyA.empty() || !onlyB.empty()) {
            msg += "\ninputDrvs diffs:";
            for (auto & p : onlyA) msg += "\n  only in A: " + p;
            for (auto & p : onlyB) msg += "\n  only in B: " + p;
        }

        // Compare inputSrcs
        std::set<std::string> srcsA, srcsB;
        for (auto & p : derivA.inputSrcs) srcsA.insert(state.store->printStorePath(p));
        for (auto & p : derivB.inputSrcs) srcsB.insert(state.store->printStorePath(p));
        std::vector<std::string> srcOnlyA, srcOnlyB;
        for (auto & p : srcsA) if (!srcsB.count(p)) srcOnlyA.push_back(p);
        for (auto & p : srcsB) if (!srcsA.count(p)) srcOnlyB.push_back(p);
        if (!srcOnlyA.empty() || !srcOnlyB.empty()) {
            msg += "\ninputSrcs diffs:";
            for (auto & p : srcOnlyA) msg += "\n  only in A: " + p;
            for (auto & p : srcOnlyB) msg += "\n  only in B: " + p;
        }
    } catch (std::exception & e) {
        msg += fmt("\n(could not diff .drv contents: %s)", e.what());
    }
    return msg;
}

/**
 * Recursively compare two Nix values, returning a diagnostic string
 * on the first mismatch or std::nullopt if they match.
 * Depth-limited to 20 to avoid infinite recursion on self-referential attrsets.
 */
[[gnu::cold]]
std::optional<std::string> deepCompare(
    EvalState & state, Value & a, Value & b,
    const std::string & path, int depth)
{
    if (depth > 20)
        return std::nullopt; // depth limit reached, assume match

    try {
        state.forceValue(a, noPos);
    } catch (std::exception & e) {
        return fmt("at '%s': could not force value a: %s", path, e.what());
    }
    try {
        state.forceValue(b, noPos);
    } catch (std::exception & e) {
        return fmt("at '%s': could not force value b: %s", path, e.what());
    }

    if (a.type() != b.type())
        return fmt("type mismatch at '%s': %s vs %s", path, showType(a), showType(b));

    switch (a.type()) {
    case nString: {
        auto sA = a.string_view();
        auto sB = b.string_view();
        if (sA != sB) {
            // Find first differing position
            size_t pos = 0;
            while (pos < sA.size() && pos < sB.size() && sA[pos] == sB[pos]) pos++;
            auto showAround = [](std::string_view s, size_t pos) -> std::string {
                size_t start = pos > 20 ? pos - 20 : 0;
                size_t end = std::min(pos + 20, s.size());
                return std::string(s.substr(start, end - start));
            };
            return fmt("string mismatch at '%s' (pos %d):\n  a: ...%s...\n  b: ...%s...",
                path, pos, showAround(sA, pos), showAround(sB, pos));
        }
        // Compare string contexts
        NixStringContext ctxA, ctxB;
        if (a.context())
            for (auto * elem : *a.context())
                ctxA.insert(NixStringContextElem::parse(elem->view()));
        if (b.context())
            for (auto * elem : *b.context())
                ctxB.insert(NixStringContextElem::parse(elem->view()));
        if (ctxA != ctxB) {
            std::string msg = fmt("string context mismatch at '%s':", path);
            for (auto & c : ctxA)
                if (!ctxB.count(c))
                    msg += fmt("\n  only in a: %s", c.to_string());
            for (auto & c : ctxB)
                if (!ctxA.count(c))
                    msg += fmt("\n  only in b: %s", c.to_string());
            return msg;
        }
        return std::nullopt;
    }
    case nInt:
        if (a.integer().value != b.integer().value)
            return fmt("int mismatch at '%s': %d vs %d", path, a.integer().value, b.integer().value);
        return std::nullopt;
    case nFloat:
        if (a.fpoint() != b.fpoint())
            return fmt("float mismatch at '%s': %f vs %f", path, a.fpoint(), b.fpoint());
        return std::nullopt;
    case nBool:
        if (a.boolean() != b.boolean())
            return fmt("bool mismatch at '%s': %s vs %s", path, a.boolean() ? "true" : "false", b.boolean() ? "true" : "false");
        return std::nullopt;
    case nNull:
        return std::nullopt;
    case nPath:
        if (a.path().path.abs() != b.path().path.abs())
            return fmt("path mismatch at '%s': %s vs %s", path, a.path().path.abs(), b.path().path.abs());
        return std::nullopt;
    case nAttrs: {
        // Collect attribute names
        std::set<std::string_view> namesA, namesB;
        for (auto & attr : *a.attrs()) namesA.insert(state.symbols[attr.name]);
        for (auto & attr : *b.attrs()) namesB.insert(state.symbols[attr.name]);

        if (namesA != namesB) {
            std::string msg = fmt("attrset key mismatch at '%s':", path);
            for (auto & n : namesA)
                if (!namesB.count(n))
                    msg += fmt("\n  only in a: %s", n);
            for (auto & n : namesB)
                if (!namesA.count(n))
                    msg += fmt("\n  only in b: %s", n);
            return msg;
        }

        // For derivations, compare drvPath first (most diagnostic)
        if (state.isDerivation(a)) {
            auto * dpA = a.attrs()->get(state.s.drvPath);
            auto * dpB = b.attrs()->get(state.s.drvPath);
            if (dpA && dpB) {
                state.forceValue(*dpA->value, noPos);
                state.forceValue(*dpB->value, noPos);
                auto svA = dpA->value->string_view();
                auto svB = dpB->value->string_view();
                if (svA != svB) {
                    std::string msg = fmt(
                        "drvPath mismatch at '%s':\n"
                        "  a: %s\n"
                        "  b: %s",
                        path, svA, svB);
                    try {
                        auto spA = state.store->parseStorePath(svA);
                        auto spB = state.store->parseStorePath(svB);
                        msg += diffDerivationInputs(state, spA, spB);
                    } catch (...) {}
                    return msg;
                }
            }
        }

        // Recursively compare children
        for (auto & attrA : *a.attrs()) {
            auto name = state.symbols[attrA.name];
            auto * attrB = b.attrs()->get(attrA.name);
            if (!attrB) continue; // already caught above
            auto childPath = path.empty()
                ? std::string(name)
                : path + "." + std::string(name);
            auto result = deepCompare(state, *attrA.value, *attrB->value, childPath, depth + 1);
            if (result) return result;
        }
        return std::nullopt;
    }
    case nList: {
        if (a.listSize() != b.listSize())
            return fmt("list size mismatch at '%s': %d vs %d", path, a.listSize(), b.listSize());
        for (size_t i = 0; i < a.listSize(); i++) {
            auto childPath = path + "[" + std::to_string(i) + "]";
            auto result = deepCompare(state, *a.listView()[i], *b.listView()[i], childPath, depth + 1);
            if (result) return result;
        }
        return std::nullopt;
    }
    case nThunk:
    case nFunction:
    case nExternal:
    case nFailed:
        return std::nullopt; // can't compare these
    }
    return std::nullopt;
}

void TraceCache::verifyCold(const std::string & attrPath, Value & tracedResult)
{
    SuspendDepTracking suspend;

    // Call rootLoader() directly (NOT getOrEvaluateRoot()) to avoid reusing
    // a cached realRoot that may have been partially forced with tracing active.
    Value * freshRoot = rootLoader();
    state.forceValue(*freshRoot, noPos);

    auto emptyArgs = state.buildBindings(0).finish();
    auto [coldV, pos] = findAlongAttrPath(state, attrPath, *emptyArgs, *freshRoot);
    state.forceValue(*coldV, noPos);

    // Force drvPath on both sides if derivation
    if (tracedResult.type() == nAttrs && state.isDerivation(tracedResult)) {
        if (auto * dp = tracedResult.attrs()->get(state.s.drvPath))
            state.forceValue(*dp->value, noPos);
    }
    if (coldV->type() == nAttrs && state.isDerivation(*coldV)) {
        if (auto * dp = coldV->attrs()->get(state.s.drvPath))
            state.forceValue(*dp->value, noPos);
    }

    auto mismatch = deepCompare(state, tracedResult, *coldV, attrPath);
    if (mismatch)
        throw Error("verify-eval-trace (cold): %s", *mismatch);

    debug("verify-eval-trace (cold): '%s' matches", attrPath);
}

} // namespace nix::eval_trace
