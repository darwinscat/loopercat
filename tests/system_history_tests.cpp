// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal's own settings in the history (#73, SYSTEM*.RC0), attacked from
// what the store promises:
//
//   - a change is one row per operation and section, the section's own
//     text before and after, back byte for byte
//   - a row that is not a section's text is refused: an unknown section,
//     an empty text, another tag's text, a change that changed nothing
//   - the timeline shows such an operation as an entry with no slots and
//     the sections it changed, in section order
//   - nothing here touches the takes: freeing space sees no new bytes
//   - the recorder writes the rows under the operation that has begun, and
//     refuses one that has not

#include "support.hpp"

#include "../app/history/HistoryRecorder.h"
#include "../app/history/HistoryStore.h"

#include <loopercat/Commands.hpp>
#include <loopercat/SystemFile.hpp>
#include <loopercat/Volume.hpp>

#include <chrono>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <string>

using namespace loopercat;
using history::HistoryRecorder;
using history::HistoryStore;
using history::OpStatus;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-system-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::int64_t count(sqlite::Db& db, const std::string& sql)
{
    sqlite::Statement read(db, sql);
    if (!read.step())
        throw Error("count returned no row: " + sql);
    return read.integer(0);
}

// A section as the file writes it: the tag, tab-indented fields, the tag.
std::string ctl(long long ctl1, long long ctl2, long long cc80 = 0)
{
    return "<CTL>\n\t<Pedal1>4</Pedal1>\n\t<Ctl1>" + std::to_string(ctl1) + "</Ctl1>\n\t<Ctl2>"
        + std::to_string(ctl2) + "</Ctl2>\n\t<Exp>0</Exp>\n\t<Cc80>" + std::to_string(cc80)
        + "</Cc80>\n</CTL>";
}
std::string midi(long long channel)
{
    return "<MIDI>\n\t<RxCh>" + std::to_string(channel) + "</RxCh>\n\t<TxCh>0</TxCh>\n</MIDI>";
}

// The settings file read off real hardware (fixtures/rc5-system.RC0), and a
// card carrying it as its two banks, so writeSystemPair has a counter to go on.
std::string systemFixture()
{
    std::ifstream in(LOOPERCAT_RC5_SYSTEM, std::ios::binary);
    if (!in)
        throw Error("cannot open fixture: " LOOPERCAT_RC5_SYSTEM);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path makeCard(const fs::path& root, const std::string& system)
{
    const fs::path volume = root / "BOSS RC-5";
    fs::create_directories(volume::dataDir(volume));
    fs::create_directories(volume / "ROLAND" / "WAVE");
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::systemPath(volume, fileNo),
                                 rc0::setTailGeneration(system, 100u + static_cast<unsigned>(fileNo)));
    return volume;
}

struct Ready {
    HistoryStore store;
    std::int64_t session;
    std::int64_t clock = 10'000;
    int ops = 0;
    explicit Ready(const fs::path& dir) : store(dir)
    {
        session = store.openSession(store.card("RC-5", "BOSS RC-5", 1000), 1000);
    }
    std::int64_t begin(const std::string& kind)
    {
        return store.beginOp(session, "op-" + std::to_string(++ops), kind, ++clock);
    }
};

} // namespace

int main()
{
    // --- one row per operation and section, byte for byte ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const auto op = r.begin("controls");
        r.store.recordSystemChange(op, { "CTL", ctl(17, 18), ctl(17, 22) });
        r.store.recordSystemChange(op, { "MIDI", midi(1), midi(2) });
        r.store.finishOp(op, OpStatus::done, "");
        const auto rows = r.store.systemChanges(op);
        CHECK_EQ(rows.size(), 2u);
        CHECK(rows.size() == 2 && rows[0].section == "MIDI"); // section order, not insertion order
        CHECK(rows.size() == 2 && rows[1].section == "CTL");
        CHECK(rows.size() == 2 && rows[1].before == ctl(17, 18));
        CHECK(rows.size() == 2 && rows[1].after == ctl(17, 22));
        CHECK(rows.size() == 2 && rows[0].before == midi(1) && rows[0].after == midi(2));
        CHECK(r.store.systemChanges(424242).empty());
        // the same section twice in one operation is not a second row
        CHECK_THROWS(r.store.recordSystemChange(op, { "CTL", ctl(1, 1), ctl(1, 2) }), "UNIQUE");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM system_changes"), 2);
        // and it survives a reopen
        HistoryStore reopened(tmp.path);
        CHECK_EQ(reopened.systemChanges(op).size(), 2u);
        CHECK(reopened.systemChanges(op).back().before == ctl(17, 18));
    }

    // --- what is not a section's change is refused, by name ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const auto op = r.begin("controls");
        CHECK_THROWS(r.store.recordSystemChange(op, { "MEM", ctl(1, 1), ctl(1, 2) }), "not a section");
        CHECK_THROWS(r.store.recordSystemChange(op, { "", ctl(1, 1), ctl(1, 2) }), "not a section");
        CHECK_THROWS(r.store.recordSystemChange(op, { "ctl", ctl(1, 1), ctl(1, 2) }), "not a section");
        CHECK_THROWS(r.store.recordSystemChange(op, { "CTL", "", ctl(1, 2) }), "section's own text");
        CHECK_THROWS(r.store.recordSystemChange(op, { "CTL", ctl(1, 1), "" }), "section's own text");
        CHECK_THROWS(r.store.recordSystemChange(op, { "CTL", midi(1), ctl(1, 2) }), "section's own text");
        CHECK_THROWS(r.store.recordSystemChange(op, { "CTL", ctl(1, 1), ctl(1, 2) + "\n<CTL>" }), "section's own text");
        CHECK_THROWS(r.store.recordSystemChange(op, { "CTL", "<CTL>x", ctl(1, 2) }), "section's own text");
        CHECK_THROWS(r.store.recordSystemChange(op, { "CTL", ctl(1, 1), ctl(1, 1) }), "did not change");
        CHECK_THROWS(r.store.recordSystemChange(9999, { "CTL", ctl(1, 1), ctl(1, 2) }), "FOREIGN KEY");
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM system_changes"), 0);
        // a section the file has, whatever its name suggests about controls
        r.store.recordSystemChange(op, { sysfile::kSectionSetup.data(), "<SETUP>\n\t<X>1</X>\n</SETUP>", "<SETUP>\n\t<X>2</X>\n</SETUP>" });
        CHECK_EQ(count(r.store.db(), "SELECT count(*) FROM system_changes"), 1);
    }

    // --- the timeline: an entry with no slots and the sections it changed ---
    {
        TempDir tmp;
        Ready r(tmp.path);
        const auto rename = r.begin("rename");
        r.store.recordBodies(rename, { { 5, "before", "after" } });
        r.store.finishOp(rename, OpStatus::done, "");
        const auto controls = r.begin("controls");
        r.store.recordSystemChange(controls, { "CTL", ctl(17, 18), ctl(17, 22) });
        r.store.recordSystemChange(controls, { "SETUP", "<SETUP>\n\t<X>1</X>\n</SETUP>", "<SETUP>\n\t<X>2</X>\n</SETUP>" });
        r.store.finishOp(controls, OpStatus::done, "");

        const auto entries = r.store.cardTimeline();
        CHECK_EQ(entries.size(), 2u);
        CHECK(entries.size() == 2 && entries[0].system.empty());            // the rename changed no setting
        CHECK(entries.size() == 2 && entries[1].slots.empty());             // the controls change touched no slot
        CHECK(entries.size() == 2 && entries[1].system.size() == 2);
        CHECK(entries.size() == 2 && entries[1].system.size() == 2 && entries[1].system[0].section == "SETUP");
        CHECK(entries.size() == 2 && entries[1].system.size() == 2 && entries[1].system[1].section == "CTL");
        CHECK(entries.size() == 2 && entries[1].system.size() == 2 && entries[1].system[1].before == ctl(17, 18));
        // and it is an operation like any other to the cursor
        CHECK(r.store.offeredTargets().undo == controls);
        // freeing space sees nothing new: the sections are rows, not bytes
        CHECK(r.store.keptBlobs({}).empty());
        CHECK_EQ(r.store.usage().audioBytes, 0);
    }

    // --- the recorder: rows under the operation that has begun, and refused otherwise ---
    {
        TempDir tmp;
        HistoryRecorder rec(tmp.path / "history", "RC-5", [] { return std::int64_t { 5000 }; });
        const fs::path volume = tmp.path / "BOSS RC-5";
        fs::create_directories(volume);
        rec.begin("op-controls", "controls", volume);
        rec.systemChanges("op-controls", { { "CTL", ctl(17, 18), ctl(17, 22) } });
        rec.finish("op-controls", "");
        sqlite::Db& db = rec.store().db();
        CHECK_EQ(count(db, "SELECT count(*) FROM system_changes WHERE section = 'CTL'"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE kind = 'controls' AND status = 'done'"), 1);
        CHECK_THROWS(rec.systemChanges("op-never", { { "CTL", ctl(1, 1), ctl(1, 2) } }), "without having begun");
        // an empty report is nothing to record, not an error
        rec.begin("op-quiet", "controls", volume);
        rec.systemChanges("op-quiet", {});
        rec.finish("op-quiet", "");
        CHECK_EQ(count(db, "SELECT count(*) FROM system_changes"), 1);
    }

    // --- end to end: a settings write through the history, as the app wires it ---
    {
        TempDir tmp;
        const std::string original = systemFixture();
        const fs::path volume = makeCard(tmp.path, original);
        auto rec = std::make_shared<HistoryRecorder>(tmp.path / "history", "RC-5",
                                                     [] { return std::int64_t { 7000 }; });
        const std::string edited = sysfile::setField(original, sysfile::kSectionCtl, "Ctl2", 22);
        CHECK(sysfile::field(original, sysfile::kSectionCtl, "Ctl2") != 22);

        // the app's wiring: the hook lands in the history under the operation
        auto options = history::withHistory(rec, { .opId = "op-controls", .skipBackup = true }, nullptr);
        rec->begin("op-controls", "controls", volume);
        commands::writeSystemPair(volume, edited, options);
        rec->finish("op-controls", "");
        sqlite::Db& db = rec->store().db();
        CHECK_EQ(count(db, "SELECT count(*) FROM system_changes"), 1);
        const auto rows = rec->store().systemChanges(1);
        CHECK_EQ(rows.size(), 1u);
        CHECK(rows.size() == 1 && rows.front().section == sysfile::kSectionCtl);
        CHECK(rows.size() == 1 && rows.front().before == sysfile::sectionText(original, sysfile::kSectionCtl));
        CHECK(rows.size() == 1 && rows.front().after == sysfile::sectionText(edited, sysfile::kSectionCtl));
        CHECK_EQ(sysfile::field(commands::readSystem(volume), sysfile::kSectionCtl, "Ctl2"), 22);
        // and the timeline reads it back as an operation on no slot
        const auto entries = rec->store().cardTimeline();
        CHECK(entries.size() == 1 && entries.front().slots.empty() && entries.front().system.size() == 1);

        // a write for an operation that never began: the history refuses
        // before the write, and the card stays as it was
        auto ghost = history::withHistory(rec, { .opId = "op-ghost", .skipBackup = true }, nullptr);
        const std::string another = sysfile::setField(edited, sysfile::kSectionCtl, "Ctl2", 33);
        CHECK_THROWS(commands::writeSystemPair(volume, another, ghost), "without having begun");
        CHECK_EQ(sysfile::field(commands::readSystem(volume), sysfile::kSectionCtl, "Ctl2"), 22);
        CHECK_EQ(count(db, "SELECT count(*) FROM system_changes"), 1);

        // a write that changes nothing records nothing, and still writes
        auto quiet = history::withHistory(rec, { .opId = "op-quiet", .skipBackup = true }, nullptr);
        rec->begin("op-quiet", "controls", volume);
        commands::writeSystemPair(volume, edited, quiet);
        rec->finish("op-quiet", "");
        CHECK_EQ(count(db, "SELECT count(*) FROM system_changes"), 1);
        CHECK_EQ(count(db, "SELECT count(*) FROM ops WHERE status = 'done'"), 2);
    }

    return testkit::summary("system_history_tests");
}
