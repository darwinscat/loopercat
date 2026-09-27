// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Undo and Redo carried out on a card (app/history/UndoRun.h), end to end:
// real core commands on a synthetic pedal, recorded through the app's own
// wiring (history::withHistory), undone and redone through the same two
// halves the app's worker job runs — beginPress, then apply.
//
// What an undo owes, checked against the card and not against the code:
//   - the card after Undo is the card before the operation — every slot's
//     body and every file under WAVE, byte for byte — and after Redo it is
//     the card after the operation, for push, trim, rename, swap, clear and
//     a settings change
//   - a slot whose audio did not change keeps its file: nothing is archived
//     or rewritten for undoing a rename
//   - a press for a target the history has moved past, a take no longer
//     kept, a slot whose take changed underneath: refused by name, with the
//     card untouched and, for a stale press, no row begun
//   - consecutive undos on one slot do not ask first: an undo row and an
//     operation already undone are not "written over"
//   - the words on the Edit menu name the operation and its slots
//   - "Restore this state" of the window puts every slot of a row back

#include "support.hpp"
#include "archive_support.hpp"

#include "../app/history/CardRestore.h"
#include "../app/history/HistoryRecorder.h"
#include "../app/history/UndoRun.h"

#include <loopercat/SystemFile.hpp>

#include <chrono>
#include <fstream>
#include <map>

using namespace loopercat;
using history::HistoryRecorder;
using history::HistoryStore;
namespace undo = history::undo;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-undo-run-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::string systemFixture()
{
    std::ifstream in(LOOPERCAT_RC5_SYSTEM, std::ios::binary);
    if (!in)
        throw Error("cannot open fixture: " LOOPERCAT_RC5_SYSTEM);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path makePedal(const fs::path& root)
{
    const fs::path volume = root / "BOSS RC-5";
    fs::create_directories(volume / "ROLAND" / "WAVE");
    fs::create_directories(volume::dataDir(volume));
    const std::string memory = testkit::syntheticMemoryText();
    const std::string system = systemFixture();
    for (const int fileNo : { 1, 2 }) {
        commands::writeFileBytes(volume::memoryPath(volume, fileNo), rc0::setTailMarker(memory, fileNo));
        commands::writeFileBytes(volume::systemPath(volume, fileNo),
                                 rc0::setTailGeneration(system, 100u + static_cast<unsigned>(fileNo)));
    }
    return volume;
}

// A float32 stereo take of `frames`, filled so two lengths never share bytes.
fs::path sourceWav(const fs::path& dir, const std::string& name, int frames)
{
    const auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = frames });
    const fs::path path = dir / name;
    commands::writeFileBytes(path, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                    bytes.size()));
    return path;
}

// What the card holds, as a player would compare it: every slot's body, every
// file under WAVE, every settings section. The banks' write counters are left
// out — they move forward with every write, by design, and an undo is a write.
using CardState = std::map<std::string, std::string>;

CardState cardState(const fs::path& volume)
{
    CardState state;
    const std::string memory = commands::readMemory(volume);
    for (int slot = 1; slot <= 99; ++slot)
        state["body " + std::to_string(slot)] = rc0::slotBody(memory, slot);
    const fs::path wave = volume / "ROLAND" / "WAVE";
    for (fs::recursive_directory_iterator it(wave), end; it != end; ++it)
        if (!it->is_directory())
            state["wave " + fs::relative(it->path(), wave).generic_string()] =
                commands::readFileBytes(it->path());
    const std::string system = commands::readSystem(volume);
    for (const std::string_view section : sysfile::kSections)
        state["system " + std::string(section)] = sysfile::sectionText(system, section);
    return state;
}

// Which keys differ, for a failure that says where.
std::string diff(const CardState& a, const CardState& b)
{
    std::string out;
    for (const auto& [key, value] : a)
        if (!b.count(key) || b.at(key) != value)
            out += key + "; ";
    for (const auto& [key, value] : b)
        if (!a.count(key))
            out += key + " (new); ";
    return out;
}

void checkSame(const CardState& got, const CardState& want, int line)
{
    const std::string where = diff(got, want);
    if (!where.empty())
        std::printf("  (line %d) differs: %s\n", line, where.c_str());
    CHECK(where.empty());
}
#define CHECK_SAME(got, want) checkSame((got), (want), __LINE__)

std::int64_t clockAt = 1'000'000;
std::int64_t tick() { return clockAt += 1000; }

struct Bench {
    TempDir tmp;
    fs::path volume = makePedal(tmp.path);
    std::shared_ptr<HistoryRecorder> rec =
        std::make_shared<HistoryRecorder>(tmp.path / "history", "RC-5", tick);
    int ops = 0;

    commands::WriteOptions options(const std::string& opId)
    {
        return history::withHistory(rec, { .opId = opId });
    }

    // One recorded operation, the way the worker wraps a job.
    template <typename Work>
    void op(const std::string& kind, Work work)
    {
        const std::string id = "op-" + std::to_string(++ops);
        rec->begin(id, kind, volume);
        std::string error;
        try {
            work(options(id));
        } catch (const std::exception& e) {
            error = e.what();
        }
        rec->finish(id, error);
        if (!error.empty())
            std::printf("  operation %s failed: %s\n", kind.c_str(), error.c_str());
    }

    // Edit -> Undo (or Redo): what the offer says, pressed through the same
    // two halves the app's job runs. Returns the error, empty on success.
    std::string press(bool redo)
    {
        const undo::Offer offer = undo::offer(rec->store());
        const std::optional<std::int64_t> target = redo ? offer.redo : offer.undo;
        if (!target)
            return "nothing on offer";
        return pressAt(redo, *target);
    }

    std::string pressAt(bool redo, std::int64_t target)
    {
        const std::string id = "op-" + std::to_string(++ops);
        std::string error, note;
        bool begun = false;
        try {
            const undo::Checked checked = undo::beginPress(*rec, id, redo, target, volume);
            begun = true;
            note = checked.note;
            undo::apply(rec->store(), checked.plan, volume, options(id));
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (begun)
            rec->finish(id, error, note);
        return error;
    }

    std::int64_t opCount()
    {
        sqlite::Statement read(rec->store().db(), "SELECT COUNT(*) FROM ops");
        read.step();
        return read.integer(0);
    }
};

// The newest finished entry of the card's timeline.
HistoryStore::CardEntry newest(HistoryStore& store)
{
    const auto timeline = store.cardTimeline();
    return timeline.back();
}

} // namespace

int main()
{
    // --- push into an empty slot: undo empties it, redo brings the take back ---
    {
        Bench b;
        const fs::path source = sourceWav(b.tmp.path, "a.wav", 44100 * 4);
        const CardState empty = cardState(b.volume);
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, source, 5, { .write = o });
        });
        const CardState pushed = cardState(b.volume);
        CHECK(!diff(empty, pushed).empty());

        const undo::Offer offer = undo::offer(b.rec->store());
        CHECK(offer.undo.has_value());
        CHECK(!offer.redo.has_value());
        CHECK_EQ(undo::menuText(false, offer), std::string("Undo push of slot 5"));
        CHECK_EQ(undo::menuText(true, offer), std::string("Redo"));

        CHECK_EQ(b.press(false), std::string());
        CHECK_SAME(cardState(b.volume), empty);
        const HistoryStore::CardEntry undone = newest(b.rec->store());
        CHECK_EQ(undone.kind, std::string("undo"));
        CHECK(undone.reverts == offer.undo); // the row names what it took back
        CHECK_EQ(undone.status, std::string("done"));
        CHECK_EQ(undone.note, std::string("push")); // "Undid push" in the slot's own words

        const undo::Offer after = undo::offer(b.rec->store());
        CHECK(!after.undo.has_value());
        CHECK_EQ(undo::menuText(true, after), std::string("Redo push of slot 5"));

        CHECK_EQ(b.press(true), std::string());
        CHECK_SAME(cardState(b.volume), pushed);
        CHECK_EQ(newest(b.rec->store()).kind, std::string("redo"));
        CHECK_EQ(undo::menuText(false, undo::offer(b.rec->store())), std::string("Undo push of slot 5"));
        CHECK(!undo::offer(b.rec->store()).redo.has_value());
    }

    // --- trim: the whole take comes back, byte for byte; redo trims again ---
    // --- rename: the body goes back and the take is not touched at all ---
    // --- two undos in a row on one slot: nothing to ask about ---
    {
        Bench b;
        const fs::path source = sourceWav(b.tmp.path, "a.wav", 44100 * 4);
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, source, 5, { .write = o });
        });
        const CardState pushed = cardState(b.volume);
        b.op("trim", [&](const commands::WriteOptions& o) {
            commands::trim(b.volume, 5, 0, 44100 * 2, { o });
        });
        const CardState trimmed = cardState(b.volume);
        CHECK(!diff(pushed, trimmed).empty());
        b.op("rename", [&](const commands::WriteOptions& o) { commands::rename(b.volume, 5, "Kitty", o); });
        const CardState renamed = cardState(b.volume);

        const auto archivedTakes = [&] {
            sqlite::Statement read(b.rec->store().db(),
                                   "SELECT count(*) FROM slot_audio WHERE side = 'before'");
            read.step();
            return read.integer(0);
        };
        const auto archivedBefore = archivedTakes();
        CHECK_EQ(undo::menuText(false, undo::offer(b.rec->store())), std::string("Undo rename of slot 5"));
        CHECK_EQ(b.press(false), std::string());
        CHECK_SAME(cardState(b.volume), trimmed);
        CHECK_EQ(archivedTakes(), archivedBefore); // the take was neither archived nor rewritten

        // The next press targets the trim; the rename and the undo row lie
        // after it on slot 5, and neither is still in effect.
        const auto timeline = b.rec->store().cardTimeline();
        const undo::Offer second = undo::offer(b.rec->store());
        CHECK_EQ(undo::menuText(false, second), std::string("Undo trim of slot 5"));
        const undo::Plan plan = undo::plan(timeline, *second.undo);
        CHECK_EQ(plan.writesOver.size(), std::size_t { 2 }); // as recorded: the rename, the undo row
        CHECK(undo::stillInEffect(timeline, plan.writesOver).empty());
        CHECK(!undo::bumpFor(plan, timeline, b.rec->sessionOn(b.volume)).needed());

        CHECK_EQ(b.press(false), std::string());
        CHECK_SAME(cardState(b.volume), pushed); // the untrimmed take, byte for byte

        CHECK_EQ(undo::menuText(true, undo::offer(b.rec->store())), std::string("Redo trim of slot 5"));
        CHECK_EQ(b.press(true), std::string());
        CHECK_SAME(cardState(b.volume), trimmed);
        CHECK_EQ(b.press(true), std::string());
        CHECK_SAME(cardState(b.volume), renamed);
        CHECK_EQ(b.press(true), std::string("nothing on offer"));

        // A step forward after an undo closes the way to redo, as in any editor.
        CHECK_EQ(b.press(false), std::string());
        b.op("tempo", [&](const commands::WriteOptions& o) { commands::setTempo(b.volume, 5, 1000, o); });
        CHECK(!undo::offer(b.rec->store()).redo.has_value());
        CHECK_EQ(undo::menuText(false, undo::offer(b.rec->store())), std::string("Undo tempo change of slot 5"));
    }

    // --- swap: undone by swapping back; redone by swapping again ---
    // --- clear: the take comes back from the history; redo clears again ---
    {
        Bench b;
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "a.wav", 44100 * 4), 5, { .write = o });
        });
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "b.wav", 44100 * 3), 7, { .write = o });
        });
        const CardState two = cardState(b.volume);
        b.op("swap", [&](const commands::WriteOptions& o) { commands::swap(b.volume, 5, 7, o); });
        const CardState swapped = cardState(b.volume);
        CHECK(!diff(two, swapped).empty());
        CHECK_EQ(undo::menuText(false, undo::offer(b.rec->store())), std::string("Undo swap of slots 5 and 7"));
        CHECK_EQ(b.press(false), std::string());
        CHECK_SAME(cardState(b.volume), two);
        CHECK_EQ(b.press(true), std::string());
        CHECK_SAME(cardState(b.volume), swapped);

        b.op("clear", [&](const commands::WriteOptions& o) { commands::clear(b.volume, { 5 }, { .write = o }); });
        const CardState cleared = cardState(b.volume);
        CHECK(!diff(swapped, cleared).empty());
        CHECK_EQ(b.press(false), std::string());
        CHECK_SAME(cardState(b.volume), swapped);
        CHECK_EQ(b.press(true), std::string());
        CHECK_SAME(cardState(b.volume), cleared);
    }

    // --- the pedal's own settings: the section goes back, the slots stay ---
    {
        Bench b;
        const CardState before = cardState(b.volume);
        const std::string original = commands::readSystem(b.volume);
        const std::string edited = sysfile::setField(original, sysfile::kSectionCtl, "Ctl2",
                                                     sysfile::field(original, sysfile::kSectionCtl, "Ctl2") + 3);
        b.op("controls", [&](const commands::WriteOptions& o) { commands::writeSystemPair(b.volume, edited, o); });
        const CardState changed = cardState(b.volume);
        CHECK(diff(before, changed) == "system CTL; ");
        CHECK_EQ(undo::menuText(false, undo::offer(b.rec->store())),
                 std::string("Undo controls change of the pedal settings"));
        CHECK_EQ(b.press(false), std::string());
        CHECK_SAME(cardState(b.volume), before);
        CHECK_EQ(b.press(true), std::string());
        CHECK_SAME(cardState(b.volume), changed);
    }

    // --- refusals: nothing written, and a stale press begins nothing ---
    {
        Bench b;
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "a.wav", 44100 * 4), 5, { .write = o });
        });
        const std::int64_t push = *undo::offer(b.rec->store()).undo;
        b.op("rename", [&](const commands::WriteOptions& o) { commands::rename(b.volume, 5, "Kitty", o); });

        // The press was for the push; the rename landed in between.
        const CardState now = cardState(b.volume);
        const std::int64_t rows = b.opCount();
        const std::string stale = b.pressAt(false, push);
        CHECK(stale.find("moved on") != std::string::npos);
        CHECK_SAME(cardState(b.volume), now);
        CHECK_EQ(b.opCount(), rows); // refused before the operation opened
        CHECK(b.pressAt(true, push).find("moved on") != std::string::npos); // nothing to redo either

        // A slot whose take changed under the history (the pedal, another
        // tool): the rename's body would describe a take that is gone.
        const fs::path elsewhere = b.tmp.path / "elsewhere";
        commands::trim(b.volume, 5, 0, 44100 * 2,
                       { { .opId = "outside",
                           .archive = testkit::fileArchive(elsewhere, "outside"),
                           .journal = testkit::noOpJournal() } });
        const CardState moved = cardState(b.volume);
        const std::string changed = b.press(false);
        CHECK(changed.find("holds another take") != std::string::npos);
        CHECK_SAME(cardState(b.volume), moved);
        CHECK_EQ(newest(b.rec->store()).status, std::string("failed")); // begun, refused, recorded
    }
    {
        // A take the history no longer keeps: push a, trim (keeps a), push b
        // over it (keeps the trimmed take), then a is released. Undoing the
        // replace works; undoing the trim needs a, and says so.
        Bench b;
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "a.wav", 44100 * 4), 5, { .write = o });
        });
        b.op("trim", [&](const commands::WriteOptions& o) { commands::trim(b.volume, 5, 0, 44100 * 2, { o }); });
        const auto trimRow = newest(b.rec->store());
        CHECK(trimRow.slots.size() == 1 && trimRow.slots[0].archived.has_value());
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "b.wav", 44100 * 3), 5,
                           { .force = true, .write = o });
        });
        HistoryStore& store = b.rec->store();
        store.releaseBlobs({ trimRow.slots[0].archived->hash }, store.offeredTargets(), tick());
        CHECK_EQ(b.press(false), std::string()); // the replace: its 'before' is the trimmed take, kept
        const CardState now = cardState(b.volume);
        const std::string gone = b.press(false);
        CHECK(gone.find("no longer kept") != std::string::npos);
        CHECK_SAME(cardState(b.volume), now);
    }

    // --- the first press after the app starts reaches into another connection ---
    {
        Bench b;
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "a.wav", 44100 * 4), 5, { .write = o });
        });
        const std::int64_t first = newest(b.rec->store()).session;
        CHECK(b.rec->sessionOn(b.volume) == first);
        CHECK(!b.rec->sessionOn(b.tmp.path / "another card").has_value());
        b.rec = std::make_shared<HistoryRecorder>(b.tmp.path / "history", "RC-5", tick); // the app again
        CHECK(!b.rec->sessionOn(b.volume).has_value()); // nothing written in this run yet
        const auto timeline = b.rec->store().cardTimeline();
        const undo::Plan plan = undo::plan(timeline, *undo::offer(b.rec->store()).undo);
        CHECK(!plan.crossesConnection); // the plan's proxy misses it: nothing newer on the card
        const undo::Bump bump = undo::bumpFor(plan, timeline, b.rec->sessionOn(b.volume));
        CHECK(bump.needed());
        CHECK(bump.keys == (std::vector<std::string> { "connection:" + std::to_string(first) }));
        // Once this run has written, a target of this run crosses nothing.
        CHECK_EQ(b.press(false), std::string());
        const auto after = b.rec->store().cardTimeline();
        const undo::Offer offer = undo::offer(b.rec->store());
        CHECK(!undo::bumpFor(undo::plan(after, *offer.redo), after, b.rec->sessionOn(b.volume)).needed());
    }

    // --- a crossing is asked about once: keyed by the connection gone back into ---
    {
        Bench b;
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "a.wav", 44100 * 4), 5, { .write = o });
        });
        const std::int64_t first = newest(b.rec->store()).session;
        b.rec = std::make_shared<HistoryRecorder>(b.tmp.path / "history", "RC-5", tick); // the app again
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "b.wav", 44100 * 3), 7, { .write = o });
        });
        CHECK(newest(b.rec->store()).session != first);
        CHECK_EQ(b.press(false), std::string()); // the push into 7: this session, nothing crossed

        const auto bumpNow = [&b]() {
            const auto timeline = b.rec->store().cardTimeline();
            return undo::bumpFor(undo::plan(timeline, *undo::offer(b.rec->store()).undo), timeline,
                                 b.rec->sessionOn(b.volume));
        };
        const undo::Bump into = bumpNow(); // back into the first run's push
        CHECK(into.needed());
        CHECK(into.keys == (std::vector<std::string> { "connection:" + std::to_string(first) }));
        CHECK_EQ(b.press(false), std::string());
        CHECK_EQ(b.press(true), std::string());
        // The press wrote rows in this session; going back into the same
        // connection again is the same crossing, under the same key.
        CHECK(bumpNow().keys == into.keys);
    }

    // --- which later operations are still in effect ---
    {
        const auto entry = [](std::int64_t op, const std::string& kind, std::optional<std::int64_t> reverts) {
            HistoryStore::CardEntry e;
            e.op = op;
            e.kind = kind;
            e.status = "done";
            e.actor = "app";
            e.reverts = reverts;
            return e;
        };
        const std::vector<HistoryStore::CardEntry> undoneOnly { entry(1, "trim", {}), entry(2, "rename", {}),
                                                                 entry(3, "undo", 2) };
        CHECK(undo::stillInEffect(undoneOnly, { 2, 3 }).empty());
        const std::vector<HistoryStore::CardEntry> redone { entry(1, "trim", {}), entry(2, "rename", {}),
                                                            entry(3, "undo", 2), entry(4, "redo", 3) };
        CHECK(undo::stillInEffect(redone, { 2, 3, 4 }) == (std::vector<std::int64_t> { 2 }));
        auto failedUndo = undoneOnly;
        failedUndo[2].status = "failed"; // an undo that did not happen took nothing back
        CHECK(undo::stillInEffect(failedUndo, { 2 }) == (std::vector<std::int64_t> { 2 }));
        auto pedal = redone;
        pedal.push_back(entry(5, "push", {}));
        pedal.back().actor = "pedal";
        CHECK(undo::stillInEffect(pedal, { 2, 5 }) == (std::vector<std::int64_t> { 2, 5 }));
    }

    // --- words ---
    {
        HistoryStore::CardEntry e;
        e.kind = "normalize";
        CHECK_EQ(undo::operationWords(e), std::string("normalize"));
        e.slots.resize(3);
        e.slots[0].slot = 1;
        e.slots[1].slot = 2;
        e.slots[2].slot = 9;
        CHECK_EQ(undo::operationWords(e), std::string("normalize of 3 slots"));
        e.kind = "frobnicate"; // a kind from a newer build: named as recorded
        e.slots.resize(1);
        e.system.push_back({ "CTL", "<CTL></CTL>", "<CTL>x</CTL>" });
        CHECK_EQ(undo::operationWords(e), std::string("frobnicate of slot 1 and the pedal settings"));
        CHECK_EQ(undo::menuText(false, {}), std::string("Undo"));
    }

    // --- a section spliced back: only its bytes change ---
    {
        const std::string text = systemFixture();
        const std::string ctl = sysfile::sectionText(text, sysfile::kSectionCtl);
        const std::string edited = sysfile::setField(text, sysfile::kSectionCtl, "Ctl2",
                                                     sysfile::field(text, sysfile::kSectionCtl, "Ctl2") + 1);
        CHECK(undo::spliceSection(edited, "CTL", ctl) == text);
        CHECK_THROWS(undo::spliceSection(edited, "CTL", "<MIDI></MIDI>"), "not that section's text");
        CHECK_THROWS(undo::spliceSection(edited, "CTL", "CTL"), "not that section's text");
    }

    // --- Restore this state (the window): every slot of the row goes back ---
    {
        Bench b;
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "a.wav", 44100 * 4), 5, { .write = o });
        });
        b.op("push", [&](const commands::WriteOptions& o) {
            commands::push(b.volume, sourceWav(b.tmp.path, "b.wav", 44100 * 3), 7, { .write = o });
        });
        b.op("swap", [&](const commands::WriteOptions& o) { commands::swap(b.volume, 5, 7, o); });
        const CardState swapped = cardState(b.volume);
        const std::int64_t swapOp = newest(b.rec->store()).op;
        b.op("trim", [&](const commands::WriteOptions& o) { commands::trim(b.volume, 5, 0, 44100, { o }); });
        b.op("clear", [&](const commands::WriteOptions& o) { commands::clear(b.volume, { 7 }, { .write = o }); });
        CHECK(!diff(cardState(b.volume), swapped).empty());
        b.op("restore", [&](const commands::WriteOptions& o) {
            history::restoreOperation(b.rec->store(), swapOp, b.volume, o);
        });
        CHECK_SAME(cardState(b.volume), swapped);
        CHECK_EQ(newest(b.rec->store()).status, std::string("done"));
        CHECK_THROWS(history::restoreOperation(b.rec->store(), 9999, b.volume, b.options("none")),
                     "not in the history");
    }

    return testkit::summary("undo_run_tests");
}
