#!/usr/bin/env bash
# Downloads the Windows extension that GitHub Actions built (the
# reaper_srt-windows-x64 artifact) into a folder, so it can be run and
# compared locally with tools/compare_windows_builds.sh.
#
#   tools/fetch_ci_windows_build.sh [artifact-id | commit-sha | latest] [dest-dir]
#
# Uses the GITHUB_TOKEN environment variable when set (artifact downloads
# need an authenticated request); in a Claude cloud session the proxy adds
# the credential. Default: the newest artifact, into build/ci-windows/<sha>.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=${SRT_GITHUB_REPO:-shaunobi/reaper-tools}
WHAT=${1:-latest}
API="https://api.github.com/repos/$REPO/actions/artifacts"
auth=()
[ -n "${GITHUB_TOKEN:-}" ] && auth=(-H "Authorization: Bearer $GITHUB_TOKEN")

list=$(curl -sS "${auth[@]}" "$API?name=reaper_srt-windows-x64&per_page=30")
pick() { python3 -I -c '
import json,sys
what=sys.argv[1]; arts=json.load(sys.stdin)["artifacts"]
arts=[a for a in arts if not a["expired"]]
for a in arts:
    sha=a["workflow_run"]["head_sha"]
    if what=="latest" or str(a["id"])==what or sha.startswith(what):
        print(a["id"], sha[:7]); break
' "$1"; }
read -r ID SHA < <(echo "$list" | pick "$WHAT") || true
if [ -z "${ID:-}" ]; then echo "no unexpired reaper_srt-windows-x64 artifact matches '$WHAT'"; exit 1; fi
DEST=${2:-build/ci-windows/$SHA}
mkdir -p "$DEST"
echo "artifact $ID (commit $SHA) -> $DEST"
curl -sSL "${auth[@]}" -o "$DEST/artifact.zip" "$API/$ID/zip"
unzip -o -q "$DEST/artifact.zip" -d "$DEST" && rm "$DEST/artifact.zip"
ls -la "$DEST"
