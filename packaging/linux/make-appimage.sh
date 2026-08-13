#!/usr/bin/env bash
# Packs a built LooperCat into an AppImage — the one Linux format a guitarist
# can use without knowing what a distribution is: download, chmod +x, run.
#
# Usage: make-appimage.sh <build-dir> <version> [out-dir]
#
# linuxdeploy and its appimage plugin are themselves AppImages, and an
# AppImage needs FUSE to mount itself — which containers and many CI runners
# do not have. APPIMAGE_EXTRACT_AND_RUN makes every AppImage in the chain
# unpack itself instead, so the same script works with or without FUSE.
set -euo pipefail
export APPIMAGE_EXTRACT_AND_RUN=1

BUILD_DIR="${1:?usage: make-appimage.sh <build-dir> <version> [out-dir]}"
VERSION="${2:?usage: make-appimage.sh <build-dir> <version> [out-dir]}"
OUT_DIR="${3:-dist}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ARCH="$(uname -m)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BINARY="$BUILD_DIR/LooperCat_artefacts/Release/LooperCat"
[ -x "$BINARY" ] || { echo "no built binary at $BINARY" >&2; exit 1; }

# The AppDir: a miniature /usr the AppImage runtime mounts at run time.
APPDIR="$WORK/AppDir"
install -Dm755 "$BINARY"                          "$APPDIR/usr/bin/LooperCat"
install -Dm644 "$HERE/loopercat.desktop"          "$APPDIR/usr/share/applications/loopercat.desktop"
# 512 is the largest size the hicolor theme (and linuxdeploy's validator)
# accepts — the 1024 master the macOS iconset is built from is rejected
# outright, so the Linux sizes are their own files.
install -Dm644 "$REPO/art/icon/loopercat-icon-512.png" \
               "$APPDIR/usr/share/icons/hicolor/512x512/apps/loopercat.png"
install -Dm644 "$REPO/art/icon/loopercat-icon-256.png" \
               "$APPDIR/usr/share/icons/hicolor/256x256/apps/loopercat.png"
install -Dm644 "$REPO/art/icon/loopercat-icon-32.png" \
               "$APPDIR/usr/share/icons/hicolor/32x32/apps/loopercat.png"

curl -fsSL -o "$WORK/linuxdeploy" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
# The appimage OUTPUT plugin lives in its own repository; linuxdeploy finds
# it by name on PATH.
curl -fsSL -o "$WORK/linuxdeploy-plugin-appimage" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous/linuxdeploy-plugin-appimage-${ARCH}.AppImage"
chmod +x "$WORK/linuxdeploy" "$WORK/linuxdeploy-plugin-appimage"
export PATH="$WORK:$PATH"

mkdir -p "$OUT_DIR"
# libudev is deliberately NOT bundled: it talks to the running systemd/eudev
# on the host, and a bundled copy from another distribution is the classic
# way to make a portable build fail on exactly the machines it was meant to
# support. Same reasoning for the graphics and sound stacks, which
# linuxdeploy excludes by default.
export LDAI_OUTPUT="$OUT_DIR/LooperCat-${VERSION}-Linux-${ARCH}.AppImage"
"$WORK/linuxdeploy" \
    --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/loopercat.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/512x512/apps/loopercat.png" \
    --exclude-library "libudev.so*" \
    --output appimage

echo "built $LDAI_OUTPUT"
