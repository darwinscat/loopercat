// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryStore.h"

#include "Schema.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <map>

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

std::string hex(std::string_view raw)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

} // namespace

HistoryStore::HistoryStore(const std::filesystem::path& dir)
    : file_(dir / "history.db"), db_([&dir, this] {
          std::error_code ec;
          std::filesystem::create_directories(dir, ec);
          if (ec)
              throw Error("cannot create the history directory " + dir.string());
          return sqlite::Db::open(file_);
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

void HistoryStore::recordPresentAudio(std::int64_t op, int slot, int track,
                                      const std::string& name, std::int64_t size,
                                      const std::optional<std::string>& hash)
{
    sqlite::Statement row(db_, "INSERT INTO slot_audio(op, slot, side, track, name, size, hash) "
                               "VALUES (?1, ?2, 'after', ?3, ?4, ?5, ?6)");
    row.bind(1, op).bind(2, slot).bind(3, track).bindText(4, name).bind(5, size);
    if (hash)
        row.bindBlob(6, *hash);
    else
        row.bindNull(6);
    row.run();
}

std::vector<int> HistoryStore::touchedSlots(std::int64_t op)
{
    sqlite::Statement read(db_, "SELECT slot FROM slot_changes WHERE op = ?1 "
                                "UNION SELECT slot FROM slot_audio WHERE op = ?1 ORDER BY slot");
    read.bind(1, op);
    std::vector<int> slots;
    while (read.step())
        slots.push_back(static_cast<int>(read.integer(0)));
    return slots;
}

bool HistoryStore::hasAfterAudio(std::int64_t op, int slot)
{
    sqlite::Statement read(db_, "SELECT 1 FROM slot_audio WHERE op = ?1 AND slot = ?2 "
                                "AND side = 'after' LIMIT 1");
    read.bind(1, op).bind(2, slot);
    return read.step();
}

std::optional<std::string> HistoryStore::hashHeldBefore(std::int64_t op, int slot,
                                                        const std::string& name, std::int64_t size)
{
    sqlite::Statement read(db_, "SELECT hash FROM slot_audio WHERE slot = ?2 AND side = 'after' "
                                "AND name = ?3 AND size = ?4 AND op < ?1 AND hash IS NOT NULL "
                                "ORDER BY op DESC LIMIT 1");
    read.bind(1, op).bind(2, slot).bindText(3, name).bind(4, size);
    if (!read.step())
        return std::nullopt;
    return read.blob(0);
}

std::vector<HistoryStore::TimelineEntry> HistoryStore::slotTimeline(int slot)
{
    std::vector<TimelineEntry> rows;
    sqlite::Statement read(db_,
                           "SELECT o.seq, o.at, o.kind, o.actor, o.status, o.note, "
                           "       c.before_body, c.after_body "
                           "FROM ops o LEFT JOIN slot_changes c ON c.op = o.seq AND c.slot = ?1 "
                           "WHERE o.seq IN (SELECT op FROM slot_changes WHERE slot = ?1 "
                           "                UNION SELECT op FROM slot_audio WHERE slot = ?1) "
                           "ORDER BY o.at, o.seq");
    read.bind(1, slot);
    while (read.step()) {
        TimelineEntry row;
        row.op = read.integer(0);
        row.at = read.integer(1);
        row.kind = read.text(2);
        row.actor = read.text(3);
        row.status = read.text(4);
        row.note = read.isNull(5) ? std::string() : read.text(5);
        if (!read.isNull(6))
            row.beforeBody = read.blob(6);
        if (!read.isNull(7))
            row.afterBody = read.blob(7);
        rows.push_back(std::move(row));
    }

    for (TimelineEntry& row : rows)
        fillSlotFacts(row, slot);
    return rows;
}

void HistoryStore::fillSlotFacts(TimelineEntry& row, int slot)
{
    // The take the row offers: the state's own, or — for a row that has
    // no state, which is what a legacy import leaves — the one it kept.
    sqlite::Statement take(db_, "SELECT name, hash, "
                                "  (SELECT count(*) FROM blobs b WHERE b.hash = a.hash) "
                                "FROM slot_audio a WHERE a.op = ?1 AND a.slot = ?2 "
                                "ORDER BY CASE side WHEN 'after' THEN 0 ELSE 1 END LIMIT 1");
    take.bind(1, row.op).bind(2, slot);
    if (take.step()) {
        row.takeName = take.text(0);
        if (!take.isNull(1))
            row.takeHash = take.blob(1);
        row.takeKept = row.takeHash && take.integer(2) > 0;
    }
    if (row.kind == "swap") {
        sqlite::Statement other(db_, "SELECT slot FROM slot_changes WHERE op = ?1 "
                                     "AND slot <> ?2 LIMIT 1");
        other.bind(1, row.op).bind(2, slot);
        if (other.step())
            row.swappedWith = static_cast<int>(other.integer(0));
    }
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

HistoryStore::Usage HistoryStore::usage()
{
    Usage out {};
    std::error_code ec;
    const auto size = std::filesystem::file_size(file_, ec);
    if (ec)
        throw Error("cannot size " + file_.string() + ": " + ec.message());
    out.fileBytes = static_cast<std::int64_t>(size);
    const std::int64_t pageSize = schema::pragmaInteger(db_, "page_size");
    out.freeBytes = schema::pragmaInteger(db_, "freelist_count") * pageSize;
    // Joined on the key, so no take is read to be counted.
    sqlite::Statement audio(db_, "SELECT coalesce(sum(m.size), 0) FROM blobs_meta m "
                                 "JOIN blobs b ON b.hash = m.hash");
    audio.step();
    out.audioBytes = audio.integer(0);
    out.otherBytes = std::max<std::int64_t>(0, out.fileBytes - out.freeBytes - out.audioBytes);
    const auto space = std::filesystem::space(file_.parent_path(), ec);
    if (ec)
        throw Error("cannot read the free space under " + file_.parent_path().string() + ": "
                    + ec.message());
    out.diskAvailable = static_cast<std::int64_t>(space.available);
    return out;
}

std::optional<std::int64_t> HistoryStore::offeredUndo()
{
    sqlite::Statement read(db_, "SELECT seq FROM ops WHERE status = 'done' AND actor != 'legacy' "
                                "ORDER BY seq DESC LIMIT 1");
    if (!read.step())
        return std::nullopt;
    return read.integer(0);
}

HistoryStore::Holds HistoryStore::holdsOn(const std::string& hash, std::optional<std::int64_t> undoOp)
{
    sqlite::Statement read(db_,
        "SELECT "
        "  (SELECT pinned FROM blobs_meta WHERE hash = ?1) "
        "  OR EXISTS (SELECT 1 FROM slot_audio a JOIN ops o ON o.seq = a.op "
        "             WHERE a.hash = ?1 AND o.pinned = 1) "
        "  OR EXISTS (SELECT 1 FROM legacy_files l JOIN ops o ON o.seq = l.op "
        "             WHERE l.hash = ?1 AND o.pinned = 1), "
        "  EXISTS (SELECT 1 FROM slot_audio a WHERE a.hash = ?1 AND a.side = 'before' AND a.op = ?2), "
        "  EXISTS (SELECT 1 FROM slot_audio a JOIN ops o ON o.seq = a.op "
        "          WHERE a.hash = ?1 AND o.status = 'pending')");
    read.bindBlob(1, hash);
    if (undoOp)
        read.bind(2, *undoOp);
    else
        read.bindNull(2);
    read.step();
    return Holds { read.integer(0) != 0, read.integer(1) != 0, read.integer(2) != 0 };
}

std::vector<retention::Blob> HistoryStore::keptBlobs(std::optional<std::int64_t> undoOp)
{
    std::vector<retention::Blob> out;
    // References: every row that names the hash — a slot's audio on either
    // side, and a legacy file (whose document has no slot_audio row).
    // The label is for a person: the newest slot row naming the take, or the
    // legacy file's path for a document no slot ever named.
    sqlite::Statement read(db_, "SELECT m.hash, m.size, m.created, "
                                "  (SELECT count(*) FROM slot_audio a WHERE a.hash = m.hash) "
                                "  + (SELECT count(*) FROM legacy_files l WHERE l.hash = m.hash), "
                                "  coalesce((SELECT 'slot ' || a.slot || ' ' || a.name FROM slot_audio a "
                                "            WHERE a.hash = m.hash ORDER BY a.op DESC LIMIT 1), "
                                "           (SELECT l.path FROM legacy_files l WHERE l.hash = m.hash "
                                "            ORDER BY l.imported DESC LIMIT 1), '') "
                                "FROM blobs_meta m JOIN blobs b ON b.hash = m.hash "
                                "ORDER BY m.created, m.size DESC, m.hash");
    while (read.step()) {
        retention::Blob blob;
        blob.hash = read.blob(0);
        blob.size = read.integer(1);
        blob.created = read.integer(2);
        blob.references = static_cast<int>(read.integer(3));
        blob.label = read.text(4);
        const Holds holds = holdsOn(blob.hash, undoOp);
        blob.pinned = holds.pinned;
        blob.undo = holds.undo;
        blob.inFlight = holds.inFlight;
        out.push_back(std::move(blob));
    }
    return out;
}

void HistoryStore::pinOp(std::int64_t op, bool pinned)
{
    sqlite::Statement pin(db_, "UPDATE ops SET pinned = ?2 WHERE seq = ?1");
    pin.bind(1, op).bind(2, pinned ? 1 : 0).run();
    if (db_.changes() != 1)
        throw Error("no operation " + std::to_string(op));
}

std::int64_t HistoryStore::releaseBlobs(const std::vector<std::string>& hashes,
                                        std::optional<std::int64_t> undoOp, std::int64_t nowMs)
{
    sqlite::Transaction tx(db_);
    std::int64_t freed = 0;
    sqlite::Statement kept(db_, "SELECT m.size FROM blobs_meta m JOIN blobs b ON b.hash = m.hash "
                                "WHERE m.hash = ?1");
    sqlite::Statement drop(db_, "DELETE FROM blobs WHERE hash = ?1");
    sqlite::Statement mark(db_, "UPDATE blobs_meta SET released = ?2 WHERE hash = ?1");
    for (const std::string& hash : hashes) {
        kept.bindBlob(1, hash);
        if (!kept.step())
            throw Error("no bytes are kept for take " + hex(hash));
        const std::int64_t size = kept.integer(0);
        kept.reset();
        const Holds holds = holdsOn(hash, undoOp);
        if (holds.pinned)
            throw Error("take " + hex(hash) + " is pinned");
        if (holds.undo)
            throw Error("take " + hex(hash) + " is needed by the undo on offer");
        if (holds.inFlight)
            throw Error("take " + hex(hash) + " belongs to an operation still running");
        drop.bindBlob(1, hash).run();
        drop.reset();
        mark.bindBlob(1, hash).bind(2, nowMs).run();
        mark.reset();
        freed += size;
    }
    tx.commit();
    return freed;
}

std::vector<retention::Write> HistoryStore::writes()
{
    std::vector<retention::Write> out;
    sqlite::Statement read(db_, "SELECT created, size FROM blobs_meta ORDER BY created");
    while (read.step())
        out.push_back({ read.integer(0), read.integer(1) });
    return out;
}

std::int64_t HistoryStore::vacuum(int pages)
{
    if (pages < 1)
        throw Error("a vacuum slice is at least one page");
    db_.exec("PRAGMA incremental_vacuum(" + std::to_string(pages) + ")");
    return schema::pragmaInteger(db_, "freelist_count");
}

std::vector<HistoryStore::CardEntry> HistoryStore::cardTimeline()
{
    std::vector<CardEntry> entries;
    sqlite::Statement read(db_, "SELECT seq, at, kind, actor, status, note, pinned FROM ops "
                                "ORDER BY at, seq");
    while (read.step()) {
        CardEntry entry;
        entry.op = read.integer(0);
        entry.at = read.integer(1);
        entry.kind = read.text(2);
        entry.actor = read.text(3);
        entry.status = read.text(4);
        entry.note = read.isNull(5) ? std::string() : read.text(5);
        entry.pinned = read.integer(6) != 0;
        entries.push_back(std::move(entry));
    }

    sqlite::Statement bodies(db_, "SELECT before_body, after_body FROM slot_changes "
                                  "WHERE op = ?1 AND slot = ?2");
    for (CardEntry& entry : entries) {
        for (const int slot : touchedSlots(entry.op)) {
            CardEntry::Slot touched;
            touched.slot = slot;
            TimelineEntry& facts = touched.facts;
            facts.op = entry.op;
            facts.at = entry.at;
            facts.kind = entry.kind;
            facts.actor = entry.actor;
            facts.status = entry.status;
            facts.note = entry.note;
            bodies.bind(1, entry.op).bind(2, slot);
            if (bodies.step()) {
                if (!bodies.isNull(0))
                    facts.beforeBody = bodies.blob(0);
                if (!bodies.isNull(1))
                    facts.afterBody = bodies.blob(1);
            }
            bodies.reset();
            fillSlotFacts(facts, slot);
            entry.slots.push_back(std::move(touched));
        }
    }

    // The last finished operation on a slot, in this order, is the state it
    // is in; a write that failed or was cut off may never have reached the card.
    std::map<int, CardEntry::Slot*> last;
    for (CardEntry& entry : entries)
        if (entry.status == "done")
            for (CardEntry::Slot& touched : entry.slots)
                last[touched.slot] = &touched;
    for (auto& [slot, touched] : last)
        touched->newest = true;
    return entries;
}

} // namespace loopercat::history
