// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The brand-mark contract, from theory, not from the drawing code:
//
//   1. COVERAGE — LooperBrandHeader overpaints appkit's drawOrbit with the
//      product mark, so the mark's opaque disc must cover the orbit COMPLETELY:
//      "orbit then mark" must render pixel-identical to "mark alone" in the
//      disc interior, at any diameter and every hover combination. If a future
//      appkit release grows an orbit that leaks past its disc, or the mark
//      shrinks, this breaks — and the header would show a mongrel of two marks.
//   2. CONTAINMENT — the mark touches nothing outside its own disc: pixels
//      beyond the disc radius (plus an antialiasing margin) keep the exact
//      background, whatever garish colour that is.
//   3. IDENTITY — the mark is actually the new one: at the ring-arrow's bottom
//      gap (design point (20,32) of the 40x40 canvas) drawOrbit shows its lilac
//      middle ring while the ears mark shows bare dark disc. A header painted
//      through LooperBrandHeader must be dark there — if the overpaint is ever
//      lost, the lilac orbit ring resurfaces and this fails.
//
// Rim note: inside a ~2px annulus at the disc edge the two composites differ
// legitimately (disc-edge antialiasing blended once vs twice), so the annulus
// is checked against a loose bound instead of equality.

#include "support.hpp"

#include "../app/LooperMark.h"

#include <BinaryData.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <functional>

namespace {

juce::Image render (int size, juce::Colour bg, const std::function<void (juce::Graphics&)>& draw)
{
    juce::Image img (juce::Image::ARGB, size, size, true);
    juce::Graphics g (img);
    g.fillAll (bg);
    draw (g);
    return img;
}

int maxChannelDiff (juce::Colour a, juce::Colour b)
{
    return juce::jmax (std::abs ((int) a.getRed()   - (int) b.getRed()),
                       std::abs ((int) a.getGreen() - (int) b.getGreen()),
                       std::abs ((int) a.getBlue()  - (int) b.getBlue()),
                       std::abs ((int) a.getAlpha() - (int) b.getAlpha()));
}

// One coverage + containment sweep at a given diameter and hover combination.
void checkCoverageAt (float d, bool orbitHover, bool markHover)
{
    const juce::Colour bg (0xffff00ff);            // garish magenta: leaks can't hide
    const int size = (int) std::ceil (d) + 16;
    const float c = (float) size / 2.0f;
    const float s = d / 40.0f;

    const auto markOnly = render (size, bg, [&] (juce::Graphics& g) {
        loopercat::ui::drawLoopMark (g, c, c, d, markHover);
    });
    const auto orbitThenMark = render (size, bg, [&] (juce::Graphics& g) {
        felitronics::appkit::brand::drawOrbit (g, c, c, d, orbitHover);
        loopercat::ui::drawLoopMark (g, c, c, d, markHover);
    });

    const float discR = 20.0f * s;
    const float rimHalf = 2.0f;                    // antialiasing annulus, either side of the edge
    bool interiorExact = true, outsideUntouched = true, rimBounded = true;
    for (int y = 0; y < size && (interiorExact || outsideUntouched || rimBounded); ++y)
        for (int x = 0; x < size; ++x)
        {
            const float r = std::hypot ((float) x + 0.5f - c, (float) y + 0.5f - c);
            const auto a = orbitThenMark.getPixelAt (x, y);
            const auto b = markOnly.getPixelAt (x, y);
            if (r <= discR - rimHalf)
            {
                if (a != b && interiorExact)
                {
                    std::printf ("  interior diff at (%d,%d) r=%.1f d=%.1f: %08x vs %08x\n",
                                 x, y, r, d, (unsigned) a.getARGB(), (unsigned) b.getARGB());
                    interiorExact = false;
                }
            }
            else if (r >= discR + rimHalf)
            {
                if ((a != bg || b != bg) && outsideUntouched)
                {
                    std::printf ("  outside touched at (%d,%d) r=%.1f d=%.1f: %08x / %08x\n",
                                 x, y, r, d, (unsigned) a.getARGB(), (unsigned) b.getARGB());
                    outsideUntouched = false;
                }
            }
            else if (maxChannelDiff (a, b) > 80 && rimBounded)   // once- vs twice-blended edge
            {
                std::printf ("  rim diff %d at (%d,%d) r=%.1f d=%.1f\n",
                             maxChannelDiff (a, b), x, y, r, d);
                rimBounded = false;
            }
        }
    CHECK (interiorExact);
    CHECK (outsideUntouched);
    CHECK (rimBounded);
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // 1 + 2: coverage and containment across sizes (17.2 is the header's mark at
    // the layout's h=20 minimum; fractional diameters exercise non-integer
    // scale) and every hover combination.
    for (const float d : { 17.2f, 27.34f, 34.4f, 40.0f, 55.9f, 128.0f })
        for (const bool orbitHover : { false, true })
            for (const bool markHover : { false, true })
                checkCoverageAt (d, orbitHover, markHover);

    // 3: the mark is the ears, not the orbit. Design point (20,32): on the
    // 40x40 canvas that is the orbit's lilac middle ring (radius 12) but the
    // ears mark's bottom gap — bare disc, far from ring ends, chevron and core.
    {
        const float d = 80.0f;                      // s = 2: design units map to 2px each
        const float s = d / 40.0f;
        const int size = (int) d + 16;
        const float c = (float) size / 2.0f;
        const auto probe = [&] (const juce::Image& img) {
            return img.getPixelAt ((int) c, (int) (c + 12.0f * s));
        };

        const auto orbit = render (size, juce::Colours::black, [&] (juce::Graphics& g) {
            felitronics::appkit::brand::drawOrbit (g, c, c, d, false);
        });
        const auto mark = render (size, juce::Colours::black, [&] (juce::Graphics& g) {
            loopercat::ui::drawLoopMark (g, c, c, d, false);
        });
        const auto lilac = felitronics::appkit::brand::lilac;
        CHECK (maxChannelDiff (probe (orbit), lilac) < 40);          // orbit: lilac ring here
        CHECK (maxChannelDiff (probe (mark), juce::Colour (0xff0b0b11)) < 8);   // ears: bare disc

        // The ears and the chevron actually EXIST — probes on the stroke
        // centrelines (design coordinates). This is the check that catches a
        // path built from moveto-only vertices, which strokes to nothing.
        const auto at = [&] (const juce::Image& img, float ux, float uy) {
            return img.getPixelAt ((int) (c + (ux - 20.0f) * s), (int) (c + (uy - 20.0f) * s));
        };
        CHECK (maxChannelDiff (at (mark, 27.33f, 9.67f), lilac) < 60);   // right ear, mid-stroke
        CHECK (maxChannelDiff (at (mark, 12.67f, 9.67f), lilac) < 60);   // left ear, mid-stroke
        CHECK (maxChannelDiff (at (mark, 23.44f, 29.98f), lilac) < 60);  // chevron tip

        // The header itself must show the ears' gap, not the orbit's ring.
        loopercat::ui::LooperBrandHeader header (
            BinaryData::catlogo_svg, BinaryData::catlogo_svgSize,
            BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize,
            "LooperCat", "https://example.invalid");
        const int hh = 52;
        header.setBounds (0, 0, 400, hh);
        juce::Image headerImg (juce::Image::ARGB, 400, hh, true);
        juce::Graphics hg (headerImg);
        header.paint (hg);
        const float hd = (float) hh * 0.86f;
        const float hs = hd / 40.0f;
        const float mx = (float) hh + 6.0f + hd * 0.5f;
        const float my = (float) hh / 2.0f;
        const auto atGap = headerImg.getPixelAt ((int) mx, (int) (my + 12.0f * hs));
        CHECK (maxChannelDiff (atGap, juce::Colour (0xff0b0b11)) < 8);
        const auto atCore = headerImg.getPixelAt ((int) mx, (int) my);
        CHECK (maxChannelDiff (atCore, felitronics::appkit::brand::orange) < 40);
        const auto atEar = headerImg.getPixelAt ((int) (mx + (27.33f - 20.0f) * hs),
                                                 (int) (my + (9.67f - 20.0f) * hs));
        CHECK (maxChannelDiff (atEar, felitronics::appkit::brand::lilac) < 80);  // header wears the ears
    }

    return testkit::summary ("brand_mark_tests");
}
