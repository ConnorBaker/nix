#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/trace-result-codec.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval.hh"

#include <algorithm>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>

#include <string>

namespace nix::eval_trace {

namespace {

nlohmann::json encodeStructuredPath(const StructuredPath & path)
{
    auto result = nlohmann::json::array();
    for (auto & component : path) {
        if (component.isIndex())
            result.push_back(component.index);
        else
            result.push_back(component.key);
    }
    return result;
}

bool decodeStructuredPath(const nlohmann::json & pathJson, StructuredPath & result)
{
    if (!pathJson.is_array())
        return false;

    for (auto & component : pathJson) {
        if (component.is_number_integer()) {
            result.push_back(StructuredPathComponent::makeIndex(component.get<int32_t>()));
            continue;
        }
        if (!component.is_string())
            return false;
        result.push_back(StructuredPathComponent::makeKey(component.get<std::string>()));
    }

    return true;
}

nlohmann::json encodeStructuredObject(const StructuredObject & structured)
{
    return {
        {"s", serializeDepSource(structured.source)},
        {"f", structured.key},
        {"p", encodeStructuredPath(structured.dataPath)},
        {"t", std::string(1, structuredFormatChar(structured.format))},
    };
}

bool decodeStructuredObject(const nlohmann::json & origJson, StructuredObject & result)
{
    if (!origJson.is_object())
        return false;
    if (!origJson.contains("s") || !origJson["s"].is_string())
        return false;
    if (!origJson.contains("f") || !origJson["f"].is_string())
        return false;
    if (!origJson.contains("p"))
        return false;
    if (!origJson.contains("t") || !origJson["t"].is_string())
        return false;

    auto fmtStr = origJson["t"].get<std::string>();
    if (fmtStr.size() != 1)
        return false;
    auto fmt = parseStructuredFormat(fmtStr[0]);
    if (!fmt) return false;

    auto depSource = parseDepSource(origJson["s"].get<std::string>());
    if (!depSource)
        return false;

    StructuredPath dataPath;
    if (!decodeStructuredPath(origJson["p"], dataPath))
        return false;
    result = StructuredObject{
        .source = std::move(*depSource),
        .key = origJson["f"].get<std::string>(),
        .dataPath = std::move(dataPath),
        .format = *fmt,
    };
    return true;
}

nlohmann::json encodePathObject(const PathObject & origin)
{
    return {
        {"s", serializeDepSource(origin.source)},
        {"r", origin.rootPath.abs()},
    };
}

std::optional<PathObject> decodePathObject(const nlohmann::json & originJson)
{
    if (!originJson.is_object())
        return std::nullopt;
    if (!originJson.contains("s") || !originJson["s"].is_string())
        return std::nullopt;
    if (!originJson.contains("r") || !originJson["r"].is_string())
        return std::nullopt;

    auto depSource = parseDepSource(originJson["s"].get<std::string>());
    if (!depSource)
        return std::nullopt;

    return PathObject{
        .source = std::move(*depSource),
        .rootPath = CanonPath(originJson["r"].get<std::string>()),
    };
}

nlohmann::json encodeResolvedDepPath(const ResolvedDepPath & depPath)
{
    return {
        {"s", serializeDepSource(depPath.source)},
        {"k", depPath.key},
    };
}

std::optional<ResolvedDepPath> decodeResolvedDepPath(const nlohmann::json & depPathJson)
{
    if (!depPathJson.is_object())
        return std::nullopt;
    if (!depPathJson.contains("s") || !depPathJson["s"].is_string())
        return std::nullopt;
    if (!depPathJson.contains("k") || !depPathJson["k"].is_string())
        return std::nullopt;

    auto depSource = parseDepSource(depPathJson["s"].get<std::string>());
    if (!depSource)
        return std::nullopt;

    return ResolvedDepPath{
        .source = std::move(*depSource),
        .key = depPathJson["k"].get<std::string>(),
    };
}

nlohmann::json encodeTextObject(const TextObject & provenance)
{
    return {
        {"d", encodeResolvedDepPath(ResolvedDepPath{provenance.source, provenance.key})},
        {"ha", std::string(evalTraceHashAlgorithmSlug(getEvalTraceHashAlgorithm()))},
        {"h", provenance.contentHash.value.toHex()},
    };
}

std::optional<TextObject> decodeTextObject(const nlohmann::json & provenanceJson)
{
    if (!provenanceJson.is_object())
        return std::nullopt;
    if (!provenanceJson.contains("d") || !provenanceJson.contains("h"))
        return std::nullopt;
    if (!provenanceJson["h"].is_string())
        return std::nullopt;
    auto algorithm = getEvalTraceHashAlgorithm();
    if (provenanceJson.contains("ha")) {
        if (!provenanceJson["ha"].is_string())
            return std::nullopt;
        auto algorithmName = provenanceJson["ha"].get<std::string>();
        if (algorithmName == "blake3")
            algorithm = EvalTraceHashAlgorithm::Blake3;
        else if (algorithmName == "sha256")
            algorithm = EvalTraceHashAlgorithm::Sha256;
        else
            return std::nullopt;
    }
    if (algorithm != getEvalTraceHashAlgorithm())
        return std::nullopt;

    auto depPath = decodeResolvedDepPath(provenanceJson["d"]);
    if (!depPath)
        return std::nullopt;

    auto hash = Hash::parseNonSRIUnprefixed(
        provenanceJson["h"].get<std::string>(),
        toHashAlgorithm(algorithm));

    return TextObject{
        .source = std::move(depPath->source),
        .key = std::move(depPath->key),
        .contentHash = DepHash{EvalTraceHash::fromHash(hash)},
    };
}

nlohmann::json encodeStringPayload(const string_t & s)
{
    nlohmann::json payload{
        {"rendered", s.first},
        {"encoding", kSemanticResultEncodingVersion},
    };
    nlohmann::json publication = nlohmann::json::object();
    if (s.publication.path)
        publication["path"] = encodePathObject(*s.publication.path);
    if (s.publication.text)
        publication["readFile"] = encodeTextObject(*s.publication.text);
    if (s.publication.hasIdentity())
        publication["identity"] = s.publication.identity->stamp;
    if (!publication.empty())
        payload["publication"] = std::move(publication);
    return payload;
}

bool decodeStringPayload(std::string_view raw, string_t & result)
{
    auto payload = nlohmann::json::parse(std::string(raw));
    if (!payload.is_object() || !payload.contains("rendered") || !payload["rendered"].is_string())
        return false;

    result.first = payload["rendered"].get<std::string>();
    result.publication = SemanticHandle{};
    if (payload.contains("publication")) {
        auto & publication = payload["publication"];
        if (!publication.is_object())
            return false;
        if (publication.contains("path")) {
            auto origin = decodePathObject(publication["path"]);
            if (!origin)
                return false;
            result.publication.path = std::move(*origin);
        }
        if (publication.contains("readFile")) {
            auto provenance = decodeTextObject(publication["readFile"]);
            if (!provenance)
                return false;
            result.publication.text = std::move(*provenance);
        }
        // Set the kind discriminator based on which fields were decoded.
        if (result.publication.path && result.publication.text)
            result.publication.kind = SemanticKind::PathText;
        else if (result.publication.path)
            result.publication.kind = SemanticKind::Path;
        else if (result.publication.text)
            result.publication.kind = SemanticKind::Text;
        if (publication.contains("identity") && publication["identity"].is_number_unsigned())
            result.publication.identity = IdentityObject{publication["identity"].get<uint32_t>()};
    }

    return true;
}

nlohmann::json encodePathPayload(const path_t & p)
{
    nlohmann::json payload{
        {"rendered", p.path},
        {"encoding", kSemanticResultEncodingVersion},
    };
    nlohmann::json publication = nlohmann::json::object();
    if (p.publication.path)
        publication["path"] = encodePathObject(*p.publication.path);
    if (p.publication.hasIdentity())
        publication["identity"] = p.publication.identity->stamp;
    if (!publication.empty())
        payload["publication"] = std::move(publication);
    return payload;
}

bool decodePathPayload(std::string_view raw, path_t & result)
{
    auto payload = nlohmann::json::parse(std::string(raw));
    if (!payload.is_object() || !payload.contains("rendered") || !payload["rendered"].is_string())
        return false;

    result.path = payload["rendered"].get<std::string>();
    result.publication = SemanticHandle{};
    if (payload.contains("publication")) {
        auto & publication = payload["publication"];
        if (!publication.is_object())
            return false;
        if (publication.contains("path")) {
            auto origin = decodePathObject(publication["path"]);
            if (!origin)
                return false;
            result.publication.path = std::move(*origin);
            result.publication.kind = SemanticKind::Path;
        }
        if (publication.contains("identity") && publication["identity"].is_number_unsigned())
            result.publication.identity = IdentityObject{publication["identity"].get<uint32_t>()};
    }

    return true;
}

nlohmann::json encodeContainerMeta(const TracedContainerMeta & meta)
{
    nlohmann::json result = nlohmann::json::object();
    if (meta.producerOrigin)
        result["o"] = encodeStructuredObject(*meta.producerOrigin);
    if (meta.valueIdentityStamp)
        result["i"] = meta.valueIdentityStamp->value;
    return result;
}

bool decodeContainerMeta(const nlohmann::json & metaJson, TracedContainerMeta & result)
{
    if (!metaJson.is_object())
        return false;
    if (metaJson.contains("o")) {
        StructuredObject origin;
        if (!decodeStructuredObject(metaJson["o"], origin))
            return false;
        result.producerOrigin = std::move(origin);
    }
    if (metaJson.contains("i")) {
        if (!metaJson["i"].is_number_integer())
            return false;
        auto stamp = metaJson["i"].get<int64_t>();
        if (stamp < 0 || stamp > std::numeric_limits<uint32_t>::max())
            return false;
        result.valueIdentityStamp = ValueIdentityStamp(static_cast<uint32_t>(stamp));
    }
    return true;
}

nlohmann::json encodeAttrEntries(const attrs_t & attrs, AttrVocabStore & vocab)
{
    nlohmann::json entries = nlohmann::json::array();
    for (auto & entry : attrs.entries) {
        nlohmann::json entryJson{
            {"n", vocab.internName(entry.name).value},
        };
        if (entry.producerOrigin)
            entryJson["o"] = encodeStructuredObject(*entry.producerOrigin);
        if (entry.aliasOf != invalidSiblingIndex)
            entryJson["a"] = entry.aliasOf;
        entries.push_back(std::move(entryJson));
    }
    nlohmann::json payload{
        {"entries", std::move(entries)},
    };
    if (attrs.meta)
        payload["m"] = encodeContainerMeta(*attrs.meta);
    return payload;
}

bool decodeCanonicalAttrEntries(
    const nlohmann::json & payload,
    attrs_t & result,
    SymbolTable & symbols,
    AttrVocabStore & vocab)
{
    if (!payload.is_object() || !payload.contains("entries") || !payload["entries"].is_array())
        return false;
    if (payload.contains("m")) {
        TracedContainerMeta meta;
        if (!decodeContainerMeta(payload["m"], meta))
            return false;
        result.meta = std::move(meta);
    }
    for (auto & entryJson : payload["entries"]) {
        auto nameId = AttrNameId(entryJson["n"].get<uint32_t>());
        CachedAttrEntry entry{
            .name = symbols.create(vocab.resolveName(nameId)),
        };
        if (entryJson.contains("o")) {
            StructuredObject origin;
            if (!decodeStructuredObject(entryJson["o"], origin))
                return false;
            entry.producerOrigin = std::move(origin);
        }
        if (entryJson.contains("a"))
            entry.aliasOf = entryJson["a"].get<uint32_t>();
        result.entries.push_back(std::move(entry));
    }
    return true;
}

nlohmann::json encodeListEntries(const list_t & list)
{
    nlohmann::json entries = nlohmann::json::array();
    for (auto & entry : list.entries) {
        nlohmann::json entryJson = nlohmann::json::object();
        if (entry.aliasOf != invalidSiblingIndex)
            entryJson["a"] = entry.aliasOf;
        entries.push_back(std::move(entryJson));
    }
    nlohmann::json payload{{"entries", std::move(entries)}};
    if (list.meta)
        payload["m"] = encodeContainerMeta(*list.meta);
    return payload;
}

bool decodeCanonicalListEntries(const nlohmann::json & payload, list_t & result)
{
    if (!payload.is_object() || !payload.contains("entries") || !payload["entries"].is_array())
        return false;
    if (payload.contains("m")) {
        TracedContainerMeta meta;
        if (!decodeContainerMeta(payload["m"], meta))
            return false;
        result.meta = std::move(meta);
    }
    for (auto & entryJson : payload["entries"]) {
        CachedListEntry entry;
        if (entryJson.contains("a"))
            entry.aliasOf = entryJson["a"].get<uint32_t>();
        result.entries.push_back(std::move(entry));
    }
    return true;
}

} // namespace

// ── Result hashing ───────────────────────────────────────────────────

ResultHash computeResultHash(
    ResultKind type,
    uint32_t encodingVersion,
    std::string_view payload,
    std::string_view auxContext)
{
    auto builder = makeDomainBuilder<hash_domain::ResultHashV2>();
    builder.field("encoding-version", encodingVersion);
    builder.field("result-kind", type);
    builder.field("payload", payload);
    builder.field("aux-context", auxContext);
    return ResultHash{builder.finish()};
}

// ── CachedResult SQL encoding/decoding ──────────────────────────────────

EncodedResultPayload encodeCachedResult(const CachedResult & value, AttrVocabStore & vocab)
{
    return std::visit(overloaded{
        [&](const attrs_t & a) -> EncodedResultPayload {
            return {ResultKind::FullAttrs, kSemanticResultEncodingVersion, encodeAttrEntries(a, vocab).dump(), ""};
        },
        [&](const string_t & s) -> EncodedResultPayload {
            std::string ctx;
            bool first = true;
            for (auto & elem : s.second) {
                if (!first) ctx.push_back(' ');
                ctx.append(elem.to_string());
                first = false;
            }
            return {ResultKind::String, kSemanticResultEncodingVersion, encodeStringPayload(s).dump(), std::move(ctx)};
        },
        [&](const trivial_t & t) -> EncodedResultPayload {
            // 1-character sub-tag payload distinguishing the four former
            // trivial variants (P-RC in epoch v25).
            const char tag = [&] {
                switch (t.kind) {
                case TrivialKind::Placeholder: return 'p';
                case TrivialKind::Missing: return 'm';
                case TrivialKind::Misc: return 's';
                case TrivialKind::Null: return 'n';
                }
                unreachable();
            }();
            return {ResultKind::Trivial, kSemanticResultEncodingVersion, std::string(1, tag), ""};
        },
        [&](const failed_t & f) -> EncodedResultPayload {
            return {ResultKind::Failed, kSemanticResultEncodingVersion, f.errorMessage, ""};
        },
        [&](bool b) -> EncodedResultPayload {
            return {ResultKind::Bool, kSemanticResultEncodingVersion, b ? "1" : "0", ""};
        },
        [&](const int_t & i) -> EncodedResultPayload {
            return {ResultKind::Int, kSemanticResultEncodingVersion, std::to_string(i.x.value), ""};
        },
        [&](const path_t & p) -> EncodedResultPayload {
            return {ResultKind::Path, kSemanticResultEncodingVersion, encodePathPayload(p).dump(), ""};
        },
        [&](const float_t & f) -> EncodedResultPayload {
            // std::to_string uses %f (6 decimal places), losing precision.
            // Use std::to_chars for round-trip exact double formatting.
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), f.x);
            return {ResultKind::Float, kSemanticResultEncodingVersion, std::string(buf, ptr), ""};
        },
        [&](const list_t & lt) -> EncodedResultPayload {
            return {ResultKind::List, kSemanticResultEncodingVersion, encodeListEntries(lt).dump(), ""};
        },
    }, value);
}

CachedResult decodeCachedResult(const ResultPayload & payload, AttrVocabStore & vocab, SymbolTable & symbols)
{
    if (payload.encodingVersion != kSemanticResultEncodingVersion)
        throw Error(
            "unsupported eval trace result encoding version %d",
            payload.encodingVersion);

    switch (payload.type) {
    case ResultKind::FullAttrs: {
        attrs_t result;
        if (!payload.payload.empty()) {
            auto parsed = nlohmann::json::parse(payload.payload);
            if (!decodeCanonicalAttrEntries(parsed, result, symbols, vocab))
                throw Error("invalid canonical FullAttrs payload in trace result");
        }
        return result;
    }
    case ResultKind::String: {
        string_t result;
        if (!payload.auxContext.empty()) {
            for (auto & elem : tokenizeString<std::vector<std::string>>(payload.auxContext, " "))
                result.second.insert(NixStringContextElem::parse(elem));
        }
        if (!decodeStringPayload(payload.payload, result))
            throw Error("invalid String payload in trace result");
        return result;
    }
    case ResultKind::Bool:
        return payload.payload != "0";
    case ResultKind::Int:
        return int_t{NixInt{payload.payload.empty() ? 0L : std::stol(payload.payload)}};
    case ResultKind::Path:
    {
        path_t result;
        if (!decodePathPayload(payload.payload, result))
            throw Error("invalid Path payload in trace result");
        return result;
    }
    case ResultKind::Float:
        return float_t{payload.payload.empty() ? 0.0 : std::stod(payload.payload)};
    case ResultKind::List: {
        list_t result;
        if (!payload.payload.empty()) {
            auto parsed = nlohmann::json::parse(payload.payload);
            if (!decodeCanonicalListEntries(parsed, result))
                throw Error("invalid canonical List payload in trace result");
        }
        return result;
    }
    case ResultKind::Trivial:
        // Sub-tag is a 1-char payload.  Must be one of "p","m","s","n".
        if (payload.payload.size() != 1)
            throw Error("invalid Trivial payload length (expected 1 char)");
        switch (payload.payload[0]) {
        case 'p': return makePlaceholder();
        case 'm': return makeMissing();
        case 's': return makeMisc();
        case 'n': return makeNull();
        default:
            throw Error("invalid Trivial sub-tag '%c'", payload.payload[0]);
        }
    case ResultKind::Failed:
        return failed_t{.errorMessage = payload.payload};
    }
    unreachable();
}

// Back-compat shims for `SqliteTraceStorage::encodeCachedResult` and the
// value-taking `SqliteTraceStorage::decodeCachedResult(payload)` are inline in
// trace-store.hh. The `(bs, resultId)` overload below is a real member
// function: it loads the payload from the SQLite-backed result cache
// and is not just a forwarding shim.

CachedResult SqliteTraceStorage::decodeCachedResult(const gdp::Proof<BlockingTag> & bs, ResultId resultId)
{
    return eval_trace::decodeCachedResult(loadResultPayloadCached(bs, resultId), vocab, symbols);
}

} // namespace nix::eval_trace
