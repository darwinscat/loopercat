// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What the history writes for an RC-5, pinned row for row. One scripted
// session on a synthetic one-track card — the first sighting with takes on
// it, push onto an empty slot and over a take, rename, trim, downmix, set
// tempo, swap, clear, Restore this state, Undo — run through the recorder
// exactly as the app wires it, and every row it leaves dumped in a canonical
// text: the operations, the bodies (by hash), every slot_audio row (track,
// side, name, size, hash) and every sighting.
//
// The expected text (tests/golden/rc5-history-rows.txt) was written by this
// suite from the code as it stood before the history learned tracks. The
// RC-5 has one track, so learning them must not move a byte of what it
// records: any difference is a change in what the recorder writes for the
// one pedal that has a history in the world, and is reported line by line.
//
// What is left out, and why: the files' modification times (a fact about
// this machine's clock, kept as "stamped" or not), and normalize — its gain
// goes through the platform's pow/log, so the bytes it lands are not the same
// on every lane, and a golden that differs by platform pins nothing.
//
//   history_golden_tests           compare with the expected text
//   history_golden_tests --write   write the expected text (only when the
//                                  change in what the RC-5 records is meant)

#include "support.hpp"

#include "../app/history/CardRestore.h"
#include "../app/history/HistoryRecorder.h"
#include "../app/history/HistoryStore.h"
#include "../app/history/UndoRun.h"

#include <loopercat/Commands.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace loopercat;
using history::HistoryRecorder;
using history::HistoryStore;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-golden-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec) {
            std::printf("FAIL cannot remove %s: %s\n", path.string().c_str(), ec.message().c_str());
            ++testkit::checksFailed;
        }
    }
};

std::string bytesOf(const std::vector<unsigned char>& wav)
{
    return std::string(reinterpret_cast<const char*>(wav.data()), wav.size());
}

// A float32 stereo take whose two channels differ — a fold has something to
// do — built from exact values only (a small integer over a power of two),
// so its bytes are the same on every platform.
std::string stereoTake(int frames)
{
    std::vector<unsigned char> b;
    const auto ascii = [&b](std::string_view t) {
        for (const char c : t)
            b.push_back(static_cast<unsigned char>(c));
    };
    const auto p16 = [&b](int v) {
        b.push_back(static_cast<unsigned char>(v & 0xff));
        b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](int v) { p16(v & 0xffff); p16((v >> 16) & 0xffff); };
    const auto sample = [&b](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        for (int shift = 0; shift < 32; shift += 8)
            b.push_back(static_cast<unsigned char>((bits >> shift) & 0xffu));
    };
    const int dataSize = frames * 8;
    ascii("RIFF"); p32(12 + 24 + 8 + dataSize - 8); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); p16(2); p32(wav::kSampleRate); p32(wav::kSampleRate * 8); p16(8); p16(32);
    ascii("data"); p32(dataSize);
    for (int frame = 0; frame < frames; ++frame) {
        sample(static_cast<float>(frame % 200 - 100) / 128.0f);
        sample(static_cast<float>(frame % 50) / 64.0f);
    }
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

fs::path makePedal(const fs::path& root)
{
    const fs::path volume = root / "BOSS RC-5";
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    const std::string text = testkit::syntheticMemoryText();
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo),
                                 rc0::setTailMarker(text, fileNo));
    return volume;
}

// A take the pedal recorded: the file in the slot's folder and a body that
// says so, written before the history has met the card.
void recordOnPedal(const fs::path& volume, int slot, const std::string& name,
                   const std::string& bytes, std::int64_t frames)
{
    fs::create_directories(volume::wavDir(volume, slot));
    commands::writeFileBytes(volume::wavDir(volume, slot) / name, bytes);
    const std::string text = commands::readMemory(volume);
    std::string body = rc0::slotBody(text, slot);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavStat", rc0::kWavStatIndexed);
    body = rc0::setSectionField(body, rc0::kSectionTrack1, "WavLen", frames);
    commands::writeMemoryPair(volume, rc0::replaceSlotBody(text, slot, body),
                              { .journal = { .bodiesChanging = [](const auto&) {},
                                             .systemChanging = [](const auto&) {} } });
}

std::string hex(const std::string& raw, std::size_t digits)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
        if (out.size() >= digits)
            break;
    }
    return out.substr(0, digits);
}

std::string bodyKey(sqlite::Statement& row, int column)
{
    return row.isNull(column) ? "-" : hex(HistoryStore::contentHash(row.blob(column)), 16);
}

// Every row the session left, in one canonical order.
std::string dump(HistoryStore& store)
{
    std::ostringstream out;
    sqlite::Statement ops(store.db(), "SELECT seq, kind, status, coalesce(note, ''), "
                                      "coalesce(reverts, 0) FROM ops ORDER BY seq");
    while (ops.step()) {
        const std::int64_t op = ops.integer(0);
        out << "op " << op << " " << ops.text(1) << " " << ops.text(2) << " reverts="
            << ops.integer(4) << " note=" << ops.text(3) << "\n";
        sqlite::Statement bodies(store.db(), "SELECT slot, before_body, after_body, "
                                             "coalesce(snapshot_error, '') FROM slot_changes "
                                             "WHERE op = ?1 ORDER BY slot");
        bodies.bind(1, op);
        while (bodies.step())
            out << "  body slot=" << bodies.integer(0) << " before=" << bodyKey(bodies, 1)
                << " after=" << bodyKey(bodies, 2) << " error=" << bodies.text(3) << "\n";
        sqlite::Statement audio(store.db(), "SELECT slot, side, track, name, size, hash, "
                                            "modified IS NOT NULL FROM slot_audio WHERE op = ?1 "
                                            "ORDER BY slot, side, track, name");
        audio.bind(1, op);
        while (audio.step())
            out << "  audio slot=" << audio.integer(0) << " " << audio.text(1)
                << " track=" << audio.integer(2) << " name=" << audio.text(3)
                << " size=" << audio.integer(4)
                << " hash=" << (audio.isNull(5) ? std::string("-") : hex(audio.blob(5), 16))
                << (audio.integer(6) != 0 ? " stamped" : " unstamped") << "\n";
        sqlite::Statement subjects(store.db(), "SELECT slot FROM op_subjects WHERE op = ?1 "
                                               "ORDER BY slot");
        subjects.bind(1, op);
        while (subjects.step())
            out << "  subject slot=" << subjects.integer(0) << "\n";
    }
    sqlite::Statement sightings(store.db(), "SELECT slot, name, size, hash, coalesce(since_op, 0) "
                                            "FROM take_sightings ORDER BY slot, name, size");
    while (sightings.step())
        out << "sighting slot=" << sightings.integer(0) << " name=" << sightings.text(1)
            << " size=" << sightings.integer(2) << " hash=" << hex(sightings.blob(3), 16)
            << " since=" << sightings.integer(4) << "\n";
    sqlite::Statement blobs(store.db(), "SELECT hash, size FROM blobs_meta ORDER BY hash");
    while (blobs.step())
        out << "blob " << hex(blobs.blob(0), 16) << " size=" << blobs.integer(1) << "\n";
    return out.str();
}

std::string readText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv)
{
    const bool write = argc > 1 && std::string(argv[1]) == "--write";
    const fs::path expectedFile = LOOPERCAT_HISTORY_GOLDEN;
    std::string got;
    {
        TempDir tmp;
        const fs::path volume = makePedal(tmp.path);
        recordOnPedal(volume, 3, "003_1.WAV",
                      bytesOf(testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 4410 })), 4410);
        recordOnPedal(volume, 5, "song.wav", stereoTake(88200), 88200);
        recordOnPedal(volume, 9, "009_1.WAV", stereoTake(22050), 22050);

        std::int64_t now = 1'760'000'000'000;
        const auto recorder = std::make_shared<HistoryRecorder>(tmp.path / "history",
                                                                [&now] { return now += 1000; });
        const auto snapshot = recorder->firstSeen(volume);
        CHECK(snapshot.has_value());
        if (snapshot)
            for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
                recorder->snapshotStep(*snapshot, slot);

        int next = 0;
        const auto run = [&](const std::string& kind, const auto& job) {
            const std::string opId = "golden-" + std::to_string(++next) + "-" + kind;
            recorder->begin(opId, kind, volume);
            std::string error;
            try {
                job(history::withHistory(recorder, { .opId = opId }));
            } catch (const std::exception& e) {
                error = e.what();
            }
            CHECK_EQ(error, std::string());
            recorder->finish(opId, error);
        };
        const fs::path incoming = tmp.path / "incoming.wav";
        commands::writeFileBytes(incoming, stereoTake(132300));
        run("push", [&](const commands::WriteOptions& o) {
            commands::push(volume, incoming, 11, { .name = std::string("Pushed"), .write = o });
        });
        commands::writeFileBytes(incoming,
                                 bytesOf(testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 176400 })));
        run("push", [&](const commands::WriteOptions& o) {
            commands::push(volume, incoming, 3, { .force = true, .write = o });
        });
        run("rename", [&](const commands::WriteOptions& o) { commands::rename(volume, 5, "Song", o); });
        run("trim", [&](const commands::WriteOptions& o) {
            commands::trim(volume, 5, 0, 44100, { .write = o });
        });
        run("downmix", [&](const commands::WriteOptions& o) {
            commands::downmixToMono(volume, 9, { .write = o });
        });
        run("tempo", [&](const commands::WriteOptions& o) { commands::setTempo(volume, 11, 905, o); });
        run("swap", [&](const commands::WriteOptions& o) { commands::swap(volume, 3, 11, o); });
        run("clear", [&](const commands::WriteOptions& o) {
            commands::clear(volume, { 5 }, { .write = o });
        });
        // Restore this state: slot 5 back to what the trim left in it.
        std::int64_t trimOp = 0;
        for (const auto& entry : recorder->store().cardTimeline())
            if (entry.kind == "trim")
                trimOp = entry.op;
        CHECK(trimOp != 0);
        run("restore", [&](const commands::WriteOptions& o) {
            history::restoreOperation(recorder->store(), trimOp, volume, o);
        });
        // Undo the restore: the cleared slot comes back, its take archived.
        const auto offered = recorder->store().offeredTargets().undo;
        CHECK(offered.has_value());
        if (offered) {
            const std::string opId = "golden-undo";
            const history::undo::Checked checked =
                history::undo::beginPress(*recorder, opId, false, *offered, volume);
            std::string error;
            try {
                history::undo::apply(recorder->store(), checked.plan, volume,
                                     history::withHistory(recorder, { .opId = opId }));
            } catch (const std::exception& e) {
                error = e.what();
            }
            CHECK_EQ(error, std::string());
            recorder->finish(opId, error, checked.note);
        }
        recorder->disconnect();
        got = "# what the history records for an RC-5 session; see history_golden_tests.cpp\n"
            + dump(recorder->store());
    }

    if (write) {
        fs::create_directories(expectedFile.parent_path());
        commands::writeFileBytes(expectedFile, got);
        std::printf("wrote %s\n", expectedFile.string().c_str());
        return testkit::summary("history_golden_tests");
    }
    const std::string expected = readText(expectedFile);
    CHECK(!expected.empty());
    if (got != expected) {
        // Line by line, so the report names what moved.
        std::istringstream a(expected), b(got);
        std::string left, right;
        int line = 0;
        while (true) {
            const bool moreA = static_cast<bool>(std::getline(a, left));
            const bool moreB = static_cast<bool>(std::getline(b, right));
            if (!moreA && !moreB)
                break;
            ++line;
            if (!moreA || !moreB || left != right) {
                std::printf("line %d\n  expected: %s\n  got:      %s\n", line,
                            moreA ? left.c_str() : "<end>", moreB ? right.c_str() : "<end>");
            }
        }
        CHECK(got == expected);
    }
    return testkit::summary("history_golden_tests");
}
