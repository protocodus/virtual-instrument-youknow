# YouKnow detailed release notes

Moved from the [README](../../README.md) on 2026-10-17; the
[version history](../../README.md#version-history) there is the summary.

### Product selection — 2026-10-05

The owner chose the complete five-mechanism B profile by ear against the
frozen shipping engine at `8f51dc8`. It is now the default for the plug-in
and maintained product renders; existing sessions keep their controls and
use this sound model. Product builds always select this profile. The former
`ProductFidelityProfile` remains available to explicit diagnostic fixtures.

- **Chorus motion and drive:** Mode I uses the original-board effective
  spectral fit (3.3803 ms ±1.7618 ms at 0.51593 Hz); Mode II shares that
  excursion at the schematic-derived 0.83761 Hz estimate. The MN3009 transfer
  uses the declared serviced-bias/noise-subtraction estimate, about 0.43%
  harmonic distortion at 0.78 Vrms. The I+II compatibility policy is retained.
- **Voice response:** Tr20's named 2SA1015 prior gives a conditional 177.78 mV
  knee, quieter late tails and consistent control loading/service gain.
  The VCF uses nominal original-hybrid/service frequency coordinates instead
  of the six Borish replacement-card fits. ROM envelope timing is retained.
- **Source interaction:** WAVE, the SUB branch and D6 now solve together,
  with continuous C56 charge, the physical hybrid input load/conversion and
  explicit source priors. The measured 8.896 V SUB-only aggregate and existing
  source scales constrain the calibration; the approximately 4.98 kΩ source
  resistance remains a selected compatibility prior, not a measured part.
- **CPU tradeoff:** the joint nonlinear mixer and continuous six-card
  processing replace the earlier idle-card skip and increase processing cost,
  including while the instrument is idle. No general CPU bound is established.

This listening decision selects the estimates for the product; it does not
upgrade their provenance or close the original-card resonance-versus-byte
gap. The A/B and normalized-difference listening files remain frozen working
artifacts outside committed audio. See [the decision log](../decisions.md).

### Changes in 1.2.0

These changes are relative to the distributed 1.1.0 development source,
`f6b6138`. Version 1.1.0 had no tagged release. The 1.2.0 downloads are
unsigned development packages, with ad-hoc signatures on macOS bundles;
they are not signed and notarized production installers.

- **Chorus circuit and timing:** the product selects the nominal nonlinear
  2SA1015 follower network, including reciprocal loading. Moving clocks use
  midpoint integration across triangle corners, and input capture and output
  updates follow complementary clock phases. At high quality, input-filter
  evaluation occurs at the capture edge and reconstruction integrates actual
  output holds. These correct numerical errors without fitting an extra gain
  or changing the chosen chorus rate/depth. Installed transistor and BBD
  output-impedance uncertainty remains documented under OQ-04.
- **Analog coupling and switching:** C59 uses the conditional service-derived
  load, the common VCA includes its drawn feedback pole, and the chorus mute
  includes C15 loading and delayed clock clamps. Capacitor and bucket state
  survive interrupted switching; the component priors remain explicit.
- **Further electronics:** BA662 output and tail mirrors add topology-derived
  noise to the resonance and voice-VCA paths. Tr5's finite base drive slightly
  delays chorus clock restart and wet admission. PHONES now includes the
  original M5218L's nominal bandwidth, sampled slew bound and a declared
  white-equivalent intrinsic floor. These are documented component estimates,
  selected under the owner's engineering authorization.
- **Main output:** the shared summer's existing clip runs with local
  antialiasing at coarse internal rates, reducing folded digital harmonics
  during loud chords. GC1 resistor noise adds tiny shared gain fluctuations.
  The complete product reports 120 samples at ordinary host rates;
  already-high host rates retain 41. These changes leave circuit calibration
  and output connection choices intact.
- **Level and noise:** the pulse leg is 1.34 dB quieter relative to saw, and
  chorus hiss uses the corrected factor 2.37 instead of 3.98. Output-jack
  resistors contribute their nominal thermal floor. These change the sound
  of existing sessions without changing their stored control positions.
- **Output connections:** the Session area's **OUTPUT...** menu adds
  High/Medium/Low and finite receiver loads, including mono-jack sharing.
  High/Open remains the default. Connection settings persist in sessions
  and are retained when selecting a preset.
- **Firmware comparison:** original A-5 receive/assignment and B-2 control
  execution now connect through a resumable UART and pin-decoder path to the
  audio engine. Its explicit warm state and timing conventions are comparison
  inputs; normal host MIDI handling and the selected chart timing are unchanged.
- **CPU tradeoff:** nonlinear chorus followers measured about 17–20% above
  finite-linear followers in the local six-voice test. Chorus-off clock-mute
  state also adds work. The later numerical changes were measured separately;
  there is no accumulated before/after CPU claim. The optional live firmware
  comparison is substantially more expensive and is not the playback default.

### Changes in 1.1.0

- The product carries the owner's four listening decisions of 2026-09-22
  (`Docs/decisions.md`). The raw engine keeps its reference configuration,
  so the frozen fingerprints are unchanged.
  - Main noise: `MainNoiseCalibrationProfile::CoreBandTp8` scales the shared
    Tr21 source by 2.281 (+7.16 dB). Service Notes p. 19 brackets its
    "4Vp-p" on the trace's dense core, about 2.9σ, where the nominal
    coordinate read the same 4 Vpp as the whole 6.6σ trace. #439522's
    2.88σ corroborates the reading and was not fitted. The C41/R79 spectrum
    and the NOISE control law are unchanged.
  - Filter drive: `configureOscillatorLevelScale` applies 0.738 to the saw,
    pulse and sub legs of every WAVE node, at render, behind a closed VCA
    and where C56 is primed. The nominal saw now reads 4.83 Vp-p at TP8
    against the 6 Vp-p VCA trim, inside the MKS-7 Service Notes'
    4.8 ± 0.5 Vp-p window for the same MC5534A/80017A voice (#439522 reads
    4.88). `process()` divides the same scale out at the final digital
    boundary, beside the VCA service gain's reciprocal. Oscillator patches
    therefore keep their loudness, with the filter and VCA saturating less
    (the C4 notes of the hiss measurement's seven factory patches moved
    +0.09 dB on average, −0.4 to +0.8 dB), while noise, self-oscillation and
    chorus hiss stand 2.64 dB higher against them. The coupled-mixer comparison keeps its own calibrated
    source scale and rejects this one.
  - Chorus hiss: `ChorusNoiseCalibrationProfile::IdleFloor439522`
    multiplies the Chorus Noise control by 3.98 at the chorus, past the
    panel's clamp, so the control keeps its range and sessions their
    positions. The owner chose to match four hash-pinned April 2026 bank
    captures of #439522, but the factor was measured through a boxcar
    window that leaked the hardware idle floors' sub-20 Hz drift into the
    band. Through a Hann window, `Tools/AnalyzeChorusIdleFloors.py` puts
    3.98's Mode I idle floor against each patch's C4 note 1.33 dB over the
    captures across 20 Hz–20 kHz and 4.48 dB A-weighted (seven patches).
    Version 1.2.0 supersedes this calibration with factor 2.37 (OQ-03).
    Only the level is targeted: the captured hiss is brighter, about 5 dB
    higher in mean PSD per hertz over 2–8 kHz against 0.2–2 kHz (the
    2026-09-29 density audit confirms +4.75 dB; OQ-03).
  - Chorus Mode II: under `OwnerBlend` it now runs Mode I's blended 3.49 ms
    ±2.04 ms at the derived II/I ratio, 0.852 Hz, as the circuit's
    rate-only mode switch implies. Its pitch-modulation depth falls from
    about ±15.5 to ±12 cents. I+II keeps its summed-rate compatibility
    coordinates.
  - Factory loudness: with noise, resonance and hiss higher against the
    oscillators, the full bank audit put fifteen programs 0.13–1.50 dB over
    the −28.5 dBFS gated ceiling: A35, A56, A76, A81, B13, B14, B25, B35,
    B43, B48, B52, B56, B68, B78 and B87. Their VR1 positions drop to the
    same 0.05 dB target below it (B87 Froggy the most, 0.800 to 0.676), and
    re-rendered they sit at −28.555 to −28.573 dBFS. No preset peaks above
    −6.2 dBFS, and the sixteen original presets pass unchanged.
- The PWM parameter now ends where the loaded hardware pot does, at stored
  byte 105 (`YouKnowEngine::pwmPanelTopByte`), at the owner's request after
  the PWM range was checked against the hardware. Roland's factory bank
  stores no PWM byte above 105, and ten tones sit exactly on it while every
  other control reaches 127. The identified unit's manual sweep of bytes
  0–105 follows the engine's byte-to-duty law within 1 % duty at every byte.
  Full travel therefore reads 100 % and plays about 96.5 % pulse width,
  inside Roland's 93–97 %. The top 17 % of the former travel, bytes 106–127,
  is gone; from byte 113 it pinned the comparator and silenced the pulse,
  which the hardware slider cannot do.
  - The value keeps the stored byte's image, so sessions and presets restore
    unchanged. A value above the new top, from an older session, a patch
    file or SysEx, is held at byte 105, including on the audio thread before
    the parameter reflects it.
  - Host automation lanes keep their normalized values, so a lane recorded
    against the old range now spans the pot's range.
  - LFO mode uses the same slider, so its sweep depth tops out at byte 105
    as the hardware's does.
- The resonance return's headroom now follows the card's temperature with
  the stages'. Both headrooms are 2 Vt referred through a resistor ratio —
  the stages' through 560/68560, the return's through the 100 kΩ/1.5 kΩ
  divider — and both OTAs are potted in the one 80017A, so a return frozen
  at its 25 °C value while the stages warmed compressed 5 % earlier than
  they did on a warm card, a state the physics does not have. Under joint
  scaling the limit cycle's loop gain and droop are invariant to nine digits
  in the harmonic balance, so the derived frequency-trim table stays exact
  and only the amplitude moves: the settled full-RES self-oscillation at Unit
  Character 1 would read 5.06–5.09 Vp-p instead of 4.8. Roland's ADJUSTMENT
  sets each card's RES trimmer for 4.8 Vp-p at code 6272 after ten minutes,
  before FREQ, so each card's full-travel loop gain is now the harmonic
  balance's solution for 2.4 V peak with that card's pole spread and
  settled headrooms, as a ratio to the nominal card's solve (Unit Character
  0 keeps the endpoint constant bit for bit; the voiced ±2 % travel residual
  stays on top as the spread that survives the adjustment). With that
  residual cleared every card settles between 4.796 and 4.804 Vp-p; a cold
  card starts near 4.15 Vp-p and grows into the figure on the warm-up law.
  The FREQ solve carries the adjustment's shift of the loop gain the
  frequency-trim table is read at, and the complete-voice ±10-cent windows
  hold at a worst of 8.7 cents at Unit Character 1 (the residual draws, not
  the solve, set that figure: the render's droop sits 2–2.5 cents from the
  harmonic balance on every card). `enableResonanceHeadroomTemperature` and
  `enableResonanceServiceTrim` keep the former arithmetic for A/B renders.
- The chassis gradient across the cards now develops on the warm-up clock.
  It is the supply's heat reaching the cards unequally, absent at power-on,
  so every card now starts at 25 °C with no cutoff spread and settles into
  the same temperatures and trims as before; the gradient's cutoff factor is
  refreshed with the Johnson scales on the 375 Hz control cadence, and the
  service trims read the settled reference rather than the running clock, so
  a serviced card is mis-trimmed by the gradient it has not yet acquired
  until it has. Settled renders are unchanged.
- The output stage's resistor floors — IC6's summer network and the loaded
  volume network — now follow the jack board's temperature through Johnson's
  law, as the cards' floors already did: +0.21 dB at the settled 40 °C of
  Unit Character 1, exactly one at 25 °C and exact silence at Unit Character
  0. IC5's −94 dBV floor is a 25 °C data-sheet band figure with no
  temperature coefficient and stays at it.
- The sub's switch transistor Tr19 leaves saturation later than it enters
  it, so the half-cycle D6 conducts begins late by the storage time and the
  sub carries H2 = π·t_s·f_sub: −88 dBc at the 16′ bottom key, −64 dBc at the
  4′ top (a render there measures −63.9 dBc). The 0.2 µs is the era's
  small-signal switching class — 2N3904 200 ns maximum, 2N2222A 225 ns
  typical — because Toshiba's 2SC1815 sheet lists no switching times; a
  borrowed class value, identified as such beside the constant, not a fit to
  the identified unit's −58.5 dBc (which would need 5.8 µs and passes through
  replacement cards). `enableSubStorageSkew` keeps the instantaneous edge.
- The resonance BA662's input offset is now part of the loop. Rohm's BA6110
  sibling sheet lists "Low offset voltage (VIO = 3 mV max)", the BA662 itself
  publishes no typical, and no service step nulls it (VR30/R112 null the voice
  VCA only), so each card draws a signed offset up to ±1.5 mV at the pair —
  half that maximum, the IR3109 stage span — which its 100 kΩ/1.5 kΩ divider
  refers to the node and its control current carries into stage one. A RES
  change therefore moves the loop's DC operating point, which C59 turns into
  a small thump, and self-oscillation settles differently per card; with the
  loop open the term is exactly zero. It scales with Unit Character and is
  absent at zero, like the stage offsets. In a level-matched render the
  difference sits about −29 dB under a low note whose RES alternates every
  0.4 s and −50 to −55 dB while RES rests; the free-running self-oscillation
  segments diverge in phase, which is not an audibility figure. The complete-
  voice service-calibration check moves its worst card from 7.64 to 7.74 cents
  at Unit Character 1, inside Roland's ±10-cent window.
- RANGE now changes the DCO charging current at the control write while
  preserving C54 charge and the independent PIT clock reload. The first
  partial saw cycle and its PWM crossing follow the newly selected resistor.
  The printed 399 kΩ resistor also restores the small 400/399 amplitude
  factor at 16′; pitch division and pulse/sub logic levels are unchanged.
- Envelopes now update in B-2’s later PWM/VCF/VCA train, after all six DCO
  pitch writes. A Voice On/Off restart keeps completed envelope stores and
  abandons stores not yet reached, so interrupted scans cannot manufacture
  extra attack/release increments. The 4.2 ms/chart timestamp convention stays.
- The chorus switch now discharges C16 through the drawn R46 330 Ω resistor.
  This preserves capacitor charge on interrupted switching and changes the
  nominal settled return opening from 113.2 to 120.6 ms. It closes the
  missing resistor in OQ-20; installed transistor and clock-clamp behavior
  remain unmeasured.
- Corrected keyboard ownership and overflow recovery to prevent stuck UI
  notes and accidental releases of overlapping host notes, including during
  transport reset. Invalid channel MIDI is discarded before note/control
  decoding, while valid events retain their sample timing.
- SysEx import now accepts legal interleaved real-time bytes. Export includes
  pending MIDI edits and reports the I+II-to-II conversion from the same
  snapshot; adding a filename extension cannot silently overwrite a different
  destination than the file chooser confirmed.
- Corrupt session chunks containing non-finite parameter values leave the
  current state intact. Restoring a session refreshes the CLAP host's cached
  controls after the complete state transaction. Re-entering the displayed
  maximum cutoff frequency preserves that displayed value.
- Filter ENV/KEY calculations now retain the B-2 firmware's operand precision
  and partial-product carries through the final VCF DAC sum. The correction
  changes fine envelope/glide steps while preserving octave and full ENV
  endpoints; the velocity extension remains available.
- The chorus input coupling and passive low-pass branch now load each other
  in one capacitor solve, removing about 0.19 dB of excess wet midrange under
  the existing ideal bias/follower boundaries. Both quality paths are covered
  by independent node-admittance tests.
- The output jack's magnitude approximation now preserves its component-derived
  high-frequency attenuation at ordinary host rates. Existing sessions inherit
  the gentle treble correction; there are no new parameters or state-format
  changes. Its phase and external-load limitations remain documented in the
  [research assessment](../../README.md#juno-106-fidelity-research).
- The LFO delay re-arms on the firmware's running-voice mask rather than on
  the held keys. B-2 rebuilds that mask each pass as the gate bits, OR-ed
  with their previous value while HOLD is down, and re-arms on the first
  voice-on after a pass that found it clear. So a key played over
  pedal-sustained voices no longer restarts the delay, while a Solo Unison
  key-up, a POLY press and a key played while only a dropped key is held now
  do. Release tails still do not count, as before.
- The envelope hands attack over to decay on the pass that overshoots the
  peak, not the one that reaches it. The firmware tests only the sum's top
  two bits, and 0x3FFF divides exactly by the increments of attack bytes 64
  and 100, so those two settings used to start their decay one 4.2 ms pass
  early.
- The common VCA's control constant follows absolute temperature. NEC's
  −5.9 mV/dB for the µPC1252H2 is two thermal voltages per decibel — the
  translinear gain cell's own law, and the data book's gain-versus-control
  graph draws its −25/25/75 °C lines fanning about 0 dB — so a stored VCA
  LEVEL's decibels shrink as the jack board warms over the chassis law's
  accelerated 3-second time constant: a level reading −16.3 dB cold reads −15.5 dB at the asymptote,
  +4.7 dB reads +4.5 dB, and 0 dB never moves. Unit Character 0 holds the
  part at the data book's 25 °C condition.
- The reported pulse duty is now the duty the comparator walk solves. Each
  card's threshold is the shared PWM hold plus its own offset, and the duty
  helper clamped that sum back to the hold's 6 V ceiling: a card at the
  +2 % end of the ramp capacitor's class reported 50.98 % where the render
  holds exactly 50 %, and the mixer coupling was primed with 0.118 V of DC
  the comparator never carries. Unit Character 0 is unchanged.
- The shared noise source draws Gaussian deviates instead of uniform ones.
  Tr21 is an avalanche junction, whose statistics are Gaussian by the central
  limit theorem — the reading the TP8 crest-factor interpretation already
  assumed — while the uniform generator's crest factor moved with the
  internal rate through the C42/C41 shaping (about 3.1 at 44.1 kHz/1× against
  4.35 at 192 kHz/4× for the same RMS), so the peaks reaching the filter and
  amplifier pairs depended on the quality rung. The RMS, every level
  coordinate and the rate normalisation are unchanged; the noise sequence is
  new, so renders carrying NOISE are no longer bit-identical to earlier
  builds. On coarse grids the bilinear C41 pole's zero at Nyquist also thins
  the rail above its corner (about −1.2 dB at 10 kHz and −4 dB at 16 kHz on a
  48 kHz/1× grid), a numerical limitation of the HQ-off rungs now stated
  beside the code.
- The quality fade's block split, the reported-latency formula and the
  un-negotiated host-rate default are corrected: the fade now lands exactly
  on zero at the sample the split scheduled (it used to leave a float crumb
  at 8, 48, 192, 384 and 768 kHz and rebuild one host sample late), the
  formula credits each half-band stage with the 46 input samples its pair
  read imposes (the 4× rung sits at 40.5 host samples end to end; the report
  still rounds to 41 and no audio moves), and a 0 Hz or negative host rate
  takes the same 48 kHz default as a NaN instead of the 8 kHz floor.
- More space below the programmer header, consistent caption baselines,
  centred button icons and text, and readable monitor/help text when enlarged.
  Synth keys share two cap widths and one height, with darker faces and
  recessed green indicators; lower utility buttons use one size.
  ABOUT and HELP replace inactive keys; patch-file notices remain visible.
- Sixteen original bass and pad presets expand the bank to 144 sounds, with
  Basses, Brass, Strings and Pads menus. Existing factory program numbers are
  preserved.
- Later preset/session recalls supersede queued MIDI patch changes. SysEx
  import recovers a valid patch after an interrupted message. Session saves
  include MIDI program and tone changes before the panel timer catches up.
- The instrument is 2.5 dB louder. Digital full scale used to be the output
  summer's clipping asymptote and nothing else, which left ordinary patches
  22 dB below it and the plug-in far quieter than peer emulations. Full scale
  is now that ceiling less a 2.5 dB level policy, sized against the
  instrument's own headroom rather than the bank's: a six-voice chord with
  every source on and both VCA LEVEL and VOLUME at maximum peaks at
  −0.51 dBFS, and the loudest factory preset at −7.04. It is a post-clip
  output scalar, so nothing about the sound moves except its level.
- The sub oscillator is 3.6 dB louder against the saw. Its mixer coordinate
  was the one source level with no end-to-end anchor — the 106 sums saw, pulse
  and sub inside a potted custom chip, so no drawing gives the ratio — and two
  independent third-party Juno models both read it low by 2.0 and 3.6 dB. It
  now follows the one of them that models the 106 rather than the JUNO-6, and
  stays labelled voiced rather than promoted. Every patch carrying sub is
  fuller; patches with SUB at zero are unchanged. Eleven factory presets
  crossed the bank's loudness ceiling as a result and had their VR1 shaft
  trims lowered by the minimum each needed.
- The converter's 23 writes now land at the pixel-measured positions of the
  service manual's timing chart instead of evenly at `ordinal/23`. The order
  and the 4.2 ms pass are unchanged; what moves is where inside the pass each
  DCO, VCF and VCA write falls, so note onsets and the free-running DCO phase
  they catch shift by up to a fraction of a pass. Chosen by ear (a two-letter
  blind set, the listener preferring the chart geometry as slightly more
  resonant on fast stabs). The plug-in and the demo renderer select it before
  they prepare the engine, exactly as the VCF solver default is applied: the
  engine's own default stays the normalised grid so every frozen fingerprint
  and ordinal-gap fixture keeps testing the reference, and the normalised and
  phase-zero profiles remain selectable.
- The Boost position of the high-pass is now its own three-capacitor
  network instead of a collapsed shelf. Held still it matches the former
  shelf to 0.016 dB; what changes is every switch event that touches Boost.
  Leaving it, the stored C8 charge keeps flowing into IC4a while IC4b keeps
  amplifying it — a tail that starts near the shelf's own DC gain times the
  node voltage and dies within 30 ms — and entering it redistributes the bus
  step between C9 and C8. IC4b's ×11 low band also stops at the shared
  ±13.5 V summer-stage swing, so a loud unison 16' bass in Boost now
  compresses its lowest octave by about a decibel where the former shelf
  passed it untouched.
- The chorus button reaches the wet return through the drawn Tr5/C16/R48/
  C13/Tr4 gate drive, including bidirectional R48 loading: the wet mutes
  about 84.5 ms after CHORUS goes off and opens about 113.2 ms after it comes
  on with the declared junction prior. A patch loaded with the effect on
  still starts with it on. `enableChorusMuteDrive` restores the immediate
  switch for A/B renders.
- Under Unit Character the two chorus lines take a fixed-seed relative
  insertion offset of up to 1.6 dB inside the MN3009's ±4 dB row, split
  ± so the absolute wet level stays where it was; the L/R image is no longer
  perfectly symmetric at 100 %. Independent circuit fixtures also exercise
  identical returns to isolate insertion-gain effects.
- The DCO ramp dispersion sits inside the ±2 % G class the module drawing
  prints on C54 instead of a voiced 3 %, and each card's net pulse duty at
  the PWM trim point is drawn inside the 48–52 % window the service
  procedure accepts, with CH1 held at the trimmed 50 %: previously the
  threshold and ramp draws were independent and could leave a card outside
  the window Roland ships them inside. Unit Character 0 is unchanged.
- The switched high-pass no longer discards the leg it leaves. Selecting a new
  position steps as before, but the departing cut leg's capacitor keeps
  discharging into the same summing node, adding a soft low tail of −26.96 dB
  of its stored charge over 15.71 ms leaving position Two and 4.92 ms leaving
  Three. It fires on any program change whose HPF byte differs; with the
  selector held still, renders are bit-identical to before.

- NOISE level follows the drawn Tr22 grounded-base control law: silent
  through the first ~4 % of travel (stored bytes 0–5), linear above; the
  4 Vp-p full-level anchor and the C42/C41 shaping are unchanged. Comparison
  switch `useCircuitDerivedNoiseLevelShape` retains the linear-from-zero law.
- Universal macOS 11+ VST3, Audio Unit, CLAP and Standalone distribution,
  plus Windows x64 VST3, CLAP and Standalone. The macOS production release
  path supports a signed, notarized, stapled PKG carrying bundled licence
  and user documentation, a build manifest and SHA-256 checksums.
- The complete 128-program factory bank, host preset navigation, edited-state
  indication and exact reload behaviour; hardware-format `.syx` patch import,
  drag-and-drop and export; MIDI Program Change mapping, modulation, sustain,
  pitch bend and host-automatable synthesis controls.
- Model and performance extensions: Unit Character, Aging, velocity, variable
  polyphony, unison, master tune, transpose and chorus noise; contextual
  control help, live output display, panic, reset and patch randomisation.
- IC6's TA75558 output summer now carries its resistor-derived 527 kHz
  closed-loop pole and the Johnson noise of its feedback, dry and wet input
  legs; the following series resistor and loaded volume/output network add
  their own thermal floor after the coupling capacitor. The five new physical
  noise terms are deterministic, quality-invariant and remain around the
  analogue circuit's roughly -120 dBFS floor rather than becoming a sound-
  design hiss control.
- The resonance loop now applies one nonlinearity to the difference of the
  BA662 pair's two divided inputs, as the drawn circuit does, instead of a
  linear feedforward at the filter input plus a separate tanh on the feedback
  return. The two agree at small signal and at zero drive — so the 4.8 Vp-p
  self-oscillation trim and the loop-gain endpoint solved against it are
  untouched — and diverge where the pair actually compresses: −2.49 dB on a
  resonant pad, −1.56 dB on a stepped resonance ramp, and +3.65 dB on a
  self-oscillating chord carrying an oscillator, which previously stopped
  ringing where the circuit keeps going.
- Each voice card now has four independent resistor-noise sources, one at
  each OTA differential input, outside the external signal's resonance
  compensation. The original 68 kΩ/560 Ω networks give 3.02 nV/√Hz at the
  node, or 370 nV/√Hz referred through the divider. Independent per-stage
  transfer and total-power checks validate their different downstream
  filtering; the integrated 192 kHz/1 kHz result agrees within 0.04 dB with
  the continuous circuit integral. This replaces the previous stage-one-only
  approximation. It does not settle semiconductor noise or OQ-16's main
  NOISE calibration. An isolated five-pair benchmark at 48 kHz/4×, 64-frame
  blocks and chorus Off measures +13.36% median CPU for six voices and
  +4.68% idle, with all other current changes held fixed.
- The resonance input-side compensation moved off its former underived 0.2296
  to 0.2751, the floor of the bracket the two sibling readings of the network
  span. Resonance-0 renders are unchanged; the instrument keeps more bottom as
  RESONANCE opens, by +0.11 dB at panel 0.10, +0.56 dB at 0.40, +0.60 dB at
  0.55 and +2.75 dB on a resonant 0.85 pad, where the extra drive also feeds
  the peak. The former coefficient and Roland's own drawn 0.3078 both remain
  selectable for comparison renders.
- The resonance converter hold now steps at the write like the DCO and NOISE
  holds, because p. 13 draws no capacitor between IC22c and the card: the
  retired 522 µs was the first revision's single undifferentiated control
  slew and never had a network behind it. Static patches render bit-
  identically; a moving RESONANCE control lands each 7-bit step hard instead
  of gliding, measured at −9.2 dBc against a stepped sweep and −13.1 dBc
  against a slow one.
- The DCO pitch-CV and NOISE converter holds now use ideal acquisition at
  the write, replacing two
  voiced 522 µs compatibility slews. The former acquisition-time bound was
  withdrawn by the later DAC audit; the ideal-acquisition policy remains.
  No audible change (a sub-2 dB one-cycle
  ramp-amplitude transient at note-on; un-smoothed 0.07 dB noise-level
  steps).
- The sub now joins the mixer as the half-cycle current its R102/R101/D6 leg
  passes, so its mean rides on the WAVE node for C56/C50 to remove:
  steady-state level and spectrum are unchanged, and SUB level jumps (patch
  change, sysex, automation) produce the C56/C59-shaped bump the DC-coupled
  leg physically makes; `enableSubHalfWaveNodeCoupling` keeps the former
  zero-mean square for A/B renders.
- The common VCA's datasheet-typical output noise floor joins the bus ahead
  of the chorus, about −107.5 dBFS on the dry leg, deterministic and
  quality-invariant like the resistor floors.
- Replaced the chorus output's ideal-source, separable tap approximation with
  the documented loaded MN3009 network. Both BBD followers, their 3.3 kΩ legs,
  the 47 kΩ/2.2 nF tap and first reconstruction section now advance as one
  exact six-state nodal system; the relative change reaches −0.87 dB at 10 kHz
  and the HISS product normalization was remeasured on both HQ rate families.
- The voice VCA now carries the BA662 differential pair's saturation instead
  of a linear multiply. Its drive is derived from Roland's own p. 19 trims
  through the 47 kΩ output load the JUNO-6/60 CPU-board drawings print for
  the same discrete circuit, originally giving 11.06 V of headroom at the filter
  output (11.19 V after the later DAC buffer correction):
  −0.75 dB of compression with harmonics at −29 dBc on a full open-filter
  voice, −0.07 dB and −49 dBc on a filtered saw, and −0.10 dB at the
  self-oscillation trim. The former linear multiply remains bit-exact behind
  an internal comparison switch.
- Replaced instantaneous DCO timer programming with explicit M82C53 Mode-3
  OUT/count staging, the recovered µPD7810 control/LSB/MSB timing relative to
  each pitch-converter poll, and the synchronous IC35 range handoff. PIT count,
  reset and compensation-CV decisions now form one atomic firmware transaction
  beginning 97.25 us before that poll. This changes pitch-step, retrigger and
  live RANGE transients. RANGE changes the shared clock and selected ramp
  resistor without entering the compensation CV, so a range-only scan no
  longer fabricates a hold step. Semantic Voice On/Off now restart B-2's one
  shared converter loop, cancelling pre-DI CPU work but completing protected
  PIT bytes and preserving every external write already made. Mid-pass
  restarts repeat only the fresh loop's LFO-delay/onset calculation, not its
  later oscillator/PWM update; already-due boundaries retain their single
  normal pass step. ADC is proven unable to enter this region; physical mux
  timestamps, installed-NMOS entry timing and serial wire phase remain open
  under OQ-08, while the separate CPU/PIT oscillators rule out one fixed
  inter-clock phase.
- Replaced the ideal nearest-count pitch conversion and frequency-normalized
  ramp hold with the recovered B-2 unsigned 8.8 paired PIT/DCO-CV mechanism.
  Policy-safe derived anchors stay within 1.272 cents and one DAC code of the
  hash-matched image without distributing Roland's tables; exact truncation,
  clamps, upper-count discontinuity and CV saturation now drive the existing
  atomic pitch transaction and its audible high-note level taper. The shifted
  physical pitch grid also exposed a 44.1 kHz reconstruction transition
  corner; the same-size kernels now keep the expanded matrix at or below
  -71.62 dBc alias, with at most 0.0026 dB error through 15 kHz and 0.144 dB
  at the worst near-20 kHz line, without adding latency or hot-loop work.
- MASTER TUNE now uses the recovered signed B-2 byte in 1/256-semitone units:
  the continuous host control still spans +/-50 cents, while its hardware top
  code correctly stops at +49.609375 cents instead of fabricating +50 exactly.
- DCO pitch bend now uses the recovered assigner/B-2 integer path: the
  scan-held 14-bit wheel reduces to the hardware's signed command with its
  two-bin centre, and the sensitivity product tops out at +/-11.96484375
  semitones rather than an ideal continuous twelve. VCF bend now takes the
  same assigner command: the one-sided bend byte times the VCF sensitivity
  ADC, shifted right four times, so the filter shares the DCO's two-bin
  centre dead zone — where the old 255-step magnitude already added about
  16 counts — and keeps its 4064-count maximum.
- DCO LFO pitch now uses B-2's nonlinear 128-position depth law and exact
  truncating panel-delay/CC1 products. CC1 remains independent of the panel
  delay, the two paths saturate before a single scan-held pitch-word multiply,
  and combined vibrato tops out at +/-3.98046875 semitones instead of the old
  additive eight-semitone span.
- VCF LFO depth now uses B-2's own cutoff word. The doubled panel byte times
  the onset byte truncates to one depth byte before the accumulator multiply
  and single shift, so the low end of the LFO fader is coarser than a
  proportional law: panel byte 1 reaches 15 counts at the LFO peak (12 or 16
  on the converter's 4-count grid) where the old 4047/127 fraction gave 32,
  byte 2 reaches 47, and full depth keeps its 4047-count maximum.
- The LFO delay fade now begins on the same 4.2 ms converter pass that crosses
  the holdoff threshold, matching B-2 instead of inserting a silent extra pass;
  its exact state-completion range is 8.4 ms to 4.3512 s.
- PWM now follows B-2's raw LFO accumulator independently of LFO DELAY and
  uses the exact pass-latched partial-product/DAC code. The loaded hardware
  slider reaches the printed 50–95% range near byte 101, and the engine
  preserves the seven-bit overrange that can enter the comparator-pinning
  region (the plug-in's slider now stops at the pot's byte 105); the onset byte
  continues to gate DCO and VCF modulation only. Four affected plug-in-only
  factory VR1 positions were minimally lowered to retain the fixed loudness
  contract.
- Pulse Off now follows the MC5534A and module drawings: its −0.8 V control
  pins the running comparator high but leaves that constant level on the fixed
  WAVE node, where C56/C50 reject its DC. This removes the former digital
  mixer gate and makes off/on automation follow comparator phase and coupling
  history; the comparison renderer retains the old path for matched A/Bs.
- Promoted the resonance law derived from the traced grounded-base CV stage
  and BA662 linear-gm path, retaining the same service-calibrated
  self-oscillation endpoint and adding no DSP work.
- The resonance control onset now sits on the anchored +0.26 V VR34/TP7
  standoff the RES CV hold shares with the voice VCA rail: first loop gain at
  stored byte ~4 instead of ~8, about +4 dB at Resonance 1/10 and +0.3 dB at
  5/10, endpoint and self-oscillation unchanged.
- The voice VCA's envelope-to-gain law now solves the traced Tr20
  grounded-base stage exactly instead of a softplus stand-in, from the
  resistors and standoff already in tree; sustain levels and full-scale
  loudness are unchanged, the bottom 45 dB of every envelope sits up to
  2.5 dB lower, so releases close slightly sooner (the same direction as the
  withdrawn wide voiced knee, by a derived and roughly five times smaller
  amount). The former softplus comparison has since been retired.
- Corrected the MN3009 transfer fit to follow its typical distortion curve
  instead of treating a guaranteed input-swing limit as a typical point.
  HISS 100% retains the established conservative normalization and the 29.86%
  default remains session-compatible; later audit separated that post-board
  policy from Panasonic's part-output maximum and withdrew the invalid
  input-swing/S/N derivation. Eight plug-in-only factory VR1 trims were
  minimally lowered to keep all 128 tones inside the established loudness
  contract.
- A three-level 1×/2×/4× QUALITY control with fixed reported latency; new
  instances default to 1× for lower CPU use and Aging at 50%, while saved
  sessions retain their stored choices. 2×/4× remain available to reduce aliasing.
- Disabled the unmeasured C14 voltage-coefficient candidate by default. Its
  internal comparison switch is not serialized, so restored sessions also use
  the evidence-conservative default; the opt-in comparison renderer retains it
  pending a level-swept installed-part measurement under OQ-21.
- Added a VCF SOLVER control beside QUALITY, choosing how much arithmetic the
  filter's solver spends per internal sample. New instances use Normal, which
  roughly halves whole-engine CPU; High and Max cost more, and Max is what
  every earlier release ran. All three keep the same filter, resonance
  calibration and self-oscillation, a blind listening test found no audible
  difference between them, and the setting applies immediately without waiting
  for an idle instrument or adding latency.
- Roughly halved whole-instrument CPU again and cut an idle instance by about
  ninety percent: the host-visible VCF Tanh selector gained a `Poly` kernel
  (worst-case transfer error 4.3e-7, self-oscillation amplitude and pitch
  preserved to six decimals and 0.003 cents) that also lets fully silent voice
  cards and a settled, switched-off chorus skip work they cannot make audible.
  New instances start on it with the `Cubic` Fast Early form; selecting
  `Exact` restores the previous engine behaviour exactly, sessions that chose
  a kernel keep it, and sessions from before the selectors existed still
  restore the exact forms.
- Reduced per-sample work in the chorus and the voice loop with no change to
  the audio: the demonstration renders are bit-identical.
- Batched adjacent settled voice filters taking one full RK4 step in the FP64
  SIMD lanes. At 48 kHz/4x, the faster of the two bracketing baselines versus
  the candidate cut median thread CPU by 21.61% to 23.91% across the six-voice
  low-cutoff, plain and resonant dry audits; every arm64 raw-float audio
  fingerprint stayed bit-identical, as did the complete scalar/SIMD cascade
  state on arm64 and x86_64. Extending that same path to the ten optional
  product-extension slots cut the 16-voice full-mixer/Chorus-II audit from the
  faster 433.047 ms bracketing baseline to 339.764 ms at 48 kHz/4x, a 21.54%
  median thread-CPU reduction with the raw-float fingerprint still identical.
- Batched the settled filters that Normal must escalate to the two-half-step
  Merson solve in those same FP64 lanes. At 48 kHz, conservative A-B-A medians
  fell from 486.740 to 353.439 ms for six voices/2x (27.39%), from 251.685 to
  187.959 ms for six voices/1x (25.32%), and from 1223.575 to 879.607 ms for
  sixteen voices/2x (28.11%). Raw-float fingerprints and complete cascade state
  remain bit-identical on arm64 and x86_64; the existing 4x RK4 path's control
  run was 0.60% slower with the same fingerprint.
- Preserved historical Audio Unit parameter ordering while adding QUALITY, and
  moved quality-dependent chorus coefficient work out of the audio callback.
- Kept the modelled instrument warm across a transport stop, so the chassis
  warm-up runs as intended instead of restarting whenever playback stops, and
  retired the voice lamps, meters and oscilloscope whenever the host bypasses
  or deactivates the instrument.
- Fixed Chorus I and II so host automation of one no longer writes the other,
  which previously made the rendered chorus mode depend on whether the plug-in
  window was open.
- Restored I+II as a distinct live and session-persistent chorus state with its
  own fourth panel button. Hardware-format patch files intentionally save it as
  II because the original 18-byte tone memory has no fourth chorus code.
- Fixed sustain (CC 64) to latch on any non-zero value instead of only at 64
  and above, matching the original's MIDI implementation chart, which prints
  hold off at zero and hold on for 1–127. A half-pressed pedal now holds.
- Fixed the channel-mode messages CC 124–127 so they release the keyboard like
  all-notes-off, which the same chart says the instrument does with every mode
  message from 123 up. They were previously ignored, leaving notes hanging in
  hosts that end playback with an omni or poly message rather than CC 123.
- Fixed the pitch-bend lever so holding one axis with the keyboard survives
  releasing the other.
- Fixed a one-sample spike in the oscillator reconstruction for an edge found
  exactly on the previous sample's instant. Each waveform track repairs its
  discontinuities from a bandlimited step response, and an event one whole
  sample back subtracted the ideal step from the sample rendered before it,
  leaving that sample at the full opposite swing (−2.0 for a −1..+1 edge
  whose own pre-ring reaches 1.18). It reached the WAVE node whenever a
  card's stored comparator level was reconciled at an interval boundary — a
  retrigger after the fast tanh modes' idle-card freewheel with the Pulse
  Off WAVE-node coupling comparison switch off — when a ramp was found
  already at the +15 V supply bound at the start of an interval after a
  Unit Character edit, and on the control-word sub flip of a note command's
  protected reset pre-stage. Every edge inside a sample is unchanged.
- Fixed the settled-chorus work skip so it engages without the host's
  flush-to-zero mode. The 5 ms wet-mute glide only ever decayed towards zero
  and parked on a denormal, so the skip's exact-zero test passed under the
  plug-in's own denormal guard and never in the JUCE-free renderers and
  audits. The glide now lands on zero at FLT_MIN, the same threshold that
  guard flushes at; the plug-in's output is unchanged.
- Panel and layout fixes: text that could truncate (monitor voice readout,
  PANIC key, voice-lamp numbers above nine voices), dimmed disabled keys, knob
  tick marks kept inside their controls at large editor sizes, help text that
  could be cut short, key legends squeezed at the smallest window size, and
  three VOICE MODE keys that sat a few pixels above the programmer keys beside
  them. The factory/custom patch rail was clarified and the oscillator
  controls widened for cleaner alignment at every supported editor size.
- Hardened host operation across transport reset, standard bypass, variable
  block sizes, concurrent state saves and synchronous state-save callbacks.
- Fixed the POLY 1, POLY 2 and UNISON keys so one press writes both assign
  parameters as one transaction, as the chorus keys already did. A host that
  saves state from the first parameter notification captured the contact
  between the two writes: pressing POLY 1 from Poly 2 saved a both-off pair
  that restores as Poly 1, and pressing one key from Unison saved the single
  mode, so the session lost the mode it was in when the save was requested.
- A non-finite control value now exports as tone byte zero on every platform.
  `std::clamp` passes NaN through and rounding it is unspecified, so the
  byte a patch file or SysEx dump carried for such a control depended on the
  machine that wrote it.
- Finalized the Protocodus bundle and package identifiers for v1 and
  documented migration from the earlier public Pluto Audio nightly builds.
- A mono host output bus is accepted and carries what the L/MONO jack does:
  the mean of the two channels, which is what the jack board's normalling
  contacts deliver with one plug inserted, never line 1 alone. Chorus Off is
  unchanged on it; I and II fold their two lines to the exact mid, as I+II
  already did.
- The output jack network — R64/R65 with C21/C22 — is modelled as one pole
  per channel at the host rate: 46.1 kHz at full volume, 33.3 kHz at half.
  At 44.1/48 kHz hosts it is nearly transparent (−0.04 dB at 20 kHz); at
  96/192 kHz the top of the band rolls off by 0.33/0.61 dB at 20 kHz, as the
  instrument's own does. A mono bus leaves its time constant unchanged.
