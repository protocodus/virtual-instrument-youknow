# Hardware capture protocol

2026-11-16. The sessions that close what the
[known-gaps research](../research/known-gaps-research-2026-11-16/youknow-known-gaps-2026-11-16-combined.md)
(section 4) and the
[analysis of the captures already in hand](../research/existing-capture-analysis-2026-11-16.md)
leave open. Each step names the open question it closes, what to record and
what result would decide it. Sessions A and B use the reference unit #439522
([recordings register](../research/hardware-recordings.md)); session C needs
original voice cards.

Session B opens the case. Mains voltages are exposed on the power supply
board: it is for a qualified technician, with the unit on an isolating
transformer, and nothing in it changes a trimmer unless a service adjustment
is needed. If one is, record the before and after.

## Rules for every take

- One capture chain at one fixed gain for the whole session, no per-patch EQ
  or normalisation. Note the interface, its rate (96 kHz or more; 192 kHz for
  chorus and idle takes), cables and which output jack and selector position.
- Drive the unit by SysEx and MIDI only, from files kept beside the takes,
  never by hand on the panel. Hash every take and MIDI file (SHA-256) and add
  them to the recordings register.
- Note the unit: serial, card types and revisions in each slot (original
  80017A or which replacement), firmware revision, time since power-on and
  room temperature at the start and end.
- Fit terminal behaviour first. Separate harmonics, broadband noise, hum and
  clock components before comparing with the engine. Keep every second take
  of a sweep out of any fit and score the result on it.
- Render the same MIDI through the engine by one protocol, as
  `Tools/AnalyzeExistingCaptures.py` does, so estimator bias is shared.

## Session A: audio only, case closed

1. **Chain floor.** Thirty seconds each with the interface input shorted, the
   synth connected and switched off, and the synth on with VOLUME at zero.
   This separates the chain's hum and floor from the instrument's. The
   existing takes found a 120 Hz line 69–73 dB under a C4 note with the
   chorus on and none with it off, from one take with no chain floor; this
   take confirms the line is the instrument's.
2. **Idle chorus at 192 kHz.** Two minutes each in chorus Off, I and II, all
   sources off, RES 0, VCA in GATE with no note held. Gives the 60–360 Hz
   lines per state, left/right coherence and the MN3101 clock residue above
   20 kHz, which would read the clock without opening the case (OQ-01, OQ-03).
3. **Dry source matrix (OQ-15, OQ-11).** Exact SysEx tones at C2, C4 and C6:
   pulse only with SUB off, saw only, SUB only, saw plus SUB at SUB 25, 50, 75
   and 100 %, and pulse-off transitions while a note holds. Chorus off, VCF
   open, RES 0. The two shipped WAVE-node roots, about 4.98 kΩ and 95.7 kΩ,
   predict different mixes here; the one that predicts the partial-SUB mixes
   within a dB stays.
4. **Cold-start pitch (OQ-10, the DCO proxy gap).** From a unit left off
   overnight: power on and start at once. One held C4 DCO note and one noise
   note, each 5 s, every 30 s for an hour, with the room temperature logged
   every five minutes. The existing session started warm and showed no drift
   (0.012 ± 0.013 cents per minute); this one sees the warm-up the proxy
   models, about one cent over its 3 s.
5. **Self-oscillation through the VCA and chorus (OQ-02, OQ-19, OQ-03).** RES
   127, all sources off, a held note: a VCA LEVEL sweep in 16 steps, then the
   same tone at three VCA LEVELs through chorus I. Gives the common VCA's
   tracking and the BBD's level dependence.
6. **Noise levels (OQ-16).** Noise only at NOISE 0, 32, 64, 96 and 127, 30 s
   each, with the VCF open and RES 0. The existing takes confirm a Gaussian
   source; the capture's kurtosis fell slightly with level (−0.065 to
   −0.122), and these levels say whether that is compression after the source.
7. **Switching.** Chorus Off/I/II and each HPF position stepped under a
   steady tone, ten times each (OQ-20, OQ-21). Twenty repeated
   self-oscillation starts from RES 0 to 127 (OQ-16).
8. **Output loads (OQ-05, OQ-06, OQ-17).** A 1 kHz sine and a self-oscillating
   tone into the interface's line input, then into 10 kΩ, 2 kΩ and 600 Ω
   loads across the jack, at each output selector position (L, M, H) and at
   VOLUME 0, 25, 50, 75 and 100 %. Note each load's measured value.

## Session B: bench, case open

1. **WAVE node DC and shunt (OQ-15, OQ-11).** On one voice card, DC at the
   WAVE node with each source alone (saw, pulse, SUB) and all off; then the
   saw's AC level open and with a 100 kΩ shunt to ground. The research
   computes −0.42 dB for one shipped root and −5.83 dB for the other, so one
   reading picks the root. If time allows, a small controlled load across
   signal states shows whether one resistor describes the node at all.
2. **Chorus clock and wet path (OQ-01, OQ-03, OQ-04).** In I and II, capture
   together on a multichannel scope or interface: the MN3101 clock (minimum
   and maximum frequency over a sweep), the TP4 triangle, the chorus input
   and both wet returns before the summer. The delay law follows from the
   clock and replaces the recording-derived Mode-I fit. Leave the bias
   trimmers as found.
3. **Where the chorus hum enters.** The existing takes place the chorus-on
   120 Hz line at the delay lines, from its low left/right coherence and
   wandering phase. With the unit idle in chorus I, record the 120 Hz
   component at the MN3009 supply pins, the bias network, the clock line and
   each wet return. The point where it first appears is its coupling path,
   which a chorus-path ripple model would need.
4. **Rails.** AC ripple at the voice-card connector and at the MN3009 pins,
   idle, with six voices held and across note on/off, captured in sync with
   the audio output. Supply current if a probe is available. The capture
   bounds 120 Hz cutoff modulation to under 0.1 cents; this measures the
   ripple the shipped 0.003-cent prior stands for.
5. **The VCF hold during a converter pass (OQ-07, OQ-08).** A scope on one
   card's VCF cutoff hold capacitor and the mux enable, with a steady
   self-oscillating tone: the step at each write and the slope between
   writes. The existing takes show a sawtooth-like cutoff FM at the pass
   rate, 0.19–0.20 cents peak, about 40 times anything the engine makes
   there; a step at the write means charge injection, a steep slope means
   leakage well above typical. Also read the pass period directly: the
   sidebands put it at 4.312–4.322 ms with one card sounding.
6. **The 8 MHz reference.** On a counter, from cold, logged with the session
   A cold-start pitch.
7. **Trimmers.** With power off, the resistance of VR28, VR32 and VR27 as
   found.

## Session C: original cards

The session A suite and `res_calibrate.mid`'s dense resonance sweep at
several cutoffs (OQ-09, OQ-18, OQ-19), on a unit with original 80017A cards,
ideally with original and replacement cards in the same unit so one chain
measures both. Alternate resonance bytes are held out of any fit. Note each
card's history. Then the same suite on a second unit, held out: one unit
establishes that unit, not the population.
