// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A card as a folder on disk (issue #77): ROLAND/DATA with the memory pair
// and ROLAND/WAVE with the takes, exactly the layout of a real card, so that
// the state of a card can leave this computer as something any file browser
// opens and any card accepts when copied onto it by hand — and can come back
// the same way, through the volume scan that already reads real cards.
//
// A folder made here carries NO SYSTEM*.RC0. It is not meant to boot a
// pedal: it is copied onto a card by hand, where the pedal's own settings
// files already are, and it is read by this app as a card is. Writing a
// settings file is a thing this app has not yet done to a pedal, and a
// folder is not where that starts.
//
// Export is the restore primitive (Commands.hpp) pointed at the folder:
// every slot written into it is one restore, journalled like any other.
// Import is volume::looksLikePedal and commands::readMemory pointed at the
// folder: the family guard stands at that door exactly as it stands at a
// mounted card's — a folder taken off another model gets the same refusal.
//
// Bytes: the memory pair is what a factory-fresh card carries (every
// never-touched memory of fixtures/rc5-card.RC0 is the factory body, and a
// memory file is that header, those 99 memories and that trailer — see
// Rc0.hpp), stamped with the factory generation pair.

#pragma once

#include "Commands.hpp"
#include "DeviceProfile.hpp"
#include "Error.hpp"
#include "Rc0.hpp"
#include "Volume.hpp"

#include <filesystem>
#include <string>

namespace loopercat::cardfolder {

namespace fs = std::filesystem;

// The XML declaration and the root opener a pedal writes, and the closer;
// the root's name attribute is the model's (fixtures/rc5-card.RC0).
inline std::string memoryFileHeader(const profile::DeviceProfile& family)
{
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<database name=\""
         + std::string(family.familyName) + "\" revision=\"0\">\n";
}

// A factory-fresh memory document of a model: the header, the 99 memories in
// the state MEMORY CLEAR leaves them, the closing tag. One <mem id="N"> per
// memory, N from 0, each body as rc0::factorySlotBody formats it, a newline
// after every closer — the shape of a real card, byte for byte. A model
// whose factory body is not on record gets no document (rc0::factorySlotBody
// refuses it), and nothing else is touched.
inline std::string factoryMemoryDocument(const profile::DeviceProfile& family)
{
    std::string text = memoryFileHeader(family);
    for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
        text += "<mem id=\"" + std::to_string(slot - 1) + "\">" + rc0::factorySlotBody(family, slot)
              + "</mem>\n";
    text += rc0::kClosingTag;
    return text;
}

// The document as one bank of the pair: the trailer added, stamped with the
// factory generation for that bank (Rc0.hpp: 0x38 and 0x39).
inline std::string factoryMemoryFile(const profile::DeviceProfile& family, int fileNo)
{
    return rc0::setTailMarker(factoryMemoryDocument(family) + std::string("\n\0\0\0\0", 5),
                              fileNo);
}

// Make an empty card folder at `root`: ROLAND/DATA with MEMORY1.RC0 and
// MEMORY2.RC0, ROLAND/WAVE empty. `root` may not exist yet or may be an
// empty directory; a root that already holds a ROLAND tree is refused — this
// never overwrites a card, on disk or on a volume. Both files are verified by
// re-reading, as every write of a memory pair is.
inline void create(const fs::path& root, const profile::DeviceProfile& family)
{
    // Everything that can refuse, refuses before a directory exists.
    const std::string bank1 = factoryMemoryFile(family, 1);
    const std::string bank2 = factoryMemoryFile(family, 2);
    std::error_code ec;
    if (fs::exists(root, ec) && !fs::is_directory(root, ec))
        throw Error("cannot make a card folder at " + root.string() + ": not a directory");
    if (fs::exists(root / "ROLAND", ec))
        throw Error(root.string() + " already holds a card folder (ROLAND/) \xe2\x80\x94 refusing"
                    " to write over it");
    fs::create_directories(volume::dataDir(root), ec);
    if (ec)
        throw Error("cannot create " + volume::dataDir(root).string() + ": " + ec.message());
    fs::create_directories(root / "ROLAND" / "WAVE", ec);
    if (ec)
        throw Error("cannot create " + (root / "ROLAND" / "WAVE").string() + ": " + ec.message());
    for (const auto& [fileNo, bytes] : { std::pair { 1, bank1 }, std::pair { 2, bank2 } }) {
        const fs::path path = volume::memoryPath(root, fileNo);
        commands::writeFileBytes(path, bytes);
        if (commands::readFileBytes(path) != bytes)
            throw Error("verification failed: " + path.string() + " read back differently");
    }
}

// A folder this app reads as a card: a directory with ROLAND/DATA and
// ROLAND/WAVE — the same test as for a mounted volume, said with a reason.
// What the memory files inside are, and whose, is commands::readMemory's
// question, answered with the family guard.
inline void assertCardFolder(const fs::path& root)
{
    std::error_code ec;
    if (!fs::exists(root, ec))
        throw Error("not a card folder: " + root.string() + " does not exist");
    if (!fs::is_directory(root, ec))
        throw Error("not a card folder: " + root.string() + " is not a directory");
    if (!volume::looksLikePedal(root))
        throw Error("not a card folder: no ROLAND/DATA and ROLAND/WAVE under " + root.string());
}

} // namespace loopercat::cardfolder
