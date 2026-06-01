#include <stdint.h>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include "nix/flake/flake-primops.hh"
#include "nix/expr/eval.hh"
#include "nix/flake/flake.hh"
#include "nix/flake/flakeref.hh"
#include "nix/flake/settings.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/materialisation-scheduler.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/error.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/pos-idx.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"
#include "nix/store/store-api.hh"

namespace nix::flake::primops {

PrimOp getFlake(const Settings & settings)
{
    auto prim_getFlake = [&settings](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
        state.forceValue(*args[0], pos);

        LockFlags lockFlags{
            .updateLockFile = false,
            .writeLockFile = false,
            .useRegistries = !state.settings.pureEval && settings.useRegistries,
            .allowUnlocked = !state.settings.pureEval,
        };

        if (args[0]->type() == nPath) {
            auto path = state.realisePath(pos, *args[0]);
            callFlake(state, lockFlake(settings, state, path, lockFlags), v);
        } else {
            /* Boundary cover-fix (Item 1, see PROPOSAL.md §6.2
               bypass sites). DetSys hit this in production (commit
               bb3846e6d, #302). Previously `forceStringNoCtx`
               rejected any context, throwing with the placeholder
               wire form `~<hash>:<name>` embedded in the error
               message when a flakeref carried a `SourceVirtual`
               element. Switch to `forceString` + explicit context
               handling: resolve `SourceVirtual` placeholders,
               rewrite the body, and proceed. Note that we still
               don't accept arbitrary context: a flakeref that
               references a derivation output (Built/DrvDeep) is
               nonsensical and should still be rejected — but we let
               `parseFlakeRef` reject it via its own validation. */
            NixStringContext context;
            auto raw =
                state.forceString(*args[0], context, pos, "while evaluating the argument passed to builtins.getFlake");
            auto rewrites = state.resolveSourceVirtualContext(context);
            state.ensureLazyPathsCopied(context);
            std::string flakeRefS = rewriteStrings(std::string{raw}, rewrites);

            auto flakeRef = nix::parseFlakeRef(state.fetchSettings, flakeRefS, {}, true);
            if (state.settings.pureEval && !flakeRef.input.isLocked(state.fetchSettings))
                throw Error(
                    "cannot call 'getFlake' on unlocked flake reference '%s', at %s (use --impure to override)",
                    flakeRefS,
                    state.positions[pos]);

            /* Backwards compatibility: since flakes used to be copied to the store eagerly, some users
               relied on being able to do builtins.getFlake on a flakeref with discarded string context.
               So if a flake input has a physical source path that is inside the store, first try to look it up in the
               storeFS. */
            if (auto sourcePath = flakeRef.input.getSourcePath();
                flakeRef.input.getType() == "path" && sourcePath && state.store->isInStore(sourcePath->string())) {
                auto [storePath, subPath] = state.store->toStorePath(sourcePath->string());
                if (auto mount = state.storeFS->getMount(storeMountKey(*state.store, storePath))) {
                    auto path = state.storePath(storePath) / CanonPath(subPath);
                    if (!flakeRef.subdir.empty())
                        path = path / flakeRef.subdir;
                    return callFlake(state, lockFlake(settings, state, path, lockFlags), v);
                }
            }

            callFlake(state, lockFlake(settings, state, flakeRef, lockFlags), v);
        }
    };

    return PrimOp{
        .name = "__getFlake",
        .args = {"args"},
        .doc = R"(
          Fetch a flake from a flake reference or a path, and return its output attributes and some metadata. For example:

          ```nix
          (builtins.getFlake "nix/55bc52401966fbffa525c574c14f67b00bc4fb3a").packages.x86_64-linux.nix
          ```

          Unless impure evaluation is allowed (`--impure`), the flake reference
          must be "locked", e.g. contain a Git revision or content hash. An
          example of an unlocked usage is:

          ```nix
          (builtins.getFlake "github:edolstra/dwarffs").rev
          ```
        )",
        .impl = prim_getFlake,
        .experimentalFeature = Xp::Flakes,
    };
}

static void prim_parseFlakeRef(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    std::string flakeRefS(
        state.forceStringNoCtx(*args[0], pos, "while evaluating the argument passed to builtins.parseFlakeRef"));
    auto attrs = nix::parseFlakeRef(state.fetchSettings, flakeRefS, {}, true).toAttrs();
    auto binds = state.buildBindings(attrs.size());
    for (const auto & [key, value] : attrs) {
        auto s = state.symbols.create(key);
        auto & vv = binds.alloc(s);
        auto resolved = forceAttr(value);
        std::visit(
            overloaded{
                [&vv, &state](const std::string & value) { vv.mkString(value, state.mem); },
                [&vv](const uint64_t & value) { vv.mkInt(value); },
                [&vv](const Explicit<bool> & value) { vv.mkBool(value.t); }},
            resolved);
    }
    v.mkAttrs(binds);
}

nix::PrimOp parseFlakeRef({
    .name = "__parseFlakeRef",
    .args = {"flake-ref"},
    .doc = R"(
      Parse a flake reference, and return its exploded form.

      For example:

      ```nix
      builtins.parseFlakeRef "github:NixOS/nixpkgs/23.05?dir=lib"
      ```

      evaluates to:

      ```nix
      { dir = "lib"; owner = "NixOS"; ref = "23.05"; repo = "nixpkgs"; type = "github"; }
      ```
    )",
    .impl = prim_parseFlakeRef,
    .experimentalFeature = Xp::Flakes,
});

static void prim_flakeRefToString(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], noPos, "while evaluating the argument passed to builtins.flakeRefToString");

    /* Boundary cover-fix (Item 1, see PROPOSAL.md §6.2 bypass
       sites). String-valued attrs (e.g. `path`, `url`, `dir`) may
       carry a `SourceVirtual` context. Reading `string_view()` raw
       and stuffing it into the `Attrs` map would persist the
       placeholder render `/<base32>` into the resulting flakeref
       URL. Accumulate context across all `nString` attrs, resolve
       SourceVirtual placeholders, rewrite each body before emplacing
       into `attrs`, and propagate Opaque-only context to the result
       string. */
    NixStringContext context;

    struct StringEntry
    {
        std::string name;
        std::string body;
    };

    std::vector<StringEntry> stringEntries;
    fetchers::Attrs attrs;
    for (const auto & attr : *args[0]->attrs()) {
        state.forceValue(*attr.value, attr.pos);
        auto t = attr.value->type();
        if (t == nInt) {
            auto intValue = attr.value->integer().value;

            if (intValue < 0) {
                state
                    .error<EvalError>(
                        "negative value given for flake ref attr %1%: %2%", state.symbols[attr.name], intValue)
                    .atPos(pos)
                    .debugThrow();
            }

            attrs.emplace(state.symbols[attr.name], uint64_t(intValue));
        } else if (t == nBool) {
            attrs.emplace(state.symbols[attr.name], Explicit<bool>{attr.value->boolean()});
        } else if (t == nString) {
            copyContext(*attr.value, context);
            stringEntries.push_back(
                StringEntry{
                    .name = std::string(static_cast<std::string_view>(state.symbols[attr.name])),
                    .body = std::string(attr.value->string_view()),
                });
        } else {
            state
                .error<EvalError>(
                    "flake reference attribute sets may only contain integers, Booleans, "
                    "and strings, but attribute '%s' is %s",
                    state.symbols[attr.name],
                    showType(*attr.value))
                .debugThrow();
        }
    }
    auto rewrites = state.resolveSourceVirtualContext(context);
    state.ensureLazyPathsCopied(context);
    for (auto & entry : stringEntries)
        attrs.emplace(entry.name, rewriteStrings(entry.body, rewrites));
    auto flakeRef = FlakeRef::fromAttrs(state.fetchSettings, attrs);

    /* Build the result context: substitute each `SourceVirtual` with
       `Opaque{resolvedStorePath}` so the result Value carries
       Opaque-only context. Same pattern as `AttrCursor::forceValue`
       (eval-cache.cc:442-450). */
    NixStringContext resultContext;
    for (auto & c : context) {
        if (auto * sv = std::get_if<NixStringContextElem::SourceVirtual>(&c.raw)) {
            auto storePath = state.materialisationScheduler->outPathOf(sv->placeholder);
            resultContext.insert(NixStringContextElem{NixStringContextElem::Opaque{storePath}});
        } else
            resultContext.insert(c);
    }
    v.mkString(flakeRef.to_string(), resultContext, state.mem);
}

nix::PrimOp flakeRefToString({
    .name = "__flakeRefToString",
    .args = {"attrs"},
    .doc = R"(
      Convert a flake reference from attribute set format to URL format.

      For example:

      ```nix
      builtins.flakeRefToString {
        dir = "lib"; owner = "NixOS"; ref = "23.05"; repo = "nixpkgs"; type = "github";
      }
      ```

      evaluates to

      ```nix
      "github:NixOS/nixpkgs/23.05?dir=lib"
      ```
    )",
    .impl = prim_flakeRefToString,
    .experimentalFeature = Xp::Flakes,
});

} // namespace nix::flake::primops
