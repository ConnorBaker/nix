#pragma once

#include "../helpers.hh"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>  // setenv, getenv

#include <map>

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/gen/Arbitrary.hpp>

namespace nix::eval_trace::test::proptest {

/// One mutable dependency slot — a file or environment variable the
/// generated expression reads.
struct DepSlot {
    enum class Kind { File, EnvVar, JsonFile, FileExistence, JsonArray, DirectoryEntries, TomlFile };

    /// How this slot's content relates to the expression's result value.
    ///   Result:   the result depends on this slot's content.  Mutating it MUST
    ///             invalidate the cache (for Kind::File) or change the result.
    ///             The dep MUST appear in stored deps.
    ///   Recorded: the evaluator reads this slot as part of producing the result
    ///             (dep IS recorded in the trace) but the slot's content may not
    ///             directly flow into the result value (e.g., non-winner in //
    ///             merge).  Mutation may NOT cause invalidation (SC override may
    ///             serve cache).  The dep MUST still appear in stored deps.
    ///   Absent:   the slot is forced during evaluation as a side effect (e.g.,
    ///             Nix forces all attrset bindings) but the dep is NOT recorded
    ///             in the stored trace because it doesn't contribute to the
    ///             result's dep chain.  The dep MUST NOT appear in stored deps.
    ///   Conditional: the slot may or may not be read depending on runtime
    ///             state (e.g., a guard file's existence).  Tests verify type
    ///             consistency when present; don't assert presence or absence.
    enum class Contribution { Result, Recorded, Absent, Conditional };

    /// What shape the slot's content must preserve under mutation.
    ///   Arbitrary:  any printable ASCII is acceptable (plain text files,
    ///               env vars).  This is the default.
    ///   NixSource:  the file is imported / evaluated as Nix code.  The
    ///               mutation must produce syntactically valid Nix so the
    ///               parser doesn't throw on re-evaluation.  Generators
    ///               for `entry.nix` / `lib.nix` / `pkg.nix` slots (in
    ///               callPackage, importTree, nestedImportPipeline) must
    ///               stamp this so mutation appends a harmless comment
    ///               rather than clobbering the source with random bytes.
    enum class ContentConstraint { Arbitrary, NixSource };

    Kind kind;
    Contribution contribution = Contribution::Result;
    ContentConstraint contentConstraint = ContentConstraint::Arbitrary;

    /// File-backed: absolute path of the temp file.
    std::filesystem::path path;
    /// File-backed: RAII owner of the temp file.  Keeps the file alive
    /// for the lifetime of this DepSlot.
    std::shared_ptr<TempExtFile> fileHandle;

    /// Directory-backed: RAII owner of the temp directory.
    std::shared_ptr<TempDir> dirHandle;
    /// Directory-backed: the entry name whose presence is toggled on mutation.
    std::string dirEntryName;

    /// Env-var-backed: variable name.
    std::string envVarName;

    /// For EnvVar slots: RAII guard that keeps the env var set and restores
    /// (or unsets) it on destruction.  Stored here so that the env var is
    /// cleaned up even if RC_ASSERT unwinds the stack — raw setenv() calls
    /// would leak the variable into process state across shrink candidates
    /// and between iterations.
    std::shared_ptr<ScopedEnvVar> envGuard;

    /// Current serialized content (file bytes or env var value).
    std::string currentValue;

    /// Mutate the slot to a new value and update currentValue.
    void mutate(const std::string & newValue);

    /// Restore to originalValue_.
    void restore();

    /// Generate a valid mutation value for this slot's kind.
    rc::Gen<std::string> generateMutation() const;

    /// Capture the initial value as the restore point.
    void setOriginal(std::string v);

private:
    std::string originalValue_;
};

/// A generated Nix expression together with its mutable dependency slots.
struct TestExpr {
    enum class ResultKind { String, Int, Bool, Null, Float, Attrset, List };

    std::string nixCode;
    ResultKind expectedKind = ResultKind::String;
    std::vector<DepSlot> depSlots;

    /// Dot-separated attribute paths to force after forceRoot.  Each entry
    /// like "top.inner" means: force root, force attr "top" on root, force
    /// attr "inner" on the result of "top".  This records deps for exactly
    /// the paths the expression accesses — no more.
    ///
    /// Empty means forceRoot alone is sufficient (the root trace contains
    /// all deps — e.g., TracedData attrsets, scalar results).
    std::vector<std::string> attrsToForce;

    /// For List results: indices to force after forceRoot.
    std::vector<size_t> indicesToForce;

    /// True for expressions that evaluate successfully; false for error-path
    /// expressions (e.g., missing key access, invalid JSON parse).
    /// Error generators set this to false.  All property tests that call
    /// eval() should guard with RC_PRE(expr.expectsSuccess()).
    bool expectsSuccess_ = true;
    bool expectsSuccess() const { return expectsSuccess_; }
};

/// RapidCheck display hook.  Called by RC on counter-example output.
/// Must be findable via ADL in namespace proptest.
inline void showValue(const TestExpr & expr, std::ostream & os)
{
    auto kindStr = [](TestExpr::ResultKind k) -> std::string_view {
        switch (k) {
        case TestExpr::ResultKind::String:  return "String";
        case TestExpr::ResultKind::Int:     return "Int";
        case TestExpr::ResultKind::Bool:    return "Bool";
        case TestExpr::ResultKind::Null:    return "Null";
        case TestExpr::ResultKind::Float:   return "Float";
        case TestExpr::ResultKind::Attrset: return "Attrset";
        case TestExpr::ResultKind::List:    return "List";
        }
        return "Unknown";  // unreachable
    };
    os << "TestExpr{\n"
       << "  kind    = " << kindStr(expr.expectedKind) << "\n"
       << "  nixCode = " << expr.nixCode << "\n"
       << "  deps[" << expr.depSlots.size() << "] = [\n";
    for (auto & s : expr.depSlots) {
        auto contribStr = [](DepSlot::Contribution c) -> std::string_view {
            switch (c) {
            case DepSlot::Contribution::Result:      return "Result";
            case DepSlot::Contribution::Recorded:    return "Recorded";
            case DepSlot::Contribution::Absent:       return "Absent";
            case DepSlot::Contribution::Conditional:  return "Conditional";
            }
            return "Unknown";
        };
        if (s.kind == DepSlot::Kind::EnvVar)
            os << "    EnvVar(" << s.envVarName
               << " = \"" << s.currentValue << "\")"
               << " [" << contribStr(s.contribution) << "]\n";
        else if (s.kind == DepSlot::Kind::JsonFile)
            os << "    [JsonFile] " << s.path.string()
               << " = \"" << s.currentValue << "\""
               << " [" << contribStr(s.contribution) << "]\n";
        else if (s.kind == DepSlot::Kind::FileExistence)
            os << "    [FileExistence] " << s.path.string()
               << " = " << s.currentValue
               << " [" << contribStr(s.contribution) << "]\n";
        else if (s.kind == DepSlot::Kind::JsonArray)
            os << "    [JsonArray] " << s.path.string()
               << " = \"" << s.currentValue << "\""
               << " [" << contribStr(s.contribution) << "]\n";
        else if (s.kind == DepSlot::Kind::DirectoryEntries)
            os << "    [DirectoryEntries] " << s.path.string()
               << " entry=" << s.dirEntryName
               << " = " << s.currentValue
               << " [" << contribStr(s.contribution) << "]\n";
        else if (s.kind == DepSlot::Kind::TomlFile)
            os << "    [TomlFile] " << s.path.string()
               << " = \"" << s.currentValue << "\""
               << " [" << contribStr(s.contribution) << "]\n";
        else
            os << "    [File] " << s.path.string()
               << " = \"" << s.currentValue << "\""
               << " [" << contribStr(s.contribution) << "]\n";
    }
    os << "  ]\n}";
}

/// Compare two forced Value objects using RC_ASSERT.
///
/// Both values must be from the same EvalState GC context (i.e., from the
/// same fixture instance).  All makeCache calls within a single TEST_F share
/// one EvalState, so Value objects from any iteration are valid.
///
/// For Phase 1 scalar types (nInt, nBool, nFloat, nNull), data is stored
/// inline in Value — no GC pointer lifetime concerns at all.
/// For nString, string_view() returns a pointer into GC-managed StringData;
/// safe as long as both values are from the same EvalState and no GC
/// collection occurs between calls (guaranteed within a single test body).
///
/// For nAttrs: compares key names (sorted Symbol order) and scalar values at
/// each key.  Both values must come from the same EvalState so Symbol ids are
/// comparable by identity.
///
/// The depth parameter limits recursive comparison of nested containers.
/// At depth == 0, nested nAttrs/nList elements are skipped (key/size match
/// is still asserted).  Callers should use the default (depth = 3).
/// RC_PRE(newValue != currentValue) in mutation callers handles any edge case
/// where generateMutation() produces the same value as the original.
inline void assertValuesEqual(
    const nix::Value & a,
    const nix::Value & b,
    int depth = 3)
{
    RC_ASSERT(a.type() == b.type());
    switch (a.type()) {
    case nInt:
        RC_ASSERT(a.integer().value == b.integer().value);
        break;
    case nFloat:
        RC_ASSERT(a.fpoint() == b.fpoint());
        break;
    case nBool:
        RC_ASSERT(a.boolean() == b.boolean());
        break;
    case nNull:
        break;  // null == null always
    case nString:
        RC_ASSERT(std::string_view(a.string_view()) ==
                  std::string_view(b.string_view()));
        break;
    case nAttrs: {
        RC_ASSERT(a.attrs()->size() == b.attrs()->size());
        auto itA = a.attrs()->begin();
        auto itB = b.attrs()->begin();
        auto endA = a.attrs()->end();
        auto endB = b.attrs()->end();
        while (itA != endA && itB != endB) {
            RC_ASSERT(itA->name == itB->name);
            if (itA->value && itB->value) {
                auto * va = itA->value;
                auto * vb = itB->value;
                if (va->type() == vb->type()) {
                    switch (va->type()) {
                    case nInt:
                        RC_ASSERT(va->integer().value == vb->integer().value);
                        break;
                    case nFloat:
                        RC_ASSERT(va->fpoint() == vb->fpoint());
                        break;
                    case nBool:
                        RC_ASSERT(va->boolean() == vb->boolean());
                        break;
                    case nString:
                        RC_ASSERT(std::string_view(va->string_view()) ==
                                  std::string_view(vb->string_view()));
                        break;
                    case nNull:
                        break;  // null == null always
                    case nAttrs:
                        if (depth > 0)
                            assertValuesEqual(*va, *vb, depth - 1);
                        break;
                    case nList:
                        if (depth > 0)
                            assertValuesEqual(*va, *vb, depth - 1);
                        break;
                    case nPath:
                    case nFunction:
                    case nExternal:
                    case nThunk:
                    case nFailed:
                        break;  // not compared in property tests
                    }
                }
            }
            ++itA;
            ++itB;
        }
        break;
    }
    case nList: {
        RC_ASSERT(a.listSize() == b.listSize());
        auto listA = a.listView();
        auto listB = b.listView();
        for (size_t i = 0; i < a.listSize(); ++i) {
            auto * va = listA[i];
            auto * vb = listB[i];
            if (va && vb && va->type() == vb->type()) {
                switch (va->type()) {
                case nInt:
                    RC_ASSERT(va->integer().value == vb->integer().value);
                    break;
                case nFloat:
                    RC_ASSERT(va->fpoint() == vb->fpoint());
                    break;
                case nBool:
                    RC_ASSERT(va->boolean() == vb->boolean());
                    break;
                case nString:
                    RC_ASSERT(std::string_view(va->string_view()) ==
                              std::string_view(vb->string_view()));
                    break;
                case nNull:
                    break;  // null == null always
                case nAttrs:
                    if (depth > 0)
                        assertValuesEqual(*va, *vb, depth - 1);
                    break;
                case nList:
                    if (depth > 0)
                        assertValuesEqual(*va, *vb, depth - 1);
                    break;
                case nPath:
                case nFunction:
                case nExternal:
                case nThunk:
                case nFailed:
                    break;
                }
            }
        }
        break;
    }
    case nPath:
    case nFunction:
    case nExternal:
    case nThunk:
    case nFailed:
        RC_ASSERT(false);  // not expected in property tests
    }
}

// JSON value type used internally and by property tests that build custom JSON.
struct JsonValue {
    enum class Kind { String, Int, Bool, Null, Float, Object, Array };
    Kind kind;
    std::string strVal;
    int64_t intVal = 0;
    bool boolVal = false;
    double floatVal = 0.0;
    // Object: map of key → scalar JsonValue (one level only — no recursive Object/Array)
    std::map<std::string, JsonValue> objectVal;
    // Array: vector of scalar JsonValues (no recursive Object/Array)
    std::vector<JsonValue> arrayVal;

    nlohmann::json toJson() const;
    TestExpr::ResultKind resultKind() const;
};

// Shared string escaping helper used by ScalarGen and any generator that
// embeds arbitrary content into a Nix string literal.
std::string nixEscapeString(const std::string & s);

// Shared identifier generator: valid Nix identifier, ≤8 chars.
// Used by JsonObjectGen and any generator that needs Nix-safe key names.
rc::Gen<std::string> makeNixIdentifierGen();

// Identifier generator for names that will become real filesystem entries
// (not just Nix attrset keys).  On case-insensitive filesystems (macOS APFS
// default, Windows) emits only lowercase letters so two distinct generated
// names cannot collide into the same inode.  On case-sensitive filesystems
// delegates to makeNixIdentifierGen so upper/lower-case coverage is preserved.
rc::Gen<std::string> makeNixFilesystemIdentifierGen();

// Generator for a JSON object with 1–5 keys.
// Returns a std::map<std::string, JsonValue> (sorted by key for stable output).
// Used internally by FromJSONGen, AttrAccessGen, and directly by some property tests.
rc::Gen<std::map<std::string, JsonValue>> makeJsonObjectGen();

// Generator for a JSON object with strictly {String, Int, Bool, Null} values
// (no Float, no nested Object or Array).  Used by AttrAccessGen and similar
// generators where the accessed key's ResultKind must be a scalar kind.
rc::Gen<std::map<std::string, JsonValue>> makeAccessibleJsonObjectGen();

// Generator declarations
//
// Note: VolatileTime is NOT included in makeNixExprGen because it always
// forces re-evaluation, which violates P1 soundness (eval != cached_eval)
// and P2 invalidation (warm eval always misses). See volatile-time.cc.
rc::Gen<TestExpr> makeScalarGen();
rc::Gen<TestExpr> makeReadFileGen();
rc::Gen<TestExpr> makeGetEnvGen();
rc::Gen<TestExpr> makeFromJSONGen();         // Phase 2: builtins.fromJSON (builtins.readFile <path>)
rc::Gen<TestExpr> makeAttrAccessGen();       // Phase 2: (builtins.fromJSON ...).key
rc::Gen<TestExpr> makePathExistsGen();       // Phase 2: builtins.pathExists <path>
rc::Gen<TestExpr> makeLetGen(int depth = 0); // Phase 3: let _letVar = <subexpr>; in _letVar
rc::Gen<TestExpr> makeCompoundGen();         // Phase 3: let _a = <readFile>; _b = <getEnv>; in { a = _a; b = _b; }
rc::Gen<TestExpr> makeMultiSourceAttrGen();   // Phase 3: ((fromJSON slotA) // (fromJSON slotB)).key
rc::Gen<TestExpr> makeTripleSourceMergeGen(); // Phase 3: let a=fromJSON A; b=fromJSON B; c=fromJSON C; in (a//b//c)."key"
rc::Gen<TestExpr> makeListFromJSONGen();     // Phase 4: builtins.fromJSON (builtins.readFile <json>) where JSON is array

// Compositional generators — chain 2-3 operations on traced data.
rc::Gen<TestExpr> makeMapAttrsAccessGen();         // (builtins.mapAttrs (k: v: v) (fromJSON readFile))."key"
rc::Gen<TestExpr> makeFilterLengthGen();           // builtins.length (builtins.filter (x: x > 0) (fromJSON readFile))
rc::Gen<TestExpr> makeMapLengthGen();              // builtins.length (builtins.map (x: x) (fromJSON readFile))
rc::Gen<TestExpr> makeSortLengthGen();             // builtins.length (builtins.sort builtins.lessThan (fromJSON readFile))
rc::Gen<TestExpr> makeIntersectAccessGen();        // (builtins.intersectAttrs (fromJSON A) (fromJSON B))."key"
rc::Gen<TestExpr> makeRemoveAttrsAccessGen();      // (builtins.removeAttrs (fromJSON readFile) ["other"])."key"
rc::Gen<TestExpr> makeAttrNamesLengthGen();        // builtins.length (builtins.attrNames (fromJSON readFile))
rc::Gen<TestExpr> makeConcatStringsGen();          // builtins.concatStringsSep ":" [readFile A, readFile B]
rc::Gen<TestExpr> makeReplaceStringsReadFileGen(); // builtins.replaceStrings ["a"] ["b"] (readFile file)
rc::Gen<TestExpr> makeFromTOMLAccessGen();         // (builtins.fromTOML (readFile tomlfile))."key"
rc::Gen<TestExpr> makeElemAtFromJSONGen();         // builtins.elemAt (fromJSON readFile) 0
rc::Gen<TestExpr> makeFoldlSumGen();               // builtins.foldl' (a: b: a + b) 0 (fromJSON readFile)
rc::Gen<TestExpr> makeStringInterpolationFromJSONGen(); // "result: ${toString (fromJSON readFile)."key"}"
rc::Gen<TestExpr> makePathExistsIfGen();           // if builtins.pathExists <file> then "yes" else "no"
rc::Gen<TestExpr> makeListPipelineGen();           // let list = fromJSON readFile; mapped = map (x: x*2) list; in length mapped
rc::Gen<TestExpr> makeAttrsetPipelineGen();        // 3-layer: mapAttrs (k:v: v+1) → removeAttrs → access
rc::Gen<TestExpr> makeOverlayGen();               // nixpkgs overlay: (self: super: { foo = super.bar + 1; baz = super.baz; }) null base → .foo
rc::Gen<TestExpr> makeConditionalDepGen();         // if builtins.pathExists <guard.txt> then builtins.readFile <data.txt> else "default"
rc::Gen<TestExpr> makeConditionalDepElseBranchGen(); // same, but guard starts MISSING → else branch taken
rc::Gen<TestExpr> makeTryEvalGen();                // (builtins.tryEval (fromJSON readFile)."key").value — success path only
rc::Gen<TestExpr> makeRecAttrsetGen();             // (rec { val = readFile <f>; name = "pkg-${val}"; qualified = "${name}-release"; }).qualified
rc::Gen<TestExpr> makeCallPackageGen();            // callPackage pattern: let f = import <pkg.nix>; in f { data = builtins.readFile <data.txt>; }
rc::Gen<TestExpr> makeSiblingTraceGen();           // mkDerivation-style: let cfg = fromJSON readFile; in { name=cfg.name; version=cfg.version; combined="${cfg.name}-${cfg.version}"; }.combined
rc::Gen<TestExpr> makeReadDirMapAttrsGen();        // readDir + mapAttrs: (mapAttrs (n: t: "${n}:${t}") (readDir <dir>))."<file>" — NixOS module discovery
rc::Gen<TestExpr> makeMultiBindingLetGen();        // let a=readFile; b=fromJSON.key; c=length(fromJSON); d=readFile; e=fromJSON.ekey; in {...}.a
rc::Gen<TestExpr> makeImportTreeGen();             // import <entry.nix> → imports lib.nix + reads data.txt — tree-shaped transitive FileBytes

// Error-path generators (expectsSuccess() == false).
// Do NOT add to makeNixExprGen — use only in dedicated error-path tests that
// catch exceptions manually (e.g., error-path.cc).
//   • missing-key variant: accesses a key guaranteed absent in the JSON
//   • invalid-json variant: feeds a non-JSON string to builtins.fromJSON
rc::Gen<TestExpr> makeErrorGen();

// Language-feature generators — all included in makeNixExprGen.
rc::Gen<TestExpr> makeIfThenElseGen();       // if <bool> then <scalar> else <scalar>
rc::Gen<TestExpr> makeStringInterpolationGen(); // "prefix-${builtins.readFile <file>}-suffix"
rc::Gen<TestExpr> makeHasAttrTestGen();      // (builtins.fromJSON (builtins.readFile <json>)) ? "key"
rc::Gen<TestExpr> makeWithExprGen();         // with builtins; readFile <file>

// Mixed-kind multi-source generators — combine different dep kinds in one result.
rc::Gen<TestExpr> makeMixedDepStringGen();        // "${readFile plain}-${(fromJSON readFile json)."key"}"
rc::Gen<TestExpr> makeSelectiveAttrsetGen();      // { plain=readFile; structured=(fromJSON ..)."k"; env=getEnv }
rc::Gen<TestExpr> makeJsonConditionalGen();       // if (fromJSON readFile).enabled then readFile data else "default"
rc::Gen<TestExpr> makeMultiJsonStringGen();       // "${(fromJSON a)."k1"}-${(fromJSON b)."k2"}"
rc::Gen<TestExpr> makeImportJsonChainGen();       // import loader.nix (which reads JSON + plain file)

// Deep pipeline generators — long primop chains that stress dep propagation.
rc::Gen<TestExpr> makeDeepPipelineGen();          // readFile→fromJSON→mapAttrs→removeAttrs→access + readFile→interpolation
rc::Gen<TestExpr> makeNestedImportPipelineGen();  // 4-file import chain: entry→lib, reads JSON config + plain version
rc::Gen<TestExpr> makeThreeSourcePipelineGen();   // JSON + TOML + plain, each through primops, combined into one string
rc::Gen<TestExpr> makeFoldMergePipelineGen();     // foldl' over JSON array + structured access + plain file → string
rc::Gen<TestExpr> makeFunctionChainGen();         // traced data through lambda application (transform + add1)
rc::Gen<TestExpr> makeFilterSortMultiGen();       // filter→sort→length on traced list + structured access from different source
rc::Gen<TestExpr> makeRecCrossBindingGen();       // rec { name=cfg.name; version=ver; qualified="${name}-${version}" }.upper
rc::Gen<TestExpr> makeNestedAttrsetAccessGen();   // nested attrset with selective forcing + Absent side attr
rc::Gen<TestExpr> makeReadDirJsonMergeGen();      // readDir + fromJSON structured access → combined string
rc::Gen<TestExpr> makeReadDirCountGen();          // toString(length(attrNames(readDir dir))) + readFile — entry count changes on mutation
rc::Gen<TestExpr> makeDeepAttrsetAccessGen();     // 3-level nested attrset with depth-3 attrsToForce + 2 Absent side attrs

rc::Gen<TestExpr> makeNixExprGen(int depth = 0); // top-level: oneOf all generators

}  // namespace nix::eval_trace::test::proptest

namespace rc {
template<>
struct Arbitrary<nix::eval_trace::test::proptest::TestExpr> {
    static Gen<nix::eval_trace::test::proptest::TestExpr> arbitrary();
};
}  // namespace rc
