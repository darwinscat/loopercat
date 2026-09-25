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

#include <loopercat/SystemFile.hpp>

#include <chrono>
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

    return testkit::summary("system_history_tests");
}
