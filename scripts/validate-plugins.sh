#!/usr/bin/env bash
# Shared Protocodus host validation of a plug-in build (U5).
#
# Master copy: reason-rack-extensions/Tools/portfolio/scripts/validate-plugins.sh.
# Every canonical repo carries a byte-identical copy in scripts/, and
# Tools/check_repo_standard.py fails on a drifted one.
#
#   scripts/validate-plugins.sh <build-dir> <Name> <au-type> <au-subtype> <au-manufacturer> <seed> [config]
#   e.g. scripts/validate-plugins.sh build-plugin Ceres aumu Cer1 Cers 0x43455253
#
# Runs pluginval at strictness 10 with the product's fixed --random-seed on
# the VST3, clap-validator on the CLAP when the build has one (<NAME>_BUILD_CLAP)
# and, on macOS, auval -strict on the AU, which it installs in the user's
# Components folder first. Both validators are pinned releases checked against
# their SHA-256 and kept in <build-dir>/validators. main-build.yml and
# release.yml call it, so a local run validates exactly as CI does.
set -euo pipefail

if [[ $# -lt 6 || $# -gt 7 ]]; then
    echo "usage: validate-plugins.sh <build-dir> <Name> <au-type> <au-subtype> <au-manufacturer> <seed> [config]" >&2
    exit 2
fi
build_dir=$1 name=$2 au_type=$3 au_subtype=$4 au_manufacturer=$5 seed=$6 config=${7:-Release}
artefacts="${build_dir}/${name}_artefacts/${config}"
tools="${build_dir}/validators"
mkdir -p "${tools}"

# pluginval 1.0.4 has no CLAP host, hence clap-validator. The clap-validator
# hashes were recorded from its 0.3.2 release assets on 2026-10-27.
PLUGINVAL_VERSION=v1.0.4
CLAP_VALIDATOR_VERSION=0.3.2
case "$(uname -s)" in
    Darwin)
        pluginval_archive=pluginval_macOS.zip
        pluginval_sha256=3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f
        pluginval="${tools}/pluginval.app/Contents/MacOS/pluginval"
        clap_archive=clap-validator-${CLAP_VALIDATOR_VERSION}-macos-universal.tar.gz
        clap_sha256=3750f3729adfd8489f2b29019f7f2ed65ba71bf9d5049735f6a2ca0fccb18ffd
        clap_validator="${tools}/binaries/clap-validator" ;;
    Linux)
        pluginval_archive=pluginval_Linux.zip
        pluginval_sha256=c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352
        pluginval="${tools}/pluginval"
        clap_archive=clap-validator-${CLAP_VALIDATOR_VERSION}-ubuntu-18.04.tar.gz
        clap_sha256=1476ed68f5657e76050e0c4f19790c02d819ecc62c35fd465059d21f05169cb1
        clap_validator="${tools}/clap-validator" ;;
    MINGW* | MSYS* | CYGWIN*)
        pluginval_archive=pluginval_Windows.zip
        pluginval_sha256=c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab
        pluginval="${tools}/pluginval.exe"
        clap_archive=clap-validator-${CLAP_VALIDATOR_VERSION}-windows.zip
        clap_sha256=68cdbe51c05489542b5420a870b7159b4cb352b5ce5c486f9bd8a9b49b75bdda
        clap_validator="${tools}/clap-validator.exe" ;;
    *)
        echo "error: unsupported platform $(uname -s)" >&2
        exit 1 ;;
esac

fetch() { # <url> <archive> <sha256>
    curl -sSfL --retry 3 -o "${tools}/$2" "$1"
    if command -v sha256sum >/dev/null 2>&1; then
        echo "$3  ${tools}/$2" | sha256sum -c -
    else
        echo "$3  ${tools}/$2" | shasum -a 256 -c -
    fi
    case "$2" in
        *macOS.zip) ditto -x -k "${tools}/$2" "${tools}" ;;
        *Linux.zip) unzip -q -o "${tools}/$2" -d "${tools}" ;;
        *.zip) 7z x -y -o"${tools}" "${tools}/$2" >/dev/null ;;
        *) tar -xzf "${tools}/$2" -C "${tools}" ;;
    esac
}

# A headless Linux runner has no display for the editor tests.
host() {
    if [[ "$(uname -s)" == "Linux" && -z "${DISPLAY:-}" ]]; then
        xvfb-run -a "$@"
    else
        "$@"
    fi
}

fetch "https://github.com/Tracktion/pluginval/releases/download/${PLUGINVAL_VERSION}/${pluginval_archive}" \
    "${pluginval_archive}" "${pluginval_sha256}"
host "${pluginval}" --strictness-level 10 --random-seed "${seed}" --timeout-ms 120000 --verbose \
    --output-dir "${build_dir}/pluginval-logs" --validate "${artefacts}/VST3/${name}.vst3"

if [[ -e "${artefacts}/CLAP/${name}.clap" ]]; then
    fetch "https://github.com/free-audio/clap-validator/releases/download/${CLAP_VALIDATOR_VERSION}/${clap_archive}" \
        "${clap_archive}" "${clap_sha256}"
    host "${clap_validator}" validate "${artefacts}/CLAP/${name}.clap"
else
    echo "NOTE: ${artefacts}/CLAP/${name}.clap was not built; clap-validator skipped"
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    components="${HOME}/Library/Audio/Plug-Ins/Components"
    mkdir -p "${components}"
    rm -rf "${components:?}/${name}.component"
    ditto "${artefacts}/AU/${name}.component" "${components}/${name}.component"
    # The registrar caches the component list; restarting it rescans.
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    auval -strict -v "${au_type}" "${au_subtype}" "${au_manufacturer}"
fi
