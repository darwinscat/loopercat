// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TakeAudition.h"

#include <loopercat/Commands.hpp>

#include <cstring>
#include <string_view>

namespace loopercat::history
{

namespace fs = std::filesystem;

namespace
{

constexpr std::size_t kHashBytes = 32;

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

// take-<64 lowercase hex>.wav, or that with .part after it.
bool isAuditionName(const std::string& name)
{
    const std::string_view prefix = TakeAudition::kPrefix;
    const std::string_view suffix = TakeAudition::kSuffix;
    const std::string_view partial = TakeAudition::kPartial;
    std::string_view rest = name;
    if (rest.ends_with(partial))
        rest.remove_suffix(partial.size());
    if (!rest.starts_with(prefix) || !rest.ends_with(suffix))
        return false;
    rest.remove_prefix(prefix.size());
    rest.remove_suffix(suffix.size());
    if (rest.size() != kHashBytes * 2)
        return false;
    for (const char c : rest)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

} // namespace

TakeAudition::TakeAudition(fs::path dir) : dir_(std::move(dir))
{
    if (dir_.empty())
        throw Error("the audition folder needs a path");
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec || !fs::is_directory(dir_, ec))
        throw Error("cannot create the audition folder " + dir_.string());
    sweepLeftovers();
}

TakeAudition::~TakeAudition()
{
    std::error_code ec;
    for (const auto& hash : made_) {
        const fs::path file = pathFor(hash);
        fs::remove(file, ec);
        fs::remove(fs::path(file.string() + kPartial), ec);
    }
}

fs::path TakeAudition::pathFor(const std::string& hash) const
{
    return dir_ / (std::string(kPrefix) + hex(hash) + kSuffix);
}

void TakeAudition::sweepLeftovers() const
{
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        if (isAuditionName(sqlite::utf8(it->path().filename())))
            fs::remove(it->path(), ec);
    }
}

std::optional<fs::path> TakeAudition::materialize(HistoryStore& store, const std::string& hash)
{
    if (hash.size() != kHashBytes)
        throw Error("a content hash is " + std::to_string(kHashBytes) + " bytes, got "
                    + std::to_string(hash.size()));
    const fs::path file = pathFor(hash);
    std::error_code ec;
    if (made_.contains(hash) && fs::is_regular_file(file, ec))
        return file;

    const std::optional<std::string> bytes = store.takeBytes(hash);
    if (!bytes)
        return std::nullopt;

    // Whole or absent: the bytes land under the partial name, and the final
    // name appears only by the rename.
    const fs::path partial(file.string() + kPartial);
    try {
        commands::writeFileBytes(partial, *bytes);
        fs::rename(partial, file);
    } catch (...) {
        fs::remove(partial, ec);
        throw;
    }
    made_.insert(hash);
    return file;
}

} // namespace loopercat::history
