# YouKnow — Linux installation

For **x86_64 Linux**. The download includes **VST3 and a standalone program**.
The current download is built on Ubuntu 24.04; other distributions need
compatible system libraries. An ARM system cannot run this binary natively.

## Extract the download

Keep the extracted files in a permanent folder so the standalone program,
guide and licences stay together. For example, if the archive is in Downloads:

```sh
mkdir -p "$HOME/.local/share/Protocodus/YouKnow"
tar -xzf "$HOME/Downloads/YouKnow-Linux-x64.tar.gz" \
  -C "$HOME/.local/share/Protocodus/YouKnow"
```

Adjust the archive path if you saved it elsewhere. If your download is
`ci-linux.zip`, extract that first to find `YouKnow-Linux-x64.tar.gz`.

## Install VST3 for your user

Close your music host. Copy the whole `YouKnow.vst3` folder, including its
`Contents` subfolder, into `~/.vst3/`:

```sh
mkdir -p "$HOME/.vst3"
cp -R "$HOME/.local/share/Protocodus/YouKnow/VST3/YouKnow.vst3" "$HOME/.vst3/"
```

This uses the standard [Linux VST3 user location](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Plugin%2BLocations.html)
and needs no administrator access. When updating, remove the previous
`~/.vst3/YouKnow.vst3` folder before copying the replacement.

Reopen your Linux VST3-compatible music host and rescan its plug-ins. Add
**YouKnow** to an instrument track, select a preset and enable MIDI input or
record monitoring.

## Play without a music host

Run the standalone program from a terminal:

```sh
"$HOME/.local/share/Protocodus/YouKnow/Standalone/YouKnow"
```

Use **Options** to select your audio output and MIDI input. Choose a preset
and play your MIDI keyboard or click the on-screen keys.

## Quick fixes

- **Missing plug-in:** confirm the full bundle is at `~/.vst3/YouKnow.vst3`,
  use an x86_64 Linux host with VST3 support, then rescan.
- **Permission denied:** the archive normally preserves executable permissions.
  If they were lost while copying, restore the standalone executable bit with
  `chmod u+x "$HOME/.local/share/Protocodus/YouKnow/Standalone/YouKnow"`.
- **Missing library or GLIBC version:** launch from a terminal and note the
  exact error. Install the matching runtime package through your distribution's
  package manager. A GLIBC version mismatch can require a newer compatible
  distribution or a build for your system; contact support with the error.
- **No sound:** check audio output, MIDI input and track monitoring. In
  standalone mode, review the devices selected in **Options**.

Keep the supplied licences and notices with the extracted files.
No activation account or licence key is required.

[Product page and demos](https://protocodus.cz/product/youknow/) ·
[Support](mailto:protocodus@proton.me)
