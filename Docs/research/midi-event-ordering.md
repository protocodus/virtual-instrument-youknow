# MIDI event ordering and adjacent notes

Audit scope: the processor's MIDI dispatch, the engine key assigner, and the
JUCE 8.0.14 / CLAP wrapper revisions pinned by `CMakeLists.txt`. This note
records the processor's normalization contract and the engine policies it
preserves.

## Zero gap and actual overlap

A note-off and a replacement note-on at the same sample describe an adjacent
boundary with no audio gap. The host can supply either event first. With the
previous key still held, applying the new note-on first can suppress a repeated
pitch's attack or drop a replacement chord note because every voice is occupied.
The processor releases the old assignments before assigning replacements
within the bounded event group described below. Both operations retain the
original sample position; no silent sample is inserted.

A note-on before the old note-off at a genuinely earlier sample is an overlap.
The engine deliberately counts repeated presses of each pitch and does not
retrigger a pitch whose held count is already nonzero. Corresponding note-offs
decrement that count; the key releases only when it reaches zero. Normalization
must preserve this behavior rather than turning overlapping notes into repeated
attacks.

## Engine policies that remain intentional

- In Poly 1 and Poly 2, the allocator never steals a voice whose key is held. A
  new pitch is dropped when the available voice count is fully occupied. A
  released key makes its slot available even if its envelope or sustain tail
  still sounds. Poly 1 prefers that pitch's previous available card, then the
  longest-free card; Poly 2 scans from the first available card.
- Repeated same-pitch note-ons share the held count and the existing assignment.
  Their first matching note-off must not release the later overlapping press.
- In Unison, a real key release with other keys remaining initiates a gate and
  keyboard rescan. The highest remaining key wins, with a fresh attack. This is
  the modeled assigner behavior, including release of a nonwinning held key;
  it is not a conventional legato-only fallback.

The implementation is in `Source/DSP/YouKnowEngine.cpp`, principally
`rememberHeldNote`, `forgetHeldNote`, `allocateVoice`, `noteOnInternal`, and
`noteOffInternal`.

## Equal-timestamp normalization

1. Process events chronologically and render audio up to their sample position.
   Form a group only from a contiguous run of note-on/note-off events with the
   same **original** MIDI sample position. A velocity-zero note-on is a note-off.
2. Make a first pass over releases without applying any new note-ons.
   Move note-offs to the front only up to that pitch's initial held count. These
   releases belong to notes already held before the run. Preserve their arrival
   order relative to other selected releases.
3. Process all remaining events in the run in their original relative order.
   Extra note-offs cannot be pulled forward using counts introduced by the new
   note-ons. In particular, an initially unheld note-on followed immediately by
   its note-off stays a zero-length note and cannot become a stuck held key.
4. Every non-note event ends the run. Controllers, program changes, SysEx and
   other messages are ordering barriers, even when they share the timestamp.
   A note-on followed by All Sound Off must retain that meaning; releases cannot
   be moved across a patch change or pedal transition.
5. Distinct original timestamps never join one group, including when both clamp
   to zero in a zero-frame callback or to the same endpoint in a defensive
   out-of-range clamp.

This is a host scheduling policy at the processor boundary, not a change to the
engine's hardware-derived overlap or allocation rules. It uses pitch identity
consistently with the existing engine's omni receive behavior.

## Wrapper behavior and limits

JUCE `MidiBuffer` inserts a new event after existing events at the same offset.
Its `findEventAfter` comparison is `<=`; there is no built-in note-off priority.
The pinned CLAP wrapper converts its incoming MIDI list in arrival order, and
JUCE's AU wrapper preserves MIDI callback order. CLAP retains the MIDI header's
sample offset even though the build disables sample-accurate host parameter
automation. The CLAP port advertises the MIDI dialect, not native CLAP note IDs.

JUCE's VST3 wrapper imports mapped controller parameter queues before its note
event list. Consequently, mapped CC, pitch bend and channel pressure precede
notes at an equal offset; their original ordering relative to notes is not
recoverable inside the processor. VST3 note IDs are also lost when JUCE converts
notes to MIDI. All wrappers preserve MIDI channel bytes, but the instrument
currently combines channels into one global set of pitch counts, sustain and
performance controls. It does not maintain independent channel note ownership.

Normalization cannot inspect a future callback. Events delivered in separate
callbacks cannot be combined just because the host regards their musical time
as equal. A boundary split across callbacks retains callback order.

## Regression validation

The 36-case processor matrix covers both event orders for adjacent repeated
notes and full-voice chord replacements in Poly 1, Poly 2 and Unison, at 1, 6
and 16 voices. It exercises block boundaries and nonzero offsets, including
velocity-zero releases, across two consecutive replacements and final release.
All 36 audio comparisons failed before the fix and are bit-identical after it.

Additional regressions verify genuine one-sample overlaps, zero-length notes,
multiple offs with fewer previously held presses, sustain/panic/program/SysEx
barriers, distinct original timestamps in zero-frame callbacks, and final
release without a panic concealing stuck keys. Engine regressions inspect
attack, velocity, assignment, sustain and final release over three consecutive
gapless handoffs with every hardware card occupied.

Validation on 2026-09-09:

- Focused engine note tests and the full Engine CTest passed (full suite:
  301.47 seconds).
- Focused processor MIDI tests and the full PluginProcessor CTest passed
  (full suite: 6.41 seconds).
- Rebuilt CLAP and VST3 binaries passed their bundle smoke tests.
- `git diff --check` passed.

The focused suites can be rerun with:

```sh
YOUKNOW_NOTE_TESTS_ONLY=1 ./build-dsp/YouKnowEngineTests
YOUKNOW_MIDI_TEST_ONLY=1 ./build-ci-clap/YouKnowPluginProcessorTests
```

The bundle checks establish loading and basic host operation; the detailed
ordering matrix runs at the processor boundary. The existing CLAP smoke test
accepts at most one event per callback and does not assert final release. The
VST3 smoke test sends its note-off during bypass. Neither replaces a manual
DAW test of adjacent-note passages.
