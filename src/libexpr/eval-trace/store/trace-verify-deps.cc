#include "trace-verify-deps.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/store/store-api.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/source-accessor.hh"
#include "nix/fetchers/fetchers.hh"

#include "expr-config-private.hh"

#include <nlohmann/json.hpp>
#include <toml.hpp>

#include <sstream>
#include <unordered_map>

namespace nix::eval_trace {

// ── VerificationScope ────────────────────────────────────────────────

struct VerificationScope {
    std::unordered_map<std::string, nlohmann::json> jsonDomCache;
    std::unordered_map<std::string, toml::value> tomlDomCache;
    std::unordered_map<std::string, SourceAccessor::DirEntries> dirListingCache;
    /// Per-file Nix AST cache: "source\tfilePath" → {bindingName → BLAKE3 hash}.
    /// Avoids re-parsing when multiple NixBinding deps reference the same file.
    std::unordered_map<std::string,
        std::unordered_map<std::string, Blake3Hash>> nixAstCache;
};

void VerificationScopeDeleter::operator()(VerificationScope * p) const noexcept
{
    delete p;
}

VerificationScopePtr createVerificationScope()
{
    return VerificationScopePtr(new VerificationScope());
}

// ── Trace verification helpers (BSàlC verifying trace check) ─────────

static std::optional<SourcePath> resolveDepPath(
    const TraceStore::ResolvedDep & dep, const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors)
{
    if (dep.source == absolutePathDep)
        return SourcePath(getFSSourceAccessor(), CanonPath(dep.key));
    auto it = inputAccessors.find(dep.source);
    if (it != inputAccessors.end())
        return it->second / CanonPath(dep.key);
    if (dep.source.empty())
        return SourcePath(getFSSourceAccessor(), CanonPath(dep.key));
    return std::nullopt;
}

// ── Structured content navigation helpers ────────────────────────────

/**
 * Navigate a JSON DOM using a JSON path array. Returns nullptr if path is invalid.
 * Path components: strings for object keys, numbers for array indices.
 */
static const nlohmann::json * navigateJson(const nlohmann::json & root, const nlohmann::json & pathArray)
{
    const nlohmann::json * node = &root;
    for (auto & component : pathArray) {
        if (component.is_number()) {
            if (!node->is_array()) return nullptr;
            auto idx = component.get<size_t>();
            if (idx >= node->size()) return nullptr;
            node = &(*node)[idx];
        } else {
            if (!node->is_object()) return nullptr;
            auto key = component.get<std::string>();
            auto it = node->find(key);
            if (it == node->end()) return nullptr;
            node = &*it;
        }
    }
    return node;
}

/**
 * Navigate a TOML DOM using a JSON path array. Returns nullptr if path is invalid.
 */
static const toml::value * navigateToml(const toml::value & root, const nlohmann::json & pathArray)
{
    const toml::value * node = &root;
    for (auto & component : pathArray) {
        if (component.is_number()) {
            if (!node->is_array()) return nullptr;
            auto idx = component.get<size_t>();
            auto & arr = toml::get<std::vector<toml::value>>(*node);
            if (idx >= arr.size()) return nullptr;
            node = &arr[idx];
        } else {
            if (!node->is_table()) return nullptr;
            auto key = component.get<std::string>();
            auto & table = toml::get<toml::table>(*node);
            auto it = table.find(key);
            if (it == table.end()) return nullptr;
            node = &it->second;
        }
    }
    return node;
}

/**
 * Canonical string form of a TOML scalar value for hashing.
 * Must match TomlDataNode::canonicalValue() in fromTOML.cc.
 */
static std::string tomlCanonical(const toml::value & v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

// ── computeCurrentHash ───────────────────────────────────────────────

std::optional<DepHashValue> computeCurrentHash(
    EvalState & state, const TraceStore::ResolvedDep & dep,
    const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
    VerificationScope & scope,
    const boost::unordered_flat_map<std::string, std::string> & dirSets)
{
    switch (dep.type) {
    case DepType::Content:
    case DepType::RawContent: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(StatHashStore::instance().depHashFile(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::NARContent: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(StatHashStore::instance().depHashPathCached(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::Directory: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(StatHashStore::instance().depHashDirListingCached(*path, path->readDirectory()));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::Existence: {
        auto path = resolveDepPath(dep, inputAccessors);
        if (!path) return std::nullopt;
        auto st = path->maybeLstat();
        return DepHashValue(st
            ? fmt("type:%d", static_cast<int>(st->type))
            : std::string("missing"));
    }
    case DepType::EnvVar:
        return DepHashValue(depHash(getEnv(dep.key).value_or("")));
    case DepType::System:
        return DepHashValue(depHash(state.settings.getCurrentSystem()));
    case DepType::CopiedPath: {
        auto sourcePath = resolveDepPath(dep, inputAccessors);
        if (!sourcePath) return std::nullopt;
        try {
            auto * storePathStr = std::get_if<std::string>(&dep.expectedHash);
            if (!storePathStr) return std::nullopt;
            auto expectedStorePath = state.store->parseStorePath(*storePathStr);
            auto name2 = std::string(expectedStorePath.name());
            auto [storePath, hash] = state.store->computeStorePath(
                name2,
                sourcePath->resolveSymlinks(SymlinkResolution::Ancestors),
                ContentAddressMethod::Raw::NixArchive,
                HashAlgorithm::SHA256, {});
            return DepHashValue(state.store->printStorePath(storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::UnhashedFetch: {
        try {
            auto input = fetchers::Input::fromURL(state.fetchSettings, dep.key);
            auto [storePath, lockedInput] = input.fetchToStore(
                state.fetchSettings, *state.store);
            return DepHashValue(state.store->printStorePath(storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::ImplicitShape:
        // ImplicitShape uses the same key format and hash computation as
        // StructuredContent. Verified only for failed sources without SC deps.
        [[fallthrough]];
    case DepType::StructuredContent: {
        // Key format: JSON object {"f":"path","t":"j","p":[...],"s":"keys","h":"key"}
        // Or aggregated DirSet: {"ds":"<hex>","h":"keyName","t":"d"} (dirs in DirSets table)
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(dep.key);
        } catch (...) {
            return std::nullopt;
        }

        // Aggregated DirSet dep: iterate all directories, check each for the key
        if (j.contains("ds")) {
            auto dsHash = j.value("ds", "");
            auto hasKeyName = j.value("h", "");
            if (hasKeyName.empty()) return std::nullopt;

            auto it = dirSets.find(dsHash);
            if (it == dirSets.end()) return std::nullopt;
            auto dirs = nlohmann::json::parse(it->second);
            for (auto & dir : dirs) {
                if (!dir.is_array() || dir.size() != 2) continue;
                auto source = dir[0].get<std::string>();
                auto filePath = dir[1].get<std::string>();
                TraceStore::ResolvedDep fileDep{source, filePath, DepHashValue{Blake3Hash{}}, DepType::Directory};
                auto path = resolveDepPath(fileDep, inputAccessors);
                if (!path) continue;
                try {
                    auto cacheKey = source + '\t' + filePath;
                    auto cacheIt = scope.dirListingCache.find(cacheKey);
                    if (cacheIt == scope.dirListingCache.end())
                        cacheIt = scope.dirListingCache.emplace(cacheKey, path->readDirectory()).first;
                    if (cacheIt->second.count(hasKeyName))
                        return DepHashValue(depHash("1")); // key found in this dir
                } catch (...) {
                    continue;
                }
            }
            return DepHashValue(depHash("0")); // key absent in all dirs
        }

        auto filePath = j.value("f", "");
        auto formatStr = j.value("t", "");
        if (formatStr.empty()) return std::nullopt;
        auto format = parseStructuredFormat(formatStr[0]);
        if (!format) return std::nullopt;
        auto pathArray = j.value("p", nlohmann::json::array());
        auto hasKeyName = j.value("h", "");
        bool isHasKey = !hasKeyName.empty();
        auto parseShapeName = [](const std::string & name) -> ShapeSuffix {
            if (name == "len") return ShapeSuffix::Len;
            if (name == "keys") return ShapeSuffix::Keys;
            if (name == "type") return ShapeSuffix::Type;
            return ShapeSuffix::None;
        };
        auto shape = parseShapeName(j.value("s", ""));

        // Construct a synthetic Content dep to resolve the file path
        TraceStore::ResolvedDep fileDep{dep.source, filePath, DepHashValue{Blake3Hash{}}, DepType::Content};
        auto path = resolveDepPath(fileDep, inputAccessors);
        if (!path) return std::nullopt;

        try {
            // Helper: compute hash for sorted key set (shared across formats)
            auto hashSortedKeys = [](std::vector<std::string> keys) -> DepHashValue {
                std::sort(keys.begin(), keys.end());
                std::string canonical;
                for (size_t i = 0; i < keys.size(); i++) {
                    if (i > 0) canonical += '\0';
                    canonical += keys[i];
                }
                return DepHashValue(depHash(canonical));
            };

            switch (*format) {
            case StructuredFormat::Json: {
                // Use DOM cache to avoid re-parsing
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = scope.jsonDomCache.find(cacheKey);
                if (cacheIt == scope.jsonDomCache.end()) {
                    auto contents = path->readFile();
                    cacheIt = scope.jsonDomCache.emplace(cacheKey, nlohmann::json::parse(contents)).first;
                }
                auto * node = navigateJson(cacheIt->second, pathArray);
                if (!node) return std::nullopt;
                switch (shape) {
                case ShapeSuffix::Len:
                    if (!node->is_array()) return std::nullopt;
                    return DepHashValue(depHash(std::to_string(node->size())));
                case ShapeSuffix::Keys: {
                    if (!node->is_object()) return std::nullopt;
                    std::vector<std::string> keys;
                    for (auto & [k, _] : node->items())
                        keys.push_back(k);
                    return hashSortedKeys(std::move(keys));
                }
                case ShapeSuffix::Type:
                    if (node->is_object()) return DepHashValue(depHash("object"));
                    if (node->is_array()) return DepHashValue(depHash("array"));
                    return std::nullopt; // scalar — type changed from container
                case ShapeSuffix::None:
                    if (isHasKey) {
                        if (!node->is_object()) return std::nullopt;
                        return DepHashValue(depHash(node->contains(hasKeyName) ? "1" : "0"));
                    }
                    return DepHashValue(depHash(node->dump()));
                }
                break;
            }
            case StructuredFormat::Toml: {
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = scope.tomlDomCache.find(cacheKey);
                if (cacheIt == scope.tomlDomCache.end()) {
                    auto contents = path->readFile();
                    std::istringstream stream(std::move(contents));
                    cacheIt = scope.tomlDomCache.emplace(cacheKey, toml::parse(
                        stream, "verifyTrace"
#if HAVE_TOML11_4
                        , toml::spec::v(1, 0, 0)
#endif
                    )).first;
                }
                auto * node = navigateToml(cacheIt->second, pathArray);
                if (!node) return std::nullopt;
                switch (shape) {
                case ShapeSuffix::Len:
                    if (!node->is_array()) return std::nullopt;
                    return DepHashValue(depHash(std::to_string(toml::get<std::vector<toml::value>>(*node).size())));
                case ShapeSuffix::Keys: {
                    if (!node->is_table()) return std::nullopt;
                    auto & table = toml::get<toml::table>(*node);
                    std::vector<std::string> keys;
                    for (auto & [k, _] : table)
                        keys.push_back(k);
                    return hashSortedKeys(std::move(keys));
                }
                case ShapeSuffix::Type:
                    if (node->is_table()) return DepHashValue(depHash("object"));
                    if (node->is_array()) return DepHashValue(depHash("array"));
                    return std::nullopt;
                case ShapeSuffix::None:
                    if (isHasKey) {
                        if (!node->is_table()) return std::nullopt;
                        auto & table = toml::get<toml::table>(*node);
                        return DepHashValue(depHash(table.count(hasKeyName) ? "1" : "0"));
                    }
                    return DepHashValue(depHash(tomlCanonical(*node)));
                }
                break;
            }
            case StructuredFormat::Directory: {
                // Directory structural dep: re-read listing, look up entry
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = scope.dirListingCache.find(cacheKey);
                if (cacheIt == scope.dirListingCache.end()) {
                    auto dirEntries = path->readDirectory();
                    cacheIt = scope.dirListingCache.emplace(cacheKey, std::move(dirEntries)).first;
                }
                auto & entries = cacheIt->second;

                switch (shape) {
                case ShapeSuffix::Len:
                    return DepHashValue(depHash(std::to_string(entries.size())));
                case ShapeSuffix::Keys: {
                    // std::map is already sorted by key
                    std::string canonical;
                    bool first = true;
                    for (auto & [k, _] : entries) {
                        if (!first) canonical += '\0';
                        canonical += k;
                        first = false;
                    }
                    return DepHashValue(depHash(canonical));
                }
                case ShapeSuffix::Type:
                    // Directories are always "object" (key→type mapping)
                    return DepHashValue(depHash("object"));
                case ShapeSuffix::None: {
                    if (isHasKey) {
                        return DepHashValue(depHash(entries.count(hasKeyName) ? "1" : "0"));
                    }
                    // Directory scalar: pathArray should have exactly one string component
                    if (pathArray.size() != 1 || !pathArray[0].is_string()) return std::nullopt;
                    auto it = entries.find(pathArray[0].get<std::string>());
                    if (it == entries.end()) return std::nullopt;
                    return DepHashValue(depHash(dirEntryTypeString(it->second)));
                }
                }
                break;
            }
            case StructuredFormat::Nix: {
                // Nix AST structural dep: re-parse .nix file, compute per-binding hash.
                // pathArray has one element: the binding name.
                if (pathArray.size() != 1 || !pathArray[0].is_string())
                    return std::nullopt;
                auto bindingName = pathArray[0].get<std::string>();
                auto cacheKey = dep.source + '\t' + filePath;
                auto cacheIt = scope.nixAstCache.find(cacheKey);
                if (cacheIt == scope.nixAstCache.end()) {
                    auto source = path->readFile();
                    auto ast = state.parseExprFromString(
                        std::move(source), state.rootPath(CanonPath(filePath)));
                    auto [exprAttrs, scopeExprs] = findNonRecExprAttrs(ast);
                    std::unordered_map<std::string, Blake3Hash> hashes;
                    if (exprAttrs) {
                        auto scopeHash = computeNixScopeHash(scopeExprs, state.symbols);
                        for (auto & [sym, def] : *exprAttrs->attrs) {
                            auto name = std::string(state.symbols[sym]);
                            // Resolve InheritedFrom → source expression, then hash.
                            // Must use the same function as registerNixBindings.
                            const Expr * exprToShow = def.e;
                            if (def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom) {
                                auto * iv = dynamic_cast<ExprInheritFrom *>(def.e);
                                if (iv && exprAttrs->inheritFromExprs
                                    && static_cast<size_t>(iv->displ) < exprAttrs->inheritFromExprs->size())
                                    exprToShow = (*exprAttrs->inheritFromExprs)[iv->displ];
                                else
                                    exprToShow = nullptr;
                            }
                            hashes[name] = computeNixBindingHash(
                                scopeHash, name, static_cast<int>(def.kind),
                                exprToShow, state.symbols);
                        }
                    }
                    cacheIt = scope.nixAstCache.emplace(cacheKey, std::move(hashes)).first;
                }
                auto & hashes = cacheIt->second;
                if (hashes.empty()) return std::nullopt;  // Structure not eligible
                auto it = hashes.find(bindingName);
                if (it == hashes.end()) return std::nullopt;  // Binding removed
                return DepHashValue(it->second);
            }
            }
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }
    case DepType::StorePathExistence: {
        try {
            auto storePath = state.store->parseStorePath(dep.key);
            return DepHashValue(state.store->isValidPath(storePath)
                ? std::string("valid") : std::string("missing"));
        } catch (std::exception &) {
            return DepHashValue(std::string("missing"));
        }
    }
    case DepType::CurrentTime:
    case DepType::Exec:
    case DepType::ParentContext:
    case DepType::EndSentinel_:
        return std::nullopt;
    }
    unreachable();
}

} // namespace nix::eval_trace
