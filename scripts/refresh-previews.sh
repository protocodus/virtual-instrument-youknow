#!/usr/bin/env bash
# Called only after every platform has built and passed its checks. The archives
# contain the maintained demos/peak table and editor image from this exact run.
set -euo pipefail

: "${GH_TOKEN:?missing workflow token}"
: "${GITHUB_REPOSITORY:?missing repository}"
: "${GITHUB_SHA:?missing source commit}"
: "${PREVIEW_DIR:?missing rendered preview directory}"
if [[ "${GITHUB_REF:-}" != "refs/heads/main" ]]; then
    echo "error: preview refresh is only allowed on main" >&2
    exit 1
fi
test -s "${PREVIEW_DIR}/audio-previews.tar.gz"
test -s "${PREVIEW_DIR}/editor-preview.tar.gz"

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
    # The archive is the complete maintained set, so renamed/removed demos must
    # disappear too. Never clear the frozen evidence in its subdirectories.
    rm -f Docs/audio/*.wav
    tar -xzf "${PREVIEW_DIR}/audio-previews.tar.gz"
    tar -xzf "${PREVIEW_DIR}/editor-preview.tar.gz"
    # Frozen review/listening evidence in subdirectories is never staged.
    git add -A -- README.md ':(glob)Docs/audio/*.wav' \
        Docs/screenshots/youknow-standalone.png
    if git diff --cached --quiet; then
        echo "Screenshot and audio demos are unchanged."
        exit 0
    fi
    git commit -m "CI: refresh screenshot and audio demos"
    if git push "${remote}" HEAD:main; then
        exit 0
    fi
    echo "Main moved during the preview update; retry ${attempt}/3."
done

echo "error: could not push the refreshed previews" >&2
exit 1
