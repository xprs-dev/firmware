#!/bin/sh
# Download the stock D340W firmware from Reolink's CDN and verify it.
#
# The vendor image is not kept in this repository (it is Reolink's, and 41 MB).
# Everything that builds a modified pak starts from the file this script
# leaves here. Run it from anywhere; output lands beside this script.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
URL=https://home-cdn.reolink.us/wp-content/uploads/2025/08/201046281755686788.162.zip
ZIP="$DIR/D340W_v3.0.0.4662_2508071282.zip"
PAK="$DIR/DB_566128M5MP_W.4662_2508071282.Reolink-Video-Doorbell-WiFi.OV05A10.5MP.WIFI8812.REOLINK.pak"
SHA=f1cd81db04fc71f7e3b71908b2d04529bec9b4b25c39bfe907c7f7d3ae60bc52

if [ -f "$PAK" ] && [ "$(sha256sum "$PAK" | cut -d' ' -f1)" = "$SHA" ]; then
  echo "already here and verified: $PAK"
  exit 0
fi

[ -f "$ZIP" ] || curl -fL --progress-bar -o "$ZIP" "$URL"
unzip -o -j "$ZIP" '*.pak' -d "$DIR" >/dev/null
# The zip names the pak exactly as the camera's web UI expects it.
[ -f "$PAK" ] || { echo "FAIL: no $PAK in the archive; the CDN may have moved on"; exit 2; }

GOT=$(sha256sum "$PAK" | cut -d' ' -f1)
if [ "$GOT" != "$SHA" ]; then
  echo "FAIL: SHA256 mismatch"
  echo "  want $SHA"
  echo "  got  $GOT"
  echo "Do not build from this file. See README.md in this folder."
  rm -f "$PAK"
  exit 2
fi
echo "verified: $PAK"
