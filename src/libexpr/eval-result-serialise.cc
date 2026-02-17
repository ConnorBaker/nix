#include "nix/expr/eval-result-serialise.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/compression.hh"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace nix::eval_cache {

std::vector<uint8_t> serializeAttrValue(const AttrValue & value, SymbolTable & symbols)
{
    nlohmann::json j;
    j["v"] = 1; // format version

    std::visit(overloaded{
        [&](const std::vector<Symbol> & attrs) {
            j["t"] = AttrType::FullAttrs;
            nlohmann::json names = nlohmann::json::array();
            for (auto & sym : attrs)
                names.push_back(std::string(symbols[sym]));
            j["n"] = std::move(names);
        },
        [&](const string_t & s) {
            j["t"] = AttrType::String;
            j["s"] = s.first;
            if (!s.second.empty()) {
                std::string ctx;
                bool first = true;
                for (auto & elem : s.second) {
                    if (!first) ctx.push_back(' ');
                    ctx.append(elem.to_string());
                    first = false;
                }
                j["c"] = std::move(ctx);
            }
        },
        [&](const placeholder_t &) {
            j["t"] = AttrType::Placeholder;
        },
        [&](const missing_t &) {
            j["t"] = AttrType::Missing;
        },
        [&](const misc_t &) {
            j["t"] = AttrType::Misc;
        },
        [&](const failed_t &) {
            j["t"] = AttrType::Failed;
        },
        [&](bool b) {
            j["t"] = AttrType::Bool;
            j["s"] = b ? "1" : "0";
        },
        [&](const int_t & i) {
            j["t"] = AttrType::Int;
            j["s"] = std::to_string(i.x.value);
        },
        [&](const std::vector<std::string> & l) {
            j["t"] = AttrType::ListOfStrings;
            // Tab-separated, matching existing SQLite encoding
            std::string val;
            bool first = true;
            for (auto & s : l) {
                if (!first) val.push_back('\t');
                val.append(s);
                first = false;
            }
            j["s"] = std::move(val);
        },
        [&](const path_t & p) {
            j["t"] = AttrType::Path;
            j["s"] = p.path;
        },
        [&](const null_t &) {
            j["t"] = AttrType::Null;
        },
        [&](const float_t & f) {
            j["t"] = AttrType::Float;
            j["s"] = std::to_string(f.x);
        },
        [&](const list_t & lt) {
            j["t"] = AttrType::List;
            j["l"] = lt.size;
        },
    }, value);

    return nlohmann::json::to_cbor(j);
}

AttrValue deserializeAttrValue(const std::vector<uint8_t> & cbor, SymbolTable & symbols)
{
    auto j = nlohmann::json::from_cbor(cbor);

    auto version = j.value("v", 0);
    if (version != 1)
        throw Error("unsupported eval cache result format version %d", version);

    auto type = static_cast<AttrType>(j.at("t").get<int>());

    switch (type) {
    case AttrType::FullAttrs: {
        std::vector<Symbol> attrs;
        for (auto & name : j.at("n"))
            attrs.push_back(symbols.create(name.get<std::string>()));
        return attrs;
    }
    case AttrType::String: {
        auto s = j.value("s", "");
        NixStringContext context;
        if (j.contains("c")) {
            auto ctxStr = j.at("c").get<std::string>();
            for (auto & elem : tokenizeString<std::vector<std::string>>(ctxStr, " "))
                context.insert(NixStringContextElem::parse(elem));
        }
        return string_t{std::move(s), std::move(context)};
    }
    case AttrType::Bool:
        return j.value("s", "0") != "0";
    case AttrType::Int:
        return int_t{NixInt{std::stol(j.value("s", "0"))}};
    case AttrType::ListOfStrings:
        return tokenizeString<std::vector<std::string>>(j.value("s", ""), "\t");
    case AttrType::Path:
        return path_t{j.value("s", "")};
    case AttrType::Null:
        return null_t{};
    case AttrType::Float:
        return float_t{std::stod(j.value("s", "0"))};
    case AttrType::List:
        return list_t{j.value("l", (size_t) 0)};
    case AttrType::Missing:
        return missing_t{};
    case AttrType::Misc:
        return misc_t{};
    case AttrType::Failed:
        return failed_t{};
    case AttrType::Placeholder:
        return placeholder_t{};
    default:
        throw Error("unexpected type %d in eval cache result", static_cast<int>(type));
    }
}

// ── Hash value serialization helpers ─────────────────────────────────

static nlohmann::json hashValueToJson(const DepHashValue & v)
{
    return std::visit(overloaded{
        [](const Blake3Hash & h) -> nlohmann::json {
            // Store as binary (CBOR bytes)
            return nlohmann::json::binary(
                std::vector<uint8_t>(h.bytes.begin(), h.bytes.end()));
        },
        [](const std::string & s) -> nlohmann::json { return s; },
    }, v);
}

static DepHashValue hashValueFromJson(const nlohmann::json & j, DepType type)
{
    if (j.is_binary()) {
        auto & bytes = j.get_binary();
        Blake3Hash h;
        if (bytes.size() == 32)
            std::memcpy(h.bytes.data(), bytes.data(), 32);
        return h;
    }
    if (j.is_string()) {
        auto s = j.get<std::string>();
        if (isBlake3Dep(type) && s.size() == 64) {
            Blake3Hash h;
            for (size_t i = 0; i < 32; i++) {
                auto hi = std::stoul(s.substr(i * 2, 2), nullptr, 16);
                h.bytes[i] = static_cast<uint8_t>(hi);
            }
            return h;
        }
        return s;
    }
    return std::string{};
}

// ── Shared dep sort+dedup ────────────────────────────────────────────

std::vector<Dep> sortAndDedupDeps(const std::vector<Dep> & deps)
{
    auto sorted = deps;
    std::sort(sorted.begin(), sorted.end(),
        [](const Dep & a, const Dep & b) {
            if (a.type != b.type) return a.type < b.type;
            if (a.source != b.source) return a.source < b.source;
            return a.key < b.key;
        });
    sorted.erase(std::unique(sorted.begin(), sorted.end(),
        [](const Dep & a, const Dep & b) {
            return a.type == b.type && a.source == b.source && a.key == b.key;
        }), sorted.end());
    return sorted;
}

// ── Shared dep-to-JSON helper ───────────────────────────────────────

static nlohmann::json buildDepsJsonArray(
    const std::vector<Dep> & sortedDeps, bool includeHash)
{
    auto depsArr = nlohmann::json::array();
    for (auto & dep : sortedDeps) {
        nlohmann::json d;
        d["t"] = static_cast<int>(dep.type);
        d["s"] = dep.source;
        d["k"] = dep.key;
        if (includeHash)
            d["h"] = hashValueToJson(dep.expectedHash);
        depsArr.push_back(std::move(d));
    }
    return depsArr;
}

// ── Dep set blob serialization (zstd-compressed CBOR) ────────────────

/**
 * Fixed zstd compression level for CAS determinism.
 * Same input → same compressed output → same store path.
 * Level 3 is the zstd default; good balance of speed vs ratio.
 */
static constexpr int zstdCompressionLevel = 3;

std::vector<uint8_t> serializeDepSet(const std::vector<Dep> & sortedDeps)
{
    auto cbor = nlohmann::json::to_cbor(buildDepsJsonArray(sortedDeps, true));
    auto compressed = compress(
        CompressionAlgo::zstd,
        std::string_view(reinterpret_cast<const char *>(cbor.data()), cbor.size()),
        false, zstdCompressionLevel);
    return std::vector<uint8_t>(compressed.begin(), compressed.end());
}

std::vector<Dep> deserializeDepSet(const std::vector<uint8_t> & compressed)
{
    auto decompressed = decompress(
        "zstd",
        std::string_view(reinterpret_cast<const char *>(compressed.data()), compressed.size()));
    std::vector<uint8_t> cbor(decompressed.begin(), decompressed.end());
    auto j = nlohmann::json::from_cbor(cbor);

    std::vector<Dep> deps;
    if (j.is_array()) {
        for (auto & d : j) {
            auto type = static_cast<DepType>(d.at("t").get<int>());
            deps.push_back(Dep{
                d.value("s", ""),
                d.value("k", ""),
                hashValueFromJson(d.at("h"), type),
                type,
            });
        }
    }
    return deps;
}

// ── Result trace serialization (v2) ──────────────────────────────────

std::vector<uint8_t> serializeEvalTrace(
    const AttrValue & result,
    const std::optional<StorePath> & parent,
    const std::optional<int64_t> & contextHash,
    const StorePath & depSetPath,
    SymbolTable & symbols,
    const StoreDirConfig & store)
{
    nlohmann::json j;
    j["v"] = 2; // trace format version

    // Result: nested CBOR bytes (reuse existing serialization)
    j["r"] = nlohmann::json::binary(serializeAttrValue(result, symbols));

    // Parent trace path
    if (parent)
        j["p"] = store.printStorePath(*parent);
    else
        j["p"] = nullptr;

    // Context hash (root only)
    if (contextHash)
        j["c"] = *contextHash;
    else
        j["c"] = nullptr;

    // Dep set blob store path
    j["ds"] = store.printStorePath(depSetPath);

    return nlohmann::json::to_cbor(j);
}

EvalTrace deserializeEvalTrace(
    const std::vector<uint8_t> & cbor,
    SymbolTable & symbols,
    const StoreDirConfig & store)
{
    auto j = nlohmann::json::from_cbor(cbor);

    auto version = j.value("v", 0);
    if (version != 2)
        throw Error("unsupported eval trace format version %d (expected 2)", version);

    // Dep set path (required)
    auto depSetPath = store.parseStorePath(j.at("ds").get<std::string>());

    // Result
    AttrValue result;
    if (j.contains("r") && j["r"].is_binary()) {
        auto & resultCbor = j["r"].get_binary();
        std::vector<uint8_t> resultBytes(resultCbor.begin(), resultCbor.end());
        result = deserializeAttrValue(resultBytes, symbols);
    } else {
        result = failed_t{};
    }

    // Parent
    std::optional<StorePath> parent;
    if (j.contains("p") && !j["p"].is_null())
        parent = store.parseStorePath(j["p"].get<std::string>());

    // Context hash
    std::optional<int64_t> contextHash;
    if (j.contains("c") && !j["c"].is_null())
        contextHash = j["c"].get<int64_t>();

    return EvalTrace{
        std::move(result),
        std::move(parent),
        std::move(contextHash),
        std::move(depSetPath),
    };
}

// ── Dep content hash ────────────────────────────────────────────────

Hash computeDepContentHashFromSorted(const std::vector<Dep> & sortedDeps)
{
    auto cbor = nlohmann::json::to_cbor(buildDepsJsonArray(sortedDeps, true));
    return hashString(HashAlgorithm::SHA256,
        {reinterpret_cast<const char *>(cbor.data()), cbor.size()});
}

Hash computeDepContentHash(const std::vector<Dep> & deps)
{
    return computeDepContentHashFromSorted(sortAndDedupDeps(deps));
}

Hash computeDepStructHashFromSorted(const std::vector<Dep> & sortedDeps)
{
    auto cbor = nlohmann::json::to_cbor(buildDepsJsonArray(sortedDeps, false));
    return hashString(HashAlgorithm::SHA256,
        {reinterpret_cast<const char *>(cbor.data()), cbor.size()});
}

Hash computeDepStructHash(const std::vector<Dep> & deps)
{
    return computeDepStructHashFromSorted(sortAndDedupDeps(deps));
}

Hash computeDepContentHashWithParentFromSorted(
    const std::vector<Dep> & sortedDeps,
    const StorePath & parent,
    const StoreDirConfig & store)
{
    auto cbor = nlohmann::json::to_cbor(buildDepsJsonArray(sortedDeps, true));

    // Append parent identity after CBOR bytes (not inside CBOR structure).
    // "P" prefix provides domain separation from plain CBOR content.
    std::string parentStr = "P" + store.printStorePath(parent);
    cbor.insert(cbor.end(), parentStr.begin(), parentStr.end());

    return hashString(HashAlgorithm::SHA256,
        {reinterpret_cast<const char *>(cbor.data()), cbor.size()});
}

Hash computeDepContentHashWithParent(
    const std::vector<Dep> & deps,
    const StorePath & parent,
    const StoreDirConfig & store)
{
    return computeDepContentHashWithParentFromSorted(sortAndDedupDeps(deps), parent, store);
}

} // namespace nix::eval_cache
