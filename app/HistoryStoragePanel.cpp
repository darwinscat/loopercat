// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryStoragePanel.h"

namespace loopercat
{

namespace retention = history::retention;

namespace
{

constexpr std::int64_t kGB = std::int64_t { 1 } << 30;
constexpr std::int64_t kMinLimit = kGB / 10;          // 0.1 GB
constexpr std::int64_t kMaxLimit = 10'000 * kGB;      // 10 TB

const juce::Colour kText(0xffd8d8d8);
const juce::Colour kDim(0xff8a8a92);
const juce::Colour kFaint(0xff6f6f78);
const juce::Colour kBackground(0xff121218);
const juce::Colour kRowBackground(0xff1a1a22);

juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

std::string takes(std::size_t n) { return std::to_string(n) + (n == 1 ? " take" : " takes"); }

} // namespace

HistoryStoragePanel::Facts HistoryStoragePanel::Facts::read(history::HistoryStore& store,
                                                            std::int64_t limit, std::int64_t nowMs)
{
    Facts facts;
    facts.usage = store.usage();
    facts.limit = limit;
    facts.blobs = store.keptBlobs(store.offeredUndo());
    facts.forecast = retention::forecast(store.writes(), nowMs, facts.usage.audioBytes, limit,
                                         facts.usage.diskAvailable);
    return facts;
}

HistoryStoragePanel::HistoryStoragePanel()
{
    title_.setText("History storage", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(title_);

    for (auto* line : { &cost_, &disk_, &forecast_ }) {
        line->setFont(juce::FontOptions(12.0f));
        line->setColour(juce::Label::textColourId, kText);
        addAndMakeVisible(line);
    }

    limitCaption_.setText("Limit", juce::dontSendNotification);
    limitCaption_.setFont(juce::FontOptions(12.0f));
    limitCaption_.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(limitCaption_);
    limit_.setInputRestrictions(8, "0123456789.");
    limit_.setJustification(juce::Justification::centredRight);
    limit_.onReturnKey = [this] { commitLimitText(limit_.getText()); };
    limit_.onFocusLost = [this] { commitLimitText(limit_.getText()); };
    addAndMakeVisible(limit_);
    limitUnit_.setText("GB", juce::dontSendNotification);
    limitUnit_.setFont(juce::FontOptions(12.0f));
    limitUnit_.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(limitUnit_);

    keepCaption_.setText("Keep at most", juce::dontSendNotification);
    keepCaption_.setFont(juce::FontOptions(12.0f));
    keepCaption_.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(keepCaption_);
    keep_.setSliderStyle(juce::Slider::LinearBar);
    keep_.setTextBoxStyle(juce::Slider::TextBoxRight, true, 90, 20);
    keep_.textFromValueFunction = [](double value) {
        return text(retention::bytesText(static_cast<std::int64_t>(value)));
    };
    keep_.onValueChange = [this] { refreshOffer(); };
    addAndMakeVisible(keep_);

    offer_.setFont(juce::FontOptions(12.0f));
    offer_.setColour(juce::Label::textColourId, kText);
    offer_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(offer_);

    rows_.setModel(this);
    rows_.setRowHeight(20);
    rows_.setColour(juce::ListBox::backgroundColourId, kRowBackground);
    addAndMakeVisible(rows_);

    release_.onClick = [this] { confirmRelease(); };
    addAndMakeVisible(release_);

    sayUnread();
    setSize(520, 300);
}

HistoryStoragePanel::~HistoryStoragePanel() { rows_.setModel(nullptr); }

void HistoryStoragePanel::sayUnread()
{
    cost_.setText("The history has not been read yet.", juce::dontSendNotification);
    disk_.setText("", juce::dontSendNotification);
    forecast_.setText("", juce::dontSendNotification);
    offer_.setText("Nothing to offer until the history has been read.", juce::dontSendNotification);
    limit_.setText("", juce::dontSendNotification);
    keep_.setRange(0.0, 1.0, 1.0);
    keep_.setValue(0.0, juce::dontSendNotification);
    keep_.setEnabled(false);
    release_.setButtonText("Release");
    release_.setEnabled(false);
    plan_ = {};
    rows_.updateContent();
}

void HistoryStoragePanel::show(Facts facts)
{
    facts_ = std::move(facts);
    releasing_ = false;
    ++shown_;
    const auto& u = facts_->usage;
    cost_.setText(text("History: " + retention::bytesText(u.fileBytes) + " on disk, of which "
                       + retention::bytesText(u.audioBytes) + " of takes and "
                       + retention::bytesText(u.otherBytes) + " of settings and records."),
                  juce::dontSendNotification);
    disk_.setText(text("Free on this disk: " + retention::bytesText(u.diskAvailable) + "."),
                  juce::dontSendNotification);
    forecast_.setText(text(retention::describeForecast(facts_->forecast)), juce::dontSendNotification);
    limit_.setText(formatGb(facts_->limit), juce::dontSendNotification);

    std::int64_t kept = 0;
    for (const auto& blob : facts_->blobs)
        kept += blob.size;
    // Over the limit the offer starts at the limit: the oldest unheld takes
    // that bring the history back under it. Under it, at "nothing goes".
    keep_.setEnabled(kept > 0);
    keep_.setRange(0.0, static_cast<double>(std::max<std::int64_t>(kept, 1)), 1.0);
    keep_.setValue(static_cast<double>(std::min(kept, facts_->limit)), juce::dontSendNotification);
    refreshOffer();
}

void HistoryStoragePanel::setKeepTarget(std::int64_t bytes)
{
    if (!facts_)
        return;
    keep_.setValue(static_cast<double>(std::max<std::int64_t>(0, bytes)), juce::sendNotificationSync);
}

void HistoryStoragePanel::refreshOffer()
{
    if (!facts_)
        return;
    plan_ = retention::plan(facts_->blobs, keepTarget());
    offer_.setText(text(retention::describe(plan_)), juce::dontSendNotification);
    rows_.updateContent();
    if (plan_.release.empty()) {
        release_.setButtonText("Nothing to release");
        release_.setEnabled(false);
    } else {
        release_.setButtonText(text("Release " + takes(plan_.release.size()) + " ("
                                    + retention::bytesText(plan_.freed) + ")"));
        release_.setEnabled(!releasing_);
    }
}

std::vector<std::string> HistoryStoragePanel::offeredHashes() const
{
    std::vector<std::string> out;
    for (const auto& blob : plan_.release)
        out.push_back(blob.hash);
    return out;
}

void HistoryStoragePanel::confirmRelease()
{
    if (!facts_ || releasing_ || plan_.release.empty())
        return; // the button offers only what the plan gives
    releasing_ = true;
    release_.setButtonText("Releasing...");
    release_.setEnabled(false);
    if (onRelease)
        onRelease(offeredHashes());
}

void HistoryStoragePanel::commitLimitText(const juce::String& typed)
{
    // The field shows what was typed until the number is judged: a typo that
    // snaps back is seen to snap back.
    limit_.setText(typed, juce::dontSendNotification);
    if (!facts_) {
        limit_.setText("", juce::dontSendNotification);
        return;
    }
    const auto parsed = parseGb(typed);
    if (!parsed) {
        // Not a limit: snap back to the one in force, visibly.
        limit_.setText(formatGb(facts_->limit), juce::dontSendNotification);
        return;
    }
    limit_.setText(formatGb(*parsed), juce::dontSendNotification);
    if (*parsed == facts_->limit)
        return;
    facts_->limit = *parsed;
    if (onLimitChanged)
        onLimitChanged(*parsed);
}

juce::String HistoryStoragePanel::formatGb(std::int64_t bytes)
{
    const double gb = static_cast<double>(bytes) / static_cast<double>(kGB);
    juce::String s(gb, 1);
    return s.endsWith(".0") ? s.dropLastCharacters(2) : s;
}

std::optional<std::int64_t> HistoryStoragePanel::parseGb(const juce::String& typed)
{
    const juce::String trimmed = typed.trim();
    if (trimmed.isEmpty() || !trimmed.containsOnly("0123456789."))
        return std::nullopt;
    const double gb = trimmed.getDoubleValue();
    const auto bytes = static_cast<std::int64_t>(gb * static_cast<double>(kGB) + 0.5);
    if (bytes < kMinLimit || bytes > kMaxLimit)
        return std::nullopt;
    return bytes;
}

int HistoryStoragePanel::getNumRows() { return static_cast<int>(plan_.release.size()); }

void HistoryStoragePanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                           bool /*selected*/)
{
    if (row < 0 || row >= getNumRows())
        return;
    const auto& blob = plan_.release[static_cast<std::size_t>(row)];
    g.setColour(kFaint);
    g.setFont(juce::FontOptions(11.0f));
    auto area = juce::Rectangle<int>(0, 0, width, height).reduced(6, 0);
    g.drawText(juce::Time(blob.created).formatted("%Y-%m-%d"), area.removeFromLeft(80),
               juce::Justification::centredLeft);
    g.drawText(text(retention::bytesText(blob.size)), area.removeFromRight(70),
               juce::Justification::centredRight);
    g.setColour(kText);
    g.drawText(text(blob.label), area, juce::Justification::centredLeft, true);
}

void HistoryStoragePanel::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void HistoryStoragePanel::resized()
{
    auto area = getLocalBounds().reduced(12, 10);
    title_.setBounds(area.removeFromTop(20));
    area.removeFromTop(4);
    cost_.setBounds(area.removeFromTop(18));
    disk_.setBounds(area.removeFromTop(18));
    forecast_.setBounds(area.removeFromTop(18));
    area.removeFromTop(8);
    auto limitRow = area.removeFromTop(24);
    limitCaption_.setBounds(limitRow.removeFromLeft(100));
    limit_.setBounds(limitRow.removeFromLeft(64));
    limitRow.removeFromLeft(6);
    limitUnit_.setBounds(limitRow.removeFromLeft(30));
    area.removeFromTop(8);
    auto keepRow = area.removeFromTop(24);
    keepCaption_.setBounds(keepRow.removeFromLeft(100));
    keep_.setBounds(keepRow);
    area.removeFromTop(6);
    offer_.setBounds(area.removeFromTop(18));
    area.removeFromTop(4);
    auto buttonRow = area.removeFromBottom(26);
    release_.setBounds(buttonRow.removeFromLeft(220));
    area.removeFromBottom(6);
    rows_.setBounds(area);
}

} // namespace loopercat
