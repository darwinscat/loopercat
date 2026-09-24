// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "LegacyImport.h"

#include <loopercat/Commands.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Volume.hpp>

#include <cctype>
#include <ctime>
#include <map>

namespace loopercat::history::legacy
{

namespace fs = std::filesystem;

namespace
{

bool allDigits(std::string_view text)
{
    if (text.empty())
        return false;
    for (const char c : text)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

int number(std::string_view digits)
{
    int value = 0;
    for (const char c : digits)
        value = value * 10 + (c - '0');
    return value;
}

// One file the import is about to record.
struct File {
    std::string path; // under the data home, '/'-separated
    fs::path absolute;
    bool take;        // else a document
    int slot = 0;
    int track = 0;
    std::string name;
};

// The folders one stamp has: a backup, a trash, or both.
struct Sources {
    std::optional<fs::path> backups;
    std::optional<fs::path> trash;
};

// A trash slot folder is exactly volume::slotDirName's shape: NNN_T, with the
// slot in range and no other spelling of the same number.
std::optional<std::pair<int, int>> parseSlotDir(const std::string& name)
{
    const auto underscore = name.find('_');
    if (underscore != 3 || name.size() < 5)
        return std::nullopt;
    const std::string_view slotDigits = std::string_view(name).substr(0, 3);
    const std::string_view trackDigits = std::string_view(name).substr(4);
    if (!allDigits(slotDigits) || !allDigits(trackDigits))
        return std::nullopt;
    const int slot = number(slotDigits);
    const int track = number(trackDigits);
    if (slot < 1 || slot > rc0::kSlotCount || track < 1)
        return std::nullopt;
    if (std::to_string(track) != trackDigits) // "01": not a name the app writes
        return std::nullopt;
    return std::make_pair(slot, track);
}

struct Collector {
    Report& report;
    std::vector<File> files;

    void skip(const std::string& path, const std::string& reason)
    {
        report.skipped.push_back({ path, reason });
    }

    // A directory's entries, junk left out, or the reason it cannot be read.
    std::optional<std::vector<fs::directory_entry>> entries(const fs::path& dir,
                                                            const std::string& path)
    {
        std::error_code ec;
        std::vector<fs::directory_entry> out;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (volume::isJunkName(sqlite::utf8(it->path().filename())))
                continue;
            out.push_back(*it);
        }
        if (ec) {
            skip(path, "cannot list the folder: " + ec.message());
            return std::nullopt;
        }
        return out;
    }

    void documents(const fs::path& dir, const std::string& path)
    {
        const auto found = entries(dir, path);
        if (!found)
            return;
        for (const auto& entry : *found) {
            const std::string name = sqlite::utf8(entry.path().filename());
            const std::string filePath = path + "/" + name;
            std::error_code ec;
            if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) {
                skip(filePath, "not a file the app wrote into a backup");
                continue;
            }
            files.push_back({ filePath, entry.path(), false, 0, 0, name });
        }
    }

    void takes(const fs::path& dir, const std::string& path)
    {
        const auto slotDirs = entries(dir, path);
        if (!slotDirs)
            return;
        for (const auto& slotDir : *slotDirs) {
            const std::string dirName = sqlite::utf8(slotDir.path().filename());
            const std::string dirPath = path + "/" + dirName;
            std::error_code ec;
            if (!slotDir.is_directory(ec)) {
                skip(dirPath, "not a slot folder");
                continue;
            }
            const auto slot = parseSlotDir(dirName);
            if (!slot) {
                skip(dirPath, "not a slot folder (expected NNN_1, slot 1.."
                                  + std::to_string(rc0::kSlotCount) + ")");
                continue;
            }
            const auto found = entries(slotDir.path(), dirPath);
            if (!found)
                continue;
            for (const auto& entry : *found) {
                const std::string name = sqlite::utf8(entry.path().filename());
                const std::string filePath = dirPath + "/" + name;
                if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) {
                    skip(filePath, "not a file the app put in the trash");
                    continue;
                }
                files.push_back({ filePath, entry.path(), true, slot->first, slot->second, name });
            }
        }
    }
};

// The stamps under one root, each with the folder it has there; what is not
// a stamp folder is reported and left.
void collectStamps(const fs::path& root, const char* rootName, bool trash,
                   std::map<std::string, Sources>& stamps, Report& report)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return; // a data home without the folder has nothing to import
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = sqlite::utf8(it->path().filename());
        if (volume::isJunkName(name))
            continue;
        const std::string path = std::string(rootName) + "/" + name;
        if (!it->is_directory(ec)) {
            report.skipped.push_back({ path, "not a folder" });
            continue;
        }
        if (!parseStamp(name)) {
            report.skipped.push_back(
                { path, "not an operation folder (expected a YYYY-MM-DDTHH-MM-SS stamp)" });
            continue;
        }
        (trash ? stamps[name].trash : stamps[name].backups) = it->path();
    }
    if (ec)
        report.skipped.push_back({ rootName, "cannot list the folder: " + ec.message() });
}

} // namespace

std::optional<Stamp> parseStamp(std::string_view name)
{
    constexpr std::string_view kShape = "0000-00-00T00-00-00";
    if (name.size() < kShape.size())
        return std::nullopt;
    for (std::size_t i = 0; i < kShape.size(); ++i) {
        const bool digit = std::isdigit(static_cast<unsigned char>(name[i])) != 0;
        if (kShape[i] == '0' ? !digit : name[i] != kShape[i])
            return std::nullopt;
    }
    const int year = number(name.substr(0, 4));
    const int month = number(name.substr(5, 2));
    const int day = number(name.substr(8, 2));
    const int hour = number(name.substr(11, 2));
    const int minute = number(name.substr(14, 2));
    const int second = number(name.substr(17, 2));
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59
        || second > 59)
        return std::nullopt;

    // opid::make's tail: '-' + four lowercase hex digits + '-' + a counter.
    const std::string_view tail = name.substr(kShape.size());
    bool minted = false;
    if (!tail.empty()) {
        if (tail.size() < 7 || tail[0] != '-' || tail[5] != '-')
            return std::nullopt;
        for (std::size_t i = 1; i <= 4; ++i) {
            const char c = tail[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return std::nullopt;
        }
        if (!allDigits(tail.substr(6)))
            return std::nullopt;
        minted = true;
    }

    std::tm local {};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;
    local.tm_isdst = -1;
    const std::time_t at = std::mktime(&local);
    if (at == static_cast<std::time_t>(-1))
        return std::nullopt;
    return Stamp { static_cast<std::int64_t>(at) * 1000, minted };
}

Report importFolders(HistoryStore& store, const fs::path& dataHome, std::int64_t nowMs)
{
    Report report;
    std::map<std::string, Sources> stamps; // sorted: the folders in time order
    collectStamps(dataHome / kBackupsFolder, kBackupsFolder, false, stamps, report);
    collectStamps(dataHome / kTrashFolder, kTrashFolder, true, stamps, report);

    // One session for the run, opened only if the run records something.
    std::optional<std::int64_t> session;
    const auto sessionId = [&] {
        if (!session)
            session = store.openSession(store.card(kCardModel, kCardLabel, nowMs), nowMs);
        return *session;
    };

    for (const auto& [stamp, sources] : stamps) {
        const std::string backupsPath = std::string(kBackupsFolder) + "/" + stamp;
        const std::string trashPath = std::string(kTrashFolder) + "/" + stamp;

        const auto existing = store.findOp(stamp);
        if (existing && existing->actor != "legacy") {
            const std::string reason = "already in the history, recorded by the app";
            if (sources.backups)
                report.skipped.push_back({ backupsPath, reason });
            if (sources.trash)
                report.skipped.push_back({ trashPath, reason });
            continue;
        }

        Collector collect { report, {} };
        if (sources.backups)
            collect.documents(*sources.backups, backupsPath);
        if (sources.trash)
            collect.takes(*sources.trash, trashPath);

        std::vector<File> pending;
        for (auto& file : collect.files) {
            if (store.legacyFileImported(file.path))
                ++report.alreadyImported;
            else
                pending.push_back(std::move(file));
        }
        if (pending.empty()) {
            if (collect.files.empty()) {
                if (sources.backups)
                    report.skipped.push_back({ backupsPath, "nothing to import" });
                if (sources.trash)
                    report.skipped.push_back({ trashPath, "nothing to import" });
            }
            continue;
        }

        std::int64_t op = 0;
        if (existing) {
            op = existing->seq; // a run that stopped mid-stamp: its files go on
        } else {
            std::string note;
            if (sources.backups)
                note = backupsPath;
            if (sources.trash)
                note += (note.empty() ? "" : ", ") + trashPath;
            op = store.recordLegacyOp(sessionId(), stamp, parseStamp(stamp)->atMs, note);
            ++report.operations;
        }

        for (const auto& file : pending) {
            try {
                const std::string bytes = commands::readFileBytes(file.absolute);
                const bool written = file.take
                    ? store.keepLegacyTake(op, file.path, file.slot, file.track, file.name, bytes,
                                           nowMs)
                    : store.keepLegacyDocument(op, file.path, bytes, nowMs);
                ++(file.take ? report.takes : report.documents);
                if (!written)
                    ++report.deduplicated;
            } catch (const Error& e) {
                report.skipped.push_back({ file.path, e.what() });
            }
        }
    }

    if (session)
        store.closeSession(*session, nowMs);
    return report;
}

std::string describe(const Report& report)
{
    const auto plural = [](int n, const char* one, const char* many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    const int recorded = report.takes + report.documents;
    const auto skipped = static_cast<int>(report.skipped.size());
    if (recorded == 0 && report.alreadyImported == 0 && skipped == 0)
        return "No folders from before the history were found.";
    std::string text;
    if (recorded > 0) {
        text = "Imported " + plural(report.operations, "operation", "operations") + ": "
               + plural(report.takes, "take", "takes") + " and "
               + plural(report.documents, "document", "documents");
        if (report.deduplicated > 0)
            text += " (" + std::to_string(report.deduplicated) + " already kept, counted once)";
        text += ".";
    } else {
        text = "Nothing new to import.";
    }
    if (report.alreadyImported > 0)
        text += " " + plural(report.alreadyImported, "file was", "files were")
                + " already in the history.";
    if (skipped > 0)
        text += " " + plural(skipped, "folder", "folders")
                + " skipped, the reasons are in operations.log.";
    return text;
}

} // namespace loopercat::history::legacy
