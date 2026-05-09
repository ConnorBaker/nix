#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json.hpp>
#include <assert.h>
#include <boost/unordered/unordered_flat_set_fwd.hpp>
#include <nlohmann/detail/iterators/iter_impl.hpp>
#include <nlohmann/detail/iterators/iteration_proxy.hpp>
#include <nlohmann/json_fwd.hpp>
#include <algorithm>
#include <iomanip>
#include <iterator>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "nix/fetchers/fetch-settings.hh"
#include "nix/flake/lockfile.hh"
#include "nix/util/strings.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/flake/flakeref.hh"
#include "nix/store/path.hh"
#include "nix/util/ansicolor.hh"
#include "nix/util/error.hh"
#include "nix/util/fmt.hh"
#include "nix/util/logging.hh"
#include "nix/util/ref.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"

namespace nix {
class Store;
} // namespace nix

namespace nix::flake {

static FlakeGraphNodeKey makeFlakeGraphNodeKey(std::string_view value)
{
    return FlakeGraphNodeKey{std::string(value)};
}

static FlakeGraphNodeKey appendFlakeGraphNodeKeySuffix(const FlakeGraphNodeKey & key, int suffix)
{
    return FlakeGraphNodeKey{fmt("%s_%d", key.value, suffix)};
}

static FlakeRef
getFlakeRef(const fetchers::Settings & fetchSettings, const nlohmann::json & json, const char * attr, const char * info)
{
    auto i = json.find(attr);
    if (i != json.end()) {
        auto attrs = fetchers::jsonToAttrs(*i);
        // FIXME: remove when we drop support for version 5.
        if (info) {
            auto j = json.find(info);
            if (j != json.end()) {
                for (auto k : fetchers::jsonToAttrs(*j))
                    attrs.insert_or_assign(k.first, k.second);
            }
        }
        return FlakeRef::fromAttrs(fetchSettings, attrs).canonicalize();
    }

    throw Error("attribute '%s' missing in lock file", attr);
}

LockedNode::LockedNode(const fetchers::Settings & fetchSettings, const nlohmann::json & json)
    : lockedRef(EvaluationLockedFlakeRef{getFlakeRef(fetchSettings, json, "locked", "info")}) // FIXME: remove "info"
    , originalRef(OriginalFlakeRef{getFlakeRef(fetchSettings, json, "original", nullptr)})
    , isFlake(json.find("flake") != json.end() ? (bool) json["flake"] : true)
    , parentInputAttrPath(
          json.find("parent") != json.end() ? (std::optional<InputAttrPath>) json["parent"] : std::nullopt)
{
    if (!lockedRef.value.input.isLocked(fetchSettings) && !lockedRef.value.input.isRelative()) {
        if (lockedRef.value.input.getNarHash())
            warn(
                "Lock file entry '%s' is unlocked (e.g. lacks a Git revision) but is checked by NAR hash. "
                "This is not reproducible and will break after garbage collection or when shared.",
                lockedRef.value.to_string());
        else
            throw Error(
                "Lock file contains unlocked input '%s'. Use '--allow-dirty-locks' to accept this lock file.",
                fetchers::attrsToJSON(lockedRef.value.input.toAttrs()));
    }

    // For backward compatibility, lock file entries are implicitly final.
    assert(!lockedRef.value.input.attrs.contains("__final"));
    lockedRef.value.input.attrs.insert_or_assign("__final", Explicit<bool>(true));
}

StorePath LockedNode::computeStorePath(Store & store) const
{
    return lockedRef.value.input.computeStorePath(store);
}

static std::shared_ptr<Node>
doFind(const ref<Node> & root, const InputAttrPath & path, std::vector<InputAttrPath> & visited)
{
    auto pos = root;

    auto found = std::find(visited.cbegin(), visited.cend(), path);

    if (found != visited.end()) {
        std::vector<std::string> cycle;
        std::transform(found, visited.cend(), std::back_inserter(cycle), printInputAttrPath);
        cycle.push_back(printInputAttrPath(path));
        throw Error("follow cycle detected: [%s]", concatStringsSep(" -> ", cycle));
    }
    visited.push_back(path);

    for (auto & elem : path) {
        if (auto i = get(pos->inputs, elem)) {
            if (auto node = edgeChild(*i))
                pos = *node;
            else if (auto follows = edgeFollows(*i)) {
                if (auto p = doFind(root, *follows, visited))
                    pos = ref(p);
                else
                    return {};
            }
        } else
            return {};
    }

    return pos;
}

std::shared_ptr<Node> LockFile::findInput(const InputAttrPath & path)
{
    std::vector<InputAttrPath> visited;
    return doFind(root, path, visited);
}

LockFile::LockFile(const fetchers::Settings & fetchSettings, std::string_view contents, std::string_view path)
{
    auto json = [=] {
        try {
            return nlohmann::json::parse(contents);
        } catch (const nlohmann::json::parse_error & e) {
            throw Error("Could not parse '%s': %s", path, e.what());
        }
    }();
    auto version = json.value("version", 0);
    if (version < 5 || version > 7)
        throw Error("lock file '%s' has unsupported version %d", path, version);

    FlakeGraphNodeKey rootKey = makeFlakeGraphNodeKey(json["root"].get<std::string>());
    std::map<FlakeGraphNodeKey, ref<Node>> nodeMap{{rootKey, root}};

    [&](this const auto & getInputs, Node & node, const nlohmann::json & jsonNode) {
        if (jsonNode.find("inputs") == jsonNode.end())
            return;
        for (auto & i : jsonNode["inputs"].items()) {
            if (i.value().is_array()) { // FIXME: remove, obsolete
                InputAttrPath path;
                for (auto & j : i.value())
                    path.push_back(j);
                node.inputs.insert_or_assign(i.key(), path);
            } else {
                FlakeGraphNodeKey inputKey = makeFlakeGraphNodeKey(i.value().get<std::string>());
                auto k = nodeMap.find(inputKey);
                if (k == nodeMap.end()) {
                    auto & nodes = json["nodes"];
                    auto jsonNode2 = nodes.find(inputKey.value);
                    if (jsonNode2 == nodes.end())
                        throw Error("lock file references missing node '%s'", inputKey);
                    auto input = make_ref<LockedNode>(fetchSettings, *jsonNode2);
                    k = nodeMap.insert_or_assign(inputKey, input).first;
                    getInputs(*input, *jsonNode2);
                }
                if (auto child = k->second.dynamic_pointer_cast<LockedNode>())
                    node.inputs.insert_or_assign(i.key(), ref(child));
                else
                    // FIXME: replace by follows node
                    throw Error("lock file contains cycle to root node");
            }
        }
    }(*root, json["nodes"][rootKey.value]);

    // FIXME: check that there are no cycles in version >= 7. Cycles
    // between inputs are only possible using 'follows' indirections.
    // Once we drop support for version <= 6, we can simplify the code
    // a bit since we don't need to worry about cycles.
}

std::pair<nlohmann::json, LockFile::KeyMap> LockFile::toJSON() const
{
    nlohmann::json nodes;
    KeyMap nodeKeys;
    boost::unordered_flat_set<FlakeGraphNodeKey, FlakeGraphNodeKey::Hash> keys;

    auto dumpNode = [&](this auto & dumpNode, FlakeGraphNodeKey key, ref<const Node> node) -> FlakeGraphNodeKey {
        auto k = nodeKeys.find(node);
        if (k != nodeKeys.end())
            return k->second;

        if (!keys.insert(key).second) {
            for (int n = 2;; ++n) {
                auto candidate = appendFlakeGraphNodeKeySuffix(key, n);
                if (keys.insert(candidate).second) {
                    key = std::move(candidate);
                    break;
                }
            }
        }

        nodeKeys.insert_or_assign(node, key);

        auto n = nlohmann::json::object();

        if (!node->inputs.empty()) {
            auto inputs = nlohmann::json::object();
            for (auto & i : node->inputs) {
                if (auto child = edgeChild(i.second)) {
                    inputs[i.first] = dumpNode(makeFlakeGraphNodeKey(i.first), *child).value;
                } else if (auto follows = edgeFollows(i.second)) {
                    auto arr = nlohmann::json::array();
                    for (auto & x : *follows)
                        arr.push_back(x);
                    inputs[i.first] = std::move(arr);
                }
            }
            n["inputs"] = std::move(inputs);
        }

        if (auto lockedNode = node.dynamic_pointer_cast<const LockedNode>()) {
            n["original"] = fetchers::attrsToJSON(lockedNode->originalRef.value.toPersistedAttrs());
            n["locked"] = fetchers::attrsToJSON(lockedNode->lockedRef.value.toPersistedAttrs());
            /* For backward compatibility, omit the "__final"
               attribute. Flake inputs are expected to be final or
               relative in lock files, but non-flake inputs may
               legitimately stay locked-but-non-final. */
            assert(
                !lockedNode->isFlake
                || lockedNode->lockedRef.value.input.isFinal()
                || lockedNode->lockedRef.value.input.isRelative());
            if (!lockedNode->isFlake)
                n["flake"] = false;
            if (lockedNode->parentInputAttrPath)
                n["parent"] = *lockedNode->parentInputAttrPath;
        }

        nodes[key.value] = std::move(n);

        return key;
    };

    nlohmann::json json;
    json["version"] = 7;
    json["root"] = dumpNode(makeFlakeGraphNodeKey("root"), root).value;
    json["nodes"] = std::move(nodes);

    return {json, std::move(nodeKeys)};
}

std::pair<std::string, LockFile::KeyMap> LockFile::to_string() const
{
    auto [json, nodeKeys] = toJSON();
    return {json.dump(2), std::move(nodeKeys)};
}

std::ostream & operator<<(std::ostream & stream, const LockFile & lockFile)
{
    stream << lockFile.toJSON().first.dump(2);
    return stream;
}

std::optional<EvaluationLockedFlakeRef> LockFile::isUnlocked(const fetchers::Settings & fetchSettings) const
{
    std::set<ref<const Node>> nodes;

    [&](this const auto & visit, ref<const Node> node) {
        if (!nodes.insert(node).second)
            return;
        for (auto & i : node->inputs)
            if (auto child = edgeChild(i.second))
                visit(*child);
    }(root);

    /* Return whether the input is either locked, or, if
       `allow-dirty-locks` is enabled, it has a NAR hash. In the
       latter case, we can verify the input but we may not be able to
       fetch it from anywhere. */
    auto isConsideredLocked = [&](const fetchers::Input & input) {
        return input.isLocked(fetchSettings) || (fetchSettings.allowDirtyLocks && input.getNarHash());
    };

    for (auto & i : nodes) {
        if (i == ref<const Node>(root))
            continue;
        auto node = i.dynamic_pointer_cast<const LockedNode>();
        if (node && (!isConsideredLocked(node->lockedRef.value.input) || !node->lockedRef.value.input.isFinal())
            && !node->lockedRef.value.input.isRelative())
            return node->lockedRef;
    }

    return {};
}

bool LockFile::operator==(const LockFile & other) const
{
    // FIXME: slow
    return toJSON().first == other.toJSON().first;
}

InputAttrPath parseInputAttrPath(std::string_view s)
{
    InputAttrPath path;

    for (auto & elem : tokenizeString<std::vector<std::string>>(s, "/")) {
        if (!std::regex_match(elem, flakeIdRegex))
            throw UsageError("invalid flake input attribute path element '%s'", elem);
        path.push_back(elem);
    }

    return path;
}

std::optional<NonEmptyInputAttrPath> NonEmptyInputAttrPath::parse(std::string_view s)
{
    auto path = parseInputAttrPath(s);
    return make(std::move(path));
}

std::optional<NonEmptyInputAttrPath> NonEmptyInputAttrPath::make(InputAttrPath path)
{
    if (path.empty())
        return std::nullopt;
    return NonEmptyInputAttrPath{std::move(path)};
}

std::map<InputAttrPath, Node::Edge> LockFile::getAllInputs() const
{
    std::set<ref<Node>> done;
    std::map<InputAttrPath, Node::Edge> res;

    [&](this const auto & recurse, const InputAttrPath & prefix, ref<Node> node) {
        if (!done.insert(node).second)
            return;

        for (auto & [id, input] : node->inputs) {
            auto inputAttrPath(prefix);
            inputAttrPath.push_back(id);
            res.emplace(inputAttrPath, input);
            if (auto child = edgeChild(input))
                recurse(inputAttrPath, *child);
        }
    }({}, root);

    return res;
}

static std::string describe(const FlakeRef & flakeRef)
{
    auto s = fmt("'%s'", flakeRef.to_string());

    if (auto lastModified = flakeRef.input.getLastModified())
        s += fmt(" (%s)", std::put_time(std::gmtime(&*lastModified), "%Y-%m-%d"));

    return s;
}

std::ostream & operator<<(std::ostream & stream, const Node::Edge & edge)
{
    if (auto node = edgeChild(edge))
        stream << describe((*node)->lockedRef.value);
    else if (auto follows = edgeFollows(edge))
        stream << fmt("follows '%s'", printInputAttrPath(*follows));
    return stream;
}

static bool equals(const Node::Edge & e1, const Node::Edge & e2)
{
    if (auto n1 = edgeChild(e1))
        if (auto n2 = edgeChild(e2))
            return (*n1)->lockedRef == (*n2)->lockedRef;
    if (auto f1 = edgeFollows(e1))
        if (auto f2 = edgeFollows(e2))
            return *f1 == *f2;
    return false;
}

std::string LockFile::diff(const LockFile & oldLocks, const LockFile & newLocks)
{
    auto oldFlat = oldLocks.getAllInputs();
    auto newFlat = newLocks.getAllInputs();

    auto i = oldFlat.begin();
    auto j = newFlat.begin();
    std::string res;

    while (i != oldFlat.end() || j != newFlat.end()) {
        if (j != newFlat.end() && (i == oldFlat.end() || i->first > j->first)) {
            res += fmt(
                "• " ANSI_GREEN "Added input '%s':" ANSI_NORMAL "\n    %s\n", printInputAttrPath(j->first), j->second);
            ++j;
        } else if (i != oldFlat.end() && (j == newFlat.end() || i->first < j->first)) {
            res += fmt("• " ANSI_RED "Removed input '%s'" ANSI_NORMAL "\n", printInputAttrPath(i->first));
            ++i;
        } else {
            if (!equals(i->second, j->second)) {
                res +=
                    fmt("• " ANSI_BOLD "Updated input '%s':" ANSI_NORMAL "\n    %s\n  → %s\n",
                        printInputAttrPath(i->first),
                        i->second,
                        j->second);
            }
            ++i;
            ++j;
        }
    }

    return res;
}

void LockFile::check()
{
    auto inputs = getAllInputs();

    for (auto & [inputAttrPath, input] : inputs) {
        if (auto follows = edgeFollows(input)) {
            if (!follows->empty() && !findInput(*follows))
                throw Error(
                    "input '%s' follows a non-existent input '%s'",
                    printInputAttrPath(inputAttrPath),
                    printInputAttrPath(*follows));
        }
    }
}

void check();

std::string printInputAttrPath(const InputAttrPath & path)
{
    return concatStringsSep("/", path);
}

} // namespace nix::flake
