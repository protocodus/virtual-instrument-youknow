# Files for e-shop distribution

Moved from the [README](../../README.md#downloads) on 2026-10-17 and updated
for the main build, whose latest packages the README's Downloads section links.

The two customer downloads are the macOS `.pkg` installer and the Windows
`.zip`. Each contains every format for its platform, so customers do not need
a separate download per plug-in format. The Linux archive is optional.

| Platform | File to give customers | Included formats |
| --- | --- | --- |
| macOS 11+, Apple silicon and Intel | `YouKnow-<version>-build.<run>-macOS-universal.pkg` | VST3, Audio Unit, CLAP, Standalone |
| Windows x64 | `YouKnow-<version>-build.<run>-Windows-x64.zip` | VST3, CLAP, Standalone |
| Linux x64 (optional) | `YouKnow-<version>-build.<run>-Linux-x64.tar.gz` | VST3, CLAP, Standalone |

All of them come from one `youknow-main-build-<run>` artifact, with a
`SHA256SUMS` covering every file and a `BUILD.txt` naming the source commit
and run. The macOS `.zip` beside the `.pkg` is for optional manual
installation; the `.pkg` is the recommended customer download.

Upload the customer files to the shop's download storage, keeping the packaged
licences and notices intact. Packages include the user guide, privacy notice
and all three installation guides. Main-build artifacts expire after 30 days,
so store the files permanently and give customers persistent shop or published
release URLs.

To download a main build with the GitHub CLI, find its run ID (the number at
the end of the run's URL), then download its collected artifact:

```sh
gh run list --repo protocodus/virtual-instrument-youknow \
  --workflow main-build.yml --status success --limit 10
gh run download RUN_ID \
  --repo protocodus/virtual-instrument-youknow \
  --pattern 'youknow-main-build-*' \
  --dir out/distribution/main-build-RUN_ID
cd out/distribution/main-build-RUN_ID/youknow-main-build-* && sha256sum -c SHA256SUMS
```

These are development builds: the macOS bundles are ad-hoc signed and not
notarized, and the Windows binaries are unsigned. For a signed and notarized
macOS release, use the
[release workflow](../../.github/workflows/release.yml) and
[release script](../../scripts/release-macos.sh). That path produces the
universal `.pkg`, a `.manifest.txt` build record and a versioned
`-SHA256SUMS.txt` file; it does not produce the optional manual-install ZIP.
The release workflow attaches the Windows and Linux packages from its own CI
run to the same draft.

Local builds write the same customer files under `build-macos/dist/`,
`build-win/dist/` and `build-dsp/dist/`; the plug-in bundles themselves sit in
each build directory's `YouKnow_artefacts/` tree.
