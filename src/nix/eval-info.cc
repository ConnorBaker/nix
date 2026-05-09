#include "nix/cmd/command-installable-value.hh"
#include "nix/cmd/installable-flake.hh"
#include "nix/cmd/installable-attr-path.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/cache/trace-backend.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/expr/eval-trace/store/trace-resolve.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/result.hh"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace nix {

using nlohmann::json;
using namespace nix::eval_trace;

namespace {

/// Tokenize a dotted attribute path. Mirrors the `parseAttrPath` helper in
/// attr-path.cc (which is file-static there) so the vocab-lookup path never
/// runs `state.symbols.create()` — interning a symbol would pollute the
/// symbol table for an attr that may not exist.
std::vector<std::string> tokenizeAttrPath(std::string_view s)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '.') {
            out.push_back(std::move(cur));
            cur.clear();
            ++i;
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < s.size() && s[i] != '"')
                cur.push_back(s[i++]);
            if (i >= s.size())
                throw ParseError("missing closing quote in attr path '%s'", s);
            ++i;
            continue;
        }
        cur.push_back(c);
        ++i;
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

/// Resolve a dotted attr path string to an AttrPathId by walking the
/// AttrVocabStore read-only. Returns nullopt if any component is not
/// interned or not linked to its parent.
std::optional<AttrPathId> resolveAttrPathId(
    const AttrVocabStore & vocab, std::string_view dotted)
{
    auto components = tokenizeAttrPath(dotted);
    AttrPathId pathId = AttrVocabStore::rootPath();
    for (auto & comp : components) {
        auto nameId = vocab.lookupName(comp);
        if (!nameId)
            return std::nullopt;
        auto next = vocab.lookupPath(pathId, *nameId);
        if (!next)
            return std::nullopt;
        pathId = *next;
    }
    return pathId;
}

/// Collect all candidate attr path strings for an installable. For a flake,
/// this is the set that `getActualAttrPaths` produces (prefixes × attrPaths).
/// For a non-flake InstallableValue with a resolved attr path, just that one.
/// For an empty-attr-path installable (e.g. `-f foo.nix` with no attr), the
/// root itself (empty string → rootPath).
std::vector<std::string> candidateAttrPaths(InstallableValue & installable)
{
    if (auto * flake = dynamic_cast<InstallableFlake *>(&installable))
        return flake->getActualAttrPaths();
    auto resolved = installable.resolvedAttrPath();
    return {resolved};
}

// ── JSON helpers ────────────────────────────────────────────────────────

json structuredPathToJson(const StructuredPath & path)
{
    auto arr = json::array();
    for (const auto & c : path) {
        if (c.isIndex())
            arr.push_back(c.index);
        else
            arr.push_back(c.key);
    }
    return arr;
}

json depToJson(const InterningPools & pools, AttrVocabStore & vocab, const Dep & dep)
{
    auto resolved = resolveDep(pools, vocab, dep);
    json out;
    out["type"] = std::string(queryKindName(dep.key.kind));
    out["behavior"] = std::string(queryBehaviorName(queryBehavior(dep.key.kind)));

    if (dep.key.isTraceContext()) {
        out["attrPath"] = resolved.key;
        out["attrPathId"] = dep.key.attrPathId.value;
    } else {
        out["source"] = resolved.source;
        if (dep.key.isStructured() && resolved.structured) {
            const auto & sk = *resolved.structured;
            out["filePath"] = sk.filePath;
            out["format"] = std::string(1, structuredFormatChar(sk.format));
            out["dataPath"] = structuredPathToJson(sk.dataPath);
            if (sk.suffix != ShapeSuffix::None)
                out["shapeSuffix"] = std::string(shapeSuffixName(sk.suffix));
            if (!sk.hasKey.empty())
                out["hasKey"] = sk.hasKey;
            if (!sk.dirSetHash.empty())
                out["dirSetHash"] = sk.dirSetHash;
        } else {
            out["key"] = resolved.key;
        }

        if (dep.key.governingRepoId.value != 0)
            out["governingRepoId"] = std::string(pools.resolve(dep.key.governingRepoId));
    }

    // Value: either an eval-trace hash or a raw string (for existence / store paths).
    std::visit(overloaded{
        [&](const DepHash & h) { out["hash"] = h.value.toHex(); },
        [&](const std::string & s) { out["valueString"] = s; },
    }, dep.hash);

    return out;
}

json resultValueToJson(const CachedResult & value)
{
    return std::visit(overloaded{
        [](bool b) -> json { return b; },
        [](const int_t & i) -> json { return i.x.value; },
        [](const float_t & f) -> json { return f.x; },
        [](const string_t & s) -> json {
            json out;
            out["kind"] = "String";
            out["value"] = s.first;
            if (!s.second.empty()) {
                auto ctx = json::array();
                for (auto & elem : s.second)
                    ctx.push_back(elem.to_string());
                out["context"] = std::move(ctx);
            }
            return out;
        },
        [](const path_t & p) -> json {
            json out;
            out["kind"] = "Path";
            out["path"] = p.path;
            return out;
        },
        [](const attrs_t & a) -> json {
            json out;
            out["kind"] = "FullAttrs";
            out["entryCount"] = a.entries.size();
            return out;
        },
        [](const list_t & l) -> json {
            json out;
            out["kind"] = "List";
            out["length"] = l.entries.size();
            return out;
        },
        [](const trivial_t & t) -> json {
            json out;
            out["kind"] = "Trivial";
            out["subKind"] = std::string(trivialKindName(t.kind));
            return out;
        },
        [](const failed_t & f) -> json {
            json out;
            out["kind"] = "Failed";
            out["errorMessage"] = f.errorMessage;
            return out;
        },
    }, value);
}

bool hasVolatileDeps(const std::vector<Dep> & deps)
{
    return std::any_of(deps.begin(), deps.end(), [](const Dep & d) {
        return isVolatile(d.key.kind);
    });
}

std::string_view cachedResultKindName(const CachedResult & value)
{
    return std::visit(overloaded{
        [](const attrs_t &)   -> std::string_view { return resultKindName(ResultKind::FullAttrs); },
        [](const string_t &)  -> std::string_view { return resultKindName(ResultKind::String); },
        [](const trivial_t &) -> std::string_view { return resultKindName(ResultKind::Trivial); },
        [](const failed_t &)  -> std::string_view { return resultKindName(ResultKind::Failed); },
        [](bool)              -> std::string_view { return resultKindName(ResultKind::Bool); },
        [](const int_t &)     -> std::string_view { return resultKindName(ResultKind::Int); },
        [](const path_t &)    -> std::string_view { return resultKindName(ResultKind::Path); },
        [](const float_t &)   -> std::string_view { return resultKindName(ResultKind::Float); },
        [](const list_t &)    -> std::string_view { return resultKindName(ResultKind::List); },
    }, value);
}

json runtimeRootsToJson(
    Store & store,
    const SqliteTraceStorage::RuntimeRootLoadResult & roots)
{
    auto arr = json::array();
    for (const auto & entry : roots.entries) {
        json obj;
        obj["source"] = serializeDepSource(entry.source);
        obj["storePath"] = store.printStorePath(entry.storePath.value);
        obj["narHash"] = entry.narHash.value.to_string(HashFormat::SRI, true);
        auto attrs = json::object();
        for (auto & [k, v] : entry.fetchIdentity.inputAttrs) {
            std::visit(overloaded{
                [&](const std::string & s) { attrs[k] = s; },
                [&](uint64_t u) { attrs[k] = u; },
                [&](Explicit<bool> b) { attrs[k] = b.t; },
                [&](const fetchers::LazyAttr &) { attrs[k] = "<lazy>"; },
            }, v);
        }
        obj["fetchIdentity"] = std::move(attrs);
        arr.push_back(std::move(obj));
    }
    return arr;
}

// ── Text rendering ─────────────────────────────────────────────────────

void printHeader(
    std::ostream & out,
    const std::string & attrPathDisplay,
    const std::string & sessionKeyHex,
    const std::optional<SqliteTraceStorage::EvalInfoRecord> & record)
{
    out << "attr path:        " << (attrPathDisplay.empty() ? "«root»" : attrPathDisplay) << "\n";
    out << "session key:      " << sessionKeyHex << "\n";
    if (!record) {
        out << "cached:           no\n";
        return;
    }
    out << "cached:           yes\n";
    out << "source:           "
        << (record->source == SqliteTraceStorage::EvalInfoRecord::Source::Session ? "session" : "history")
        << "\n";
    out << "trace id:         " << record->traceId.value << "\n";
    out << "result id:        " << record->resultId.value << "\n";
    out << "trace hash:       " << record->traceHash.value.toHex() << "\n";
    out << "dep-key-set:      " << record->keySetHash.value.toHex() << " (id=" << record->depKeySetId.value << ")\n";
}

void printValue(std::ostream & out, const CachedResult & value)
{
    std::visit(overloaded{
        [&](bool b) { out << "result:           Bool " << (b ? "true" : "false") << "\n"; },
        [&](const int_t & i) { out << "result:           Int " << i.x.value << "\n"; },
        [&](const float_t & f) {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), f.x);
            out << "result:           Float " << std::string_view(buf, ptr - buf) << "\n";
        },
        [&](const string_t & s) {
            out << "result:           String " << s.first.size() << " bytes";
            if (!s.second.empty())
                out << " (+" << s.second.size() << " context elements)";
            out << "\n";
        },
        [&](const path_t & p) { out << "result:           Path " << p.path << "\n"; },
        [&](const attrs_t & a) { out << "result:           FullAttrs (" << a.entries.size() << " entries)\n"; },
        [&](const list_t & l) { out << "result:           List (" << l.entries.size() << " entries)\n"; },
        [&](const trivial_t & t) {
            out << "result:           Trivial/" << trivialKindName(t.kind) << "\n";
        },
        [&](const failed_t & f) {
            out << "result:           Failed\n";
            out << "                  [CACHED FAILURE] " << f.errorMessage << "\n";
        },
    }, value);
}

void printDeps(
    std::ostream & out,
    const InterningPools & pools,
    AttrVocabStore & vocab,
    const std::vector<Dep> & deps)
{
    if (deps.empty()) {
        out << "dependencies:     (none)\n";
        return;
    }
    out << "dependencies:     " << deps.size();
    if (hasVolatileDeps(deps))
        out << " [contains volatile deps — this trace will always re-evaluate]";
    out << "\n";

    for (const auto & dep : deps) {
        auto resolved = resolveDep(pools, vocab, dep);
        out << "  " << queryKindName(dep.key.kind);
        if (!dep.key.isTraceContext())
            out << "  [" << resolved.source << "]";
        out << "\n      " << resolved.key << "\n";
        std::visit(overloaded{
            [&](const DepHash & h) { out << "      hash=" << h.value.toHex() << "\n"; },
            [&](const std::string & s) {
                if (!s.empty())
                    out << "      value=" << s << "\n";
            },
        }, dep.hash);
        if (!dep.key.isTraceContext() && dep.key.governingRepoId.value != 0)
            out << "      governing-repo=" << pools.resolve(dep.key.governingRepoId) << "\n";
    }
}

void printRuntimeRoots(
    std::ostream & out, Store & store,
    const SqliteTraceStorage::RuntimeRootLoadResult & roots)
{
    if (roots.entries.empty()) {
        out << "runtime fetch roots: (none)\n";
        return;
    }
    out << "runtime fetch roots: " << roots.entries.size() << "\n";
    for (const auto & entry : roots.entries) {
        out << "  " << serializeDepSource(entry.source) << "\n";
        out << "      store-path=" << store.printStorePath(entry.storePath.value) << "\n";
        out << "      nar-hash="  << entry.narHash.value.to_string(HashFormat::SRI, true) << "\n";
    }
    if (roots.rejectedCount > 0)
        out << "  (" << roots.rejectedCount << " malformed rows ignored)\n";
}

// ── Command ────────────────────────────────────────────────────────────

struct CmdEvalInfo : InstallableValueCommand, MixJSON, MixReadOnlyOption
{
    bool includeHistory = false;

    CmdEvalInfo()
    {
        addFlag({
            .longName = "include-history",
            .description =
                "If the current session key has no row, fall back to the most recent "
                "History row under the stable recovery key. Off by default.",
            .handler = {&includeHistory, true},
        });
    }

    std::string description() override
    {
        return "query the eval-trace cache for a previously-evaluated installable";
    }

    std::string doc() override
    {
        return
#include "eval-info.md"
            ;
    }

    Category category() override { return catSecondary; }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        auto state = getEvalState();

        if (!state->settings.useTraceCache)
            throw UsageError(
                "eval-trace is disabled for this invocation — nix eval-info has nothing to report. "
                "Remove '--no-eval-trace' or set 'eval-trace = true' in nix.conf, then re-run "
                "'nix eval' to populate the cache before querying it.");

        auto session = installable->getOrCreateTraceCache(*state);
        auto * backend = session->traceBackend();
        if (!backend)
            throw Error(
                "the eval-trace backend could not be opened for '%s' "
                "(check that 'eval-trace-hash-algorithm' matches the value used to populate the cache)",
                installable->what());

        auto & vocab = state->vocabStore();
        auto & pools = state->tracingPools();

        auto sessionKeyHex = backend->currentSemanticSessionKey().toHex();
        auto candidates = candidateAttrPaths(*installable);

        std::string matchedAttrPath;
        std::optional<SqliteTraceStorage::EvalInfoRecord> record;

        for (auto & candidate : candidates) {
            auto pathId = resolveAttrPathId(vocab, candidate);
            if (!pathId)
                continue;
            auto r = backend->queryEvalInfo(*pathId, includeHistory);
            if (r) {
                matchedAttrPath = candidate;
                record = std::move(r);
                break;
            }
            // First vocab-resolvable candidate wins the miss-message attr path.
            if (matchedAttrPath.empty())
                matchedAttrPath = candidate;
        }
        if (matchedAttrPath.empty() && !candidates.empty())
            matchedAttrPath = candidates.front();

        auto runtimeRoots = backend->loadRuntimeRoots(*store);

        if (json) {
            nlohmann::json out;
            out["version"] = 1;
            out["attrPath"] = matchedAttrPath;
            out["sessionKey"] = sessionKeyHex;
            out["cached"] = record.has_value();
            out["source"] = record
                ? (record->source == SqliteTraceStorage::EvalInfoRecord::Source::Session ? "session" : "history")
                : "none";
            if (record) {
                out["traceId"] = record->traceId.value;
                out["resultId"] = record->resultId.value;
                out["traceHash"] = record->traceHash.value.toHex();
                out["depKeySetHash"] = record->keySetHash.value.toHex();
                out["depKeySetId"] = record->depKeySetId.value;
                out["resultKind"] = std::string(cachedResultKindName(record->value));
                out["value"] = resultValueToJson(record->value);
                if (auto * failure = std::get_if<failed_t>(&record->value)) {
                    nlohmann::json failureObj;
                    failureObj["errorMessage"] = failure->errorMessage;
                    out["failure"] = std::move(failureObj);
                } else {
                    out["failure"] = nullptr;
                }
                out["hasVolatileDeps"] = hasVolatileDeps(*record->deps);
                auto depsArr = nlohmann::json::array();
                for (auto & dep : *record->deps)
                    depsArr.push_back(depToJson(pools, vocab, dep));
                out["deps"] = std::move(depsArr);
            }
            out["runtimeRoots"] = runtimeRootsToJson(*store, runtimeRoots);
            printJSON(out);
            return;
        }

        std::ostringstream out;
        printHeader(out, matchedAttrPath, sessionKeyHex, record);
        if (record) {
            printValue(out, record->value);
            printDeps(out, pools, vocab, *record->deps);
        } else {
            out << "\n"
                << "No cached trace for '" << (matchedAttrPath.empty() ? "«root»" : matchedAttrPath)
                << "' under the current session key.\n"
                << "Populate the cache by re-running the same 'nix eval' invocation "
                << "with --eval-trace enabled.\n";
            if (!includeHistory)
                out << "Pass --include-history to search the stable recovery namespace.\n";
        }
        out << "\n";
        printRuntimeRoots(out, *store, runtimeRoots);
        // `out` already ends with a newline; `logger->cout` appends its own,
        // so trim the trailing newline to avoid a double blank line.
        auto text = out.str();
        while (!text.empty() && text.back() == '\n')
            text.pop_back();
        logger->cout("%s", text);
    }
};

static auto rCmdEvalInfo = registerCommand<CmdEvalInfo>("eval-info");

} // namespace

} // namespace nix
