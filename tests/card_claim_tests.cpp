// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What meeting a card does to its root (CardClaim.hpp, Spotlight.hpp),
// tested from issue #169 and from what the card must keep — not from the
// functions' own shape:
//
//   - minting a marker or finding one leaves .metadata_never_index at the
//     root, empty, so Spotlight leaves the card alone from its next mount;
//   - something already there by that name is never rewritten, never
//     removed, never followed — whatever it is;
//   - the pedal's own files and the marker are byte for byte what they were,
//     and so are the things a host keeps at the root (.Spotlight-V100, an
//     older marker file);
//   - a marker this build cannot read stops the claim before anything is
//     written;
//   - a card that will not take the file is still known by its marker: the
//     failure is reported, never the card's name and history lost to it;
//   - a write ends with the removal of its own sidecar and nothing else; no
//     write means nothing removed.

#include "support.hpp"

#include <loopercat/CardClaim.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Spotlight.hpp>
#include <loopercat/Volume.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

// A scratch tree that cleans up after itself, and fails the suite if it cannot.
struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-claim-test-" + std::to_string(stamp));
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

void put(const fs::path& p, std::string_view bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A card as the pedal lays it out, with what a Mac and an older LooperCat
// leave at its root: the memory bank pair with the factory stamps, one take,
// Spotlight's store, the 2026-09 marker file.
fs::path makeCard(const fs::path& root)
{
    const fs::path card = root / "BOSS RC-5";
    for (const int bank : { 1, 2 })
        put(volume::memoryPath(card, bank), rc0::setTailMarker(testkit::syntheticMemoryText(), bank));
    put(card / "ROLAND" / "WAVE" / "001_1" / "001_1.WAV", std::string("RIFF\x24\0\0\0WAVE", 12));
    put(card / ".Spotlight-V100" / "VolumeConfiguration.plist", "<plist>indexing on</plist>");
    put(card / "loopercat-card.json", R"({"loopercat_card":1,"id":"734978af","name":"RC-5 Drummer"})");
    return card;
}

// Every file on the card with its bytes — the never-index file left out, so
// "nothing else changed" is one comparison.
std::map<std::string, std::string> contents(const fs::path& card)
{
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(card))
        if (entry.is_regular_file() && entry.path().filename() != spotlight::kNeverIndexFileName)
            files[fs::relative(entry.path(), card).generic_string()] = slurp(entry.path());
    return files;
}

bool isEmptyFile(const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file(fs::symlink_status(p, ec)) && fs::file_size(p, ec) == 0 && !ec;
}

const std::string kKittyMarker = R"([loopercat_card]
format = 1
id = "11111111-2222-4333-8444-555555555555"
name = "RC-5 Kitty"
model = "RC-5"
created = "2026-09-24T21:34:33Z"
by = "LooperCat"
)";

} // namespace

int main()
{
    // --- a card that has never met LooperCat: marker minted, Spotlight kept off ---

    {
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        const auto before = contents(card);
        CHECK(!fs::exists(spotlight::neverIndexPath(card)));

        const cardclaim::Claimed claimed = cardclaim::claim(card);
        CHECK(claimed.minted);
        CHECK(claimed.spotlightKeptOff);
        CHECK_EQ(claimed.card.model, "RC-5");
        CHECK(isEmptyFile(spotlight::neverIndexPath(card)));
        CHECK_EQ(spotlight::neverIndexPath(card).filename().string(), ".metadata_never_index");
        CHECK(claimed.sweep.failed.empty());

        // Everything that was on the card is still there, byte for byte; the
        // marker is the one new file besides the never-index one.
        auto after = contents(card);
        CHECK_EQ(after.size(), before.size() + 1);
        CHECK(after.contains(std::string(marker::kFileName)));
        after.erase(std::string(marker::kFileName));
        CHECK(after == before);
    }

    // --- a card that already carries our marker: the marker is not touched ---

    {
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        put(marker::markerPath(card), kKittyMarker);
        const auto before = contents(card);

        const cardclaim::Claimed claimed = cardclaim::claim(card);
        CHECK(!claimed.minted);
        CHECK(claimed.spotlightKeptOff);
        CHECK_EQ(claimed.card.name, "RC-5 Kitty");
        CHECK_EQ(claimed.card.id, "11111111-2222-4333-8444-555555555555");
        CHECK(isEmptyFile(spotlight::neverIndexPath(card)));
        CHECK(contents(card) == before);
        CHECK_EQ(slurp(marker::markerPath(card)), kKittyMarker);

        // The next Connect finds it there and writes nothing at all.
        const auto stamp = fs::last_write_time(spotlight::neverIndexPath(card));
        const cardclaim::Claimed again = cardclaim::claim(card);
        CHECK(!again.minted);
        CHECK(!again.spotlightKeptOff);
        CHECK(again.sweep.removed.empty() && again.sweep.failed.empty());
        CHECK(fs::last_write_time(spotlight::neverIndexPath(card)) == stamp);
        CHECK(contents(card) == before);
    }

    // --- whatever is already there by that name is left exactly as it is ---

    {
        // A file with content: not truncated, not rewritten.
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        put(spotlight::neverIndexPath(card), "placed by the player");
        const auto old = fs::last_write_time(spotlight::neverIndexPath(card)) - std::chrono::hours(24);
        fs::last_write_time(spotlight::neverIndexPath(card), old);
        const spotlight::KeptOff kept = spotlight::keepOff(card);
        CHECK(!kept.created);
        CHECK_EQ(slurp(spotlight::neverIndexPath(card)), "placed by the player");
        CHECK(fs::last_write_time(spotlight::neverIndexPath(card)) == old);
    }
    {
        // A directory by that name: still a directory, its contents kept.
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        put(spotlight::neverIndexPath(card) / "inside", "kept");
        const spotlight::KeptOff kept = spotlight::keepOff(card);
        CHECK(!kept.created);
        CHECK(fs::is_directory(spotlight::neverIndexPath(card)));
        CHECK_EQ(slurp(spotlight::neverIndexPath(card) / "inside"), "kept");
    }
    {
        // The write itself is exclusive, not only the look before it: a file
        // that appears between the two is still not truncated or replaced.
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        put(spotlight::neverIndexPath(card), "arrived just now");
        CHECK(!spotlight::detail::createExclusive(spotlight::neverIndexPath(card)));
        CHECK_EQ(slurp(spotlight::neverIndexPath(card)), "arrived just now");
        CHECK(spotlight::detail::createExclusive(card / "fresh"));
        CHECK(isEmptyFile(card / "fresh"));
    }
#if !defined(_WIN32)
    {
        // A link by that name — even one that points nowhere — is not
        // followed: nothing is created at its target.
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        const fs::path target = tmp.path / "elsewhere";
        fs::create_symlink(target, spotlight::neverIndexPath(card));
        const spotlight::KeptOff kept = spotlight::keepOff(card);
        CHECK(!kept.created);
        CHECK(fs::is_symlink(fs::symlink_status(spotlight::neverIndexPath(card))));
        CHECK(!fs::exists(fs::symlink_status(target)));
    }
#endif

    // --- a marker this build cannot read: refused, and nothing is written ---

    {
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        put(marker::markerPath(card), "[another]\nformat = 1\n");
        const auto before = contents(card);
        CHECK_THROWS(cardclaim::claim(card), "not a LooperCat card marker");
        CHECK(!fs::exists(fs::symlink_status(spotlight::neverIndexPath(card))));
        CHECK(contents(card) == before);
    }

    // --- no card at all: refused by name, nothing created ---

    {
        TempDir tmp;
        const fs::path gone = tmp.path / "BOSS RC-5";
        CHECK_THROWS(cardclaim::claim(gone), "no volume at");
        CHECK_THROWS(spotlight::keepOff(gone), ".metadata_never_index");
        CHECK(!fs::exists(gone));
    }

    // --- a write removes its own sidecar, and nothing else of the card's ---

    {
        // macOS plants ._.metadata_never_index beside the fresh file (seen on
        // macOS 26 when the file was first written by hand); here a planted
        // one stands in for it, and the write must take it away. Junk that
        // was on the card before is not this write's: the card sweep reports
        // it, and a connect that only met the card leaves it where it is.
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        put(marker::markerPath(card), kKittyMarker);
        put(card / "._.metadata_never_index", std::string(4096, '\0'));
        put(card / "._stray", "x");
        put(card / "ROLAND" / "WAVE" / "001_1" / "._001_1.WAV", "y");
        const cardclaim::Claimed claimed = cardclaim::claim(card);
        CHECK(claimed.spotlightKeptOff);
        CHECK(!fs::exists(card / "._.metadata_never_index"));
        CHECK_EQ(claimed.sweep.removed.size(), 1u);
        CHECK(std::find(claimed.sweep.removed.begin(), claimed.sweep.removed.end(),
                        card / "._.metadata_never_index")
              != claimed.sweep.removed.end());
        CHECK(fs::exists(card / "._stray"));
        CHECK(fs::exists(card / "ROLAND" / "WAVE" / "001_1" / "._001_1.WAV"));

        // Once the file is there, a Connect writes nothing and removes nothing.
        put(card / "._.metadata_never_index", "z");
        const cardclaim::Claimed again = cardclaim::claim(card);
        CHECK(!again.spotlightKeptOff);
        CHECK(again.sweep.removed.empty());
        CHECK(fs::exists(card / "._.metadata_never_index"));
    }

    // --- a root that will not take the file: the card is known all the same ---

#if !defined(_WIN32)
    if (::geteuid() != 0) {
        // A card the system mounted read-only after a FAT error, as far as a
        // test can make one: a root nobody may write.
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        put(marker::markerPath(card), kKittyMarker);
        const auto before = contents(card);
        ::chmod(card.c_str(), 0555);
        // The bare write says where and why.
        CHECK_THROWS(spotlight::keepOff(card), (card / ".metadata_never_index").string());
        // The claim keeps the card's identity and reports the file.
        cardclaim::Claimed claimed;
        bool claimThrew = false;
        try {
            claimed = cardclaim::claim(card);
        } catch (const std::exception&) {
            claimThrew = true;
        }
        ::chmod(card.c_str(), 0755);
        CHECK(!claimThrew);
        CHECK_EQ(claimed.card.id, "11111111-2222-4333-8444-555555555555");
        CHECK_EQ(claimed.card.name, "RC-5 Kitty");
        CHECK(!claimed.minted);
        CHECK(!claimed.spotlightKeptOff);
        CHECK(claimed.spotlightProblem.find((card / ".metadata_never_index").string()) != std::string::npos);
        CHECK(!fs::exists(spotlight::neverIndexPath(card)));
        CHECK(contents(card) == before);
    }
#endif

    // --- a card that is found and written fine carries no problem ---

    {
        TempDir tmp;
        const fs::path card = makeCard(tmp.path);
        CHECK_EQ(cardclaim::claim(card).spotlightProblem, "");
        CHECK_EQ(cardclaim::claim(card).spotlightProblem, "");
    }

    return testkit::summary("card_claim_tests");
}
