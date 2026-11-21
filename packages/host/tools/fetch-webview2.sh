#!/bin/sh
# Fetches WebView2's SDK (win_views.cpp: its header and its static loader) from NuGet into native/webview2/, as
# libs/mbui/scripts/win.sh does. Run once before building the Windows host ([link.windows] in pion.toml points there).
set -e
V=1.0.4258.31
HERE="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
D="$HERE/native/webview2"
if [ -f "$D/$V/build/native/include/WebView2.h" ]; then
  echo "fetch-webview2: $V kept"
  exit 0
fi
mkdir -p "$D"
curl -sSfL --retry 3 -o "$D/$V.nupkg" "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$V"
rm -rf "$D/$V" && mkdir -p "$D/$V"
(cd "$D/$V" && unzip -q ../$V.nupkg)
rm -f "$D/$V.nupkg"
echo "fetch-webview2: $V ready"
