// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

//==============================================================================
// loopercat::sqlite — the SQLite C API behind RAII and typed errors, and no
// more. Every failure throws loopercat::Error carrying SQLite's own message;
// nothing here returns a code for the caller to forget to check.
//
// Ownership is plain: a Db owns its connection, a Statement its prepared
// statement, a Transaction the outcome of its BEGIN — rolled back unless
// commit() was reached. Blobs are bound WITHOUT a copy: the bytes must stay
// alive until the statement has stepped, which every caller here satisfies
// by binding and stepping inside one call (a 161 MB take is not copied twice
// on its way into the archive).
//==============================================================================
namespace loopercat::sqlite
{

// SQLite takes file names as UTF-8 on every platform. path::string() is the
// ANSI code page on Windows, which mangles any name outside it — a player
// whose account is not named in Latin letters would lose their history.
inline std::string utf8(const std::filesystem::path& file)
{
    const std::u8string text = file.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[noreturn]] inline void fail(sqlite3* db, std::string_view what)
{
    throw Error(std::string(what) + ": "
                + (db != nullptr ? sqlite3_errmsg(db) : "out of memory"));
}

class Db
{
public:
    static Db open(const std::filesystem::path& file)
    {
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(utf8(file).c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                           | SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
        Db db(raw); // owns it even on failure: sqlite3_open_v2 hands one back regardless
        if (rc != SQLITE_OK)
            fail(raw, "cannot open " + utf8(file));
        sqlite3_extended_result_codes(raw, 1);
        return db;
    }

    void exec(std::string_view sql)
    {
        const std::string text(sql);
        if (sqlite3_exec(db_.get(), text.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            fail(db_.get(), "SQL failed");
    }

    sqlite3* raw() const { return db_.get(); }
    std::int64_t lastInsertRowid() const { return sqlite3_last_insert_rowid(db_.get()); }
    int changes() const { return sqlite3_changes(db_.get()); }

private:
    struct Close {
        void operator()(sqlite3* db) const { sqlite3_close_v2(db); }
    };

    explicit Db(sqlite3* raw) : db_(raw) {}
    std::unique_ptr<sqlite3, Close> db_;
};

class Statement
{
public:
    Statement(Db& db, std::string_view sql) : db_(db.raw())
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &raw, nullptr)
            != SQLITE_OK)
            fail(db_, "cannot prepare \"" + std::string(sql) + "\"");
        stmt_.reset(raw);
    }

    Statement& bind(int index, std::int64_t value)
    {
        check(sqlite3_bind_int64(stmt_.get(), index, value), index);
        return *this;
    }

    Statement& bindText(int index, std::string_view text)
    {
        check(sqlite3_bind_text64(stmt_.get(), index, text.data(), text.size(),
                                  SQLITE_TRANSIENT, SQLITE_UTF8),
              index);
        return *this;
    }

    // Bytes are NOT copied — see the header comment.
    Statement& bindBlob(int index, std::string_view bytes)
    {
        check(sqlite3_bind_blob64(stmt_.get(), index, bytes.data(), bytes.size(), SQLITE_STATIC),
              index);
        return *this;
    }

    Statement& bindNull(int index)
    {
        check(sqlite3_bind_null(stmt_.get(), index), index);
        return *this;
    }

    // true: a row is ready to read; false: the statement ran to completion.
    bool step()
    {
        const int rc = sqlite3_step(stmt_.get());
        if (rc == SQLITE_ROW)
            return true;
        if (rc == SQLITE_DONE)
            return false;
        fail(db_, "statement failed");
    }

    // A statement that must not produce rows.
    void run()
    {
        if (step())
            throw Error("statement returned a row where none was expected");
    }

    // Ready to bind and step again; the previous bindings are cleared, so a
    // parameter the next round forgets is NULL — and a NOT NULL column says so.
    void reset()
    {
        sqlite3_reset(stmt_.get());
        sqlite3_clear_bindings(stmt_.get());
    }

    bool isNull(int column) const
    {
        return sqlite3_column_type(stmt_.get(), column) == SQLITE_NULL;
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(stmt_.get(), column); }
    std::string text(int column) const
    {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_.get(), column));
        return p != nullptr ? std::string(p, static_cast<std::size_t>(
                                                 sqlite3_column_bytes(stmt_.get(), column)))
                            : std::string();
    }
    std::string blob(int column) const
    {
        const auto* p = static_cast<const char*>(sqlite3_column_blob(stmt_.get(), column));
        return p != nullptr ? std::string(p, static_cast<std::size_t>(
                                                 sqlite3_column_bytes(stmt_.get(), column)))
                            : std::string();
    }

private:
    void check(int rc, int index)
    {
        if (rc != SQLITE_OK)
            fail(db_, "cannot bind parameter " + std::to_string(index));
    }

    struct Finalize {
        void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
    };

    sqlite3* db_;
    std::unique_ptr<sqlite3_stmt, Finalize> stmt_;
};

// BEGIN IMMEDIATE: the write lock is taken up front, so a transaction never
// discovers halfway through that it cannot write. Rolled back on destruction
// unless commit() ran — an exception anywhere inside leaves nothing behind.
class Transaction
{
public:
    explicit Transaction(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    ~Transaction()
    {
        if (!done_)
            sqlite3_exec(db_.raw(), "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit()
    {
        db_.exec("COMMIT");
        done_ = true;
    }

private:
    Db& db_;
    bool done_ = false;
};

} // namespace loopercat::sqlite
