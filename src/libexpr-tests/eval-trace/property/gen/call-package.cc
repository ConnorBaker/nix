#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── CallPackageGen ───────────────────────────────────────────────────
//
// Models nixpkgs' callPackage pattern: a function imported from a .nix file
// is called with an attrset argument that supplies a file-backed value.
//
// Expression:
//   let f = import <pkg.nix>; in f { data = builtins.readFile <data.txt>; }
//
// pkg.nix content (fixed):
//   { data, prefix ? "pkg" }: "${prefix}-${data}"
//
// This exercises two distinct file deps in one expression:
//   slot[0]: Kind::File — data.txt, randomized printable ASCII content
//   slot[1]: Kind::File — pkg.nix, fixed content (the Nix function body)
//
// The expression evaluates to "${prefix}-${data}" = "pkg-<content of data.txt>".
// ResultKind: String.
//
// pkg.nix uses Nix ${} string interpolation — the C++ raw string literal avoids
// escaping issues.  The pkg.nix file content is always the same fixed string,
// so its dep hash is stable; only data.txt's dep hash changes on mutation.

rc::Gen<TestExpr> makeCallPackageGen()
{
    // The fixed pkg.nix body.  Nix ${} interpolation is fine inside a C++ raw
    // string literal — no escaping needed.
    static const std::string pkgNixContent =
        R"({ data, prefix ? "pkg" }: "${prefix}-${data}")";

    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
        [](std::string dataContent) {
            // slot[0]: data.txt — variable content, Kind::File
            auto dataHandle = std::make_shared<TempTextFile>(dataContent);
            DepSlot dataSlot;
            dataSlot.kind = DepSlot::Kind::File;
            dataSlot.path = dataHandle->path;
            dataSlot.fileHandle = dataHandle;
            dataSlot.currentValue = dataContent;
            dataSlot.setOriginal(dataContent);

            // slot[1]: pkg.nix — fixed content, Kind::File.
            // Mark as NixSource so generateMutation preserves Nix syntax
            // (appends a comment rather than replacing with random bytes).
            auto pkgHandle = std::make_shared<TempExtFile>("nix", pkgNixContent);
            DepSlot pkgSlot;
            pkgSlot.kind = DepSlot::Kind::File;
            pkgSlot.contentConstraint = DepSlot::ContentConstraint::NixSource;
            pkgSlot.path = pkgHandle->path;
            pkgSlot.fileHandle = pkgHandle;
            pkgSlot.currentValue = pkgNixContent;
            pkgSlot.setOriginal(pkgNixContent);

            // Nix expression: import pkg.nix, call with data from data.txt.
            // builtins.readFile returns a string; the function concatenates
            // prefix (default "pkg") with data using Nix string interpolation.
            std::string nixCode =
                "let f = import " + pkgHandle->path.string() + "; "
                "in f { data = builtins.readFile " + dataHandle->path.string() + "; }";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(dataSlot));
            deps.push_back(std::move(pkgSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
