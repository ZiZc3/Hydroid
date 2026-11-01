#!/bin/bash
# upload.sh — a game app onto the console over FTP: waits for the app to close (FTP 550 while it
# runs), uploads its eboot.bin, imports.txt and what changed in sce_sys (name, icon, pictures),
# and checks the size on the console. The game file itself goes in once by hand (its game/ folder).
#
#   PS5=<console IP> bash tools/upload.sh PPSA26379        (dist/PPSA26379 -> /data/homebrew/PPSA26379)
#   PS5=<console IP> PORT=2121 bash tools/upload.sh <title id>
#
# The first time, copy the whole dist/<title id> folder instead (any FTP client).

set -u
cd "$(dirname "$0")/.."
ID=${1:?usage: PS5=<console IP> bash tools/upload.sh <title id>}
PS5=${PS5:?set PS5 to your console\'s IP address (an FTP server running on it)}
PORT=${PORT:-2121}
DIST=dist/$ID
URL=ftp://$PS5:$PORT/data/homebrew/$ID
STAMP=$DIST/.uploaded # what went up last time (sce_sys only when it changed)
[ -f "$DIST/eboot.bin" ] || { echo "no $DIST/eboot.bin: make the app first (title/make_game_app.py)"; exit 1; }

size=$(stat -c %s "$DIST/eboot.bin")
echo "eboot.bin: $size bytes -> $PS5"
said=0
start=$(date +%s)
while ! curl -s -T "$DIST/eboot.bin" "$URL/eboot.bin"; do
    if [ $said = 0 ]; then
        echo "the console refused (the app is open?): close it, I keep trying ..."
        said=1
    fi
    if [ $(($(date +%s) - start)) -gt 3600 ]; then
        echo "gave up after an hour"
        exit 1
    fi
    sleep 5
done
# the eboot's import slots, always with it (app_main.c check_imports reads them)
curl -s -T "$DIST/imports.txt" "$URL/imports.txt" && echo "  imports.txt updated"
for f in "$DIST"/sce_sys/icon0.png "$DIST"/sce_sys/param.json "$DIST"/sce_sys/pic0.dds "$DIST"/sce_sys/pic1.dds "$DIST"/touchmap.txt; do
    [ -f "$f" ] || continue
    if [ ! -f "$STAMP" ] || [ "$f" -nt "$STAMP" ]; then
        rel=${f#"$DIST"/}
        curl -s --ftp-create-dirs -T "$f" "$URL/$rel" && echo "  $rel updated"
    fi
done
on=$(curl -s -I "$URL/eboot.bin" | tr -d '\r' | awk '/Content-Length/ {print $2}')
if [ "$on" = "$size" ]; then
    touch "$STAMP"
    echo "uploaded: the console has $on bytes - start the app"
else
    echo "size check failed: the console has '$on', the build $size"
    exit 1
fi
