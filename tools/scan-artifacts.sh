#!/bin/sh
# Fails if a firmware image carries something that looks private: an IPv4 address (apart from
# the fixed ones the firmware needs), a LAN host name, an e-mail address or a build path.
# Usage: tools/scan-artifacts.sh build/channelstv.bin [more.bin ...]
set -eu
allowed='^(0\.0\.0\.0|127\.0\.0\.1|255\.255\.255\.255|239\.255\.255\.250)$'
found=0
for f in "$@"; do
    hits=$(strings -n 4 "$f" | grep -oE \
        '([0-9]{1,3}\.){3}[0-9]{1,3}|[A-Za-z0-9._+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}\b|[A-Za-z0-9-]+\.(lan|local|home|internal|localdomain|corp)\b|/(home|Users|root)/[^ ]*' \
        | grep -vE "$allowed" | sort -u || true)
    if [ -n "$hits" ]; then
        echo "$f:"
        echo "$hits" | sed 's/^/  /'
        found=1
    fi
done
if [ "$found" -ne 0 ]; then
    echo "private-looking strings found" >&2
    exit 1
fi
echo "clean: $*"
