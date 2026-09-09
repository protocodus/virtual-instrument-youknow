# YouKnow — Windows installation

For Windows x64 and a compatible 64-bit music host. The download includes
**VST3, CLAP and a standalone program**.

## Install the plug-in

1. Close your music host and any running copy of YouKnow.
2. Right-click `YouKnow-1.1.0-Windows-x64.zip`, choose **Extract All**, and keep
   the extracted folder in a permanent location. Run files from that folder,
   not from inside the ZIP.
3. Copy the format your host supports to the location below. Create the
   destination folder if needed; Windows may ask for administrator permission.
4. Reopen your music host and rescan its plug-ins. Add **YouKnow** to an
   instrument track, choose a preset and enable MIDI input or record monitoring.

| Format | Copy from the extracted download | Destination |
| --- | --- | --- |
| VST3 | The whole `VST3\YouKnow.vst3` folder | `C:\Program Files\Common Files\VST3\` |
| CLAP | The `CLAP\YouKnow.clap` file | `C:\Program Files\Common Files\CLAP\` |

The VST3 folder must retain its `Contents` subfolder; copying only the binary
inside it will not install the complete plug-in. These are the standard
[VST3 locations](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Plugin%2BLocations.html)
and [CLAP locations](https://github.com/free-audio/clap/blob/main/include/clap/entry.h).
When updating, replace the previous YouKnow folder or file.

If your download is `ci-windows.zip`, extract it first to find the inner
`YouKnow-1.1.0-Windows-x64.zip`, then extract that too. Keep the supplied
`README.md`, licences, privacy notice and `ThirdParty` folder with your download.

## Play without a music host

Open `Standalone\YouKnow.exe` in the extracted folder. Use **Options** to
select your audio output and MIDI input. Choose a preset and play your MIDI
keyboard or click the on-screen keys. You can create a shortcut to this
program; the extracted folder must remain in place.

## If Windows shows a security prompt

Current development builds are unsigned. If SmartScreen says the app is
unrecognized and you trust the download, **More info → Run anyway** may be
available. See [Microsoft's SmartScreen guidance](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/publish-first-app#step-6-handle-smartscreen-for-new-apps).
If antivirus reports malware or your computer's policy blocks the app,
contact support or your administrator instead of disabling protection.

## Quick fixes

- **Missing plug-in:** use a 64-bit host, check the complete VST3 folder or CLAP
  file is in the correct destination, enable that format and rescan.
- **No sound:** check the audio output, MIDI input and track monitoring. In
  standalone mode, review the devices selected in **Options**.

No activation account or licence key is required.

[Product page and demos](https://protocodus.cz/product/youknow/) ·
[Support](mailto:protocodus@proton.me)
