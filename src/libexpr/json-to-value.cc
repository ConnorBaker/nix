#include "nix/expr/json-to-value.hh"
#include "nix/expr/eval-trace/data/traced-data.hh"
#include "eval-trace/data/traced-data-nodes.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/semantic-objects.hh"
#include "nix/expr/value.hh"
#include "nix/expr/eval.hh"

#include <limits>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace nix {

// for more information, refer to
// https://github.com/nlohmann/json/blob/master/include/nlohmann/detail/input/json_sax.hpp
class JSONSax : nlohmann::json_sax<json>
{
    class JSONState
    {
    protected:
        std::unique_ptr<JSONState> parent;
        RootValue v;
    public:
        virtual std::unique_ptr<JSONState> resolve(EvalState &)
        {
            throw std::logic_error("tried to close toplevel json parser state");
        }

        explicit JSONState(std::unique_ptr<JSONState> && p)
            : parent(std::move(p))
        {
        }

        explicit JSONState(Value * v)
            : v(allocRootValue(v))
        {
        }

        JSONState(JSONState & p) = delete;

        Value & value(EvalState & state)
        {
            if (!v)
                v = allocRootValue(state.allocValue());
            return **v;
        }

        virtual ~JSONState() {}

        virtual void add() {}
    };

    class JSONObjectState : public JSONState
    {
        using JSONState::JSONState;
        ValueMap attrs;

        std::unique_ptr<JSONState> resolve(EvalState & state) override
        {
            auto attrs2 = state.buildBindings(attrs.size());
            for (auto & i : attrs)
                attrs2.insert(i.first, i.second);
            parent->value(state).mkAttrs(attrs2);
            return std::move(parent);
        }

        void add() override
        {
            v = nullptr;
        }
    public:
        void key(string_t & name, EvalState & state)
        {
            forceNoNullByte(name);
            attrs.insert_or_assign(state.symbols.create(name), &value(state));
        }
    };

    class JSONListState : public JSONState
    {
        ValueVector values;

        std::unique_ptr<JSONState> resolve(EvalState & state) override
        {
            auto list = state.buildList(values.size());
            for (const auto & [n, v2] : enumerate(list))
                v2 = values[n];
            parent->value(state).mkList(list);
            return std::move(parent);
        }

        void add() override
        {
            values.push_back(*v);
            v = nullptr;
        }
    public:
        JSONListState(std::unique_ptr<JSONState> && p, std::size_t reserve)
            : JSONState(std::move(p))
        {
            values.reserve(reserve);
        }
    };

    EvalState & state;
    std::unique_ptr<JSONState> rs;

public:
    JSONSax(EvalState & state, Value & v)
        : state(state)
        , rs(new JSONState(&v)) {};

    bool null() override
    {
        rs->value(state).mkNull();
        rs->add();
        return true;
    }

    bool boolean(bool val) override
    {
        rs->value(state).mkBool(val);
        rs->add();
        return true;
    }

    bool number_integer(number_integer_t val) override
    {
        rs->value(state).mkInt(val);
        rs->add();
        return true;
    }

    bool number_unsigned(number_unsigned_t val_) override
    {
        if (val_ > std::numeric_limits<NixInt::Inner>::max()) {
            throw Error("unsigned json number %1% outside of Nix integer range", val_);
        }
        NixInt::Inner val = val_;
        rs->value(state).mkInt(val);
        rs->add();
        return true;
    }

    bool number_float(number_float_t val, const string_t & s) override
    {
        rs->value(state).mkFloat(val);
        rs->add();
        return true;
    }

    bool string(string_t & val) override
    {
        forceNoNullByte(val);
        rs->value(state).mkString(val, state.mem);
        rs->add();
        return true;
    }

#if NLOHMANN_JSON_VERSION_MAJOR >= 3 && NLOHMANN_JSON_VERSION_MINOR >= 8
    bool binary(binary_t &) override
    {
        // This function ought to be unreachable
        assert(false);
        return true;
    }
#endif

    bool start_object(std::size_t len) override
    {
        rs = std::make_unique<JSONObjectState>(std::move(rs));
        return true;
    }

    bool key(string_t & name) override
    {
        dynamic_cast<JSONObjectState *>(rs.get())->key(name, state);
        return true;
    }

    bool end_object() override
    {
        rs = rs->resolve(state);
        rs->add();
        return true;
    }

    bool end_array() override
    {
        return end_object();
    }

    bool start_array(size_t len) override
    {
        rs = std::make_unique<JSONListState>(std::move(rs), len != std::numeric_limits<size_t>::max() ? len : 128);
        return true;
    }

    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception & ex) override
    {
        throw JSONParseError("%s", ex.what());
    }
};

void parseJSON(EvalState & state, const std::string_view & s_, Value & v)
{
    JSONSax parser(state, v);
    bool res = json::sax_parse(s_, &parser);
    if (!res)
        throw JSONParseError("Invalid JSON Value");
}

// ═══════════════════════════════════════════════════════════════════════
// Lazy structural dependency tracking for JSON (traced data)
// ═══════════════════════════════════════════════════════════════════════
//
// `JsonDataNode` and its siblings are defined in
// `eval-trace/data/traced-data-nodes.hh`; payload access for the
// abstract `TracedDataNode` base is dispatched out-of-line via
// `traced-data-dispatch.cc`.  No virtuals.

// ── ExprTracedData::eval() ──────────────────────────────────────────

void ExprTracedData::eval(EvalState & state, Env & env, Value & v)
{
    auto access = eval_trace::TraceAccess::current();
    auto & pools = access ? access->tracingPools() : state.tracingPools();
    auto nodeKind = node->kind();
    auto fmt = node->formatTag();

    switch (nodeKind) {
    case TracedDataNode::Kind::Object: {
        auto keys = node->objectKeys();
        auto attrs = state.buildBindings(
            keys.size(),
            access && keys.empty()
                ? EmptyBindingsAllocation::AllocateFresh
                : EmptyBindingsAllocation::ReuseSharedEmpty);

        bool tracking = access && !keys.empty();
        auto originHandle = tracking
            ? state.positions.addOriginHandle(
                  allocateProvenanceRef(pools, sourceId, filePathId, dataPathId, fmt),
                  keys.size())
            : PosTable::OriginHandle{};

        for (size_t idx = 0; idx < keys.size(); idx++) {
            auto & k = keys[idx];
            auto childPathId = pools.dataPathPool.internChild(dataPathId, k);
            auto * childNode = node->objectGet(k);
            auto * childVal = state.allocValue();

            auto * childExpr = new ExprTracedData(
                childNode, sourceId, filePathId, childPathId);
            childVal->mkThunk(&state.baseEnv, childExpr);
            eval_trace::nrTracedExprFromDataFile++;
            eval_trace::nrDataFileContainerChildren++;
            forceNoNullByte(k);
            PosIdx keyPos = tracking ? state.positions.add(originHandle, idx) : PosIdx{};
            attrs.insert(state.symbols.create(k), childVal, keyPos);
        }
        v.mkAttrs(attrs);
        if (access) {
            CompactDepComponents keysComp{sourceId, filePathId, fmt, dataPathId,
                                          ShapeSuffix::Keys, StringId(), StringId()};
            StructuredObject structured{
                .source = pools.resolveDepSource(sourceId),
                .key = std::string(pools.resolve(filePathId)),
                .dataPath = resolveStructuredPath(pools, dataPathId),
                .format = fmt,
            };
            if (keys.empty()) {
                // Empty objects: blocking SC #keys so key additions are caught.
                recordStructuredDep(pools, keysComp, DepHashValue(sentinel(SentinelHash::Empty)));

                state.publishStructuredProvenance(v, structured);
                auto * prov = access->allocateProvenance(sourceId, filePathId, dataPathId, fmt);
                access->registerTracedContainer(&v, prov);
            } else {
                // Non-empty: ImplicitShape #keys fingerprint at creation time.
                auto keysHash = eval_trace::canonicalKeysHash(keys);
                recordStructuredDep(pools, keysComp, DepHashValue(keysHash), CanonicalQueryKind::ImplicitStructure);

                PosIdx anyKeyPos = v.attrs()->begin()->pos;
                if (auto off = state.positions.originOffsetOf(anyKeyPos))
                    access->registerPrecomputedKeys(*off, PrecomputedKeysInfo{
                        keysHash,
                        static_cast<uint32_t>(keys.size()),
                        sourceId, filePathId, dataPathId, fmt,
                    });

                // Attach StructuredObject on the Bindings via the sealed
                // EvalState::publishStructuredProvenance helper (Bindings::setPublication
                // is private, friend EvalState only).
                state.publishStructuredProvenance(v, structured);
                auto * prov = access->allocateProvenance(sourceId, filePathId, dataPathId, fmt);
                access->registerTracedContainer(&v, prov);
            }
        }
        break;
    }
    case TracedDataNode::Kind::Array: {
        auto sz = node->arraySize();
        auto list = state.buildList(sz, access.has_value());
        for (size_t i = 0; i < sz; i++) {
            auto childPathId = pools.dataPathPool.internArrayChild(dataPathId, static_cast<int32_t>(i));
            auto * childNode = node->arrayGet(i);
            auto * childVal = state.allocValue();

            auto * childExpr = new ExprTracedData(
                childNode, sourceId, filePathId, childPathId);
            childVal->mkThunk(&state.baseEnv, childExpr);
            eval_trace::nrTracedExprFromDataFile++;
            eval_trace::nrDataFileContainerChildren++;
            list[i] = childVal;
        }
        v.mkList(list);
        if (access) {
            CompactDepComponents lenComp{sourceId, filePathId, fmt, dataPathId,
                                         ShapeSuffix::Len, StringId(), StringId()};
            if (sz == 0) {
                // Empty lists: blocking SC #len so element additions are caught.
                recordStructuredDep(pools, lenComp, DepHashValue(sentinel(SentinelHash::Zero)));

                auto * prov = access->allocateProvenance(sourceId, filePathId, dataPathId, fmt);
                access->registerTracedContainer(&v, prov);
            } else {
                // Non-empty: ImplicitShape #len fingerprint at creation time.
                recordStructuredDep(pools, lenComp, DepHashValue(depHash(std::to_string(sz))), CanonicalQueryKind::ImplicitStructure);

                auto * prov = access->allocateProvenance(sourceId, filePathId, dataPathId, fmt);
                access->registerTracedContainer(&v, prov);
            }
            // NOTE: publishStructuredProvenance is intentionally not called for list
            // values. List provenance is carried via ContainerProvenanceRegistry
            // (registerTracedContainer above). The packed setStorage(List) on x86-64
            // drops any inline publication field; the registry is the authoritative
            // mechanism. See value.hh List struct comment.
        }
        break;
    }
    case TracedDataNode::Kind::String:
    case TracedDataNode::Kind::Number:
    case TracedDataNode::Kind::Bool:
    case TracedDataNode::Kind::Null: {
        // Scalar leaf — record StructuredContent dep
        CompactDepComponents scalarComp{sourceId, filePathId, fmt, dataPathId,
                                        ShapeSuffix::None, StringId(), StringId()};
        auto hash = depHash(node->canonicalValue());
        recordStructuredDep(pools, scalarComp, DepHashValue(hash));
        node->materializeScalar(state, v);
        eval_trace::nrDataFileScalarChildren++;
        break;
    }
    }
}

void parseTracedJSON(EvalState & state, const std::string_view & s, Value & v,
                     const DepSource & depSource, const std::string & depKey)
{
    auto j = json::parse(s);
    if (j.is_object() || j.is_array()) {
        auto access = eval_trace::TraceAccess::current();
        auto & pools = access ? access->tracingPools() : state.tracingPools();
        auto srcId = pools.intern<DepSourceId>(depSource);
        auto fpId = pools.intern<FilePathId>(depKey);
        auto * rootNode = new JsonDataNode(std::move(j));
        auto * rootExpr = new ExprTracedData(rootNode, srcId, fpId, pools.dataPathPool.root());
        rootExpr->eval(state, state.baseEnv, v);
    } else {
        parseJSON(state, s, v);
    }
}

} // namespace nix
