#pragma once
///@file

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "nix/util/error.hh"

struct sqlite3;
struct sqlite3_stmt;
struct sqlite3_value;

namespace nix {

using SQLiteValue = std::unique_ptr<sqlite3_value, void (*)(sqlite3_value *)>;

enum class SQLiteOpenMode {
    /**
     * Open the database in read-write mode.
     * If the database does not exist, it will be created.
     */
    Normal,
    /**
     * Open the database in read-write mode.
     * Fails with an error if the database does not exist.
     */
    NoCreate,
    /**
     * Open the database in immutable mode.
     * In addition to the database being read-only,
     * no wal or journal files will be created by sqlite.
     * Use this mode if the database is on a read-only filesystem.
     * Fails with an error if the database does not exist.
     */
    Immutable,
};

struct SQLiteSettings
{
    SQLiteOpenMode mode = SQLiteOpenMode::Normal;
    bool useWAL;
    /** The caller serializes all use of this connection and its statements. */
    bool externalSynchronization = false;
};

/**
 * RAII wrapper to close a SQLite database automatically.
 */
struct SQLite
{
    sqlite3 * db = 0;

    SQLite() {}

    using Settings = SQLiteSettings;

    SQLite(const std::filesystem::path & path, Settings && settings);
    SQLite(const SQLite & from) = delete;
    SQLite & operator=(const SQLite & from) = delete;

    // NOTE: This is noexcept since we are only copying and assigning raw pointers.
    SQLite & operator=(SQLite && from) noexcept
    {
        db = from.db;
        from.db = 0;
        return *this;
    }

    ~SQLite();

    operator sqlite3 *()
    {
        return db;
    }

    /**
     * Disable synchronous mode, set truncate journal mode.
     */
    void isCache();

    void exec(const std::string & stmt);

    uint64_t getLastInsertedRowId();
};

/**
 * RAII wrapper to create and destroy SQLite prepared statements.
 */
struct SQLiteStmt
{
    sqlite3 * db = 0;
    sqlite3_stmt * stmt = 0;

    SQLiteStmt() {}

    SQLiteStmt(sqlite3 * db, const std::string & sql)
    {
        create(db, sql);
    }

    void create(sqlite3 * db, const std::string & s);
    ~SQLiteStmt();

    operator sqlite3_stmt *()
    {
        return stmt;
    }

    enum class Bindings { Copy, Borrow };

    /**
     * Borrow requires unchanged text/blob storage through Use exit, which resets then clears all bindings.
     */
    class Use
    {
        friend struct SQLiteStmt;
    private:
        SQLiteStmt & stmt;
        unsigned int curArg = 1;
        const bool borrowed;
        Use(SQLiteStmt & stmt, Bindings bindings);
        Use & checkBind(int result);

    public:

        Use(const Use &) = delete;

        ~Use();

        /**
         * Bind the next parameter.
         */
        Use & apply(std::string_view value, bool notNull = true);
        Use & apply(const unsigned char * data, size_t len, bool notNull = true);
        Use & apply(int64_t value, bool notNull = true);
        Use & apply(const sqlite3_value & value);
        Use & bind(); // null

        /** Restart parameter binding/execution, retaining current bound values. */
        void reset();

        /**
         * Execute a statement that does not return rows.
         */
        void exec();

        /**
         * For statements that return 0 or more rows. Returns true iff
         * a row is available.
         */
        bool next();

        std::string getStr(int col);
        /** BLOB view; throws on OOM. Invalidated by next/exec, reset, Use exit, finalize, or column conversion. */
        std::string_view getBlobView(int col);
        int64_t getInt(int col);
        /** Own the original SQLite storage class and value beyond this statement. */
        SQLiteValue getValue(int col);
        bool isNull(int col);
    };

    Use use(Bindings bindings = Bindings::Copy)
    {
        return Use(*this, bindings);
    }

    /**
     * Execute without rows; pass every parameter and keep borrowed text valid and unchanged throughout this call.
     * Resets and clears all bindings before returning or propagating a bind/execution error.
     */
    template<typename... Args>
    void exec(const Args &... args)
    {
        auto use = this->use(Bindings::Borrow);
        (static_cast<void>(use.apply(args)), ...);
        use.exec();
    }
};

/**
 * RAII helper that ensures transactions are aborted unless explicitly
 * committed.
 */
struct SQLiteTxn
{
    bool active = false;
    sqlite3 * db;

    SQLiteTxn(sqlite3 * db);

    void commit();

    ~SQLiteTxn();
};

class SQLiteError : public CloneableError<SQLiteError, Error>
{
    std::string path;
    std::string errMsg;
    int errNo, extendedErrNo, offset;

    void anchor() override;

public:
    template<typename... Args>
    [[noreturn]] static void throw_(sqlite3 * db, const auto & fs, Args &&... args)
    {
        throw_(db, HintFmt(fs, std::forward<Args>(args)...));
    }

    SQLiteError(const char * path, const char * errMsg, int errNo, int extendedErrNo, int offset, HintFmt && hf);

protected:

    template<typename... Args>
    SQLiteError(
        const char * path,
        const char * errMsg,
        int errNo,
        int extendedErrNo,
        int offset,
        const std::string & fs,
        Args &&... args)
        : SQLiteError(path, errMsg, errNo, extendedErrNo, offset, HintFmt(fs, std::forward<Args>(args)...))
    {
    }

    [[noreturn]] static void throw_(sqlite3 * db, HintFmt && hf);
};

MakeError(SQLiteBusy, SQLiteError);

void handleSQLiteBusy(const SQLiteBusy & e, time_t & nextWarning);

/**
 * Convenience function for retrying a SQLite transaction when the
 * database is busy.
 */
template<typename T, typename F>
T retrySQLite(const F & fun)
{
    time_t nextWarning = time(nullptr) + 1;

    while (true) {
        try {
            return fun();
        } catch (SQLiteBusy & e) {
            handleSQLiteBusy(e, nextWarning);
        }
    }
}

} // namespace nix
