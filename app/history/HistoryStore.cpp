// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryStore.h"

#include "Schema.h"

#include <juce_cryptography/juce_cryptography.h>

namespace loopercat::history
{

namespace
{

// Every storage property the store's promises rest on, read back rather than
// assumed: a pragma that silently did not take is how a file meant to return
// space never can, or a foreign key meant to hold does not.
void requirePragma(sqlite::Db& db, const std::string& pragma, const std::string& expected)
{
    const std::string found = schema::pragmaText(db, pragma);
    if (found != expected)
        throw Error("the history store needs " + pragma + " = " + expected + ", found " + found);
}

} // namespace

HistoryStore::HistoryStore(const std::filesystem::path& dir)
    : db_([&dir] {
          std::error_code ec;
          std::filesystem::create_directories(dir, ec);
          if (ec)
              throw Error("cannot create the history directory " + dir.string());
          return sqlite::Db::open(dir / "history.db");
      }())
{
    sqlite3_busy_timeout(db_.raw(), 5000);
    // Both take effect only on a file that has no tables yet, which is the
    // whole point: they are set before migrate() creates the first one.
    db_.exec("PRAGMA page_size = 16384");
    db_.exec("PRAGMA auto_vacuum = INCREMENTAL");
    db_.exec("PRAGMA journal_mode = DELETE");
    db_.exec("PRAGMA synchronous = FULL");
    db_.exec("PRAGMA foreign_keys = ON");

    // A file that holds tables but no store version is some other program's
    // database: refused before anything is created in it.
    if (schema::pragmaInteger(db_, "user_version") == 0) {
        sqlite::Statement existing(db_, "SELECT count(*) FROM sqlite_master WHERE type = 'table'");
        existing.step();
        if (existing.integer(0) != 0)
            throw Error((dir / "history.db").string()
                        + " holds tables but is not a LooperCat history");
    }

    schema::migrate(db_);

    requirePragma(db_, "journal_mode", "delete");
    requirePragma(db_, "foreign_keys", "1");
    requirePragma(db_, "auto_vacuum", "2"); // INCREMENTAL
    requirePragma(db_, "page_size", "16384");

    db_.exec("UPDATE ops SET status = 'interrupted' WHERE status = 'pending'");
}

std::string HistoryStore::contentHash(std::string_view bytes)
{
    const juce::SHA256 sha(bytes.data(), bytes.size());
    const juce::MemoryBlock raw = sha.getRawData();
    return std::string(static_cast<const char*>(raw.getData()), raw.getSize());
}

std::int64_t HistoryStore::card(const std::string& model, const std::string& label,
                                std::int64_t nowMs)
{
    sqlite::Transaction tx(db_);
    sqlite::Statement find(db_, "SELECT id FROM cards WHERE model = ?1 AND label = ?2");
    find.bindText(1, model).bindText(2, label);
    std::int64_t id = 0;
    if (find.step()) {
        id = find.integer(0);
        sqlite::Statement seen(db_, "UPDATE cards SET last_seen = ?2 WHERE id = ?1");
        seen.bind(1, id).bind(2, nowMs).run();
    } else {
        sqlite::Statement add(db_, "INSERT INTO cards(model, label, first_seen, last_seen) "
                                   "VALUES (?1, ?2, ?3, ?3)");
        add.bindText(1, model).bindText(2, label).bind(3, nowMs).run();
        id = db_.lastInsertRowid();
    }
    tx.commit();
    return id;
}

std::int64_t HistoryStore::openSession(std::int64_t card, std::int64_t nowMs)
{
    sqlite::Statement add(db_, "INSERT INTO sessions(card, connected_at) VALUES (?1, ?2)");
    add.bind(1, card).bind(2, nowMs).run();
    return db_.lastInsertRowid();
}

void HistoryStore::closeSession(std::int64_t session, std::int64_t nowMs)
{
    sqlite::Statement close(db_, "UPDATE sessions SET disconnected_at = ?2 "
                                 "WHERE id = ?1 AND disconnected_at IS NULL");
    close.bind(1, session).bind(2, nowMs).run();
    if (db_.changes() != 1)
        throw Error("session " + std::to_string(session) + " is not open");
}

std::int64_t HistoryStore::beginOp(std::int64_t session, const std::string& opId,
                                   const std::string& kind, std::int64_t atMs)
{
    sqlite::Statement add(db_, "INSERT INTO ops(id, session, kind, actor, status, at) "
                               "VALUES (?1, ?2, ?3, 'app', 'pending', ?4)");
    add.bindText(1, opId).bind(2, session).bindText(3, kind).bind(4, atMs).run();
    return db_.lastInsertRowid();
}

bool HistoryStore::keepBlob(const std::string& hash, std::string_view bytes, std::int64_t nowMs)
{
    sqlite::Statement meta(db_, "SELECT released IS NOT NULL FROM blobs_meta WHERE hash = ?1");
    meta.bindBlob(1, hash);
    const bool known = meta.step();
    const bool released = known && meta.integer(0) != 0;
    // Metadata first: the bytes' foreign key points at it.
    if (!known) {
        sqlite::Statement add(db_, "INSERT INTO blobs_meta(hash, size, created, pinned) "
                                   "VALUES (?1, ?2, ?3, 0)");
        add.bindBlob(1, hash).bind(2, static_cast<std::int64_t>(bytes.size())).bind(3, nowMs).run();
    } else if (released) {
        sqlite::Statement back(db_, "UPDATE blobs_meta SET released = NULL WHERE hash = ?1");
        back.bindBlob(1, hash).run();
    }
    if (!known || released) {
        sqlite::Statement put(db_, "INSERT INTO blobs(hash, bytes) VALUES (?1, ?2)");
        put.bindBlob(1, hash).bindBlob(2, bytes).run();
        return true;
    }
    return false;
}

void HistoryStore::keepAudio(std::int64_t op, int slot, int track, const std::string& name,
                             std::string_view bytes, std::int64_t nowMs)
{
    const std::string hash = contentHash(bytes);
    sqlite::Transaction tx(db_);
    keepBlob(hash, bytes, nowMs);
    sqlite::Statement row(db_, "INSERT INTO slot_audio(op, slot, side, track, name, size, hash) "
                               "VALUES (?1, ?2, 'before', ?3, ?4, ?5, ?6)");
    row.bind(1, op).bind(2, slot).bind(3, track).bindText(4, name)
        .bind(5, static_cast<std::int64_t>(bytes.size())).bindBlob(6, hash).run();
    tx.commit();
}

void HistoryStore::recordBodies(std::int64_t op, const std::vector<commands::SlotChange>& changes)
{
    sqlite::Transaction tx(db_);
    sqlite::Statement row(db_, "INSERT INTO slot_changes(op, slot, before_body, after_body) "
                               "VALUES (?1, ?2, ?3, ?4)");
    for (const auto& change : changes) {
        row.bind(1, op).bind(2, change.slot).bindBlob(3, change.before).bindBlob(4, change.after);
        row.run();
        row.reset();
    }
    tx.commit();
}

void HistoryStore::recordLanded(std::int64_t op, int slot, int track, const std::string& name,
                                std::string_view bytes)
{
    sqlite::Statement row(db_, "INSERT INTO slot_audio(op, slot, side, track, name, size, hash) "
                               "VALUES (?1, ?2, 'after', ?3, ?4, ?5, ?6)");
    const std::string hash = contentHash(bytes);
    row.bind(1, op).bind(2, slot).bind(3, track).bindText(4, name)
        .bind(5, static_cast<std::int64_t>(bytes.size())).bindBlob(6, hash).run();
}

void HistoryStore::finishOp(std::int64_t op, OpStatus status, const std::string& note)
{
    sqlite::Statement finish(db_, "UPDATE ops SET status = ?2, note = ?3 "
                                  "WHERE seq = ?1 AND status = 'pending'");
    finish.bind(1, op).bindText(2, status == OpStatus::done ? "done" : "failed");
    if (note.empty())
        finish.bindNull(3);
    else
        finish.bindText(3, note);
    finish.run();
    if (db_.changes() != 1)
        throw Error("operation " + std::to_string(op) + " is not pending");
}

std::optional<std::string> HistoryStore::takeBytes(const std::string& hash)
{
    sqlite::Statement read(db_, "SELECT bytes FROM blobs WHERE hash = ?1");
    read.bindBlob(1, hash);
    if (!read.step())
        return std::nullopt;
    return read.blob(0);
}

std::string HistoryStore::opStatus(std::int64_t op)
{
    sqlite::Statement read(db_, "SELECT status FROM ops WHERE seq = ?1");
    read.bind(1, op);
    if (!read.step())
        throw Error("no operation " + std::to_string(op));
    return read.text(0);
}

std::optional<HistoryStore::OpIdentity> HistoryStore::findOp(const std::string& opId)
{
    sqlite::Statement read(db_, "SELECT seq, actor FROM ops WHERE id = ?1");
    read.bindText(1, opId);
    if (!read.step())
        return std::nullopt;
    return OpIdentity { read.integer(0), read.text(1) };
}

std::int64_t HistoryStore::recordLegacyOp(std::int64_t session, const std::string& opId,
                                          std::int64_t atMs, const std::string& note)
{
    sqlite::Statement add(db_, "INSERT INTO ops(id, session, kind, actor, status, at, note) "
                               "VALUES (?1, ?2, 'legacy', 'legacy', 'done', ?3, ?4)");
    add.bindText(1, opId).bind(2, session).bind(3, atMs).bindText(4, note).run();
    return db_.lastInsertRowid();
}

bool HistoryStore::recordLegacyFile(std::int64_t op, const std::string& path, const char* kind,
                                    std::string_view bytes, std::int64_t nowMs,
                                    const std::string& hash)
{
    const bool written = keepBlob(hash, bytes, nowMs);
    sqlite::Statement ledger(db_, "INSERT INTO legacy_files(path, op, kind, hash, imported) "
                                  "VALUES (?1, ?2, ?3, ?4, ?5)");
    ledger.bindText(1, path).bind(2, op).bindText(3, kind).bindBlob(4, hash).bind(5, nowMs).run();
    return written;
}

bool HistoryStore::keepLegacyTake(std::int64_t op, const std::string& path, int slot, int track,
                                  const std::string& name, std::string_view bytes,
                                  std::int64_t nowMs)
{
    const std::string hash = contentHash(bytes);
    sqlite::Transaction tx(db_);
    const bool written = recordLegacyFile(op, path, "take", bytes, nowMs, hash);
    sqlite::Statement row(db_, "INSERT INTO slot_audio(op, slot, side, track, name, size, hash) "
                               "VALUES (?1, ?2, 'before', ?3, ?4, ?5, ?6)");
    row.bind(1, op).bind(2, slot).bind(3, track).bindText(4, name)
        .bind(5, static_cast<std::int64_t>(bytes.size())).bindBlob(6, hash).run();
    tx.commit();
    return written;
}

bool HistoryStore::keepLegacyDocument(std::int64_t op, const std::string& path,
                                      std::string_view bytes, std::int64_t nowMs)
{
    const std::string hash = contentHash(bytes);
    sqlite::Transaction tx(db_);
    const bool written = recordLegacyFile(op, path, "document", bytes, nowMs, hash);
    tx.commit();
    return written;
}

bool HistoryStore::legacyFileImported(const std::string& path)
{
    sqlite::Statement read(db_, "SELECT 1 FROM legacy_files WHERE path = ?1");
    read.bindText(1, path);
    return read.step();
}

} // namespace loopercat::history
