// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// TakeAudition (issue #50): an archived take as a file, attacked from what it
// promises:
//
//   - the file is the take, byte for byte, however large
//   - the same hash is one file: a second ask neither writes nor reads again
//   - a hash without bytes — released, never kept — is nullopt and no file
//   - the files go with the object; what a dead run left is swept by the next
//   - a folder shared with something else keeps that something
//   - a name that is there is a file that is whole: nothing partial is ever
//     named as done

#include "support.hpp"

#include "../app/history/HistoryStore.h"
#include "../app/history/TakeAudition.h"

#include <loopercat/Commands.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

using namespace loopercat;
using history::HistoryStore;
using history::OpStatus;
using history::TakeAudition;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-audition-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

// Deterministic bytes that are not text — every value 0..255 appears.
std::string take(std::size_t size, unsigned seed)
{
    std::string out(size, '\0');
    std::uint32_t x = 2463534242u ^ seed;
    for (auto& c : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x & 0xFF);
    }
    return out;
}

std::set<std::string> names(const fs::path& dir)
{
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out.insert(it->path().filename().string());
    return out;
}

// A store holding takes, each under an op that replaced it.
struct Archive {
    HistoryStore store;
    std::int64_t session;
    int nextOp = 0;
    explicit Archive(const fs::path& dir) : store(dir)
    {
        session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
    }
    std::string keep(const std::string& bytes)
    {
        const auto op = store.beginOp(session, "op-" + std::to_string(++nextOp), "trim", 2000);
        store.keepAudio(op, 1 + nextOp % 99, 1, "take.wav", bytes, 2000);
        store.finishOp(op, OpStatus::done, "");
        return HistoryStore::contentHash(bytes);
    }
    void release(const std::string& hash)
    {
        sqlite::Transaction tx(store.db());
        sqlite::Statement drop(store.db(), "DELETE FROM blobs WHERE hash = ?1");
        drop.bindBlob(1, hash).run();
        sqlite::Statement mark(store.db(), "UPDATE blobs_meta SET released = 3000 WHERE hash = ?1");
        mark.bindBlob(1, hash).run();
        tx.commit();
    }
};

std::string hex(std::string_view raw)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

} // namespace

int main()
{
    // --- the file is the take, named for what it holds, and it is a .wav ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string bytes = take(100000, 1);
        const std::string hash = archive.keep(bytes);

        TakeAudition audition(tmp.path / "audition");
        const auto file = audition.materialize(archive.store, hash);
        CHECK(file.has_value());
        CHECK(file && fs::is_regular_file(*file));
        CHECK(file && commands::readFileBytes(*file) == bytes);
        CHECK(file && file->extension() == ".wav");
        CHECK(file && file->parent_path() == tmp.path / "audition");
        CHECK(file && file->filename().string().find(hex(hash)) != std::string::npos);
        CHECK_EQ(names(tmp.path / "audition").size(), 1u); // nothing partial beside it
    }

    // --- the same hash is one file: a second ask writes nothing ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string hash = archive.keep(take(50000, 2));

        TakeAudition audition(tmp.path / "audition");
        const auto first = audition.materialize(archive.store, hash);
        CHECK(first.has_value());
        // If the second ask wrote again, this mark would be gone.
        commands::writeFileBytes(*first, "touched by the test");
        const auto second = audition.materialize(archive.store, hash);
        CHECK(second.has_value());
        CHECK(first == second);
        CHECK_EQ(commands::readFileBytes(*second), std::string("touched by the test"));
        CHECK_EQ(names(tmp.path / "audition").size(), 1u);
        // The bytes gone from the store change nothing: the file is there already.
        archive.release(hash);
        CHECK(audition.materialize(archive.store, hash) == first);
    }

    // --- two takes are two files; they do not collide ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string a = take(20000, 3);
        const std::string b = take(20000, 4);
        const std::string ha = archive.keep(a);
        const std::string hb = archive.keep(b);

        TakeAudition audition(tmp.path / "audition");
        const auto fa = audition.materialize(archive.store, ha);
        const auto fb = audition.materialize(archive.store, hb);
        CHECK(fa.has_value() && fb.has_value());
        CHECK(fa != fb);
        CHECK(fa && commands::readFileBytes(*fa) == a);
        CHECK(fb && commands::readFileBytes(*fb) == b);
        CHECK_EQ(names(tmp.path / "audition").size(), 2u);
    }

    // --- no bytes, no file: never kept, or released ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string released = archive.keep(take(30000, 5));
        archive.release(released);
        const std::string never = HistoryStore::contentHash("never kept");
        // released: the metadata row is there, the bytes are not
        sqlite::Statement meta(archive.store.db(), "SELECT count(*) FROM blobs_meta WHERE hash = ?1");
        meta.bindBlob(1, released);
        CHECK(meta.step() && meta.integer(0) == 1);

        TakeAudition audition(tmp.path / "audition");
        CHECK(!audition.materialize(archive.store, released).has_value());
        CHECK(!audition.materialize(archive.store, never).has_value());
        CHECK_EQ(names(tmp.path / "audition").size(), 0u);
        // and a hash that is not a hash is refused, not looked up
        CHECK_THROWS(audition.materialize(archive.store, "short"), "32 bytes");
        CHECK_THROWS(audition.materialize(archive.store, std::string(33, 'x')), "32 bytes");
        CHECK_EQ(names(tmp.path / "audition").size(), 0u);
    }

    // --- the files go with the object; a stranger's file stays ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string ha = archive.keep(take(10000, 6));
        const std::string hb = archive.keep(take(10000, 7));
        const fs::path dir = tmp.path / "audition";
        fs::create_directories(dir);
        commands::writeFileBytes(dir / "notes.txt", "someone else's");
        commands::writeFileBytes(dir / "other.wav", "not ours either");
        {
            TakeAudition audition(dir);
            CHECK(audition.materialize(archive.store, ha).has_value());
            CHECK(audition.materialize(archive.store, hb).has_value());
            CHECK_EQ(names(dir).size(), 4u);
        }
        CHECK_EQ(names(dir).size(), 2u);
        CHECK(names(dir).contains("notes.txt"));
        CHECK(names(dir).contains("other.wav"));
        CHECK(fs::is_directory(dir)); // the folder itself is the caller's
    }

    // --- what a dead run left is swept by the next; a stranger's file is not ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string ha = archive.keep(take(10000, 8));
        const std::string hb = archive.keep(take(10000, 9));
        const fs::path dir = tmp.path / "audition";
        // A run that never got to its destructor: leaked on purpose.
        auto* dead = new TakeAudition(dir);
        const auto stale = dead->materialize(archive.store, ha);
        CHECK(stale.has_value());
        // and one it was still writing when it died
        const std::string partial = std::string(TakeAudition::kPrefix) + hex(hb)
                                  + TakeAudition::kSuffix + TakeAudition::kPartial;
        commands::writeFileBytes(dir / partial, "half a take");
        commands::writeFileBytes(dir / "notes.txt", "someone else's");
        commands::writeFileBytes(dir / "take-notahash.wav", "not our shape");
        CHECK_EQ(names(dir).size(), 4u);

        TakeAudition next(dir);
        CHECK_EQ(names(dir).size(), 2u);
        CHECK(names(dir).contains("notes.txt"));
        CHECK(names(dir).contains("take-notahash.wav"));
        CHECK(stale && !fs::exists(*stale));
        // and it works from the clean slate: the stale take is written afresh
        const auto fresh = next.materialize(archive.store, ha);
        CHECK(fresh.has_value());
        CHECK(fresh && commands::readFileBytes(*fresh) == take(10000, 8));
        CHECK_EQ(names(dir).size(), 3u);
        (void) dead; // never deleted: that is the point
    }

    // --- the folder is made when absent, however deep ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string hash = archive.keep(take(1000, 10));
        const fs::path dir = tmp.path / "a" / "b" / "audition";
        CHECK(!fs::exists(dir));
        TakeAudition audition(dir);
        CHECK(fs::is_directory(dir));
        CHECK(audition.materialize(archive.store, hash).has_value());
        CHECK_THROWS(TakeAudition(fs::path()), "needs a path");
    }

    // --- a folder that cannot be made is refused, not used ---
    {
        TempDir tmp;
        commands::writeFileBytes(tmp.path / "not-a-folder", "a file in the way");
        CHECK_THROWS(TakeAudition(tmp.path / "not-a-folder"), "cannot create");
    }

    // --- a large take goes through whole ---
    {
        TempDir tmp;
        Archive archive(tmp.path / "history");
        const std::string big = take(48u * 1024u * 1024u, 11);
        const std::string hash = archive.keep(big);
        TakeAudition audition(tmp.path / "audition");
        const auto file = audition.materialize(archive.store, hash);
        CHECK(file.has_value());
        CHECK(file && fs::file_size(*file) == big.size());
        CHECK(file && commands::readFileBytes(*file) == big);
    }

    return testkit::summary("take_audition_tests");
}
