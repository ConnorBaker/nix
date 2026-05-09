#include "expr-gen.hh"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unistd.h>  // getpid()

#include <nlohmann/json.hpp>

#include <rapidcheck/gen/Arbitrary.hpp>
#include <rapidcheck/gen/Container.hpp>
#include <rapidcheck/gen/Create.hpp>
#include <rapidcheck/gen/Numeric.hpp>
#include <rapidcheck/gen/Select.hpp>
#include <rapidcheck/gen/Transform.hpp>
#include <rapidcheck/gen/Predicate.hpp>

namespace nix::eval_trace::test::proptest {

// ── DepSlot method bodies ────────────────────────────────────────────

void DepSlot::mutate(const std::string & newValue)
{
    if (kind == Kind::EnvVar) {
        setenv(envVarName.c_str(), newValue.c_str(), 1);
    } else if (kind == Kind::JsonArray) {
        // Kind::JsonArray — plain content write (same as File/JsonFile).
        std::ofstream ofs(path, std::ios::trunc);
        ofs << newValue;
    } else if (kind == Kind::FileExistence) {
        // newValue must be "exists" or "missing".
        // "exists"  → create/truncate the file (makes it present)
        // "missing" → remove the file (returns false if already gone; that's OK)
        if (newValue == "exists") {
            std::ofstream ofs(path);
        } else {
            std::filesystem::remove(path);
        }
    } else if (kind == Kind::DirectoryEntries) {
        // newValue must be "exists" or "missing" — toggles the tracked entry.
        // Uses the same model as FileExistence but operates on a directory entry.
        if (newValue == "exists") {
            dirHandle->addFile(dirEntryName, "content");
        } else {
            dirHandle->removeEntry(dirEntryName);
        }
    } else {
        // Kind::File, Kind::JsonFile, and Kind::TomlFile all do a plain content write.
        std::ofstream ofs(path, std::ios::trunc);
        ofs << newValue;
    }
    currentValue = newValue;
}

void DepSlot::restore()
{
    mutate(originalValue_);
}

rc::Gen<std::string> DepSlot::generateMutation() const
{
    switch (kind) {
    case Kind::File:
        if (contentConstraint == ContentConstraint::NixSource) {
            // Preserve Nix syntax: append a trailing comment line.
            // Appending a comment changes file bytes (so the FileBytes dep
            // sees a different hash) but keeps the parsed expression
            // identical, so re-evaluation produces the same result.  The
            // random int is a salt so the mutation is always different
            // from the original file content.
            std::string original = currentValue;
            return rc::gen::map(
                rc::gen::inRange(1, 1000000),
                [original](int seed) -> std::string {
                    return original + "\n# property-test mutation " + std::to_string(seed) + "\n";
                });
        }
        [[fallthrough]];
    case Kind::EnvVar:
        // Arbitrary printable ASCII — the original behavior for file/env slots.
        return rc::gen::container<std::string>(rc::gen::inRange('!', '~'));

    case Kind::JsonFile: {
        // Must produce valid JSON that preserves the same keys AND value types
        // so that: (1) attribute access expressions don't throw "attribute missing",
        // and (2) the SC dep detects the change (SC deps may not invalidate on
        // type changes at the structured projection level).
        //
        // Diversity note: a single random int seed is used to perturb all values.
        // For bools, toggling (!v) is the only available mutation since there are
        // exactly two bool values — this always produces a different value.
        // For strings, appending "_N" always differs from the original.
        // For integers, adding a positive seed always differs from the original.
        // RC_PRE(newValue != currentValue) in P2/mutation callers handles any
        // remaining edge case where the serialized JSON happens to be identical
        // (e.g., null values replaced with the same seed as on a prior mutation).
        auto current = nlohmann::json::parse(currentValue, nullptr, false);
        if (current.is_object() && !current.empty()) {
            // Build a mutated copy preserving key names and value types.
            // Use a single random int seed to perturb all values deterministically.
            return rc::gen::map(
                rc::gen::inRange(1, 10000),
                [current](int seed) -> std::string {
                    nlohmann::json obj = nlohmann::json::object();
                    for (auto & [k, v] : current.items()) {
                        if (v.is_string())
                            obj[k] = v.get<std::string>() + "_" + std::to_string(seed);
                        else if (v.is_number_integer())
                            obj[k] = v.get<int64_t>() + seed;
                        else if (v.is_boolean())
                            obj[k] = !v.get<bool>();
                        else
                            obj[k] = seed;  // null or other → replace with int
                    }
                    return obj.dump();
                });
        }
        // Fallback: current value is not a parseable object — generate fresh.
        return rc::gen::map(
            makeJsonObjectGen(),
            [](std::map<std::string, JsonValue> obj) -> std::string {
                nlohmann::json json = nlohmann::json::object();
                for (auto & [k, v] : obj)
                    json[k] = v.toJson();
                return json.dump();
            });
    }

    case Kind::JsonArray:
        // Must produce a valid JSON array so that builtins.fromJSON does not
        // throw on re-evaluation after mutation.  Generate a fresh integer array.
        return rc::gen::map(
            rc::gen::container<std::vector<int>>(rc::gen::inRange(-100, 101)),
            [](std::vector<int> elems) -> std::string {
                nlohmann::json arr = nlohmann::json::array();
                for (auto e : elems)
                    arr.push_back(e);
                return arr.dump();
            });

    case Kind::FileExistence:
        // Toggle: if the file currently exists, the mutation makes it missing,
        // and vice versa.
        return rc::gen::just(currentValue == "exists"
            ? std::string("missing")
            : std::string("exists"));

    case Kind::DirectoryEntries:
        // Toggle the tracked entry's presence, same model as FileExistence.
        return rc::gen::just(currentValue == "exists"
            ? std::string("missing")
            : std::string("exists"));

    case Kind::TomlFile:
        // Must produce valid TOML with the same key names but different values.
        // makeFromTOMLAccessGen always generates TOML with "name" (string) and
        // "count" (integer) keys.  Produce a fresh TOML string with changed values:
        // name = "changed_N", count = M (N and M random).
        return rc::gen::map(
            rc::gen::apply(
                [](int n, int m) { return std::make_pair(n, m); },
                rc::gen::inRange(0, 10000),
                rc::gen::inRange(0, 10000)),
            [](std::pair<int,int> nm) -> std::string {
                return "name = \"changed_" + std::to_string(nm.first) + "\"\n"
                     + "count = " + std::to_string(nm.second) + "\n";
            });
    }
    // Unreachable — all Kind enumerators are handled above.
    return rc::gen::just(std::string{});
}

void DepSlot::setOriginal(std::string v)
{
    originalValue_ = std::move(v);
}

// ── String escaping ──────────────────────────────────────────────────

std::string nixEscapeString(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '$')  out += "\\$";   // prevent ${} interpolation
        else if (c < 0x20 || c == 0x7f) {
            // Skip non-printable ASCII — Nix string literals must be UTF-8
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// ── NixIdentifierGen ─────────────────────────────────────────────────

namespace {

/// Probes the temp directory (where TempDir places generated directories)
/// to determine whether the filesystem distinguishes case.  Evaluated
/// once and cached.  macOS default APFS volumes (including per-build
/// $TMPDIR under /nix/var/nix/builds) are case-insensitive; most Linux
/// filesystems are case-sensitive.
///
/// Throws if probe I/O fails: returning an inferred default in either
/// direction is unsafe — a silent "case-sensitive" default would
/// re-introduce the inode-collision bug on macOS, and a silent
/// "case-insensitive" default would silently narrow coverage on Linux.
bool tempFsIsCaseSensitive()
{
    static const bool result = []() {
        auto base = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
        std::error_code ec;
        try { createDirs(base); } catch (...) {}
        // Pre-delete to defend against stale files from a crashed prior run
        // where PID was reused (see adversarial review #4).
        auto probe = base / ("case-probe-" + std::to_string(getpid()));
        auto upper = std::filesystem::path(probe.string() + "-A");
        auto lower = std::filesystem::path(probe.string() + "-a");
        std::filesystem::remove(upper, ec);
        std::filesystem::remove(lower, ec);
        {
            std::ofstream ofs(upper);
            ofs << "probe";
            ofs.flush();
            if (!ofs)
                throw std::runtime_error(
                    "case-sensitivity probe: failed to write " + upper.string());
        }
        if (!std::filesystem::exists(upper, ec))
            throw std::runtime_error(
                "case-sensitivity probe: wrote " + upper.string()
                + " but it does not exist after flush");
        // If the FS is case-insensitive, stat'ing the lowercase variant
        // resolves to the same inode we just created.
        bool sensitive = !std::filesystem::exists(lower, ec);
        std::filesystem::remove(upper, ec);
        return sensitive;
    }();
    return result;
}

} // namespace

/// Generates a valid Nix identifier: starts with a letter or '_',
/// followed by 0–7 letters, digits, or '_'.
/// Kept short (≤8 chars) to keep JSON compact in shrunk counter-examples.
rc::Gen<std::string> makeNixIdentifierGen()
{
    // First character: letter or underscore
    auto firstChar = rc::gen::oneOf(
        rc::gen::inRange('a', (char)('z' + 1)),
        rc::gen::inRange('A', (char)('Z' + 1)),
        rc::gen::just('_'));

    // Subsequent characters: letter, digit, or underscore
    // (no '-' — Nix allows it in identifiers only in some contexts; skip to be safe)
    auto tailChar = rc::gen::oneOf(
        rc::gen::inRange('a', (char)('z' + 1)),
        rc::gen::inRange('A', (char)('Z' + 1)),
        rc::gen::inRange('0', (char)('9' + 1)),
        rc::gen::just('_'));

    // Use mapcat: generate first char, then generate tail of 0–7 chars
    return rc::gen::mapcat(
        std::move(firstChar),
        [tailChar = std::move(tailChar)](char first) {
            // mapcat over tail length (0–7), then generate that many tail chars
            return rc::gen::mapcat(
                rc::gen::inRange<size_t>(0, 8),
                [first, tailChar](size_t tailLen) {
                    return rc::gen::map(
                        rc::gen::container<std::string>(tailLen, tailChar),
                        [first](std::string tail) -> std::string {
                            return std::string(1, first) + tail;
                        });
                });
        });
}

/// Variant of makeNixIdentifierGen for names used as filesystem entries.
/// On case-insensitive filesystems, restricts letters to lowercase so that
/// two distinct generated names cannot collide into the same inode (e.g.
/// "_d" and "_D" on APFS).  On case-sensitive filesystems, delegates to
/// makeNixIdentifierGen to keep coverage of upper/lower-case interplay.
rc::Gen<std::string> makeNixFilesystemIdentifierGen()
{
    if (tempFsIsCaseSensitive())
        return makeNixIdentifierGen();

    auto firstChar = rc::gen::oneOf(
        rc::gen::inRange('a', (char)('z' + 1)),
        rc::gen::just('_'));

    auto tailChar = rc::gen::oneOf(
        rc::gen::inRange('a', (char)('z' + 1)),
        rc::gen::inRange('0', (char)('9' + 1)),
        rc::gen::just('_'));

    return rc::gen::mapcat(
        std::move(firstChar),
        [tailChar = std::move(tailChar)](char first) {
            return rc::gen::mapcat(
                rc::gen::inRange<size_t>(0, 8),
                [first, tailChar](size_t tailLen) {
                    return rc::gen::map(
                        rc::gen::container<std::string>(tailLen, tailChar),
                        [first](std::string tail) -> std::string {
                            return std::string(1, first) + tail;
                        });
                });
        });
}

// ── JsonObjectGen ────────────────────────────────────────────────────

// JsonValue method bodies (declared in expr-gen.hh, defined here).
nlohmann::json JsonValue::toJson() const {
    switch (kind) {
    case Kind::String: return strVal;
    case Kind::Int:    return intVal;
    case Kind::Bool:   return boolVal;
    case Kind::Null:   return nullptr;
    case Kind::Float:  return floatVal;
    case Kind::Object: {
        nlohmann::json obj = nlohmann::json::object();
        for (auto & [k, v] : objectVal)
            obj[k] = v.toJson();
        return obj;
    }
    case Kind::Array: {
        nlohmann::json arr = nlohmann::json::array();
        for (auto & v : arrayVal)
            arr.push_back(v.toJson());
        return arr;
    }
    }
    return nullptr;  // unreachable
}

TestExpr::ResultKind JsonValue::resultKind() const {
    switch (kind) {
    case Kind::String: return TestExpr::ResultKind::String;
    case Kind::Int:    return TestExpr::ResultKind::Int;
    case Kind::Bool:   return TestExpr::ResultKind::Bool;
    case Kind::Null:   return TestExpr::ResultKind::Null;
    case Kind::Float:  return TestExpr::ResultKind::Float;
    case Kind::Object: return TestExpr::ResultKind::Attrset;
    case Kind::Array:  return TestExpr::ResultKind::List;
    }
    return TestExpr::ResultKind::String;  // unreachable
}

// Scalar-only JSON value generator (no Object/Array).  Used for building
// Object/Array element values to avoid infinite recursion.
static rc::Gen<JsonValue> makeScalarJsonValueGen()
{
    return rc::gen::oneOf(
        // String: printable ASCII, avoid chars that cause JSON issues
        rc::gen::map(
            rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
            [](std::string s) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::String, .strVal = std::move(s)};
            }),
        // Integer (avoid INT64_MIN which overflows Nix parser)
        rc::gen::map(
            rc::gen::suchThat(rc::gen::arbitrary<int64_t>(),
                [](int64_t n) { return n != std::numeric_limits<int64_t>::min(); }),
            [](int64_t n) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::Int, .intVal = n};
            }),
        // Boolean
        rc::gen::map(rc::gen::arbitrary<bool>(), [](bool b) -> JsonValue {
            return JsonValue{.kind = JsonValue::Kind::Bool, .boolVal = b};
        }),
        // Null
        rc::gen::just(JsonValue{.kind = JsonValue::Kind::Null}),
        // Float: filter out NaN and Inf — JSON doesn't support them
        rc::gen::map(
            rc::gen::suchThat(rc::gen::arbitrary<double>(),
                [](double d) { return std::isfinite(d); }),
            [](double d) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::Float, .floatVal = d};
            })
    );
}

// Scalar identifier generator for Object keys (reuses makeNixIdentifierGen logic
// via a forward reference — defined inline here for use in makeJsonValueGen).
static rc::Gen<JsonValue> makeJsonValueGen()
{
    // 90% scalars, ~5% object, ~5% array.  Use weightedOneOf with integer weights.
    return rc::gen::weightedOneOf<JsonValue>({
        // weight 18: scalars (String, Int, Bool, Null, Float — 5 variants × weight 18/20 total)
        {18, rc::gen::mapcat(
            rc::gen::inRange(0, 5),
            [](int choice) -> rc::Gen<JsonValue> {
                switch (choice) {
                case 0:
                    return rc::gen::map(
                        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                        [](std::string s) -> JsonValue {
                            return JsonValue{.kind = JsonValue::Kind::String, .strVal = std::move(s)};
                        });
                case 1:
                    return rc::gen::map(
                        rc::gen::suchThat(rc::gen::arbitrary<int64_t>(),
                            [](int64_t n) { return n != std::numeric_limits<int64_t>::min(); }),
                        [](int64_t n) -> JsonValue {
                            return JsonValue{.kind = JsonValue::Kind::Int, .intVal = n};
                        });
                case 2:
                    return rc::gen::map(
                        rc::gen::arbitrary<bool>(),
                        [](bool b) -> JsonValue {
                            return JsonValue{.kind = JsonValue::Kind::Bool, .boolVal = b};
                        });
                case 3:
                    return rc::gen::just(JsonValue{.kind = JsonValue::Kind::Null});
                default:  // case 4
                    return rc::gen::map(
                        rc::gen::suchThat(rc::gen::arbitrary<double>(),
                            [](double d) { return std::isfinite(d); }),
                        [](double d) -> JsonValue {
                            return JsonValue{.kind = JsonValue::Kind::Float, .floatVal = d};
                        });
                }
            })},
        // weight 1: nested Object (scalar elements only)
        {1, rc::gen::map(
            rc::gen::mapcat(
                rc::gen::inRange<size_t>(1, 4),
                [](size_t n) {
                    return rc::gen::container<std::vector<std::pair<std::string, JsonValue>>>(
                        n,
                        rc::gen::apply(
                            [](std::string k, JsonValue v) {
                                return std::make_pair(std::move(k), std::move(v));
                            },
                            makeNixIdentifierGen(),
                            makeScalarJsonValueGen()));
                }),
            [](std::vector<std::pair<std::string, JsonValue>> pairs) -> JsonValue {
                std::map<std::string, JsonValue> obj;
                for (auto & [k, v] : pairs)
                    obj.insert_or_assign(std::move(k), std::move(v));
                return JsonValue{.kind = JsonValue::Kind::Object, .objectVal = std::move(obj)};
            })},
        // weight 1: nested Array (scalar elements only)
        {1, rc::gen::map(
            rc::gen::mapcat(
                rc::gen::inRange<size_t>(1, 6),
                [](size_t n) {
                    return rc::gen::container<std::vector<JsonValue>>(
                        n, makeScalarJsonValueGen());
                }),
            [](std::vector<JsonValue> elems) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::Array, .arrayVal = std::move(elems)};
            })}
    });
}

/// Generates a JSON object with 1–5 keys.  Keys are valid Nix identifiers.
/// Returns a std::map<std::string, JsonValue> (sorted by key for stable output).
rc::Gen<std::map<std::string, JsonValue>> makeJsonObjectGen()
{
    // Generate a list of (key, value) pairs, then deduplicate keys.
    // Use mapcat to first generate a size (1–5), then generate that many pairs.
    return rc::gen::mapcat(
        rc::gen::inRange<size_t>(1, 6),  // 1 to 5 keys
        [](size_t n) {
            return rc::gen::map(
                rc::gen::container<std::vector<std::pair<std::string, JsonValue>>>(
                    n,
                    rc::gen::apply(
                        [](std::string k, JsonValue v) {
                            return std::make_pair(std::move(k), std::move(v));
                        },
                        makeNixIdentifierGen(),
                        makeJsonValueGen())),
                [](std::vector<std::pair<std::string, JsonValue>> pairs)
                    -> std::map<std::string, JsonValue>
                {
                    // Insert into map — later duplicates overwrite earlier ones;
                    // std::map enforces unique keys, which is what we want.
                    std::map<std::string, JsonValue> result;
                    for (auto & [k, v] : pairs)
                        result.insert_or_assign(std::move(k), std::move(v));
                    return result;
                });
        });
}

// Generates a JSON object whose values are strictly {String, Int, Bool, Null} —
// no Float, no nested Object or Array.  Used by makeAttrAccessGen and
// makeMultiSourceAttrGen so that the accessed key's ResultKind is always one of
// the four scalar kinds.  This preserves the ResultKind assertions in
// generator-test.cc (which must not be modified).
rc::Gen<std::map<std::string, JsonValue>> makeAccessibleJsonObjectGen()
{
    auto strictScalarGen = rc::gen::oneOf(
        rc::gen::map(
            rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
            [](std::string s) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::String, .strVal = std::move(s)};
            }),
        rc::gen::map(
            rc::gen::suchThat(rc::gen::arbitrary<int64_t>(),
                [](int64_t n) { return n != std::numeric_limits<int64_t>::min(); }),
            [](int64_t n) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::Int, .intVal = n};
            }),
        rc::gen::map(rc::gen::arbitrary<bool>(), [](bool b) -> JsonValue {
            return JsonValue{.kind = JsonValue::Kind::Bool, .boolVal = b};
        }),
        rc::gen::just(JsonValue{.kind = JsonValue::Kind::Null})
    );

    return rc::gen::mapcat(
        rc::gen::inRange<size_t>(1, 6),
        [strictScalarGen](size_t n) {
            return rc::gen::map(
                rc::gen::container<std::vector<std::pair<std::string, JsonValue>>>(
                    n,
                    rc::gen::apply(
                        [](std::string k, JsonValue v) {
                            return std::make_pair(std::move(k), std::move(v));
                        },
                        makeNixIdentifierGen(),
                        strictScalarGen)),
                [](std::vector<std::pair<std::string, JsonValue>> pairs)
                    -> std::map<std::string, JsonValue>
                {
                    std::map<std::string, JsonValue> result;
                    for (auto & [k, v] : pairs)
                        result.insert_or_assign(std::move(k), std::move(v));
                    return result;
                });
        });
}

// ── Top-level ────────────────────────────────────────────────────────

rc::Gen<TestExpr> makeNixExprGen(int depth)
{
    // Phase 2 generators (makeFromJSONGen, makeAttrAccessGen, makePathExistsGen)
    // and Phase 3 generators (makeMultiSourceAttrGen, makeListFromJSONGen) are
    // included.  They are safe here because:
    //   (a) Kind::JsonFile slots use generateMutation() which produces valid JSON,
    //       so P2 re-evaluation via builtins.fromJSON never throws.
    //   (b) invalidation.cc's guard covers Kind::JsonFile for invalidateFileCache.
    //   (c) Kind::FileExistence mutation toggles "exists"↔"missing" (not raw ASCII).
    //   (d) Kind::JsonArray mutation produces valid JSON arrays.
    //   (e) makeLetGen uses an explicit depth counter to bound nesting; passing
    //       depth here ensures the combined depth limit is respected across
    //       makeLetGen → makeNixExprGen → makeLetGen call chains.
    return rc::gen::oneOf(
        makeScalarGen(),
        makeReadFileGen(),
        makeGetEnvGen(),
        makeFromJSONGen(),
        makeAttrAccessGen(),
        makePathExistsGen(),
        makeLetGen(depth),
        makeCompoundGen(),
        makeMultiSourceAttrGen(),
        makeTripleSourceMergeGen(),
        makeListFromJSONGen(),
        makeIfThenElseGen(),
        makeStringInterpolationGen(),
        makeHasAttrTestGen(),
        makeWithExprGen(),
        makeMapAttrsAccessGen(),
        makeFilterLengthGen(),
        makeMapLengthGen(),
        makeSortLengthGen(),
        makeIntersectAccessGen(),
        makeRemoveAttrsAccessGen(),
        makeAttrNamesLengthGen(),
        makeConcatStringsGen(),
        makeReplaceStringsReadFileGen(),
        makeFromTOMLAccessGen(),
        makeElemAtFromJSONGen(),
        makeFoldlSumGen(),
        makeStringInterpolationFromJSONGen(),
        makePathExistsIfGen(),
        makeListPipelineGen(),
        makeAttrsetPipelineGen(),
        makeOverlayGen(),
        makeConditionalDepGen(),
        makeTryEvalGen(),
        makeCallPackageGen(),
        makeRecAttrsetGen(),
        makeReadDirMapAttrsGen(),
        makeSiblingTraceGen(),
        makeMultiBindingLetGen(),
        makeImportTreeGen(),
        makeMixedDepStringGen(),
        makeSelectiveAttrsetGen(),
        makeJsonConditionalGen(),
        makeMultiJsonStringGen(),
        makeImportJsonChainGen(),
        makeDeepPipelineGen(),
        makeNestedImportPipelineGen(),
        makeThreeSourcePipelineGen(),
        makeFoldMergePipelineGen(),
        makeFunctionChainGen(),
        makeFilterSortMultiGen(),
        makeRecCrossBindingGen(),
        makeNestedAttrsetAccessGen(),
        makeReadDirJsonMergeGen(),
        makeDeepAttrsetAccessGen()
    );
}

}  // namespace nix::eval_trace::test::proptest

namespace rc {
Gen<nix::eval_trace::test::proptest::TestExpr>
Arbitrary<nix::eval_trace::test::proptest::TestExpr>::arbitrary()
{
    return nix::eval_trace::test::proptest::makeNixExprGen();
}
}  // namespace rc
