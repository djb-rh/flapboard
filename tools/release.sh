#!/bin/sh
# Builds the release files into dist/ and the web installer into site/:
#   flapboard-<ver>.bin         full image (bootloader, partition table, app),
#                               flashed at offset 0 by the web installer or
#                               esptool; erases settings
#   flapboard-<ver>-update.bin  the app alone, for System > Update firmware on
#                               the sign's web page; keeps settings
# The version comes from FLAPBOARD_VERSION in platformio.ini. Release builds
# leave out the developer Wi-Fi seed (include/secrets.h), and this script
# stops if that network's name turns up in the image anyway.
# Usage: tools/release.sh
set -e
cd "$(dirname "$0")/.."
ver=$(sed -n 's/.*FLAPBOARD_VERSION=\\"\([^\\]*\)\\".*/\1/p' platformio.ini)
[ -n "$ver" ] || { echo "no FLAPBOARD_VERSION in platformio.ini" >&2; exit 1; }
PLATFORMIO_BUILD_FLAGS="-DFLAPBOARD_RELEASE" ~/.platformio/penv/bin/pio run -e app
if [ -f include/secrets.h ]; then
  ssid=$(sed -n 's/.*WIFI_SSID[[:space:]]*"\([^"]*\)".*/\1/p' include/secrets.h)
  if [ -n "$ssid" ] && LC_ALL=C grep -qaF "$ssid" .pio/build/app/firmware.bin; then
    echo "the developer Wi-Fi network is in the image; not releasing" >&2; exit 1
  fi
fi
rm -rf dist site && mkdir -p dist site
cp .pio/build/app/firmware.factory.bin "dist/flapboard-$ver.bin"
cp .pio/build/app/firmware.bin "dist/flapboard-$ver-update.bin"
cp "dist/flapboard-$ver.bin" "site/flapboard-$ver.bin"
sed -e "s/@VER@/$ver/g" web-installer/index.html > site/index.html
sed -e "s/@VER@/$ver/g" web-installer/manifest.json > site/manifest.json
[ -f docs/sign.jpg ] && cp docs/sign.jpg site/sign.jpg
ls -la dist site
# Nothing is rebuilt for development after this: the next `pio run -e app`
# compiles with the developer seed again.
