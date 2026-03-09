#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval.hh"

#include <nlohmann/json.hpp>

#include <string>

namespace nix::eval_trace {

// ── Result hashing ───────────────────────────────────────────────────

ResultHash computeResultHash(ResultKind type, std::string_view value, std::string_view context)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    sink(std::string_view("T", 1));
    auto typeStr = std::to_string(std::to_underlying(type));
    sink(typeStr);
    sink(std::string_view("V", 1));
    sink(value);
    sink(std::string_view("C", 1));
    sink(context);
    return ResultHash::fromSink(sink);
}

// ── CachedResult SQL encoding/decoding ──────────────────────────────────

std::tuple<ResultKind, std::string, std::string> TraceStore::encodeCachedResult(const CachedResult & value)
{
    return std::visit(overloaded{
        [&](const attrs_t & a) -> std::tuple<ResultKind, std::string, std::string> {
            // Encode attr names as space-separated AttrNameId integers via the
            // AttrVocabStore. This is more compact than raw strings and avoids
            // delimiter ambiguity (attr names can contain tabs/spaces, but
            // integer IDs cannot). Requires matching decode via vocab.resolveName.
            // NOTE: This is a breaking format change — existing SQLite databases
            // with the old tab-separated string format must be deleted.
            std::string val;
            bool first = true;
            for (auto & sym : a.names) {
                if (!first) val.push_back(' ');
                val.append(std::to_string(vocab.internName(sym).value));
                first = false;
            }
            // Encode origins into the context field as JSON when present.
            std::string ctx;
            if (!a.origins.empty()) {
                nlohmann::json originsJson;
                nlohmann::json origArr = nlohmann::json::array();
                for (auto & orig : a.origins) {
                    origArr.push_back({
                        {"s", orig.depSource},
                        {"f", orig.depKey},
                        {"p", orig.dataPath},  // already a JSON array string
                        {"t", std::string(1, structuredFormatChar(orig.format))},
                    });
                }
                originsJson["origins"] = std::move(origArr);
                originsJson["indices"] = a.originIndices;
                ctx = originsJson.dump();
            }
            return {ResultKind::FullAttrs, std::move(val), std::move(ctx)};
        },
        [&](const string_t & s) -> std::tuple<ResultKind, std::string, std::string> {
            std::string ctx;
            bool first = true;
            for (auto & elem : s.second) {
                if (!first) ctx.push_back(' ');
                ctx.append(elem.to_string());
                first = false;
            }
            return {ResultKind::String, s.first, std::move(ctx)};
        },
        [&](const placeholder_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Placeholder, "", ""};
        },
        [&](const missing_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Missing, "", ""};
        },
        [&](const misc_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Misc, "", ""};
        },
        [&](const failed_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Failed, "", ""};
        },
        [&](bool b) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Bool, b ? "1" : "0", ""};
        },
        [&](const int_t & i) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Int, std::to_string(i.x.value), ""};
        },
        [&](const std::vector<std::string> & l) -> std::tuple<ResultKind, std::string, std::string> {
            std::string val;
            bool first = true;
            for (auto & s : l) {
                if (!first) val.push_back('\t');
                val.append(s);
                first = false;
            }
            return {ResultKind::ListOfStrings, std::move(val), ""};
        },
        [&](const path_t & p) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Path, p.path, ""};
        },
        [&](const null_t &) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Null, "", ""};
        },
        [&](const float_t & f) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::Float, std::to_string(f.x), ""};
        },
        [&](const list_t & lt) -> std::tuple<ResultKind, std::string, std::string> {
            return {ResultKind::List, std::to_string(lt.size), ""};
        },
    }, value);
}

CachedResult TraceStore::decodeCachedResult(const TraceRow & row)
{
    switch (row.type) {
    case ResultKind::FullAttrs: {
        attrs_t result;
        if (!row.value.empty()) {
            // Decode space-separated AttrNameId integers back to Symbols
            // via AttrVocabStore. Matches the encode path above.
            for (auto & idStr : tokenizeString<std::vector<std::string>>(row.value, " ")) {
                auto nameId = AttrNameId(std::stoul(idStr));
                result.names.push_back(symbols.create(vocab.resolveName(nameId)));
            }
        }
        // Decode origins from JSON context field when present.
        if (!row.context.empty()) {
            auto ctx = nlohmann::json::parse(row.context);
            for (auto & origJson : ctx["origins"]) {
                attrs_t::Origin orig;
                orig.depSource = origJson["s"].get<std::string>();
                orig.depKey = origJson["f"].get<std::string>();
                orig.dataPath = origJson["p"].get<std::string>();
                auto fmt = parseStructuredFormat(origJson["t"].get<std::string>()[0]);
                if (!fmt) continue;
                orig.format = *fmt;
                result.origins.push_back(std::move(orig));
            }
            for (auto & idx : ctx["indices"])
                result.originIndices.push_back(idx.get<int8_t>());
        }
        return result;
    }
    case ResultKind::String: {
        NixStringContext context;
        if (!row.context.empty()) {
            for (auto & elem : tokenizeString<std::vector<std::string>>(row.context, " "))
                context.insert(NixStringContextElem::parse(elem));
        }
        return string_t{row.value, std::move(context)};
    }
    case ResultKind::Bool:
        return row.value != "0";
    case ResultKind::Int:
        return int_t{NixInt{row.value.empty() ? 0L : std::stol(row.value)}};
    case ResultKind::ListOfStrings:
        return row.value.empty()
            ? std::vector<std::string>{}
            : tokenizeString<std::vector<std::string>>(row.value, "\t");
    case ResultKind::Path:
        return path_t{row.value};
    case ResultKind::Null:
        return null_t{};
    case ResultKind::Float:
        return float_t{row.value.empty() ? 0.0 : std::stod(row.value)};
    case ResultKind::List:
        return list_t{row.value.empty() ? (size_t)0 : std::stoull(row.value)};
    case ResultKind::Missing:
        return missing_t{};
    case ResultKind::Misc:
        return misc_t{};
    case ResultKind::Failed:
        return failed_t{};
    case ResultKind::Placeholder:
        return placeholder_t{};
    default:
        throw Error("unexpected type %d in eval trace", std::to_underlying(row.type));
    }
}

} // namespace nix::eval_trace
