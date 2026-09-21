#include "nix/store/sqlite.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/util.hh"
#include "nix/util/url.hh"
#include "nix/util/signals.hh"

#ifdef __linux__
#  include "nix/util/processes.hh"
#  include <sys/vfs.h>
#endif

#include <sqlite3.h>

#include <thread>

namespace nix {

SQLiteError::SQLiteError(
    const char * path, const char * errMsg, int errNo, int extendedErrNo, int offset, HintFmt && hf)
    : CloneableError("")
    , path(path)
    , errMsg(errMsg)
    , errNo(errNo)
    , extendedErrNo(extendedErrNo)
    , offset(offset)
{
    auto offsetStr = (offset == -1) ? "" : "at offset " + std::to_string(offset) + ": ";
    err.msg = HintFmt(
        "%s: %s%s, %s (in '%s')",
        Uncolored(hf.str()),
        offsetStr,
        sqlite3_errstr(extendedErrNo),
        errMsg,
        path ? path : "(in-memory)");
}

void SQLiteError::anchor() {}

void SQLiteBusy::anchor() {}

[[noreturn]] void SQLiteError::throw_(sqlite3 * db, HintFmt && hf)
{
    int err = sqlite3_errcode(db);
    int exterr = sqlite3_extended_errcode(db);
    int offset = sqlite3_error_offset(db);

    auto path = sqlite3_db_filename(db, nullptr);
    auto errMsg = sqlite3_errmsg(db);

    if (err == SQLITE_BUSY || err == SQLITE_PROTOCOL) {
        auto exp = SQLiteBusy(path, errMsg, err, exterr, offset, std::move(hf));
        exp.err.msg = HintFmt(
            err == SQLITE_PROTOCOL ? "SQLite database '%s' is busy (SQLITE_PROTOCOL)" : "SQLite database '%s' is busy",
            path ? path : "(in-memory)");
        throw std::move(exp);
    } else
        throw SQLiteError(path, errMsg, err, exterr, offset, std::move(hf));
}

static void traceSQL(void * x, const char * sql)
{
    // wacky delimiters:
    //   so that we're quite unambiguous without escaping anything
    // notice instead of trace:
    //   so that this can be enabled without getting the firehose in our face.
    notice("SQL<[%1%]>", sql);
};

SQLite::SQLite(const std::filesystem::path & path, Settings && settings)
{
    // Work around a ZFS issue where SQLite's truncate() call on
    // db.sqlite-shm can randomly take up to a few seconds. See
    // https://github.com/openzfs/zfs/issues/14290#issuecomment-3074672917.
    // Remove this workaround when a fix is widely installed, perhaps 2027? Candidate:
    // https://github.com/search?q=repo%3Aopenzfs%2Fzfs+%22Linux%3A+zfs_putpage%3A+complete+async+page+writeback+immediately%22&type=commits
#ifdef __linux__
    auto shmFile = path.string() + "-shm";
    struct statfs fs;
    if (statfs(shmFile.c_str(), &fs) == 0 && fs.f_type == /* ZFS_SUPER_MAGIC */ 801189825) {
        /* Never open and close this inode in the SQLite process: POSIX
           close() releases ALL of its fcntl locks on that inode, including
           another connection's WAL locks. Another process could then
           truncate shared memory still mapped here, causing SIGBUS.
           A child does not inherit those locks, so its close is harmless.
           https://sqlite.org/howtocorrupt.html#posix_advisory_locks_canceled_by_a_separate_thread_doing_close_ */
        Pid child = startProcess([&] {
            AutoCloseFD fd = open(shmFile.c_str(), O_RDWR | O_CLOEXEC);
            if (fd && fdatasync(fd.get()) != 0)
                _exit(1);
            _exit(0);
        });
        if (child.wait() != 0)
            throw Error("syncing SQLite shared memory '%s'", shmFile);
    }
#endif

    // useSQLiteWAL also indicates what virtual file system we need.  Using
    // `unix-dotfile` is needed on NFS file systems and on Windows' Subsystem
    // for Linux (WSL) where useSQLiteWAL should be false by default.
    const char * vfs = settings.useWAL ? 0 : "unix-dotfile";
    bool immutable = settings.mode == SQLiteOpenMode::Immutable;
    int flags = immutable ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (settings.externalSynchronization)
        flags |= SQLITE_OPEN_NOMUTEX;
    if (settings.mode == SQLiteOpenMode::Normal)
        flags |= SQLITE_OPEN_CREATE;
    auto uri = "file:" + percentEncode(path.string()) + "?immutable=" + (immutable ? "1" : "0");
    int ret = sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_URI | flags, vfs);
    if (ret != SQLITE_OK) {
        const char * err = sqlite3_errstr(ret);
        throw Error("cannot open SQLite database %s: %s", PathFmt(path), err);
    }

    if (sqlite3_busy_timeout(db, 60 * 60 * 1000) != SQLITE_OK)
        SQLiteError::throw_(db, "setting timeout");

    if (getEnv("NIX_DEBUG_SQLITE_TRACES") == "1") {
        // To debug sqlite statements; trace all of them
        sqlite3_trace(db, &traceSQL, nullptr);
    }

    exec("pragma foreign_keys = 1");
}

SQLite::~SQLite()
{
    try {
        if (db && sqlite3_close(db) != SQLITE_OK)
            SQLiteError::throw_(db, "closing database");
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void SQLite::isCache()
{
    exec("pragma synchronous = off");
    exec("pragma main.journal_mode = wal");
}

void SQLite::exec(const std::string & stmt)
{
    retrySQLite<void>([&]() {
        if (sqlite3_exec(db, stmt.c_str(), 0, 0, 0) != SQLITE_OK)
            SQLiteError::throw_(db, "executing SQLite statement '%s'", stmt);
    });
}

uint64_t SQLite::getLastInsertedRowId()
{
    return sqlite3_last_insert_rowid(db);
}

void SQLiteStmt::create(sqlite3 * db, const std::string & sql)
{
    checkInterrupt();
    assert(!stmt);
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "creating statement '%s'", sql);
    this->db = db;
    this->sql = sql;
}

SQLiteStmt::~SQLiteStmt()
{
    try {
        if (stmt && sqlite3_finalize(stmt) != SQLITE_OK)
            SQLiteError::throw_(db, "finalizing statement '%s'", sql);
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

SQLiteStmt::Use::Use(SQLiteStmt & stmt)
    : stmt(stmt)
{
    assert(stmt.stmt);
    /* A newly prepared statement is ready; every previous Use resets it
       on destruction, including when binding or stepping throws. */
}

SQLiteStmt::Use::~Use()
{
    /* Release any active reader and leave the statement ready for reuse.
       reset returns the preceding step's error, which was handled there. */
    sqlite3_reset(stmt);
}

SQLiteStmt::Use & SQLiteStmt::Use::apply(std::string_view value, bool notNull)
{
    if (notNull) {
        if (sqlite3_bind_text(stmt, curArg++, value.data(), value.size(), SQLITE_TRANSIENT) != SQLITE_OK)
            SQLiteError::throw_(stmt.db, "binding argument");
    } else
        bind();
    return *this;
}

SQLiteStmt::Use & SQLiteStmt::Use::apply(const unsigned char * data, size_t len, bool notNull)
{
    if (notNull) {
        if (sqlite3_bind_blob(stmt, curArg++, data, len, SQLITE_TRANSIENT) != SQLITE_OK)
            SQLiteError::throw_(stmt.db, "binding argument");
    } else
        bind();
    return *this;
}

SQLiteStmt::Use & SQLiteStmt::Use::apply(int64_t value, bool notNull)
{
    if (notNull) {
        if (sqlite3_bind_int64(stmt, curArg++, value) != SQLITE_OK)
            SQLiteError::throw_(stmt.db, "binding argument");
    } else
        bind();
    return *this;
}

SQLiteStmt::Use & SQLiteStmt::Use::bind()
{
    if (sqlite3_bind_null(stmt, curArg++) != SQLITE_OK)
        SQLiteError::throw_(stmt.db, "binding argument");
    return *this;
}

int SQLiteStmt::Use::step()
{
    return sqlite3_step(stmt);
}

void SQLiteStmt::Use::exec()
{
    int r = step();
    assert(r != SQLITE_ROW);
    if (r != SQLITE_DONE)
        SQLiteError::throw_(stmt.db, fmt("executing SQLite statement '%s'", sqlite3_expanded_sql(stmt.stmt)));
}

bool SQLiteStmt::Use::next()
{
    int r = step();
    if (r != SQLITE_DONE && r != SQLITE_ROW)
        SQLiteError::throw_(stmt.db, fmt("executing SQLite query '%s'", sqlite3_expanded_sql(stmt.stmt)));
    return r == SQLITE_ROW;
}

std::string SQLiteStmt::Use::getBlobStr(int col)
{
    auto p = (const char *) sqlite3_column_blob(stmt, col);
    auto n = sqlite3_column_bytes(stmt, col);
    return p ? std::string(p, n) : std::string();
}

std::string SQLiteStmt::Use::getStr(int col)
{
    auto s = (const char *) sqlite3_column_text(stmt, col);
    // FIXME: Don't crash on nulls?
    assert(s);
    /* The length, not the first NUL: the fetcher cache's rows may hold one.
       (`sqlite3_column_bytes` after `sqlite3_column_text`, as SQLite asks.) */
    return std::string(s, sqlite3_column_bytes(stmt, col));
}

int64_t SQLiteStmt::Use::getInt(int col)
{
    // FIXME: detect nulls?
    return sqlite3_column_int64(stmt, col);
}

bool SQLiteStmt::Use::isNull(int col)
{
    return sqlite3_column_type(stmt, col) == SQLITE_NULL;
}

SQLiteTxn::SQLiteTxn(sqlite3 * db)
{
    this->db = db;
    if (sqlite3_exec(db, "begin;", 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "starting transaction");
    active = true;
}

void SQLiteTxn::commit()
{
    if (sqlite3_exec(db, "commit;", 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "committing transaction");
    active = false;
}

SQLiteTxn::~SQLiteTxn()
{
    try {
        if (active && sqlite3_exec(db, "rollback;", 0, 0, 0) != SQLITE_OK)
            SQLiteError::throw_(db, "aborting transaction");
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void handleSQLiteBusy(const SQLiteBusy & e, time_t & nextWarning)
{
    time_t now = time(nullptr);
    if (now > nextWarning) {
        nextWarning = now + 10;
        logWarning({.msg = e.info().msg});
    }

    /* Sleep for a while since retrying the transaction right away
       is likely to fail again. */
    checkInterrupt();
    /* <= 0.1s */
    std::this_thread::sleep_for(std::chrono::milliseconds{rand() % 100});
}

} // namespace nix
