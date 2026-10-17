#!/bin/sh
set -eu

cd "$(dirname "$0")/../.."
export PATH=/opt/homebrew/bin:$PATH

OUT_DIR="${1:-package}"
TAG="${2:-dev}"

fail() {
  printf 'package_macos_dmg: %s\n' "$*" >&2
  exit 1
}

echo '==> Checking platform and dependencies'
[ "$(uname -s)" != "Darwin" ] && fail "This script must be run on macOS"
[ ! -f host/build/ConkerRecomp ] && fail "missing host/build/ConkerRecomp; build it first"
for cmd in create-dmg dylibbundler hdiutil codesign; do
  command -v "$cmd" >/dev/null 2>&1 || fail "Missing required tool: $cmd"
done

STAGE="$OUT_DIR/dmg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
echo '==> Building package'
sh host/package_macos.sh "$STAGE" || fail "host/package_macos.sh failed"
[ ! -d "$STAGE/ConkerRecomp.app" ] && fail "ConkerRecomp.app not created"

echo '==> Preparing Read Me files'
mkdir "$STAGE/Read Me"
for file in .github/release/README.txt LICENSE .github/release/LICENSES.txt tools/N64ModernRuntime/COPYING; do
  [ -f "$file" ] || fail "Missing file: $file"
done
cp .github/release/README.txt "$STAGE/Read Me/"
cp LICENSE "$STAGE/Read Me/"
cp .github/release/LICENSES.txt "$STAGE/Read Me/"
cp tools/N64ModernRuntime/COPYING "$STAGE/Read Me/LICENSE-GPL-3.0.txt"

DMG="$OUT_DIR/ConkerRecompiled-$TAG-macOS.dmg"
rm -f "$DMG"
ICNS="$STAGE/ConkerRecomp.app/Contents/Resources/ConkerRecomp.icns"
[ ! -f "$ICNS" ] && fail "Missing icon file: $ICNS"

echo '==> Creating DMG'
create-dmg --volname "Conker Recompiled $TAG" --volicon "$ICNS" --window-size 600 400 --icon-size 112 --icon "ConkerRecomp.app" 150 190 --hide-extension "ConkerRecomp.app" --app-drop-link 450 190 --icon "Read Me" 300 330 --no-internet-enable "$DMG" "$STAGE" || fail "create-dmg failed. Check /Volumes for leftover dmg.* volumes."

echo '==> Verifying DMG'
hdiutil verify "$DMG" || fail "hdiutil verify failed"

MNT='' TMPD=''
cleanup() {
  if [ -n "$MNT" ]; then
    hdiutil detach "$MNT" -quiet || hdiutil detach "$MNT" -force || true
  fi
  if [ -n "$TMPD" ]; then
    rm -rf "$TMPD"
  fi
}
trap cleanup EXIT INT TERM

echo '==> Attaching DMG for verification'
ATTACH_OUT=$(hdiutil attach -nobrowse -readonly "$DMG") || fail "hdiutil attach failed"
MNT=$(printf '%s\n' "$ATTACH_OUT" | sed -n 's|.*\(/Volumes/.*\)$|\1|p' | tail -n 1)
[ -z "$MNT" ] && fail "Failed to attach DMG"
ls -lA "$MNT"

TMPD=$(mktemp -d)
cp -R "$MNT/ConkerRecomp.app" "$TMPD/"
codesign --verify --deep --strict "$TMPD/ConkerRecomp.app" || fail "codesign verification failed"

echo '==> Checking for forbidden files'
forbidden=$(find "$MNT" \( -name '*.z64' -o -name '*.n64' -o -name '*.v64' -o -name 'baserom*' \) -print)
if [ -n "$forbidden" ]; then
  printf '%s\n' "$forbidden" >&2
  fail 'ROM file found in DMG'
fi

echo '==> Summary'
echo "DMG: $DMG"
ls -l "$DMG"
shasum -a 256 "$DMG"
