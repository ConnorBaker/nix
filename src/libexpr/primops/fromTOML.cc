#include "nix/expr/primops.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/static-string-data.hh"
#include "nix/expr/eval-trace/data/traced-data.hh"
#include "../eval-trace/data/traced-data-nodes.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "../eval-trace/toml-canonical.hh"

#include "expr-config-private.hh"

#include <sstream>

#include <toml.hpp>

namespace nix {

// `TomlDataNode` and its helpers (incl. `normalizeDatetimeFormatTomlNode`
// under `HAVE_TOML11_4`) now live in `eval-trace/data/traced-data-nodes.hh`.
// The payload dispatch (objectKeys, materializeScalar, etc) is handled
// out-of-line in `eval-trace/data/traced-data-dispatch.cc`.  No
// virtuals; zero vtable cost.

static void parseTracedTOML(EvalState & state, const std::string_view & s, Value & v,
                            const DepSource & depSource, const std::string & depKey)
{
    std::istringstream tomlStream(std::string{s});
    auto parsed = toml::parse(
        tomlStream,
        "fromTOML"
#if HAVE_TOML11_4
        ,
        toml::spec::v(1, 0, 0)
#endif
    );
    if (parsed.type() == toml::value_t::table) {
        auto access = eval_trace::TraceAccess::current();
        auto & pools = access ? access->tracingPools() : state.tracingPools();
        auto srcId = pools.intern<DepSourceId>(depSource);
        auto fpId = pools.intern<FilePathId>(depKey);
        auto * rootNode = new TomlDataNode(std::move(parsed));
        auto * rootExpr = new ExprTracedData(rootNode, srcId, fpId, pools.dataPathPool.root());
        rootExpr->eval(state, state.baseEnv, v);
    } else {
        throw std::runtime_error("TOML root is not a table");
    }
}

static void prim_fromTOML(EvalState & state, const PosIdx pos, Value ** args, Value & val)
{
    auto toml = state.forceStringNoCtx(*args[0], pos, "while evaluating the argument passed to builtins.fromTOML");

    // If the string came directly from readFile (provenance hash matches),
    // produce lazy traced data with fine-grained StructuredContent deps.
    if (state.traceActiveDepth) [[unlikely]] {
        if (auto * pub = args[0]->publication()) {
            if (pub->text) {
                try {
                    parseTracedTOML(state, toml, val, pub->text->source, pub->text->key);
                    return;
                } catch (std::exception & e) {
                    state.error<EvalError>("while parsing TOML: %s", e.what()).atPos(pos).debugThrow();
                }
            }
        }
    }

    std::istringstream tomlStream(std::string{toml});

    auto visit = [&](this auto & self, Value & v, ::toml::value t) -> void {
        switch (t.type()) {
        case toml::value_t::table: {
            auto table = toml::get<toml::table>(t);
            auto attrs = state.buildBindings(table.size());

            for (auto & elem : table) {
                forceNoNullByte(elem.first);
                self(attrs.alloc(elem.first), elem.second);
            }

            v.mkAttrs(attrs);
        } break;
        case toml::value_t::array: {
            auto array = toml::get<std::vector<toml::value>>(t);

            auto list = state.buildList(array.size());
            for (const auto & [n, v] : enumerate(list))
                self(*(v = state.allocValue()), array[n]);
            v.mkList(list);
        } break;
        case toml::value_t::boolean:
            v.mkBool(toml::get<bool>(t));
            break;
        case toml::value_t::integer:
            v.mkInt(toml::get<int64_t>(t));
            break;
        case toml::value_t::floating:
            v.mkFloat(toml::get<NixFloat>(t));
            break;
        case toml::value_t::string: {
            auto s = toml::get<std::string_view>(t);
            forceNoNullByte(s);
            v.mkString(s, state.mem);
        } break;
        case toml::value_t::local_datetime:
        case toml::value_t::offset_datetime:
        case toml::value_t::local_date:
        case toml::value_t::local_time: {
            if (experimentalFeatureSettings.isEnabled(Xp::ParseTomlTimestamps)) {
#if HAVE_TOML11_4
                normalizeDatetimeFormatTomlNode(t);
#endif
                auto attrs = state.buildBindings(2);
                attrs.alloc("_type").mkStringNoCopy("timestamp"_sds);
                std::ostringstream s;
                s << t;
                auto str = s.view();
                forceNoNullByte(str);
                attrs.alloc("value").mkString(str, state.mem);
                v.mkAttrs(attrs);
            } else {
                throw std::runtime_error("Dates and times are not supported");
            }
        } break;
        case toml::value_t::empty:
            v.mkNull();
            break;
        }
    };

    try {
        visit(
            val,
            toml::parse(
                tomlStream,
                "fromTOML" /* the "filename" */
#if HAVE_TOML11_4
                ,
                toml::spec::v(1, 0, 0) // Be explicit that we are parsing TOML 1.0.0 without extensions
#endif
                ));
    } catch (std::exception & e) { // TODO: toml::syntax_error
        state.error<EvalError>("while parsing TOML: %s", e.what()).atPos(pos).debugThrow();
    }
}

static RegisterPrimOp primop_fromTOML(
    {.name = "fromTOML",
     .args = {"e"},
     .doc = R"(
      Convert a TOML string to a Nix value. For example,

      ```nix
      builtins.fromTOML ''
        x=1
        s="a"
        [table]
        y=2
      ''
      ```

      returns the value `{ s = "a"; table = { y = 2; }; x = 1; }`.
    )",
     .impl = prim_fromTOML});

} // namespace nix
