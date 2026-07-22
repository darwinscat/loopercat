// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

//==============================================================================
// loopercat::ui — the product's own brand mark ("ears") and the header that
// wears it. The mark follows the Felitronics mark formula (appkit Brand.h):
// dark disc + violet outer ring + a product-specific middle motif in lilac +
// orange core. Looper Cat's motif is a ring-arrow — a loop chasing its own
// start, the arrowhead the same family chevron orbit-capture points inward —
// grown a pair of cat ears. Chosen from the six-candidate exploration of
// 2026-07-22 (the runner-ups live in the Felitronics mark bank).
//
// The drawing stays product-local on purpose: the family rule is that shared
// abstractions appear with the second consumer, and every other mark consumer
// today draws an orbit. When a second product needs its own mark slot, this
// moves to appkit next to drawOrbit and BrandHeader grows a hook.
//==============================================================================

#include <felitronics/appkit/Brand.h>
#include <felitronics/appkit/BrandHeader.h>

#include <juce_gui_basics/juce_gui_basics.h>

namespace loopercat::ui
{

// The "ears" mark. Same contract and coordinate space as brand::drawOrbit:
// centred at (cx, cy), diameter d, geometry authored on a 40x40 canvas with
// centre (20,20). The polyline coordinates ARE the final design data —
// verbatim from the chosen SVG (see art/icon/loopercat-mark.svg, the one
// source of the geometry).
inline void drawLoopMark (juce::Graphics& g, float cx, float cy, float d, bool hover = false)
{
    namespace brand = felitronics::appkit::brand;
    const float s = d / 40.0f;
    auto X = [&] (float p) { return cx + (p - 20.0f) * s; };
    auto Y = [&] (float p) { return cy + (p - 20.0f) * s; };
    auto polyline = [&] (std::initializer_list<juce::Point<float>> pts, float w)
    {
        juce::Path p;
        bool first = true;    // NOT Path::isEmpty(): a lone subpath start still counts as empty
        for (const auto& pt : pts)
        {
            if (first) { p.startNewSubPath (X (pt.x), Y (pt.y)); first = false; }
            else       { p.lineTo (X (pt.x), Y (pt.y)); }
        }
        g.strokePath (p, juce::PathStrokeType (w * s, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    };

    g.setColour (juce::Colour (0xff0b0b11));                         // dark disc body
    g.fillEllipse (cx - 20.0f * s, cy - 20.0f * s, 40.0f * s, 40.0f * s);
    g.setColour (hover ? brand::violet.brighter (0.2f) : brand::violet);
    g.drawEllipse (cx - 18.5f * s, cy - 18.5f * s, 37.0f * s, 37.0f * s, 2.0f * s);   // violet outer ring

    g.setColour (brand::lilac);
    // The ring-arrow: radius 10.5 with the gap at the bottom (SVG screen angles
    // 65..115 deg, 0 = east; JUCE arc angles run from north, hence +90).
    juce::Path ring;
    ring.addCentredArc (cx, cy, 10.5f * s, 10.5f * s, 0.0f,
                        juce::degreesToRadians (205.0f), juce::degreesToRadians (515.0f), true);
    g.strokePath (ring, juce::PathStrokeType (1.8f * s, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    polyline ({ { 25.71f, 25.39f }, { 23.44f, 29.98f }, { 28.42f, 31.19f } }, 2.0f);   // chevron into the gap
    polyline ({ { 27.16f, 12.32f }, { 27.50f, 7.01f },  { 23.07f, 9.96f } },  1.8f);   // right ear
    polyline ({ { 16.93f, 9.96f },  { 12.50f, 7.01f },  { 12.84f, 12.32f } }, 1.8f);   // left ear

    g.setColour (brand::orange);                                     // orange core
    g.fillEllipse (cx - 3.5f * s, cy - 3.5f * s, 7.0f * s, 7.0f * s);
}

// BrandHeader wearing the Looper Cat mark. The appkit header hardcodes
// brand::drawOrbit, so this paints the family header first and then lays the
// product mark over it at the exact spot paint() used (d = h * 0.86, centred
// at x = h + 6 + d/2) — the mark's opaque disc covers the orbit completely,
// an invariant pinned by brand_mark_tests. Hover for the mark mirrors the
// base header's link-run hover via the public linkArea().
struct LooperBrandHeader : felitronics::appkit::BrandHeader
{
    using BrandHeader::BrandHeader;

    void paint (juce::Graphics& g) override
    {
        BrandHeader::paint (g);
        const float h = (float) getHeight();
        const float d = h * 0.86f;
        drawLoopMark (g, h + 6.0f + d * 0.5f, getLocalBounds().toFloat().getCentreY(), d, markHover);
    }

    void mouseEnter (const juce::MouseEvent& e) override { trackHover (e); BrandHeader::mouseEnter (e); }
    void mouseMove  (const juce::MouseEvent& e) override { trackHover (e); BrandHeader::mouseMove (e); }
    void mouseExit  (const juce::MouseEvent& e) override { markHover = false; BrandHeader::mouseExit (e); }

private:
    void trackHover (const juce::MouseEvent& e) { markHover = linkArea (e.position); }

    bool markHover = false;
};

} // namespace loopercat::ui
