// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "history/HistoryStore.h"
#include "history/Retention.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
// loopercat::HistoryStoragePanel — what the history costs, and the button that
// gives space back (issue #74), for the Settings dialog.
//
// The panel shows and asks; it never fetches and never frees. The owner hands
// it the store's facts (show) and hears back through two callbacks: the limit
// the person typed, and the takes the person confirmed for release. Between
// the two the panel is on its own — the plan behind the offer is a pure
// function of the facts it holds (retention::plan), so moving the "keep at
// most" control redraws the offer at once, with no database in the way. That
// is also what makes the panel testable without one.
//
// The button is there at all times, not only at the limit: below the limit
// the offer starts at "nothing needs to go", and lowering the amount to keep
// names the oldest unheld takes that would go. What the plan does not give,
// the button does not offer — it is disabled, and the sentence says why.
//==============================================================================
namespace loopercat
{

class HistoryStoragePanel final : public juce::Component, private juce::ListBoxModel
{
public:
    struct Facts {
        history::HistoryStore::Usage usage;
        std::int64_t limit = 0;                       // bytes
        std::vector<history::retention::Blob> blobs;  // keptBlobs(offeredUndo())
        history::retention::Forecast forecast;

        // The owner's side of the contract, gathered where the store lives —
        // the worker thread — and never by the panel: what the file holds,
        // the limit in force, every kept blob with what holds it (the undo on
        // offer included), and the rate the history grows at, against that
        // limit and the disk the file is on.
        static Facts read(history::HistoryStore& store, std::int64_t limit, std::int64_t nowMs);
    };

    HistoryStoragePanel();
    ~HistoryStoragePanel() override;

    // The owner's numbers arrive. Until the first show the panel says it has
    // not read the history, and offers nothing.
    void show(Facts facts);

    std::function<void(std::int64_t bytes)> onLimitChanged;
    std::function<void(std::vector<std::string> hashes)> onRelease;

    // The controls' actions, callable without a mouse: the controls call
    // these. commitLimitText is what typing into the limit field and pressing
    // Return does — the text lands in the field and is judged.
    void setKeepTarget(std::int64_t bytes);
    void commitLimitText(const juce::String& text);
    void confirmRelease();

    // What the panel says, for the owner and the tests.
    juce::String costLine() const { return cost_.getText(); }
    juce::String diskLine() const { return disk_.getText(); }
    juce::String forecastLine() const { return forecast_.getText(); }
    juce::String offerLine() const { return offer_.getText(); }
    juce::String limitText() const { return limit_.getText(); }
    juce::String releaseButtonText() const { return release_.getButtonText(); }
    bool releaseEnabled() const { return release_.isEnabled(); }
    std::int64_t keepTarget() const { return static_cast<std::int64_t>(keep_.getValue()); }
    std::vector<std::string> offeredHashes() const;
    int offeredRows() const { return static_cast<int>(plan_.release.size()); }
    int factsShown() const { return shown_; } // how many times the owner has come back

    // "5", "2.5": gigabytes with one decimal at most — the number a person
    // typed, not a printf artefact. Limits below 0.1 GB or above 10 TB are
    // typos, not choices.
    static juce::String formatGb(std::int64_t bytes);
    static std::optional<std::int64_t> parseGb(const juce::String& text);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;

    void refreshOffer();
    void sayUnread();

    std::optional<Facts> facts_;
    history::retention::Plan plan_;
    bool releasing_ = false;
    int shown_ = 0;

    juce::Label title_;
    juce::Label cost_;
    juce::Label disk_;
    juce::Label forecast_;
    juce::Label limitCaption_;
    juce::TextEditor limit_;
    juce::Label limitUnit_;
    juce::Label keepCaption_;
    juce::Slider keep_;
    juce::Label offer_;
    juce::ListBox rows_;
    juce::TextButton release_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HistoryStoragePanel)
};

} // namespace loopercat
