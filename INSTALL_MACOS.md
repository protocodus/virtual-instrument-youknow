# YouKnow — macOS installation

For macOS 11 or later, on Apple silicon or Intel. The universal download
includes **VST3, Audio Unit (AU), CLAP and the standalone app**.

## Install

1. Close your music host and any running copy of YouKnow.
2. Open the macOS universal `.pkg` installer and follow its prompts.
   macOS may ask for an administrator password.
3. Reopen your music host and rescan its plug-ins if YouKnow does not appear.
4. Add **YouKnow** to an instrument track using a format your host supports.
   Select a preset, enable MIDI input or record monitoring, and play.

If your download is `ci-macos.zip`, extract it first to find the `.pkg`.
You only need one plug-in format on a track.

## Play without a music host

Open **YouKnow** in Applications. Use **Options** to select your audio output
and MIDI input. Choose a preset and play your MIDI keyboard or click the
on-screen keys.

## Manual installation from the optional ZIP

Extract the optional macOS universal ZIP (its name ends in
`-macOS-universal.zip`). Copy the individual bundles from
its `Applications` and `Library/Audio/Plug-Ins` folders to these destinations:

| Item | Destination |
| --- | --- |
| `YouKnow.app` | `/Applications/` |
| `YouKnow.component` (AU) | `/Library/Audio/Plug-Ins/Components/` |
| `YouKnow.vst3` | `/Library/Audio/Plug-Ins/VST3/` |
| `YouKnow.clap` | `/Library/Audio/Plug-Ins/CLAP/` |

Create a destination folder if it is missing. Copy each whole bundle without
opening or changing its contents. When updating, replace the previous YouKnow
bundle. Copy the included `Library/Application Support/Protocodus/YouKnow`
folder to `/Library/Application Support/Protocodus/` to keep the guide and
notices accessible. Copy the individual items, not the entire `Library` folder.

## If macOS blocks the installer or app

Current development builds are not notarized; the installer is unsigned.
If the warning is about an unidentified developer and you trust the download,
try opening it once, then use **System Settings → Privacy & Security → Open
Anyway** for that specific item. On macOS 11–12, use **System Preferences →
Security & Privacy → General**. See [Apple's instructions](https://support.apple.com/en-gb/102445).

If macOS reports malware, or the plug-in still cannot load, contact support
with the exact message. Keep macOS security protections enabled.

## Quick fixes

- **Missing plug-in:** check the destination above, enable that format in your
  host and rescan. Restart the Mac if an installed AU still does not appear.
- **No sound:** check the audio output, MIDI input and track monitoring. In
  standalone mode, review the devices selected in **Options**.

The installer places the customer guide and notices in
`/Library/Application Support/Protocodus/YouKnow/Documentation/`.
No activation account or licence key is required.

[Product page and demos](https://protocodus.cz/product/youknow/) ·
[Support](mailto:protocodus@proton.me)
