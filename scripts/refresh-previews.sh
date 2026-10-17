#!/usr/bin/env bash
# Called by main-build.yml only after every platform has built, validated and
# been collected. Publishes this exact run's screenshot, demos and README blocks
# (portfolio standard G2, G3, G5) in one commit, never over newer source.
set -euo pipefail

: "${GH_TOKEN:?missing workflow token}"
: "${GITHUB_REPOSITORY:?missing repository}"
: "${GITHUB_SHA:?missing source commit}"
: "${PREVIEW_DIR:?missing rendered preview directory}"
: "${DISTRIBUTION_URL:?missing collected main-build artifact URL}"
: "${BUILD_NUMBER:?missing main-build run number}"
if [[ "${GITHUB_REF:-}" != "refs/heads/main" ]]; then
    echo "error: preview refresh is only allowed on main" >&2
    exit 1
fi
test -s "${PREVIEW_DIR}/audio-previews.tar.gz"
test -s "${PREVIEW_DIR}/editor-preview.tar.gz"
test -s "${PREVIEW_DIR}/preset-demos/demos.json"
UPDATE_README_BLOCKS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/Tools/update_readme_blocks.py"

remote="https://x-access-token:${GH_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"

for attempt in 1 2 3; do
    git fetch "${remote}" main
    # Never replay an older render over newer code or README prose. Another
    # preview-only commit is harmless; the current main build will render any
    # newer source. Compare before overlaying files, without a forced rebase.
    if ! git diff --quiet "${GITHUB_SHA}" FETCH_HEAD -- . \
        ':(exclude)Docs/audio' ':(exclude)Docs/screenshots'; then
        echo "Main changed since this render; leaving previews to its build."
        exit 0
    fi
    git checkout --detach FETCH_HEAD
    # The archives and preset demos are the complete maintained set, so
    # renamed/removed demos must disappear too. Never clear the frozen evidence
    # in Docs/audio's subdirectories.
    rm -f Docs/audio/*.wav Docs/audio/*.mp3
    tar -xzf "${PREVIEW_DIR}/audio-previews.tar.gz"
    tar -xzf "${PREVIEW_DIR}/editor-preview.tar.gz"
    cp "${PREVIEW_DIR}"/preset-demos/*.mp3 Docs/audio/
    python3 "${UPDATE_README_BLOCKS}" demos --readme README.md \
        --manifest "${PREVIEW_DIR}/preset-demos/demos.json"
    python3 "${UPDATE_README_BLOCKS}" download --readme README.md \
        --url "${DISTRIBUTION_URL}" --build "${BUILD_NUMBER}" --commit "${GITHUB_SHA}"
    git add -A -- README.md ':(glob)Docs/audio/*.wav' ':(glob)Docs/audio/*.mp3' \
        Docs/screenshots/youknow.png
    if [[ -f Docs/audio/showcase-manifest.json ]] || git ls-files --error-unmatch Docs/audio/showcase-manifest.json >/dev/null 2>&1; then
        git add -A -- Docs/audio/showcase-manifest.json
    fi
    if git diff --cached --quiet; then
        echo "Screenshot, audio demos and README blocks are unchanged."
        exit 0
    fi
    git commit -m "chore: refresh generated media for ${GITHUB_SHA:0:12}"
    if git push "${remote}" HEAD:main; then
        exit 0
    fi
    echo "Main moved during the preview update; retry ${attempt}/3."
done

echo "error: could not push the refreshed previews" >&2
exit 1
