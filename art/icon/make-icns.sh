#!/bin/sh
# Build a full ten-size .icns from the 1024 px master render.
#
# JUCE's own icns writer emits exactly the two entries it is given — a
# 1024 px ic10 and a 32 px icp5, both PNG. Sequoia's Launchpad wants the
# 128 px family, falls back to icp5, and decodes its PNG payload as raw
# RGB pixels — the icon renders as colored noise (user report, 2026-08-09).
# Apple's iconutil writes the same chunk set system apps carry (ic04..ic14),
# so every consumer finds a size it can actually decode.
#
# Usage: make-icns.sh <master-1024.png> <out.icns>
set -eu

src="$1"
out="$2"

iconset="$(mktemp -d)/AppIcon.iconset"
mkdir "$iconset"
trap 'rm -rf "$(dirname "$iconset")"' EXIT

for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$src" \
        --out "$iconset/icon_${size}x${size}.png" > /dev/null
    sips -z "$((size * 2))" "$((size * 2))" "$src" \
        --out "$iconset/icon_${size}x${size}@2x.png" > /dev/null
done

iconutil -c icns "$iconset" -o "$out"
