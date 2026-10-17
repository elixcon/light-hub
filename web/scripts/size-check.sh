#!/bin/sh
# The UI is embedded in the coordinator image, which has to fit its partition
# (see partitions.csv); the UI may take 150 KB gzipped and no more. zopfli is
# what main/CMakeLists.txt runs, so this is the number that lands in flash.
set -e
f="$(dirname "$0")/../../main/www/index.html"
limit=153600
raw=$(wc -c < "$f" | tr -d ' ')
n=$(zopfli -c "$f" | wc -c | tr -d ' ')
echo "index.html: $raw bytes raw, $n bytes zopfli (limit $limit)"
[ "$n" -le "$limit" ] || { echo "UI is over the size budget" >&2; exit 1; }
