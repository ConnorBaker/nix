#include "../expr-gen.hh"

#include <set>

namespace nix::eval_trace::test::proptest {

// ── ReadDirMapAttrsGen ───────────────────────────────────────────────
//
// Models NixOS module system discovery: readDir enumerates a directory and
// mapAttrs processes each entry.
//
// Expression:
//   (builtins.mapAttrs (name: type: "${name}:${type}") (builtins.readDir <dir>))."<filename>"
//
// The generator:
//   • Creates a TempDir with 2–4 randomly-named regular files
//     (makeNixFilesystemIdentifierGen — case-insensitive FSes restrict to
//     lowercase so generated names cannot alias into the same inode).
//   • Picks one existing file as the accessed key.
//   • The dirEntryName on the slot is a NEW name (not yet present) for mutation.
//
// DepSlot construction (Kind::DirectoryEntries):
//   slot.path        = dir->path()       — the directory path
//   slot.dirHandle   = dir               — shared_ptr<TempDir> RAII owner
//   slot.dirEntryName = newEntryName     — name to add on mutate("exists")
//   slot.currentValue = "missing"        — newEntryName doesn't exist yet
//   slot.setOriginal("missing")
//
// mutate("exists") adds the entry; mutate("missing") removes it.
//
// ResultKind: String — e.g. "myfile:regular".
//
// Wired into makeNixExprGen().

rc::Gen<TestExpr> makeReadDirMapAttrsGen()
{
    // Generate 2–4 valid Nix identifier filenames for the initial directory
    // contents.  All are created as regular files.  Uses the filesystem
    // variant of the identifier generator so that on case-insensitive
    // filesystems (macOS APFS default) two distinct generated names cannot
    // alias onto the same inode (e.g. "_d" and "_D").
    return rc::gen::mapcat(
        rc::gen::inRange<size_t>(2, 5),   // 2 to 4 existing files
        [](size_t n) {
            return rc::gen::mapcat(
                // Generate n distinct names for the initial files.
                rc::gen::container<std::vector<std::string>>(n, makeNixFilesystemIdentifierGen()),
                [](std::vector<std::string> names) {
                    // Deduplicate: if any names collide, fall back to distinct suffixes.
                    // Use a std::set to detect duplicates without altering order.
                    {
                        std::set<std::string> seen;
                        bool hasDup = false;
                        for (auto & nm : names) {
                            if (!seen.insert(nm).second) { hasDup = true; break; }
                        }
                        if (hasDup) {
                            // Replace with uniquified names: "f0", "f1", ...
                            for (size_t i = 0; i < names.size(); ++i)
                                names[i] = "f" + std::to_string(i);
                        }
                    }

                    // Pick one as the accessed key and generate a new entry name for mutation.
                    return rc::gen::mapcat(
                        rc::gen::inRange<size_t>(0, names.size()),   // index of accessed file
                        [names](size_t accessIdx) {
                            return rc::gen::map(
                                makeNixFilesystemIdentifierGen(),   // new entry name for mutation
                                [names, accessIdx](std::string newEntryName) {
                                    // Ensure newEntryName is not already in the directory.
                                    // If it collides with an existing name, append "_new".
                                    bool collision = false;
                                    for (auto & nm : names) {
                                        if (nm == newEntryName) { collision = true; break; }
                                    }
                                    if (collision)
                                        newEntryName += "_new";

                                    // Create the directory and populate with regular files.
                                    auto dir = std::make_shared<TempDir>();
                                    for (auto & nm : names)
                                        dir->addFile(nm, "content");

                                    // Accessed file name (quoted for safety — generated
                                    // identifiers may be Nix reserved words).
                                    const std::string & accessedName = names[accessIdx];

                                    // Build the DepSlot (Kind::DirectoryEntries).
                                    DepSlot slot;
                                    slot.kind = DepSlot::Kind::DirectoryEntries;
                                    slot.path = dir->path();
                                    slot.dirHandle = dir;
                                    slot.dirEntryName = newEntryName;
                                    slot.currentValue = "missing";
                                    slot.setOriginal("missing");

                                    // Expression:
                                    //   (builtins.mapAttrs (name: type: "${name}:${type}")
                                    //     (builtins.readDir <dir>))."<accessedName>"
                                    std::string nixCode =
                                        "(builtins.mapAttrs (name: type: \"${name}:${type}\") "
                                        "(builtins.readDir " + dir->path().string() + "))"
                                        ".\"" + accessedName + "\"";

                                    return TestExpr{
                                        .nixCode = std::move(nixCode),
                                        .expectedKind = TestExpr::ResultKind::String,
                                        .depSlots = {std::move(slot)},
                                    };
                                });
                        });
                });
        });
}

} // namespace nix::eval_trace::test::proptest
