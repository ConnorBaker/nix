#include "nix/expr/parse-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/store/sqlite.hh"
#include "nix/store/globals.hh"
#include "nix/util/users.hh"
#include "nix/util/file-system.hh"
#include "nix/util/sync.hh"

#include <cstring>

namespace nix {

/* Tagged binary encoding of the data subset of Value. Big-endian
   length-prefixed; symbol names stored as bytes (not symbol IDs) so two
   processes deserialise identical trees regardless of intern order. */
namespace {

enum : uint8_t {
    TAG_NULL = 1,
    TAG_BOOL = 2,
    TAG_INT = 3,
    TAG_FLOAT = 4,
    TAG_STRING = 5,
    TAG_LIST = 6,
    TAG_ATTRS = 7,
};

void writeU8(std::string & buf, uint8_t v)
{
    buf.push_back(static_cast<char>(v));
}

void writeU32(std::string & buf, uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        buf.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}

void writeI64(std::string & buf, int64_t v)
{
    auto u = static_cast<uint64_t>(v);
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<char>((u >> (i * 8)) & 0xff));
}

void writeF64(std::string & buf, double v)
{
    uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    for (int i = 7; i >= 0; --i)
        buf.push_back(static_cast<char>((u >> (i * 8)) & 0xff));
}

void writeBytes(std::string & buf, std::string_view s)
{
    if (s.size() > UINT32_MAX)
        throw Error("parse-cache: blob field exceeds 4GB");
    writeU32(buf, static_cast<uint32_t>(s.size()));
    buf.append(s);
}

} // anonymous namespace

/* Encode/decode are methods on a small ParseCacheCodec that holds an
   EvalState reference — attr name encoding requires the symbol
   table. */

struct ParseCacheCodec
{
    EvalState & state;

    bool encode(const Value & v, std::string & buf);
    void decode(std::string_view & in, Value & out);
};

bool ParseCacheCodec::encode(const Value & v, std::string & buf)
{
    switch (v.type<true>()) {
    case nNull:
        writeU8(buf, TAG_NULL);
        return true;
    case nBool:
        writeU8(buf, TAG_BOOL);
        writeU8(buf, v.boolean() ? 1 : 0);
        return true;
    case nInt:
        writeU8(buf, TAG_INT);
        writeI64(buf, v.integer().value);
        return true;
    case nFloat:
        writeU8(buf, TAG_FLOAT);
        writeF64(buf, v.fpoint());
        return true;
    case nString:
        /* Refuse strings with context: the parse cache stores pure
           data values, not string-with-context references. JSON
           parsing produces context-free strings. */
        if (v.context())
            return false;
        writeU8(buf, TAG_STRING);
        writeBytes(buf, v.string_view());
        return true;
    case nList: {
        writeU8(buf, TAG_LIST);
        auto view = v.listView();
        writeU32(buf, static_cast<uint32_t>(v.listSize()));
        for (auto & elem : view)
            if (!encode(*elem, buf))
                return false;
        return true;
    }
    case nAttrs: {
        writeU8(buf, TAG_ATTRS);
        const auto * attrs = v.attrs();
        writeU32(buf, static_cast<uint32_t>(attrs->size()));
        for (auto & attr : *attrs) {
            std::string_view name = state.symbols[attr.name];
            writeBytes(buf, name);
            if (!encode(*attr.value, buf))
                return false;
        }
        return true;
    }
    case nThunk:
    case nFailed:
    case nPath:
    case nFunction:
    case nExternal:
        /* Not data forms — can't be deterministically replayed. */
        return false;
    }
    /* All ValueType cases handled above. */
    return false;
}

namespace {

uint8_t readU8(std::string_view & in)
{
    if (in.empty())
        throw Error("parse-cache: truncated blob");
    uint8_t v = static_cast<uint8_t>(in.front());
    in.remove_prefix(1);
    return v;
}

uint32_t readU32(std::string_view & in)
{
    if (in.size() < 4)
        throw Error("parse-cache: truncated blob");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | static_cast<uint8_t>(in[i]);
    in.remove_prefix(4);
    return v;
}

int64_t readI64(std::string_view & in)
{
    if (in.size() < 8)
        throw Error("parse-cache: truncated blob");
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i)
        u = (u << 8) | static_cast<uint8_t>(in[i]);
    in.remove_prefix(8);
    return static_cast<int64_t>(u);
}

double readF64(std::string_view & in)
{
    if (in.size() < 8)
        throw Error("parse-cache: truncated blob");
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i)
        u = (u << 8) | static_cast<uint8_t>(in[i]);
    in.remove_prefix(8);
    double d;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}

std::string_view readBytes(std::string_view & in)
{
    auto n = readU32(in);
    if (in.size() < n)
        throw Error("parse-cache: truncated blob");
    auto s = in.substr(0, n);
    in.remove_prefix(n);
    return s;
}

} // anonymous namespace

void ParseCacheCodec::decode(std::string_view & in, Value & out)
{
    auto tag = readU8(in);
    switch (tag) {
    case TAG_NULL:
        out.mkNull();
        return;
    case TAG_BOOL:
        out.mkBool(readU8(in) != 0);
        return;
    case TAG_INT:
        out.mkInt(NixInt::Inner{readI64(in)});
        return;
    case TAG_FLOAT:
        out.mkFloat(NixFloat{readF64(in)});
        return;
    case TAG_STRING: {
        auto s = readBytes(in);
        out.mkString(s, state.mem);
        return;
    }
    case TAG_LIST: {
        auto n = readU32(in);
        auto builder = state.buildList(n);
        for (uint32_t i = 0; i < n; ++i) {
            auto * elem = state.allocValue();
            decode(in, *elem);
            builder[i] = elem;
        }
        out.mkList(builder);
        return;
    }
    case TAG_ATTRS: {
        auto n = readU32(in);
        auto bindings = state.buildBindings(n);
        for (uint32_t i = 0; i < n; ++i) {
            auto name = readBytes(in);
            auto * elem = state.allocValue();
            decode(in, *elem);
            bindings.insert(state.symbols.create(name), elem);
        }
        out.mkAttrs(bindings);
        return;
    }
    default:
        throw Error("parse-cache: unknown tag %d in encoded value", (int) tag);
    }
}

namespace {

constexpr const char * schema = R"sql(
create table if not exists Documents (
    fingerprint text not null,
    format      text not null,
    parser_key  text not null,
    path        text not null,
    body        blob not null,
    primary key (fingerprint, format, parser_key, path)
);
)sql";

struct ParseCacheImpl : ParseCache
{
    struct State
    {
        SQLite db;
        SQLiteStmt upsert, lookup;
    };

    Sync<State> _state;

    /* When the cache directory isn't writable (sandboxed test env,
       read-only HOME, container with /homeless-shelter, etc.) we
       silently degrade to a no-op cache: every lookup misses, every
       upsert is dropped. The point of a parse cache is performance,
       not correctness — falling back to "always re-parse" is the
       right behaviour when persistence is unavailable. */
    bool functional = false;

    ParseCacheImpl()
    {
        try {
            auto state(_state.lock());
            auto dbPath = getCacheDir() / "parse-cache-v1.sqlite";
            createDirs(dbPath.parent_path());
            state->db = SQLite(dbPath, {.useWAL = nix::settings.useSQLiteWAL});
            state->db.isCache();
            state->db.exec(schema);
            state->upsert.create(
                state->db,
                "insert or replace into Documents(fingerprint, format, parser_key, path, body) values (?, ?, ?, ?, ?)");
            state->lookup.create(
                state->db,
                "select body from Documents where fingerprint = ? and format = ? and parser_key = ? and path = ?");
            functional = true;
        } catch (Error & e) {
            debug("parse-cache disabled: %s", e.what());
        } catch (std::filesystem::filesystem_error & e) {
            debug("parse-cache disabled: %s", e.what());
        }
    }

    bool lookup(
        std::string_view fingerprint,
        std::string_view format,
        std::string_view parserKey,
        const CanonPath & path,
        EvalState & evalState,
        Value & out) override
    {
        if (!functional)
            return false;
        std::string body;
        {
            auto state(_state.lock());
            auto stmt(state->lookup.use()(fingerprint)(format) (parserKey) (path.abs()));
            if (!stmt.next())
                return false;
            body = stmt.getBlob(0);
        }
        std::string_view in = body;
        ParseCacheCodec codec{evalState};
        /* The parse cache is a PERFORMANCE optimisation, not a
           correctness store: a damaged row (disk corruption, partial
           write, or a codec/version skew the `parserKey` didn't catch)
           must degrade to a re-parse, never abort evaluation or return a
           truncated value. So: catch any decode error and miss, and treat
           leftover bytes after a successful decode as corruption (the
           encoder writes exactly one root value, so trailing bytes mean
           the row is not what this codec produced). */
        try {
            codec.decode(in, out);
        } catch (Error & e) {
            debug("parse-cache: undecodable entry for '%s' (%s); re-parsing", path.abs(), e.what());
            return false;
        }
        if (!in.empty()) {
            debug("parse-cache: trailing bytes after decode for '%s'; re-parsing", path.abs());
            return false;
        }
        return true;
    }

    bool upsert(
        std::string_view fingerprint,
        std::string_view format,
        std::string_view parserKey,
        const CanonPath & path,
        EvalState & evalState,
        const Value & value) override
    {
        ParseCacheCodec codec{evalState};
        std::string body;
        if (!codec.encode(value, body))
            return false;
        if (!functional)
            return false;
        auto state(_state.lock());
        state->upsert
            .use()(fingerprint)(format) (parserKey) (path.abs())(
                reinterpret_cast<const unsigned char *>(body.data()), body.size())
            .exec();
        return true;
    }
};

Sync<std::shared_ptr<ParseCache>> parseCacheSingleton;

} // anonymous namespace

ref<ParseCache> getParseCache()
{
    auto sing(parseCacheSingleton.lock());
    if (!*sing)
        *sing = std::make_shared<ParseCacheImpl>();
    return ref<ParseCache>(*sing);
}

} // namespace nix
