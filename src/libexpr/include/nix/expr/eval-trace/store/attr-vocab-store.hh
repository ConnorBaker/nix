#pragma once
/// @file
/// AttrVocabStore: Structured attribute name/path trie backed by SQLite.
/// Replaces flat delimited-string attr path representations with a recursive
/// trie. Attribute names are interned via StringInternTable. Shared across
/// evaluations (context-independent), grows monotonically (never pruned).

#include "nix/expr/symbol-table.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/string-intern-table.hh"
#include "nix/expr/eval-trace/ids.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace nix::eval_trace {

class CanonicalHashBuilder;

struct AttrVocabStore {

    AttrVocabStore(SymbolTable & symbols);

    AttrVocabStore(const AttrVocabStore &) = delete;
    AttrVocabStore & operator=(const AttrVocabStore &) = delete;

    static constexpr AttrNameId rootName() { return AttrNameId(0); }
    static constexpr AttrPathId rootPath() { return AttrPathId(0); }

    // ── Intern (O(1) hash lookup; new entries tracked for DB flush) ──

    AttrNameId internName(std::string_view name);
    AttrNameId internName(Symbol sym);
    AttrPathId internPath(AttrPathId parent, AttrNameId child);

    /// Convenience: extend a path by a Symbol (intern name + path in one call).
    AttrPathId extendPath(AttrPathId parent, Symbol child);

    // ── Resolution ───────────────────────────────────────────────────

    std::string_view resolveName(AttrNameId id) const;
    AttrNameId childName(AttrPathId id) const;
    AttrPathId parentPath(AttrPathId id) const;

    /// Return the Symbol for a path's leaf component.
    /// Requires the name to have been pre-populated via internName(Symbol).
    Symbol childSymbol(AttrPathId id) const;

    /// Dot-separated display path for debug output: "foo.bar.baz".
    /// Root returns "«root»".
    std::string displayPath(AttrPathId id) const;

    /// Feed path components as canonical length-framed hash fields.
    void feedPath(CanonicalHashBuilder & builder, AttrPathId id) const;

    // ── Lookup without interning ─────────────────────────────────────

    std::optional<AttrNameId> lookupName(std::string_view name) const;
    std::optional<AttrPathId> lookupPath(AttrPathId parent, AttrNameId child) const;

    // ── DB lifecycle ─────────────────────────────────────────────────

    /// Write pending entries above watermarks using externally-owned prepared
    /// statements (bound to the ATTACH'd vocab.* schema by TraceStore).
    void flushTo(SQLiteStmt & insertName, SQLiteStmt & insertPath);

    /// Flush pending entries to attr-vocab.sqlite via a brief independent
    /// connection. Called from TraceStore::flush() BEFORE writing trace
    /// entities, ensuring vocab entries are persisted before any traces
    /// that reference them.  This closes the crash-safety gap: cross-DB
    /// ATTACH'd transactions are not atomic in WAL mode, so without this,
    /// a crash could leave the main DB with traces referencing vocab IDs
    /// that were never committed to attr-vocab.sqlite.
    /// Orphan vocab entries (persisted without matching traces) are harmless.
    void checkpoint();

    /// Return the path to the vocab SQLite database file.
    /// Used by TraceStore to ATTACH the vocab DB.
    const std::filesystem::path & getDbPath() const { return dbPath; }

    // The vocab store grows monotonically and is shared across all
    // TraceStore instances in a session.

private:
    SymbolTable & symbols;
    std::filesystem::path dbPath;

    // Attr names: arena-backed, dedup, bulkLoad support.
    // Separate instance from InterningPools::strings (independent ID space).
    StringInternTable nameTable;

    // Attr paths: dense vector (trie) + dedup map.
    struct PathEntry {
        AttrPathId parent;
        AttrNameId child;
    };
    std::vector<PathEntry> paths;  // [pathId.value] → {parent, child}

    /// Pack (parent, child) into a single uint64_t for dedup map key.
    static uint64_t packKey(AttrPathId parent, AttrNameId child) {
        return (uint64_t(parent.value) << 32) | child.value;
    }

    boost::unordered_flat_map<uint64_t, AttrPathId> pathByKey;

    // Symbol bridge (bidirectional AttrNameId ↔ Symbol).
    std::vector<Symbol> nameToSymbol;                               // [nameId.value] → Symbol
    boost::unordered_flat_map<uint32_t, AttrNameId> symbolToName;   // Symbol::getId() → AttrNameId

    // High-water marks for flush (entries ≤ this are already in DB).
    uint32_t maxLoadedNameId = 0;
    uint32_t maxLoadedPathId = 0;

    /// Seed root sentinels in memory (id=0 entries for name/path/symbol).
    void seedRootSentinels();

    /// Load all entries from a DB into memory.
    void loadFrom(SQLite & db);
};

} // namespace nix::eval_trace
