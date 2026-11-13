#!/bin/sh
# Renders the README images into docs/images with headless Chrome. Needs tools/mock_dashboard.py
# running on port 8120 (search done and a device playing gives the best pictures).
# Usage (Git Bash on Windows): sh tools/make_mockups.sh
set -e
cd "$(dirname "$0")/.."
CH="${CHROME:-/c/Program Files/Google/Chrome/Application/chrome.exe}"
mkdir -p docs/images build
sed 's/127\.0\.0\.1:8120/127.0.0.1:8999/g' web/start.html > build/start_offline.html
shot() {   # url, width, height, scale, output
    "$CH" --headless=new --disable-gpu --hide-scrollbars --lang=en-US --force-device-scale-factor="$4" --window-size="$2,$3" \
        --virtual-time-budget=5000 --user-data-dir="$(mktemp -d)" --screenshot="$(cygpath -w "$PWD/$5")" "$1" >/dev/null 2>&1
    echo "$5"
}
shot "file:///$(cygpath -m "$PWD/tools/mockup.html")" 1600 1000 1 docs/images/mockup.png
shot "http://localhost:8120/?lang=en" 820 1180 1 docs/images/dashboard.png
shot "file:///$(cygpath -m "$PWD/build/start_offline.html")" 1280 1000 1 docs/images/tile-not-running.png
