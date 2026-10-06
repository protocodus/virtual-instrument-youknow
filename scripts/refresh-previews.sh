#!/usr/bin/env bash
# Called only after every platform has built and passed its checks. The archives
# contain the maintained demos/peak table and editor image from this exact run;
# the package directories hold the three customer distributables it produced.
set -euo pipefail

: "${GH_TOKEN:?missing workflow token}"
: "${GITHUB_REPOSITORY:?missing repository}"
: "${GITHUB_SHA:?missing source commit}"
: "${PREVIEW_DIR:?missing rendered preview directory}"
: "${DIST_DIR:?missing package directory}"
if [[ "${GITHUB_REF:-}" != "refs/heads/main" ]]; then
    echo "error: preview refresh is only allowed on main" >&2
    exit 1
fi
# The package gate walks `git log -- dist` for the commit that published the
# committed set. In a shallow checkout that walk ends at this very commit and
# every source reads as unchanged, so the workflow checks out full history.
if [[ "$(git rev-parse --is-shallow-repository)" == "true" ]]; then
    echo "error: preview refresh needs full history (checkout with fetch-depth: 0)" >&2
    exit 1
fi
test -s "${PREVIEW_DIR}/audio-previews.tar.gz"
test -s "${PREVIEW_DIR}/editor-preview.tar.gz"
# Each platform directory is one package artifact: the customer files and the
# SHA256SUMS.txt the packaging step wrote beside them. Verify every package
# before anything is copied, so a truncated download can never reach main.
for platform in macos windows linux; do
    (
        cd "${DIST_DIR}/${platform}"
        checksums=(YouKnow-*-SHA256SUMS.txt)
        [[ ${#checksums[@]} -eq 1 ]]
        test -s "${checksums[0]}"
        sha256sum --check --strict --quiet "${checksums[0]}"
    )
done

remote="https://x-access-token:${GH_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"

# The packages carry the run's build number, so every run's set differs. They
# are republished only when the source that produced them changed since the
# committed set; a nightly rebuild of unchanged main keeps what is there.
publish_packages() {
    local last
    last="$(git log -1 --format=%H -- dist)"
    if [[ -n "${last}" ]] && git diff --quiet "${last}" "${GITHUB_SHA}" -- . \
        ':(exclude)dist' ':(exclude)Docs/audio' ':(exclude)Docs/screenshots'; then
        echo "Source unchanged since the committed packages; keeping dist/."
        return 0
    fi
    local archive distribution version build target platform_suffix shared_identity=""
    for platform in macos windows linux; do
        archive="$(find "${DIST_DIR}/${platform}" -maxdepth 1 -name 'YouKnow-*-SHA256SUMS.txt' -exec basename {} \;)"
        if [[ ! "$archive" =~ ^YouKnow-([0-9]+\.[0-9]+\.[0-9]+)-build\.([1-9][0-9]*(\.[1-9][0-9]*)?)-(macOS-universal|Windows-x64|Linux-x64)-SHA256SUMS.txt$ ]]; then
            echo "error: invalid package identity: $archive" >&2; exit 1
        fi
        version="${BASH_REMATCH[1]}"; build="${BASH_REMATCH[2]}"; platform_suffix="${BASH_REMATCH[4]}"
        distribution="${version}-build.${build}"
        if [[ -n "$shared_identity" && "$shared_identity" != "$distribution" ]]; then
            echo "error: platform packages have different build identities" >&2; exit 1
        fi
        shared_identity="$distribution"
        target="dist/${version}/build.${build}/${platform_suffix}"
        if [[ -d "$target" && -n "$(find "$target" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
            echo "error: distribution identity already exists: $target" >&2; exit 1
        fi
        mkdir -p "$target"
        cp "${DIST_DIR}/${platform}"/* "$target/"
    done
    cp Docs/screenshots/youknow-standalone.png dist/youknow-standalone.png
    printf 'source=%s\nversion=%s\npath=%s\n' "${GITHUB_SHA}" "$distribution" \
        "dist/${version}/build.${build}" > dist/BUILD.txt

}

for attempt in 1 2 3; do
    git fetch "${remote}" main
    # Never replay an older render over newer code or README prose. Another
    # preview-only commit is harmless; the current main build will render any
    # newer source. Compare before overlaying files, without a forced rebase.
    if ! git diff --quiet "${GITHUB_SHA}" FETCH_HEAD -- . \
        ':(exclude)Docs/audio' ':(exclude)Docs/screenshots' ':(exclude)dist'; then
        echo "Main changed since this render; leaving previews to its build."
        exit 0
    fi
    git checkout --detach FETCH_HEAD
    # The archive is the complete maintained set, so renamed/removed demos must
    # disappear too. Never clear the frozen evidence in its subdirectories.
    rm -f Docs/audio/*.wav
    tar -xzf "${PREVIEW_DIR}/audio-previews.tar.gz"
    tar -xzf "${PREVIEW_DIR}/editor-preview.tar.gz"
    publish_packages
    # Frozen review/listening evidence in subdirectories is never staged. All
    # maintained demo audio lives directly under Docs/audio.
    git add -A -- README.md ':(glob)Docs/audio/*.wav' \
        Docs/screenshots/youknow-standalone.png
    if [[ -f Docs/audio/showcase-manifest.json ]] || git ls-files --error-unmatch Docs/audio/showcase-manifest.json >/dev/null 2>&1; then
        git add -A -- Docs/audio/showcase-manifest.json
    fi
    if [[ -d dist ]]; then
        git add -A -- dist
    fi
    if git diff --cached --quiet; then
        echo "Screenshot, audio demos and packages are unchanged."
        exit 0
    fi
    git commit -m "CI: refresh screenshot, audio demos and packages"
    if git push "${remote}" HEAD:main; then
        exit 0
    fi
    echo "Main moved during the preview update; retry ${attempt}/3."
done

echo "error: could not push the refreshed previews" >&2
exit 1
