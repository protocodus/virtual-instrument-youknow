# YouKnow — User Guide

YouKnow is a polyphonic synthesizer from Protocodus, with circuit-modeled
oscillators, filters and stereo chorus.

## Install

Detailed installation instructions: [macOS](INSTALL_MACOS.md) ·
[Windows](INSTALL_WINDOWS.md) · [Linux](INSTALL_LINUX.md).

### macOS

The macOS builds require macOS 11 or later. The universal Audio Unit (AU), VST3,
CLAP and Standalone builds support both Apple silicon and Intel Macs.

Open the supplied `.pkg` installer and follow its prompts. It installs:

- Standalone: `/Applications/YouKnow.app`
- Audio Unit: `/Library/Audio/Plug-Ins/Components/YouKnow.component`
- VST3: `/Library/Audio/Plug-Ins/VST3/YouKnow.vst3`
- CLAP: `/Library/Audio/Plug-Ins/CLAP/YouKnow.clap`
- This guide and notices: `/Library/Application Support/Protocodus/YouKnow/Documentation/`

If using the macOS ZIP, extract it and copy the enclosed app and plug-in bundles
to the same locations.

### Windows

Extract the x64 ZIP. Copy the whole `VST3/YouKnow.vst3` folder to
`C:\Program Files\Common Files\VST3\`, and copy `CLAP/YouKnow.clap` to
`C:\Program Files\Common Files\CLAP\`. Create these folders if needed. The
standalone program is `Standalone/YouKnow.exe` in the extracted archive; keep
the guide, licences and notices from the archive with your installation.

### Linux

Extract the versioned archive ending in `-Linux-x64.tar.gz` into a permanent
folder. Copy the whole
`VST3/YouKnow.vst3` folder to `~/.vst3/`, creating that destination if needed.
Run `Standalone/YouKnow` from the extracted folder to play without a host.
Keep the guide, licences and notices with the extracted files. This x86_64
package includes VST3 and Standalone; the current CI build uses Ubuntu 24.04.

### In your music host

Restart your music host after installation and rescan its plug-ins if needed.
Choose VST3 on any supported platform, AU on macOS or CLAP on macOS/Windows,
according to the formats your host supports.

## Play your first sound

In a music host, insert YouKnow on an instrument track, route your MIDI keyboard
to that track, and enable the host's input monitoring or record arm as needed.
In Standalone, open YouKnow from Applications on macOS, run
`Standalone/YouKnow.exe` on Windows or run `Standalone/YouKnow` on Linux.
Choose your audio output and MIDI input devices in **Options**.

Choose a preset, then play your MIDI keyboard or click the on-screen keys.
Use **VOLUME** to set the output level. If there is no sound, check the selected
audio output, MIDI routing and the track's mute state.

## Explore the presets

The preset menu contains **144 sounds plus INIT**: 128 factory sounds in groups
A and B, and 16 original YouKnow sounds. The originals are **YB1–YB8** (basses)
and **YP1–YP8** (pads).

| Menu family | Sounds |
| --- | ---: |
| Basses | 12 |
| Brass | 11 |
| Strings | 10 |
| Pads | 15 |
| Other factory sounds, groups A and B | 96 |

Use the menu to browse by family. **GROUP**, **BANK** and **PATCH** select the
128 A/B factory locations. The previous/next buttons step through the host
program list, including the originals.

**RELOAD** discards edits and restores the selected program. **INIT** starts a
fresh tone and performance setup. Preset selection, RELOAD and INIT retain
**Aging**, **Quality**, filter-processing settings and the current pitch-bend
and modulation positions. Save your work before replacing a sound.

## Shape the sound

- **DCO** sets oscillator range, waveforms, pulse width, sub oscillator and noise.
- **HPF** removes low frequencies; **VCF** sets cutoff, resonance and modulation.
- **ENV** sets attack, decay, sustain and release. **VCA** controls amplification.
- **LFO** adds repeating modulation. The bender and portamento shape performance.
- **CHORUS** adds stereo movement; choose Off, I, II or I+II.
- **CHARACTER**, **AGING**, **HISS**, **VELOCITY** and **VOICES** adjust the model
  and playing response. Aging defaults to 50%; zero represents a freshly serviced
  instrument. Six voices is the standard polyphonic setup.

Hover over a control or reach it with Tab for its description and current value.

## Save and load

Save your **host project or host preset** to retain the full YouKnow setup.
**SAVE .SYX** exports only the hardware-compatible tone. It does not store
volume, bender settings, portamento, voice assignment or plug-in extensions.
The live I+II chorus state is saved as Chorus II in `.syx` because that format
has only Off, I and II.

Use **LOAD .SYX**, or drop a `.syx` file onto the instrument, to import a tone.
A file containing a whole bank loads its first supported patch.

## CPU, help and support

**Quality 1x** and **VCF Solver Normal** are the defaults for lower CPU use.
Choose Quality 2x or 4x to reduce aliasing; the change takes effect when the
instrument is idle. At high host sample rates, the factor is limited automatically. High and Max solver
settings use more CPU for additional numerical precision.

**PANIC** clears held notes, sustain and sounding voices if a note gets stuck.
**HELP** opens the quick start; **ABOUT** shows the version, licence and contact.

YouKnow works offline, with no accounts, telemetry, automatic update checks or
network licensing. Audio and MIDI are processed locally. macOS and your host
have their own security checks and privacy settings; see [PRIVACY.md](PRIVACY.md).

YouKnow uses the [MIT License](LICENSE). Dependencies retain their own terms;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the included ThirdParty folder.

Support: [protocodus@proton.me](mailto:protocodus@proton.me)
Website: [protocodus.cz](https://protocodus.cz)
