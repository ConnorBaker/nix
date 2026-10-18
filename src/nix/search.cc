#include "nix/cmd/command-installable-value.hh"
#include "nix/store/globals.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/store/names.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/attr-path.hh"
#include "nix/util/hilite.hh"
#include "nix/util/strings-inline.hh"

#include <boost/regex.hpp>
#include <nlohmann/json.hpp>

#include "nix/util/strings.hh"

using json = nlohmann::json;

namespace nix {

std::string wrap(std::string prefix, std::string s)
{
    return concatStrings(prefix, s, ANSI_NORMAL);
}

struct CmdSearch : InstallableValueCommand, MixJSON
{
    std::vector<std::string> res;
    std::vector<std::string> excludeRes;

    CmdSearch()
    {
        expectArgs("regex", &res);
        addFlag(
            Flag{
                .longName = "exclude",
                .shortName = 'e',
                .description = "Hide packages whose attribute path, name or description contain *regex*.",
                .labels = {"regex"},
                .handler = {[this](std::string s) { excludeRes.push_back(s); }},
            });
    }

    std::string description() override
    {
        return "search for packages";
    }

    std::string doc() override
    {
        return
#include "search.md"
            ;
    }

    Strings getDefaultFlakeAttrPaths() override
    {
        return {"packages." + settings.thisSystem.get(), "legacyPackages." + settings.thisSystem.get()};
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        settings.readOnlyMode = true;
        evalSettings.enableImportFromDerivation.setDefault(false);

        // Recommend "^" here instead of ".*" due to differences in resulting highlighting
        if (res.empty())
            throw UsageError(
                "Must provide at least one regex! To match all packages, use '%s'.", "nix search <installable> ^");

        std::vector<boost::regex> regexes;
        std::vector<boost::regex> excludeRegexes;
        regexes.reserve(res.size());
        excludeRegexes.reserve(excludeRes.size());

        for (auto & re : res)
            regexes.push_back(boost::regex(re, boost::regex::extended | boost::regex::icase));

        for (auto & re : excludeRes)
            excludeRegexes.emplace_back(re, boost::regex::extended | boost::regex::icase);

        auto state = getEvalState();

        /* Cache-respecting parallel fan-out (ported from DetSys's search.cc,
           adapted to our thread model and to keep output byte-identical to the
           serial path):

           - The eval-cache cursor traversal itself is parallelized across the
             evaluator's worker threads when `eval-cores > 1`
             (`state->executor->enabled`), so the COLD, cache-on first search
             speeds up. At `eval-cores == 1` the executor is disabled and the
             serial code path below runs unchanged (byte-identical to base,
             including the per-visit `Activity` and the `UnforceBatch`
             batching).

           - To keep stdout byte-identical regardless of the nondeterministic
             worker-completion order, matches are COLLECTED rather than printed
             as they are found, then emitted in the serial traversal order at
             the end. The emit order is the depth-first order over children
             sorted by symbol string (what `AttrCursor::getAttrs` produces),
             which is exactly element-wise lexicographic order of the resolved
             attr-path segments -- so sorting the collected results by that key
             reproduces the serial order. JSON output is an object keyed by the
             attr-path string, whose keys nlohmann::json orders for us, so its
             bytes are independent of insertion order. */

        /* A collected search hit. `hiliteMatches` results are computed eagerly
           (the `boost::smatch`es borrow the source strings, which do not
           outlive the match), so only finished strings are stored. */
        struct Result
        {
            std::vector<std::string> orderKey;
            std::string attrPathStr;
            std::string highlightedAttrPath;
            std::string pname;
            std::string version;
            std::string description;
            std::string highlightedDescription;
        };

        const bool parallel = state->executor->enabled;

        std::atomic<uint64_t> results = 0;

        /* Parallel path: collected hits are sorted and emitted after the join,
           to make output deterministic despite nondeterministic worker order. */
        Sync<std::vector<Result>> collected;

        /* Serial path (eval-cores == 1): hits are emitted INLINE during the DFS,
           exactly as base does -- no collect/sort. This keeps the serial warm
           path at base cost (no per-hit `orderKey` build, no final `std::sort`)
           and preserves base's partial-output-on-abort: if a non-swallowed
           `EvalError` aborts mid-search, the hits found so far are already on
           stdout. For `--json` the object is built inline and printed once at
           the end (base behaviour; JSON is all-or-nothing anyway). */
        std::optional<nlohmann::json> jsonOut;
        if (!parallel && json)
            jsonOut = nlohmann::json::object();

        /* Match a derivation cursor against the regexes. On a hit, invoke
           `onMatch(name, description, attrPathMatches, descriptionMatches)`;
           on exclude / non-match do nothing. Shared, identical matching logic
           for both drivers. `onMatch` is a generic callback so the serial
           driver can emit INLINE (computing `hiliteMatches` in the `cout`
           args, exactly like base, with no intermediate `Result`) while the
           parallel driver builds a `Result` to collect. */
        auto matchDrv = [&](eval_cache::AttrCursor & cursor,
                            const AttrPath & attrPath,
                            const std::string & attrPathStr,
                            auto && onMatch) {
            DrvName name(cursor.getAttr(state->s.name)->getString());

            auto aMeta = cursor.maybeGetAttr(state->s.meta);
            auto aDescription = aMeta ? aMeta->maybeGetAttr(state->s.description) : nullptr;
            auto description = aDescription ? aDescription->getString() : "";
            std::replace(description.begin(), description.end(), '\n', ' ');

            std::vector<boost::smatch> attrPathMatches;
            std::vector<boost::smatch> descriptionMatches;
            std::vector<boost::smatch> nameMatches;
            bool found = false;

            for (auto & regex : excludeRegexes) {
                if (boost::regex_search(attrPathStr, regex) || boost::regex_search(name.name, regex)
                    || boost::regex_search(description, regex))
                    return;
            }

            for (auto & regex : regexes) {
                found = false;
                auto addAll = [&found](boost::sregex_iterator it, std::vector<boost::smatch> & vec) {
                    const auto end = boost::sregex_iterator();
                    while (it != end) {
                        vec.push_back(*it++);
                        found = true;
                    }
                };

                addAll(boost::sregex_iterator(attrPathStr.begin(), attrPathStr.end(), regex), attrPathMatches);
                addAll(boost::sregex_iterator(name.name.begin(), name.name.end(), regex), nameMatches);
                addAll(boost::sregex_iterator(description.begin(), description.end(), regex), descriptionMatches);

                if (!found)
                    break;
            }

            if (!found)
                return;

            onMatch(name, description, attrPathMatches, descriptionMatches);
        };

        using Matches = std::vector<boost::smatch>;

        /* Serial driver callback: emit the hit inline, byte-identical to base
           (no `Result`, no collect/sort). */
        auto emitSerial = [&](const std::string & attrPathStr,
                              const DrvName & name,
                              const std::string & description,
                              Matches & attrPathMatches,
                              Matches & descriptionMatches) {
            auto n = ++results;
            if (json) {
                (*jsonOut)[attrPathStr] = {
                    {"pname", name.name},
                    {"version", name.version},
                    {"description", description},
                };
            } else {
                if (n > 1)
                    logger->cout("");
                logger->cout(
                    "* %s%s",
                    wrap("\e[0;1m", hiliteMatches(attrPathStr, attrPathMatches, ANSI_GREEN, "\e[0;1m")),
                    optionalBracket(" (", name.version, ")"));
                if (description != "")
                    logger->cout("  %s", hiliteMatches(description, descriptionMatches, ANSI_GREEN, ANSI_NORMAL));
            }
        };

        /* Parallel driver callback: build the `Result` (with the ordering key)
           and collect it for the post-join sort/emit. */
        auto recordParallel = [&](const AttrPath & attrPath,
                                  const std::string & attrPathStr,
                                  const DrvName & name,
                                  const std::string & description,
                                  Matches & attrPathMatches,
                                  Matches & descriptionMatches) {
            Result r;
            r.attrPathStr = attrPathStr;
            r.highlightedAttrPath = wrap("\e[0;1m", hiliteMatches(attrPathStr, attrPathMatches, ANSI_GREEN, "\e[0;1m"));
            r.pname = name.name;
            r.version = name.version;
            r.description = description;
            r.highlightedDescription = hiliteMatches(description, descriptionMatches, ANSI_GREEN, ANSI_NORMAL);
            r.orderKey.reserve(attrPath.size());
            for (auto & sym : attrPath)
                r.orderKey.emplace_back(std::string(state->symbols[sym]));
            results++;
            collected.lock()->push_back(std::move(r));
        };

        /* The parallel driver uses a `FutureVector` so that the command waits
           for every spawned work item (including nested ones) before returning
           and rethrows the first recorded exception -- a clean, completion-
           based join rather than fire-and-forget (which risks a teardown
           crash). Constructed unconditionally; it only ever spawns on the
           parallel path, and its destructor is a no-op with nothing pending. */
        FutureVector futures(*state->executor);

        std::function<void(eval_cache::AttrCursor & cursor, const AttrPath & attrPath, bool initialRecurse)> visit;

        visit = [&](eval_cache::AttrCursor & cursor, const AttrPath & attrPath, bool initialRecurse) {
            auto attrPathS = state->symbols.resolve({attrPath});
            auto attrPathStr = attrPath.to_string(*state);

            /* Per-visit progress activity, only on the serial path: it is part
               of base's behaviour at eval-cores == 1, and we avoid creating one
               per concurrent visit on the parallel path (as DetSys does). */
            std::optional<Activity> act;
            if (!parallel)
                act.emplace(*logger, lvlTalkative, actUnknown, fmt("evaluating '%s'", attrPathStr));

            try {
                auto recurse = [&]() {
                    if (parallel) {
                        /* Fan the children out as work items. Enumerate the
                           parent (getAttrs) and create the child cursors on
                           THIS thread -- so the parent cursor's `cachedValue`
                           is fully written before any child runs -- then spawn.
                           The spawn/dequeue through the executor queue
                           establishes the happens-before that makes a child's
                           read of its parent's `cachedValue` (via getKey)
                           race-free. */
                        Executor::WorkItems work;
                        for (const auto & attr : cursor.getAttrs()) {
                            auto cursor2 = cursor.getAttr(state->symbols[attr]);
                            auto attrPath2(attrPath);
                            attrPath2.push_back(attr);
                            uint8_t prio =
                                std::string_view(state->symbols[attr]).find("Packages") != std::string_view::npos ? 0
                                                                                                                   : 2;
                            work.emplace_back([cursor2, attrPath2, visit]() { visit(*cursor2, attrPath2, false); }, prio);
                            /* Spawn incrementally so idle workers can start on
                               the first attributes while we enumerate the rest. */
                            if (work.size() >= 256)
                                futures.spawn(std::exchange(work, {}));
                        }
                        futures.spawn(std::move(work));
                    } else {
                        /* What visiting the children forces is unforced in
                           batches. Cursors keep only roots to values, and each
                           child's cursors die with it. */
                        auto attrs = cursor.getAttrs();
                        UnforceBatch batch(*state);
                        for (const auto & attr : attrs) {
                            UnforceBatch::Element element(batch);
                            auto cursor2 = cursor.getAttr(state->symbols[attr]);
                            auto attrPath2(attrPath);
                            attrPath2.push_back(attr);
                            visit(*cursor2, attrPath2, false);
                        }
                    }
                };

                if (cursor.isDerivation()) {
                    if (parallel)
                        matchDrv(
                            cursor,
                            attrPath,
                            attrPathStr,
                            [&](const DrvName & name,
                                const std::string & description,
                                Matches & attrPathMatches,
                                Matches & descriptionMatches) {
                                recordParallel(
                                    attrPath, attrPathStr, name, description, attrPathMatches, descriptionMatches);
                            });
                    else
                        matchDrv(
                            cursor,
                            attrPath,
                            attrPathStr,
                            [&](const DrvName & name,
                                const std::string & description,
                                Matches & attrPathMatches,
                                Matches & descriptionMatches) {
                                emitSerial(attrPathStr, name, description, attrPathMatches, descriptionMatches);
                            });
                }

                else if (
                    attrPath.size() == 0 || (attrPathS[0] == "legacyPackages" && attrPath.size() <= 2)
                    || (attrPathS[0] == "packages" && attrPath.size() <= 2))
                    recurse();

                else if (initialRecurse)
                    recurse();

                else if (attrPathS[0] == "legacyPackages" && attrPath.size() > 2) {
                    auto attr = cursor.maybeGetAttr(state->s.recurseForDerivations);
                    if (attr && attr->getBool())
                        recurse();
                }

            } catch (EvalError & e) {
                if (!(attrPath.size() > 0 && attrPathS[0] == "legacyPackages"))
                    throw;
            }
        };

        if (parallel) {
            Executor::WorkItems work;
            for (auto & cursor : installable->getCursors(*state, AutoCall::Yes))
                work.emplace_back(
                    [cursor, attrPath(cursor->getAttrPath()), visit]() { visit(*cursor, attrPath, true); }, uint8_t(1));
            futures.spawn(std::move(work));
            futures.finishAll();
        } else {
            for (auto & cursor : installable->getCursors(*state, AutoCall::Yes))
                visit(*cursor, cursor->getAttrPath(), true);
        }

        if (parallel) {
            /* Emit the collected hits in the serial traversal order (text), or
               as a key-sorted JSON object (nlohmann orders the keys). */
            auto hits(collected.lock());
            if (json) {
                auto jo = nlohmann::json::object();
                for (auto & r : *hits)
                    jo[r.attrPathStr] = {
                        {"pname", r.pname},
                        {"version", r.version},
                        {"description", r.description},
                    };
                printJSON(jo);
            } else {
                std::sort(hits->begin(), hits->end(), [](const Result & a, const Result & b) {
                    return a.orderKey < b.orderKey;
                });
                bool first = true;
                for (auto & r : *hits) {
                    if (!first)
                        logger->cout("");
                    first = false;
                    logger->cout("* %s%s", r.highlightedAttrPath, optionalBracket(" (", r.version, ")"));
                    if (r.description != "")
                        logger->cout("  %s", r.highlightedDescription);
                }
            }
        } else if (json)
            /* Serial text was emitted inline during the DFS; only JSON is
               deferred to here, matching base. */
            printJSON(*jsonOut);

        if (!json && !results)
            throw Error("no results for the given search term(s)!");
    }
};

static auto rCmdSearch = registerCommand<CmdSearch>("search");

} // namespace nix
