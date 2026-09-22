#include "YouKnowEngine.h"
#include "YouKnowVcaControl.h"
#include "YouKnowReferenceVcf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__x86_64__) && defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(YOUKNOW_WORK_AUDIT)
#include "../../Tools/OversamplingAuditSupport.h"

#define YOUKNOW_COUNT_DOMAIN_WORK(field, amount)                         \
    do                                                                      \
    {                                                                       \
        if (auto* counters =                                                \
                youknow::oversampling_audit::activeDomainWorkCounters)   \
            counters->field += (amount);                                    \
    } while (false)
#endif

namespace youknow
{
namespace
{
constexpr float pi = 3.14159265358979323846f;
constexpr float twoPi = 6.28318530717958647692f;

// RANGE drives both IC35's clock preset and IC2/6/10's analogue mux.
// C54's charging resistor changes at the PF write, independently of the
// synchronous clock's later reload. Roland prints R85/87/86 = 399/200/100 kOhm:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13
// The p.9 Miller-integrator description gives dV/dt = I/C, I proportional
// to 1/R. This nominal ideal-switch relationship does not assign unmeasured
// mux charge injection or custom-IC reset/saturation characteristics.
constexpr double dcoChargingResistance(DcoRange range) noexcept
{
    switch (range)
    {
        case DcoRange::Sixteen: return 399000.0;
        case DcoRange::Four:    return 100000.0;
        case DcoRange::Eight:
        default:               return 200000.0;
    }
}


// Signal levels use the established 2.6 V-per-unit model coordinate so the
// transconductor and BBD nonlinearities retain their existing drive. The
// service notes' 12 Vpp oscillator adjustment and 4 Vpp TP8 noise adjustment
// do not by themselves establish the complete loaded source-to-filter transfer;
// the 0.40 mixer coordinate remains a voiced compatibility value (OQ-15).
constexpr float filterInputAttenuation = 0.40f;
constexpr float voltsToSample = 1.0f / YouKnowEngine::internalVoltsPerUnit;

// Voiced source-coordinate values. Saw and pulse are constrained by the service
// anchor near 12 Vpp; intervening loading is still open, and the sub value has
// no equivalent end-to-end anchor, so none of these may be presented as
// measured mixer voltages.
//
// A sibling does print the end-to-end figure. Roland's MKS-7 Service Notes
// (Jul 1985) test the same MC5534A/80017A voice, with the same 27k/33k/diode
// sub leg and 10 uF coupling, at its VCA output against its own 6.0 Vpp
// VCA-gain sine: saw 4.8 Vpp +/-0.5 V at every range and key (p. 10 items
// 10-11), sub 1.5/3.5/5.5 Vpp over its four steps, and pulse/saw 0.79-0.83 at
// the mix output (p. 11 items 13, 15, 16). Read the same way -- 20 kHz-band
// peak-to-peak, self-oscillation scaled to 6 Vpp -- these coordinates put the
// nominal saw at 6.50 Vpp, and the identified unit's May isolator take reads
// 4.88: the filter and VCA pair are driven about 2.6 dB harder than that
// factory window. Sub/saw agrees with both within 0.3 dB (RMS); pulse/saw
// sits 1-2.5 dB above them. The owner chose x0.738 on all three by ear
// (2026-09-22); the product applies it as configureOscillatorLevelScale, so
// these raw coordinates stay the reference the fingerprints freeze (OQ-15).
// Through that product, #439522's take reads the 50% pulse 1.34 dB hot
// against the saw and the sub 0.24 dB light (AnalyzeHardwareIsolators.py).
// The MKS-7's 0.81 nominal would read the pulse 0.46 dB light, but its
// printed tolerances span 0.64-1.05, so it cannot overrule the shared
// coordinate on its own. A x0.857 pulse leg (the #439522 match) and a x0.81
// one await a listening choice (2026-09-22).
// https://www.polynominal.com/roland-mks7/Roland-MKS-7-Service-Notes.pdf#page=10
// (SHA-256 179234b24c20b5a3a010827e5606cf6d9744bb9585664e219d6b643e2c7eb8ae)
constexpr float sawMixVolts = 6.0f;
constexpr float pulseMixVolts = 6.0f;
// subMixVolts lives on the class (YouKnowEngine::subMixVolts) so the
// DCO-scan audit's independent Fourier reference can read it instead of
// restating it as a literal. Its history and evidence class are recorded
// there.
// The noise coordinate names the SHAPED rail, not the raw generator, and that
// distinction is the whole of it. The service procedure adjusts VR32 for 4 Vpp
// at TP8 -- the CH1 voice VCA output -- and a +/-2 V figure written onto the
// source *ahead* of the shaping below does not arrive there as 4 Vpp, because
// the 4.82 kHz pole keeps only 7.27% of a white source's power (-11.383 dB).
// A previous revision made exactly that substitution and left the audible noise
// 11.38 dB light: referred to the model's own calibrated 4.8 Vpp
// self-oscillation it measured -23.35 dB where the paired TP8 figures put it
// between -8.5 and -12.6 dB, the spread being what crest convention a scope
// trace of random noise is read with.
//
// 2.0 / sqrt(0.0727330) = 7.4161 restores precisely what the shaping discards,
// so the same +/-2 V now describes the rail the adjustment measures. It is a
// mechanical correction of the misplacement rather than a fit, which is why it
// assumes no crest convention; it lands at -11.96 dB against self-oscillation,
// inside the anchored band at its conservative end.
//
// Raising it required the output boundary to be referred to the summer model's
// provisional asymptote first (see outputBoundaryGain). Without that, a
// six-note NOISE-10 chord peaks
// at +1.97 dBFS: one shared generator sums coherently across held voices, at
// 20*log10(N), so noise chords reach full scale far sooner than oscillator
// chords do. That is what "high Noise settings sound broken" was.
//
// What stays open is the absolute source-coordinate boundary, not the ordered
// topology below: the anchors fix the product of this constant and
// filterInputAttenuation, and only the coincidence between the deficit and the
// shaping loss says the noise leg alone was light. OQ-15/OQ-16.
//
// The crest convention is documented after all, and it is not the one this
// value lands on. p. 18's test program runs bank 6 (NOISE LEVEL) at FREQ 10,
// RES 0, KYBD 10, VCA ENV, S 10 -- the per-voice VCA state of bank 3's 6 Vpp
// trim -- so the two TP8 figures pair directly. p. 19's own figure draws the
// 4 Vpp bracket on the trace's dense core with faint excursions reaching 1.91x
// it (600 dpi: bracket 90 px, drawn extent 172 px; the ink-density FWHM puts
// the bracket at 2.9 sigma, and one 10 ms sweep of this 7.6 kHz-bandwidth noise
// spans about 5.3 sigma, 5.3 / 1.91 = 2.8). This value reads 4 Vpp as the whole
// trace, 6.6 sigma: sigma 0.60 V against the 248 Hz self-oscillation in the
// product profile. The identified unit reads 2.88 sigma (noise/self-oscillation
// -3.63 dB, 20 Hz-20 kHz, hash-verified May isolator take). The same take's
// noise band shape follows the C41/R79 pole within 1.5 dB to 20 kHz, and the
// separately printed HS-60/JUNO-106S notes, whose p. 3 lists this module board
// (76139170) as common to both, repeat C41 100p, R79 330k, C42 1u and R81 4.7k
// on p. 15, so the gap is level, not spectrum. A core-band reading raises this
// constant 5.6-7.2 dB (3.45 or 2.9 sigma). The owner chose 2.9 sigma by ear
// (2026-09-22); the product applies it as the CoreBandTp8 noise profile, and
// this raw value stays the reference the fingerprints freeze.
// https://seriescircuits.com/wp-content/uploads/2024/07/Roland-Juno-106S-HS-60-Service-Manual.pdf#page=15
// (SHA-256 55118ea03a995eead22977e2ba5185b6971a5d7fcb1074e1e202ec706f3585eb)
constexpr float noiseMixVolts = 7.4161f;

// The noise generator's support circuit, module board p. 13: Tr21 (2SC945,
// factory-selected for noise) with R104 470 kOhm EMITTER load -- its base and
// collector are tied to ground and its emitter-base junction is reverse-biased
// through R104 from +15 V, an E-B avalanche source -- with C42 1 uF taken from
// that emitter node into the BA662 level OTA whose input pin sits on a 4.7 kOhm
// bias resistor -- a 33.9 Hz high-pass -- and whose output is loaded by
// C41 100 pF against R79 330 kOhm -- a 4.82 kHz pole -- before the buffered
// NOISE rail.
// The audible source is therefore band-shaped by its own circuit, not flat.
// The shaping passes its passband at unity, so it does not change in-band
// density -- but it does change total power: the 4.82 kHz pole keeps only
// 6982.4 Hz of noise-equivalent bandwidth out of the 96 kHz the source is
// white across at the 192 kHz internal rate, which is 7.3% of its power, or
// -11.38 dB of RMS. That loss is why the pre-filter coordinate above cannot be
// read as the TP8 figure (OQ-16).
constexpr float noiseCouplingCapacitanceF = 1.0e-6f;      // C42
constexpr float noiseCouplingLoadOhms = 4700.0f;          // R81, IC14 input
constexpr float noiseOtaLoadCapacitanceF = 100.0e-12f;    // C41
constexpr float noiseOtaLoadResistanceOhms = 330000.0f;   // R79

// Voice-summer output coupling ahead of the four-position HPF selector:
// IC1a -> C14 10 uF NP -> IC3 common input, with R39 33 kOhm from that
// common node to ground. The selected Cut leg also leaves its mux-side
// 1 MOhm bleed connected directly to the common node.
constexpr float voiceBusCouplingCapacitanceF = 10.0e-6f;
constexpr float voiceBusCouplingResistanceOhms = 33000.0f;
constexpr float highPassCutBleedResistanceOhms = 1000000.0f; // R21 / R23
// How much of a departed cut leg reaches IC4a, and how much slower it decays.
// Both are the same ratio of two p. 15 resistances: with the mux open the
// leg's timing resistance becomes R23+R28 (= R21+R26) instead of R28 alone, so
// its corner falls by R29/(R23+R28) -- and because R29 equals R28, that same
// figure is the output-referred fraction of the stored capacitor voltage the
// leg delivers through its own 47 kOhm. 47/1047 = 0.0448902, or -26.96 dB.
constexpr float highPassDepartRatio =
    47000.0f / (highPassCutBleedResistanceOhms + 47000.0f);
// The Boost leg's own parts, jack board p. 15 (read at designator level from
// the 300 dpi scan): the C9||R22 link from Y3 to node N, C8 from N to ground,
// IC4b's R18/C6 over R19, and the two returns into IC4a's R29 summing node.
constexpr double boostLinkOhms = 47.0e3;        // R22
constexpr double boostLinkFarads = 47.0e-9;     // C9
constexpr double boostShuntFarads = 10.0e-9;    // C8
constexpr double boostSumOhms = 47.0e3;         // R25 (Y3 into the node)
constexpr double boostFeedbackOhms = 100.0e3;   // R18
constexpr double boostFeedbackFarads = 22.0e-9; // C6
constexpr double boostGroundOhms = 10.0e3;      // R19
constexpr double boostReturnOhms = 220.0e3;     // R24 (IC4b into the node)
constexpr double boostSummerOhms = 47.0e3;      // R29
// Fraction of a Y3 step that C9 in series with C8 hands to node N, and the
// gains the two returns carry into IC4a's output.
constexpr double boostLinkShare =
    boostLinkFarads / (boostLinkFarads + boostShuntFarads);
constexpr double boostDirectGain = boostSummerOhms / boostSumOhms;   // 1
constexpr double boostReturnGain = boostSummerOhms / boostReturnOhms; // 0.2136
constexpr double boostAmplifierDcGain = 1.0 + boostFeedbackOhms / boostGroundOhms;

// Per-voice module-input coupling, module board p. 13: the summed WAVE node
// reaches the voice module's pin 1 VCF IN only through C56/C50 10 uF NP. The
// topology is settled -- the 2026-08-07 designator read lists this capacitor
// with the rest of the mixer node. It rejects settled mixer DC; changes in
// the node's mean still pass as decaying transients into the nonlinear filter.
//
// The capacitor is the read part; the resistance it works against is not.
// R99/R102 33 kOhm is *not* this pole's load -- the 2026-08-20 p. 13 junction
// read re-roles it as the sub switch transistor's collector load returning to
// the shared SUB LEVEL rail, retiring the 2026-08-07 "bridge across the diode"
// reading, so the sub's conducting path reaches the WAVE line through 60 kOhm
// (R101/R97 27 kOhm behind D6/D5, then this 33 kOhm) -- and the node's
// termination, together with the WAVE output's
// source impedance, is exactly OQ-15's remaining measurement. 33 kOhm is
// therefore a voiced stand-in, taken by analogy with the two settled
// 10 uF NP / 33 kOhm couplings downstream (C14/R39 and C12/R36). A
// hypothetical 10-100 kOhm total resistance puts the corner between 0.16
// and 1.6 Hz. That is below the keybed, but changes the settling transient;
// it does not establish the installed time constant or its audible effect.
// Nor is 68k + 560 ohms the pin-1 load: that stage attenuator omits the
// hybrid's 4.7k summing and 24k/1.5k resonance-input paths (see the
// resonance-compensation derivation). Do not infer a C56 pole from it alone.
constexpr float moduleCouplingCapacitanceF = 10.0e-6f;      // C56 / C50
constexpr float moduleCouplingResistanceOhms = 33000.0f;    // legacy raw reference; product selects hybrid load

// Per-voice coupling out of the filter and into the amplifier, module board
// pp. 18-19: pin 3 VCF OUT reaches pin 9 VCA IN only through C59 1 uF/50 V NP
// and the VR27/R108 network. The capacitor is anchored -- it is in the voice
// module's VCA row of the research contract and in OQ-19's own topology
// listing -- and Roland's service procedure trims VR30/25/20/15/10/5 through
// R112 2.2 MOhm at this same node for minimum thump, which is the factory
// saying in a procedure that pin 9 is meant to sit at zero.
//
// Page 13 prints R108 82 kOhm in series with VR27 50 kOhm, wired as a
// rheostat. Both sit AFTER C59 and BEFORE pin 9; the R112 2.2 MOhm offset
// injection joins at pin 9. R108 alone therefore supplies the minimum load
// resistance for this nominal RC model. The old 33 kOhm analogy to C14/R39
// was below that physical minimum and rejected too much bass.
//
// Use the conservative 82 kOhm limit, hence an 82 ms minimum time constant
// and 1.941 Hz maximum corner for the nominal 1 uF part. The original-module
// reading now corroborates the internal 4.7k series/560-ohm shunt network
// (Sound Doctorin, cited by VoiceVcaSignalLaw), but VR27's setting and finite
// module/buffer impedances remain unresolved (OQ-19). Their nonnegative
// contributions lower the corner further. This remains a conservative bound,
// not an exact installed pole or a new gain trim. Capacitor tolerance is not
// inferred from the nominal value.
constexpr float vcaInputCouplingCapacitanceF = 1.0e-6f;     // C59
constexpr float vcaInputCouplingResistanceOhms = 82000.0f;  // R108 minimum, OQ-19

// Manufacturer application input for IC5/uPC1252H2, populated by Roland as
// C12 10 uF NP followed by R36 33 kOhm.
constexpr float commonVcaInputCapacitanceF = 10.0e-6f;
constexpr float commonVcaInputResistanceOhms = 33000.0f;

// NEC 1983 consumer-IC data book, uPC1252H2 electrical characteristics
// (p. 257; https://archive.org/download/bitsavers_necdataBooCircuitsforConsumerUse_42422169/1983_NEC_Integrated_Circuits_for_Consumer_Use.pdf#page=262): Output Noise Level NV typ -94 dBV, max -84 dBV, at Av = 0 dB,
// RIN = 33 kOhm, BPF 10 Hz-20 kHz, Vcc/Vee +/-12 V, ISET 2 mA, RIN = ROUT =
// 33 kOhm -- measured at the external 33 kOhm I/V output. Roland's IC5
// wiring on p. 15 is that test circuit: C12 10 uF / R36 33 kOhm in, R34
// 5.6K + R35 680 from pin 5 to -15 V so ISET = (15 - 2.4)/6.28k = 2.006 mA,
// pin 8 into IC2b's 33 kOhm I/V (R16, C5 22p), and +15 V through R17 1.5K
// (about +12 V after the 2 mA supply drop). NEC publishes NV only at
// Av = 0 dB, so it is applied as a constant output-referred floor across
// the installed -16.3..+4.7 dB VCA LEVEL span -- likely slightly high below
// 0 dB. The 33 kOhm resistors' own thermal floor (4.66 uVrms in that band)
// sits 12 dB under the part's figure and is not added separately.
constexpr float commonVcaOutputNoiseDbv = -94.0f;
constexpr float commonVcaOutputNoiseBandwidthHz = 19990.0f;

// Stored VCA LEVEL control path on the jack board. The firmware stores byte b
// as physical 12-bit code b<<5. Page 15 shows R30/C7 at the held node, R32
// into IC5 GC1, R31 to ground and R165 to +15 V. The DAC uses the usual ideal
// 4096-step R-2R convention; the largest reachable stored code is 4064.
constexpr float commonVcaDacReferenceVolts = 5.0f;
constexpr float commonVcaDacSteps = 4096.0f;
constexpr float commonVcaMaximumDacCode = 4064.0f;
// Page 8 rounds IC28a's span to +4..-6 V; p. 13 gives its actual nominal
// summing network: R130 4.99k from TP4, R131 10k feedback and R129 39k from
// -15 V. With the noninverting input grounded, KCL gives
// Vout = 15*(10k/39k) - Vdac*(10k/4.99k). There is no trim on this buffer.
// This common-VCA path uses those component values; the VCF's service fit
// and PWM's independently calibrated endpoints retain their coordinates.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13
constexpr float commonVcaBufferOffsetVolts = 15.0f * 10000.0f / 39000.0f;
constexpr float commonVcaBufferGain = -10000.0f / 4990.0f;
constexpr float commonVcaR30Ohms = 2200.0f;
constexpr float commonVcaR32Ohms = 1500.0f;
constexpr float commonVcaR31Ohms = 47.0f;
constexpr float commonVcaR165Ohms = 15000.0f;
constexpr float commonVcaBiasVolts = 15.0f;
constexpr float commonVcaC7Farads = 10.0e-6f;
// NEC 1983 data book p. 257: Vc = -5.9 mV/dB typical (-5.8 to -6.1) at
// Ta = 25 C over Av = -30 to +30 dB. That is two thermal voltages per decibel,
// 2 (kT/q) ln(10) / 20 = 5.916 mV/dB at 298.15 K -- the translinear gain-cell
// law -- and p. 260's "voltage gain vs control constant voltage" graph draws
// its Ta = -25/25/75 C lines fanning about 0 dB, the -25 C line steepest, so
// the constant is proportional to absolute temperature: this figure times
// T / 298.15 K (patchLevelGain).
// https://archive.org/download/bitsavers_necdataBooCircuitsforConsumerUse_42422169/1983_NEC_Integrated_Circuits_for_Consumer_Use.pdf#page=262
constexpr float commonVcaControlVoltsPerDecibel = -5.9e-3f;

// Stereo post-IC6 coupling, identically C17/R54/VR1 and C20/R57/VR1. The fixed
// internal load is the complete selector ladder in parallel with IC7's input;
// external jack loads and driven headphone behavior remain OQ-17, and the mono
// normaling is the host bus's fold (PluginProcessor.cpp, monoJackFoldGain).
constexpr float outputCouplingCapacitanceF = 10.0e-6f;
constexpr float outputCouplingSeriesOhms = 1500.0f;
constexpr float outputCouplingPotOhms = 10000.0f;
constexpr float outputSelectorLadderOhms = 33000.0f + 6800.0f + 1500.0f;
constexpr float headphoneInputOhms = 1000.0f + 100000.0f;
constexpr float outputWiperInternalLoadOhms =
    outputSelectorLadderOhms * headphoneInputOhms
    / (outputSelectorLadderOhms + headphoneInputOhms);

// The jack network after the selector, identically R64/C22 into JA2 and
// R65/C21 into JA1: 2.2 kOhm in series with each jack and 1 nF (".001x2")
// from the jack node to ground. With the jack open (OQ-17) the pole's
// resistance is the series part plus the wiper's own Thevenin resistance.
constexpr float outputJackSeriesOhms = 2200.0f;
constexpr float outputJackCapacitanceF = 1.0e-9f;

// The card's own thermal floor at the filter input. This is not the shared
// audible TP8 noise; it is what the voice card's own resistors do, and it is
// also what gives an otherwise perfectly silent numerical filter its
// self-oscillation seed.
//
// It used to be a voiced 20 uV compatibility amplitude, on the grounds that no
// healthy-card capture fixes it. That was the wrong bar: nothing needs to be
// captured, because the resistors are printed and the law is the same
// Johnson-Nyquist one already applied to IC6's five resistor groups (see
// outputSummerResistorNoiseDensity). Each transconductor stage's input node is
// the 68 kOhm feedback against the 560 Ohm shunt, so the source resistance is
// their parallel combination, 555.43 Ohm, giving sqrt(4kTR) = 3.02 nV/rtHz at
// the 25 C reference. The live card-temperature ratio scales that reference
// density at both rendering and idle-card injection sites. This follows
// sqrt(4kTR), without assigning an avalanche-noise temperature coefficient:
// https://www.ti.com/document-viewer/lit/html/SBOA345/GUID-F87CE11A-8998-4FB4-BEA6-8D520E81351E
// The chassis warm-up remains the existing software model, not an original
// 80017A temperature measurement. That node sits behind the stage's own
// 560/(68000+560) attenuator, so referred to the filter-module input coordinate
// the model works in it is 3.02 nV / 0.0081680 = 370.2 nV/rtHz.
//
// Four independent sources now enter their respective OTA differential
// inputs. For small signals and k=0 their output transfers are H^4, H^3,
// H^2 and H, H=1/(1+s/w). They must not be summed ahead of four poles or
// enter the resonance pair's input-compensation branch. These resistor
// values are independently read on a de-potted original 80017A:
// https://www.sounddoctorin.com/synthtec/roland/juno106.htm (8/6/2017).
// Johnson's original law: https://doi.org/10.1103/PhysRev.32.97
// No BA662/IR3109 device-noise density is invented or added here.
// Evidence class: primary component reads, derived resistor density.
//
// The generator is a bipolar uniform sequence at the 192 kHz reference rate, so
// amplitude A gives RMS A/sqrt(3) over an fs/2 band: A = sqrt(3) * density *
// sqrt(fs/2) = 198.7 uV, which is 19.9 dB above the retired value.
constexpr float filterNoiseSourceOhms = 68000.0f * 560.0f / (68000.0f + 560.0f);
constexpr float filterNoiseStageAttenuation = 560.0f / (68000.0f + 560.0f);
const float filterNoiseVoltsDerived = []
{
    const float density =
        std::sqrt(4.0f * YouKnowEngine::boltzmannConstant
                  * YouKnowEngine::outputNoiseTemperatureKelvin
                  * filterNoiseSourceOhms)
        / filterNoiseStageAttenuation;
    // 192 kHz is YouKnowEngine::noiseReferenceRateHz, which is private to
    // the class; the static_assert beside the injection site keeps the two
    // from drifting apart.
    return std::sqrt(3.0f) * density * std::sqrt(192000.0f * 0.5f);
}();
// The retired voiced amplitude, kept bit-exactly for controlled A/B renders.
constexpr float filterNoiseVoltsVoiced = 2.0e-5f;

float clamp01(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

float sanitised(float value, float fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

std::uint8_t storedControlByte(float value) noexcept
{
    return static_cast<std::uint8_t>(
        std::floor(clamp01(sanitised(value, 0.0f)) * 127.0f + 0.5f));
}

// The 0-1 fraction a converter destination's stored panel value maps to at
// the physical DAC, shared by updateSharedScan, performConverterWrite and
// passiveHoldWriteTarget alike (RESONANCE, common VCA, SUB and NOISE all read
// this same conversion) rather than each of the three carrying its own
// identical copy of the lambda. PWM has B-2's separate doubled-byte product.
float converterDacFraction(float value) noexcept
{
    return static_cast<float>(YouKnowEngine::storedControlDacCode(value))
        / 4064.0f;
}

std::uint8_t controlAdcByte(float value) noexcept
{
    return static_cast<std::uint8_t>(
        std::floor(clamp01(sanitised(value, 0.0f)) * 255.0f + 0.5f));
}

std::int16_t dcoBendCommand(float normalised) noexcept
{
    // The assigner left-aligns the fourteen-bit MIDI word and sends its high
    // byte to B-2 as two one-sided magnitudes. Recombining those bytes gives
    // signed -127..127 with high-byte values 127 and 128 both at rest.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt#L1068-L1087
    const double bounded = std::clamp(
        static_cast<double>(sanitised(normalised, 0.0f)), -1.0, 1.0);
    const long wheel = std::clamp(
        std::lround(8192.0 + bounded * 8192.0), 0L, 16383L);
    const int high = static_cast<int>(wheel) >> 6;
    return static_cast<std::int16_t>(
        high >= 128 ? high - 128 : -(127 - high));
}

std::int32_t dcoBendWordForCommand(std::int16_t command,
                                   std::uint8_t sensitivity) noexcept
{
    // B-2 multiplies the one-sided bend byte by the DCO sensitivity ADC, then
    // forms 0.75*product plus its high byte before extracting the 8.8 pitch
    // word. Spell the individual truncating shifts exactly; replacing them by
    // a floating 1.75 scale changes low-level and endpoint codes.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1006-L1059
    const std::uint32_t magnitude = command == 0
        ? 0u : static_cast<std::uint32_t>(2 * std::abs(static_cast<int>(command)) + 1);
    const std::uint32_t product = magnitude * sensitivity;
    const auto word = static_cast<std::int32_t>(
        ((product >> 1u) + (product >> 2u) + (product >> 8u)) >> 4u);
    return command < 0 ? -word : word;
}

// The single-pole RC corner frequency a capacitor in series with a resistance
// to ground gives, shared by every coupling/loading law below that reduces to
// exactly that network: voice-bus, module, VCA-input and common-VCA-input
// coupling, both noise support poles, and both output-coupling laws each
// carried their own copy of the identical 1/(2*pi*R*C) before this helper.
// highPassCornerHz's four-leg switched network stays its own derivation --
// it selects between four distinct branches, not one fixed R and C.
float rcCornerHz(float capacitanceF, float resistanceOhms) noexcept
{
    return 1.0f / (twoPi * capacitanceF * resistanceOhms);
}

// The loaded lower-track resistance and the total pole resistance the output
// coupling network presents at a given VOLUME shaft position. Both the
// loaded corner frequency and the loaded passthrough gain below are read off
// this same wiper network, so it is solved once here rather than carrying
// two independently maintained copies of the identical track-loading algebra.
struct OutputCouplingWiperNetwork
{
    float loadedLower;
    float resistance;
};

OutputCouplingWiperNetwork outputCouplingWiperNetworkFor(
    float volumePosition) noexcept
{
    const float position = clamp01(sanitised(volumePosition, 0.0f));
    const float lowerTrack = position * outputCouplingPotOhms;
    const float loadedLower = lowerTrack > 0.0f
        ? lowerTrack * outputWiperInternalLoadOhms
            / (lowerTrack + outputWiperInternalLoadOhms)
        : 0.0f;
    const float upperTrack = (1.0f - position) * outputCouplingPotOhms;
    return { loadedLower, outputCouplingSeriesOhms + upperTrack + loadedLower };
}

// The corner R64/R65 and C22/C21 make with the wiper's own Thevenin
// resistance: the upper leg (R54/R57 plus the unused track) in parallel with
// the loaded lower track -- the same two paths outputWiperNoiseResistance()
// sums, read here off the one already-solved network. Full volume: 1.249 kOhm
// + 2.2 kOhm against 1 nF is 46.15 kHz; half volume, 2.578 kOhm + 2.2 kOhm,
// is 33.32 kHz.
float outputJackCornerHzFor(
    const OutputCouplingWiperNetwork& network) noexcept
{
    const float upperLeg = network.resistance - network.loadedLower;
    const float wiperOhms =
        upperLeg * network.loadedLower / network.resistance;
    return rcCornerHz(outputJackCapacitanceF, outputJackSeriesOhms + wiperOhms);
}

} // namespace

// ---------------------------------------------------------------------------
// Modelled hardware laws
// ---------------------------------------------------------------------------

double YouKnowEngine::rangeClockHz(DcoRange range) noexcept
{
    switch (range)
    {
        case DcoRange::Sixteen: return masterClockHz / 8.0;
        case DcoRange::Four:    return masterClockHz / 2.0;
        case DcoRange::Eight:
        default:                return masterClockHz / 4.0;
    }
}

namespace
{
// The hash-matched B-2 image stores two proprietary 104-word pitch tables.
// Keep the recovered clamp/interpolation mechanism exact without putting
// either table (or a reversible residual dump) in this project. These compact
// smooth generators were fitted against the semantic words at 0x0f30 and
// 0x0e60:
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1845-L1883
//
// Across all 104 anchors the divider generator is within four counts (56 are
// exact), and after the firmware interpolation it stays within 1.272 cents at
// every valid 8.8 coordinate. The CV generator is the intended 32-code/octave
// exponential, saturates at 4095, and differs from the recovered image by at
// most one code. These anchors are therefore derived, not ROM-resolved; the
// surrounding unsigned clamp and truncating interpolation below are exact.
std::uint32_t derivedDcoDividerAnchor(int index) noexcept
{
    const double octave = std::exp2(-static_cast<double>(index) / 12.0);
    const double value = -0.25 + 61382.5 * octave
                       + 169.0 * octave * octave;
    return static_cast<std::uint32_t>(std::floor(value + 0.5));
}

std::uint16_t derivedDcoCvAnchor(int index) noexcept
{
    const double value = 32.0 * std::exp2(
        static_cast<double>(index) / 12.0);
    return static_cast<std::uint16_t>(std::min(
        4095.0, std::floor(value + 0.5)));
}

std::uint16_t aggregatePitchWord(double midiNote,
                                 std::int32_t controlOffset = 0) noexcept
{
    // B-2 starts the shared master coordinate at 0x1818, then adds the
    // per-voice 8.8 portamento word. The engine's portamento state already
    // advances on that 1/256-semitone grid. Exact integer control terms can be
    // added independently; any remaining continuous controls are rounded only
    // once when this final host adapter produces the word.
    constexpr double b2MasterPitchWord = 0x1818;
    const double units = b2MasterPitchWord + midiNote * 256.0
                       + static_cast<double>(controlOffset);
    const double bounded = std::clamp(units, 0.0, 65535.0);
    return static_cast<std::uint16_t>(std::floor(bounded + 0.5));
}
} // namespace

YouKnowEngine::DcoPitchPair YouKnowEngine::dcoPitchPair(
    std::uint16_t pitchWord) noexcept
{
    // B-2 uses h=48..150 as table indices 0..102. Entry 103 is lookahead
    // only; both clamps discard the fractional byte. The two products are
    // unsigned and divide by 256 with truncation, exactly as the paired MUL
    // sequences at 0x0440..0x0489 do.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L682-L780
    const int high = pitchWord >> 8u;
    int index = 0;
    std::uint32_t fraction = pitchWord & 0xffu;
    if (high <= 0x2f)
        fraction = 0u;
    else if (high >= 0x97)
    {
        index = 0x66;
        fraction = 0u;
    }
    else
        index = high - 0x30;

    const std::uint32_t divider = derivedDcoDividerAnchor(index);
    const std::uint32_t nextDivider = derivedDcoDividerAnchor(index + 1);
    const std::uint32_t cvCode = derivedDcoCvAnchor(index);
    const std::uint32_t nextCvCode = derivedDcoCvAnchor(index + 1);
    return {
        divider - fraction * (divider - nextDivider) / 256u,
        static_cast<std::uint16_t>(
            cvCode + fraction * (nextCvCode - cvCode) / 256u)
    };
}

std::int16_t YouKnowEngine::masterTunePitchWordOffset(
    double cents) noexcept
{
    // IC29 subtracts 128 from the processed Tune ADC byte. The plug-in keeps
    // its declared continuous +/-50-cent host parameter and maps it onto that
    // signed-byte grid; clamping before lround also keeps hostile finite input
    // away from the integral conversion's undefined overflow range.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1107-L1119
    const double bounded = std::isfinite(cents)
        ? std::clamp(cents, -50.0, 50.0) : 0.0;
    const long units = std::lround(bounded * 2.56);
    return static_cast<std::int16_t>(std::clamp(units, -128L, 127L));
}

std::int32_t YouKnowEngine::dcoPitchBendWordOffset(
    float normalisedBipolar, float depth) noexcept
{
    return dcoBendWordForCommand(
        dcoBendCommand(normalisedBipolar), controlAdcByte(depth));
}

std::int32_t YouKnowEngine::vcfBendCountsWord(
    std::int16_t command, std::uint8_t sensitivity) noexcept
{
    // The serial handler stores a zero command as zero and any other as twice
    // its magnitude plus one (0x022a..0x0232). B-2 multiplies that byte by
    // the VCF sensitivity ADC and shifts right four times (0x0674..0x0687)
    // before the same value feeds the DCO's 1.75x path, so the filter shares
    // the DCO's two-bin centre dead zone and tops out at 255 * 255 >> 4.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L379-L384
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1021-L1031
    const std::uint32_t magnitude = command == 0
        ? 0u : static_cast<std::uint32_t>(2 * std::abs(static_cast<int>(command)) + 1);
    const auto word = static_cast<std::int32_t>((magnitude * sensitivity) >> 4u);
    return command < 0 ? -word : word;
}

std::uint8_t YouKnowEngine::dcoLfoDepthScale(
    std::uint8_t storedDepth) noexcept
{
    // Exact compact form of B-2's 128-byte DCO-LFO depth table at 0x0a80.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1765-L1773
    const int depth = std::min<int>(storedDepth, 127);
    if (depth < 3)
        return 0u;
    if (depth < 65)
        return static_cast<std::uint8_t>(depth - 2);
    if (depth < 96)
        return static_cast<std::uint8_t>(2 * depth - 66);
    if (depth < 125)
        return static_cast<std::uint8_t>(4 * depth - 256);
    if (depth == 125)
        return 248u;
    return 255u;
}

std::int32_t YouKnowEngine::dcoLfoPitchWordOffset(
    std::uint16_t accumulator, bool positivePolarity,
    std::uint8_t delayByte, std::uint8_t storedDepth,
    std::uint8_t modWheel, std::uint8_t benderSensitivity) noexcept
{
    // The two high-byte products and saturating add at 0x032d..0x0338 produce
    // one depth byte. The 16x8 multiply at 0x033b..0x034f then divides by
    // eight, which is (accumulator * depth) >> 11 in pitch-word units.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L541-L560
    const std::uint32_t panel =
        (static_cast<std::uint32_t>(dcoLfoDepthScale(storedDepth))
         * delayByte) >> 8u;
    const std::uint32_t controller =
        (2u * std::min<std::uint32_t>(modWheel, 127u)
         * benderSensitivity) >> 8u;
    const std::uint32_t depth = std::min(255u, panel + controller);
    const std::uint32_t word =
        (std::min<std::uint32_t>(accumulator, 0x1fffu) * depth) >> 11u;
    return positivePolarity ? static_cast<std::int32_t>(word)
                            : -static_cast<std::int32_t>(word);
}

std::int32_t YouKnowEngine::vcfLfoCountsWord(
    std::uint16_t accumulator, bool positivePolarity,
    std::uint8_t delayByte, std::uint8_t storedDepth) noexcept
{
    // The VCF-LFO panel byte is stored doubled at 0x01b3..0x01bd. Right after
    // the pitch word, 0x0352..0x0356 takes the high byte of that doubled byte
    // times the same onset byte, and the 16x8 multiply at 0x0357..0x0365 then
    // shifts once, so the cutoff term is (accumulator * depth) >> 9: 4047
    // counts at full depth and onset, and only 15 at panel byte 1.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L306-L313
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L562-L574
    const std::uint32_t depth =
        (2u * std::min<std::uint32_t>(storedDepth, 127u) * delayByte) >> 8u;
    const std::uint32_t word =
        (std::min<std::uint32_t>(accumulator, 0x1fffu) * depth) >> 9u;
    return positivePolarity ? static_cast<std::int32_t>(word)
                            : -static_cast<std::int32_t>(word);
}

std::uint16_t YouKnowEngine::vcfEnvelopeCountsWord(
    std::uint16_t envelopeLevel, std::uint8_t storedDepth) noexcept
{
    // B-2's two MULs and EADD at 05C5..05D2 calculate
    // floor(envelope_RAM * doubled_ENV_byte / 256). Unlike the VCA write,
    // this path sees all fourteen envelope bits; multiplying its 12-bit DAC
    // fraction by a normalized 16255 endpoint loses partial-product carries and can move
    // the final cutoff by a DAC step during a slow envelope.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L926-L938
    const std::uint32_t level = std::min<std::uint32_t>(envelopeLevel, 0x3fffu);
    const std::uint32_t depth = 2u * std::min<std::uint32_t>(storedDepth, 127u);
    return static_cast<std::uint16_t>((level * depth) >> 8u);
}

std::int32_t YouKnowEngine::vcfKeyFollowCountsWord(
    std::int32_t voicePitchWord, std::uint8_t storedDepth) noexcept
{
    // 05E1..05EA forms floor(pitch/4) + floor(pitch/8), not a continuous
    // 3/8 multiply. 05EC..0633 subtracts C4 (0x1680), multiplies the absolute
    // distance by the doubled KEY byte and discards the low product byte
    // before restoring the sign. The maximum slope is still 1143 counts per
    // octave, but fractional glide positions have the firmware's own steps.
    // The signed divisions extend the same law to the host's below-zero
    // transpose range; actual unsigned 8.8 voice words divide exactly as DSLR.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L944-L992
    const std::int64_t coordinate = static_cast<std::int64_t>(voicePitchWord) / 4
                                  + static_cast<std::int64_t>(voicePitchWord) / 8
                                  - 0x1680;
    const std::int64_t depth = 2 * std::min<int>(storedDepth, 127);
    // Truncation toward zero is the positive magnitude's >>8 followed by its
    // original sign, including sub-count distances immediately below C4.
    return static_cast<std::int32_t>((coordinate * depth) / 256);
}

std::uint32_t YouKnowEngine::dcoDivider(double frequencyHz) noexcept
{
    if (!(frequencyHz > 0.0) || !std::isfinite(frequencyHz))
        return maximumDivider;

    const double midiNote = 69.0 + 12.0 * std::log2(frequencyHz / 440.0);
    return dcoPitchPair(aggregatePitchWord(midiNote)).divider;
}

double YouKnowEngine::dcoQuantisedFrequency(std::uint32_t divider,
                                               DcoRange range) noexcept
{
    const std::uint32_t limited = std::clamp(divider, minimumDivider, maximumDivider);
    return rangeClockHz(range) / static_cast<double>(limited);
}

namespace
{
// The anti-log converter's own transfer, with no endpoint policy on it. The
// transconductor's control-current saturation is what actually bends the top
// of this law, and it has to see the unclamped value or the product cap would
// be limiting the input to the physics instead of the result of it.
float vcfAntilogHz(float counts) noexcept
{
    const float safe = std::clamp(sanitised(counts, 0.0f), -2000.0f, 20000.0f);
    return YouKnowEngine::vcfBaseFrequencyHz
         * std::exp2(safe / YouKnowEngine::vcfCountsPerOctave);
}
} // namespace

float YouKnowEngine::vcfCutoffHz(float counts) noexcept
{
    // The digital sum upstream is already clamped to the 14-bit accumulator;
    // the margin here only covers analogue trim and drift. This is the
    // converter's law alone -- what the transconductor can still follow is
    // vcfEffectiveCutoffHz -- with the published 50 kHz endpoint as a
    // transparent product cap.
    return std::clamp(vcfAntilogHz(counts), 1.0f, vcfSafetyCapHz);
}

float YouKnowEngine::vcfPanelCounts(float panelPosition) noexcept
{
    // The panel slider is read as a 0..127 byte and the converter is driven
    // with that byte times 128, so the whole slider spans 16256 counts.
    const float byte = std::floor(clamp01(panelPosition) * 127.0f + 0.5f);
    return byte * 128.0f;
}

float YouKnowEngine::VoicedResonanceCompatibilityProfile::loopGain(
    float panelPosition) noexcept
{
    // This piecewise curve is retained solely for YouKnow preset/sound
    // compatibility. No qualifying original-unit sweep establishes its
    // intermediate points, threshold position or maximum as hardware facts.
    const float position = clamp01(panelPosition);
    if (position <= nominalOscillationTravel)
        return 2.3277778f * position + 2.3518519f * position * position;
    const float past = (position - nominalOscillationTravel)
                     / (1.0f - nominalOscillationTravel);
    return nominalOscillationFeedback
         + (maximumFeedback - nominalOscillationFeedback) * past;
}

float YouKnowEngine::VoicedResonanceCompatibilityProfile::inputCompensation(
    float feedback, ResonanceCompensationShape shape) noexcept
{
    // The direction is settled by the drawn network; the coefficient is
    // voiced inside the bracket the two readings span. See the constants.
    const float k = std::clamp(sanitised(feedback, 0.0f), 0.0f, 8.0f);
    const float c = shape == ResonanceCompensationShape::Drawn
        ? drawnInputCompensationPerFeedback
        : (shape == ResonanceCompensationShape::Legacy
               ? legacyInputCompensationPerFeedback
               : inputCompensationPerFeedback);
    return 1.0f + c * k;
}

namespace
{
// ------------------------------------------------------------------------
// The cascade's own limit cycle, solved by harmonic balance.
//
// A compressive nonlinearity inside an integrator lowers that integrator's
// pole in proportion to its first-harmonic gain. For a sinusoid of amplitude
// A driving tanh(v / H) that gain is the classical sinusoidal-input
// describing function
//
//     N(a) = (2 / (pi a)) * integral_0^pi tanh(a sin t) sin t dt,   a = A / H,
//
// which is 1 - a^2/4 + O(a^4) at small a (int sin^2 = pi/2, int sin^4 =
// 3pi/8) and falls to 4/(pi a) once the tanh is square. It is identically 1
// at a = 0, and that is the property the frequency correction is built on:
// with no limit cycle there is no droop to correct.
//
// The cascade carries two different nonlinearities on two different
// headrooms, and both are needed. The four stage pairs compress on
// `otaHeadroomVolts` = 2 Vt / stageAttenuation and set the *frequency*; the
// resonance return compresses on `loopHeadroomVolts` = 2 Vt * 67.7 and sets
// the *amplitude*. Both are 2 Vt through a resistor ratio, so one card
// temperature moves both (resonanceHeadroomFor), and scaling the two
// together with the amplitude leaves every ratio below unchanged: the table
// this builds at 25 C is exact at any temperature. Referring one node to
// the other's headroom is what makes
// a single-node account of this miss: at the service anchor the fourth
// stage's drive is a = 0.35 and supplies 64 cents on its own, while the
// first stage's is a = 1.17, because a four-pole loop oscillating at its own
// corner carries sqrt(2) more amplitude at every step back towards the
// input. All four have to be carried.
//
// Harmonic balance, with the loop running at its own corner scaled by each
// stage's gain N_n and D = w_osc / w_corner:
//
//   phase:      sum_n atan(D / N_n) = pi
//   amplitude:  k * N_fb(A / Hfb) * prod_n 1 / sqrt(1 + (D / N_n)^2) = 1
//   drive:      a_n = |V_n| * (D / N_n) / H, with |V_n| walking back from
//               the output by sqrt(1 + (D / N_{n+1})^2) per stage
//
// At A = 0 every N_n is 1, the phase condition gives D = tan(pi/4) = 1 and
// the amplitude condition gives k = 4: the threshold is
// `nominalOscillationFeedback` exactly, and it is not a fitted number.
//
// The solve is parameterised by amplitude rather than by loop gain, which
// removes the outer root-find: every A determines the loop that sustains it.
// Sweeping A and inverting the resulting monotone map gives the correction
// on a uniform grid in loop gain, once, at first use.
// ------------------------------------------------------------------------
constexpr double piHigh = 3.14159265358979323846;
constexpr double describingCeiling = 8.0;
constexpr int describingSteps = 512;

// Composite Simpson over the integrand's own quarter period; it is smooth and
// bounded, so 64 panels hold it far inside the interpolation error of the
// table it fills.
double describingIntegral(double a) noexcept
{
    constexpr int panels = 64;
    const double width = 0.5 * piHigh / static_cast<double>(panels);
    const auto sample = [a](double t) {
        const double s = std::sin(t);
        return std::tanh(a * s) * s;
    };
    double sum = 0.0;
    for (int panel = 0; panel < panels; ++panel)
    {
        const double left = width * static_cast<double>(panel);
        sum += width / 6.0
             * (sample(left) + 4.0 * sample(left + 0.5 * width)
                + sample(left + width));
    }
    return 2.0 * sum;
}

// N(a), tabulated on [0, 8] and continued by its own 1/a asymptote above it.
const std::array<double, describingSteps + 1> describingTable = [] {
    std::array<double, describingSteps + 1> values {};
    values[0] = 1.0;
    for (int index = 1; index <= describingSteps; ++index)
    {
        const double argument = describingCeiling
            * static_cast<double>(index) / static_cast<double>(describingSteps);
        values[static_cast<std::size_t>(index)] =
            2.0 / (piHigh * argument) * describingIntegral(argument);
    }
    return values;
}();

double describingGain(double a) noexcept
{
    const double magnitude = std::abs(a);
    if (magnitude >= describingCeiling)
        return describingTable[describingSteps] * describingCeiling / magnitude;
    const double position = magnitude / describingCeiling
                          * static_cast<double>(describingSteps);
    const auto lower = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(lower);
    return describingTable[lower] * (1.0 - fraction)
         + describingTable[lower + 1] * fraction;
}

// The oscillation frequency the phase condition puts on a cascade whose four
// stages have been slowed to N_n of their control corner. Newton on a sum of
// arctangents, seeded at the caller's previous answer.
double oscillationRatio(const std::array<double, 4>& gains, double seed) noexcept
{
    double ratio = std::max(seed, 1.0e-6);
    for (int iteration = 0; iteration < 24; ++iteration)
    {
        double phase = 0.0;
        double slope = 0.0;
        for (const double gain : gains)
        {
            const double x = ratio / gain;
            phase += std::atan(x);
            slope += (1.0 / gain) / (1.0 + x * x);
        }
        const double step = (piHigh - phase) / slope;
        ratio = std::max(1.0e-6, ratio + step);
        if (std::abs(step) < 1.0e-13 * ratio)
            break;
    }
    return ratio;
}

struct LimitCycle
{
    double loopGain;
    double droop;
};

// The loop that sustains a limit cycle of the given output amplitude, and the
// factor by which that limit cycle drags its own corner down. `gains` carries
// the previous amplitude's solution in and this one's out, so the sweep that
// builds the table converges in two or three passes instead of a dozen.
LimitCycle limitCycleFor(double amplitude, double stageHeadroom,
                         double returnHeadroom,
                         std::array<double, 4>& gains,
                         const std::array<double, 4>& poleScales =
                             { 1.0, 1.0, 1.0, 1.0 }) noexcept
{
    std::array<double, 4> ratios { 1.0, 1.0, 1.0, 1.0 };
    double droop = 1.0;
    for (int iteration = 0; iteration < 32; ++iteration)
    {
        droop = oscillationRatio(gains, droop);
        for (std::size_t stage = 0; stage < 4; ++stage)
            ratios[stage] = droop / gains[stage];

        std::array<double, 4> nodes {};
        nodes[3] = amplitude;
        for (int stage = 2; stage >= 0; --stage)
        {
            const auto index = static_cast<std::size_t>(stage);
            nodes[index] = nodes[index + 1]
                * std::sqrt(1.0 + ratios[index + 1] * ratios[index + 1]);
        }

        double moved = 0.0;
        for (std::size_t stage = 0; stage < 4; ++stage)
        {
            const double next =
                poleScales[stage]
                * describingGain(nodes[stage] * ratios[stage] / stageHeadroom);
            moved = std::max(moved, std::abs(next - gains[stage]));
            gains[stage] = next;
        }
        if (moved < 1.0e-11)
            break;
    }

    double loss = describingGain(amplitude / returnHeadroom);
    for (const double ratio : ratios)
        loss /= std::sqrt(1.0 + ratio * ratio);
    return LimitCycle { 1.0 / loss, droop };
}
} // namespace

float YouKnowEngine::VoicedResonanceCompatibilityProfile::frequencyTrim(
    float feedback) noexcept
{
    // The correction that puts the oscillation back on the control law is the
    // reciprocal of the droop the limit cycle imposes on the corner. Built
    // once by sweeping the limit-cycle amplitude and resampling the resulting
    // loop gain onto a uniform grid from the oscillation threshold to the
    // clamp this function already carried. Parameterising the sweep by
    // amplitude rather than by loop gain is what keeps it cheap: every
    // amplitude determines the loop that sustains it outright, so there is no
    // outer root-find. `prepare` warms this so no audio callback pays for it.
    constexpr int trimSteps = 128;
    constexpr float trimCeiling = 8.0f;
    static const std::array<float, trimSteps + 1> table = [] {
        constexpr int sweep = 1024;
        constexpr double amplitudeCeiling = 12.0;
        std::array<float, trimSteps + 1> values {};
        values[0] = 1.0f;
        const double step = (static_cast<double>(trimCeiling)
                             - static_cast<double>(nominalOscillationFeedback))
                          / static_cast<double>(trimSteps);
        int filled = 0;
        LimitCycle previous { static_cast<double>(nominalOscillationFeedback), 1.0 };
        std::array<double, 4> gains { 1.0, 1.0, 1.0, 1.0 };
        for (int index = 1; index <= sweep && filled < trimSteps; ++index)
        {
            const double amplitude = amplitudeCeiling
                * static_cast<double>(index) / static_cast<double>(sweep);
            const LimitCycle current = limitCycleFor(
                amplitude, static_cast<double>(otaHeadroomVolts),
                static_cast<double>(loopHeadroomVolts), gains);
            while (filled < trimSteps)
            {
                const double wanted =
                    static_cast<double>(nominalOscillationFeedback)
                    + step * static_cast<double>(filled + 1);
                if (wanted > current.loopGain)
                    break;
                const double span = current.loopGain - previous.loopGain;
                const double blend = span > 0.0
                    ? (wanted - previous.loopGain) / span : 0.0;
                const double droop = previous.droop
                    + blend * (current.droop - previous.droop);
                values[static_cast<std::size_t>(++filled)] =
                    static_cast<float>(1.0 / droop);
            }
            previous = current;
        }
        // The sweep covers the clamp with room to spare; hold the last solved
        // value if a future headroom ever shortens it.
        for (int index = filled + 1; index <= trimSteps; ++index)
            values[static_cast<std::size_t>(index)] =
                values[static_cast<std::size_t>(index - 1)];
        return values;
    }();

    const float k = std::clamp(sanitised(feedback, 0.0f), 0.0f, trimCeiling);
    if (k <= nominalOscillationFeedback)
        return 1.0f;
    const float position = (k - nominalOscillationFeedback)
        / (trimCeiling - nominalOscillationFeedback)
        * static_cast<float>(trimSteps);
    const auto lower = static_cast<std::size_t>(position);
    if (lower >= trimSteps)
        return table[trimSteps];
    const float fraction = position - static_cast<float>(lower);
    return table[lower] * (1.0f - fraction) + table[lower + 1] * fraction;
}

float YouKnowEngine::vcfConverterCarryCounts(float counts) noexcept
{
    // Retained effective cutoff calibration from the original analyst's V4
    // frequency table: 93 sampled codes, with each boundary's excess inferred
    // by log-frequency extrapolation from the preceding two measurements.
    // These are end-to-end VCF observations from #439522 (Borish replacement
    // voice cards), not direct TP4 measurements of the shared DAC's voltage.
    // https://github.com/kayrockscreenprinting/ultramaster_kr106/blob/bc15caee5843ab238a25d0969e68d57db2b1615f/Source/DSP/J106DACHzTable.h#L1-L22
    // https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16
    // A physical ladder error would reach all 23 holds through their buffers,
    // but that attribution and transfer are not uniquely identified here.
    // Keep this established VCF calibration until TP4 voltage data resolve it.
    // At strength 1 the largest increment is 5.551 physical codes; cumulative
    // error peaks at 4.446 codes and is zero below code 1024 in this model.
    // Full-scale voltage percentages are not relative gain-error bounds at
    // low controls, nor audibility bounds for PWM or a resonant feedback loop.
    // Cents to counts: 1143 counts is an octave and 1200 cents is an octave.
    constexpr float perCent = vcfCountsPerOctave / 1200.0f;
    // Cumulative excess step at each of the three top bit boundaries. The
    // converter takes the top twelve bits, so a DAC code is four counts.
    float carry = 0.0f;
    if (counts >= 4096.0f)   // DAC 1024
        carry += -4.64f * perCent;
    if (counts >= 8192.0f)   // DAC 2048 -- the major carry
        carry += 23.31f * perCent;
    if (counts >= 12288.0f)  // DAC 3072
        carry += -4.48f * perCent;
    return carry;
}

float YouKnowEngine::vcfEffectiveCutoffHz(float counts,
                                             float calibrationFeedback,
                                             int referenceCard) noexcept
{
    // Service Notes p. 13 connects VR29 FREQ/VR28 WIDTH only to the cutoff
    // path; VR26 RES feeds a separate grounded-base transistor/BA662. Page
    // 19 adjusts the two frequency trimmers AFTER the 4.8Vp-p full-RES trim.
    // Their setting cannot subsequently follow the resonance slider.
    // The engine therefore passes the card's fixed full-RES service-condition
    // feedback here. Its legacy comparison passes the live feedback instead.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
    const bool reference = referenceCard >= 0
        && referenceCard < static_cast<int>(serviced439522Vcf.size());
    float antilogHz = vcfAntilogHz(counts);
    float saturationHz = vcfControlSaturationHz;
    if (reference)
    {
        const auto& calibration = serviced439522Vcf[static_cast<std::size_t>(referenceCard)];
        const float safeCounts = std::clamp(sanitised(counts, 0.0f), -2000.0f, 20000.0f);
        antilogHz = static_cast<float>(calibration.baseHertz
            * std::exp2(static_cast<double>(safeCounts)
                * calibration.centsPerByte / (128.0 * 1200.0)));
        saturationHz = static_cast<float>(calibration.selfOscillationCeilingHertz)
            * VoicedResonanceCompatibilityProfile::frequencyTrim(
                VoicedResonanceCompatibilityProfile::maximumFeedback);
    }
    const float rawHz = antilogHz
        * VoicedResonanceCompatibilityProfile::frequencyTrim(calibrationFeedback);
    // The transconductor's control current saturates internally, so the pole
    // stops following the anti-log converter near the top of the slider. The
    // generalized algebraic clip keeps the law numerically exact through the
    // musical range -- under five cents of correction below 2.7 kHz -- and
    // bends it only as the current approaches its own limit.
    const double normalised = static_cast<double>(rawHz)
                            / static_cast<double>(saturationHz);
    const double exponent = static_cast<double>(vcfControlSaturationExponent);
    const double saturated = static_cast<double>(rawHz)
        / algebraicSoftClipDenominator(normalised, exponent);
    // The identified capture reaches 51.35kHz, so the nominal 50kHz product
    // cap would invalidate its upper held-outs. This opt-in profile keeps
    // its explicit historical-model fit bound; both render paths additionally
    // cap every physical pole at 0.45 of the actual internal sample rate.
    return std::min(reference ? 72900.0f : vcfSafetyCapHz,
                    static_cast<float>(saturated));
}

float YouKnowEngine::chassisGradientCelsius(int cardIndex) noexcept
{
    // A per-card constant, read once per voice per internal sample. The
    // exponential belongs in a table, not in the audio path.
    static const std::array<float, maxVoices> profile = [] {
        std::array<float, maxVoices> values {};
        for (int card = 0; card < maxVoices; ++card)
            values[static_cast<std::size_t>(card)] = chassisGradientPeakCelsius
                * std::exp(-static_cast<float>(card) / chassisGradientCards);
        return values;
    }();

    const int index = std::max(cardIndex, 0);
    if (index < maxVoices)
        return profile[static_cast<std::size_t>(index)];
    return chassisGradientPeakCelsius
         * std::exp(-static_cast<float>(index) / chassisGradientCards);
}

float YouKnowEngine::chassisGradientMeanCelsius() noexcept
{
    // A constant, but not one the language will fold: std::exp is not
    // constexpr. Thermal-scale refreshes read it for every card, so compute it
    // once rather than repeating the same six exponentials per refresh.
    static const float mean = [] {
        float total = 0.0f;
        for (int card = 0; card < hardwareVoices; ++card)
            total += chassisGradientCelsius(card);
        return total / static_cast<float>(hardwareVoices);
    }();
    return mean;
}

double YouKnowEngine::thermalFilterOmegaScaleFor(
    const EngineParameters& parameters, int cardIndex,
    float warmupFraction) noexcept
{
    if (!parameters.enableSpatialThermalGradient)
        return 1.0;
    return 1.0 + static_cast<double>(vcfCutoffTempcoPerCelsius)
        * static_cast<double>(parameters.calibration)
        * (static_cast<double>(chassisGradientCelsius(cardIndex))
           - static_cast<double>(chassisGradientMeanCelsius()))
        * static_cast<double>(warmupFraction);
}

float YouKnowEngine::boundedThermalFilterOmegaStep(
    float baseOmegaStep, const EngineParameters& parameters,
    int cardIndex, float warmupFraction) noexcept
{
    return static_cast<float>(OtaCascade::clampOmegaStep(
        static_cast<double>(baseOmegaStep)
        * thermalFilterOmegaScaleFor(parameters, cardIndex, warmupFraction)));
}

namespace
{
// Compact laws independently derived from the hash-matched B-2 regions. They
// reproduce all observable coefficients while avoiding a ROM/table dump.
std::uint16_t attackIncrementForByte(std::uint8_t byte) noexcept
{
    const int index = byte;
    if (index == 0)
        return 0x4000u;
    if (index <= 63)
    {
        int value = (8192 + index / 2) / index;
        // Three values in the otherwise rounded harmonic region are one count
        // lower in the verified image.
        if (index == 3 || index == 20 || index == 28)
            --value;
        return static_cast<std::uint16_t>(value);
    }
    if (index <= 80)
        return static_cast<std::uint16_t>(127 - 11 * (index - 64) / 4);
    if (index <= 86)
        return static_cast<std::uint16_t>(83 - (17 * (index - 80) + 1) / 6);
    if (index <= 107)
        return static_cast<std::uint16_t>(64 - 3 * (index - 87) / 2);
    if (index <= 121)
    {
        const int numerator = 429 - 6 * (index - 108);
        return static_cast<std::uint16_t>((2 * numerator + 13) / 26);
    }
    return static_cast<std::uint16_t>(26 - (index - 122));
}

std::uint16_t decayReleaseMultiplierForByte(std::uint8_t byte) noexcept
{
    std::uint32_t value = 0x1000u;
    for (int index = 1; index <= byte; ++index)
    {
        if (index <= 4)       value += 0x2000u;
        else if (index == 5)  value += 0x1000u;
        else if (index <= 15) value += 0x0800u;
        else if (index <= 43) value += 0x0080u;
        else if (index <= 65) value += 0x000cu;
        else if (index <= 123)value += 0x0004u;
        else                  value += 0x0001u;
    }
    return static_cast<std::uint16_t>(value);
}

std::uint16_t lfoRateIncrementForByte(std::uint8_t byte) noexcept
{
    // B-2's rate table at $0C60 opens 0005 000f 0019 0028 0037 0046 0050 005a,
    // and this generator reproduces it entry for entry. The 5 -> 15 first step
    // is a 3.0x ratio where every neighbour is at most 1.7x, which looks like a
    // transcription error and is not one: the listing's own hex plainly reads
    // 0005, and it was re-checked for exactly that reason.
    //
    // It is also what reconciles the printed specification. These coefficients
    // give 0.0363 Hz at byte 0 and 0.1088 Hz at byte 1, so Roland's "0.1 Hz ~
    // 30 Hz" is byte 1 to byte 127, both ends inside 0.9 %. Byte 0 is a
    // below-spec entry the panel reaches anyway, because the LFO RATE row
    // carries no minimum-stop resistor.
    int value = 5;
    for (int index = 1; index <= byte; ++index)
    {
        if (index <= 2)        value += 10;
        else if (index <= 5)   value += 15;
        else if (index <= 63)  value += 10;
        else if (index <= 95)  value += 16;
        else if (index <= 102) value += 52;
        else if (index == 103) value += 54;
        else if (index <= 105) value += 70;
        else if (index <= 109) value += 80;
        else if (index <= 119) value += 100;
        else if (index <= 122) value += 120;
        else if (index <= 126) value += 150;
        else                   value += 96;
    }
    return static_cast<std::uint16_t>(value);
}

std::uint16_t lfoDelayFadeIncrementForByte(std::uint8_t byte) noexcept
{
    switch (byte >> 4u)
    {
        case 0: return 0xffffu;
        case 1: return 1049u;
        case 2: return 524u;
        case 3: return 350u;
        default: return 256u;
    }
}

std::uint8_t portamentoIncrementForIndex(std::uint8_t index) noexcept
{
    if (index == 0u)
        return 0u;
    if (index <= 25u)
        return static_cast<std::uint8_t>(263 - 8 * index);
    if (index <= 47u)
        return static_cast<std::uint8_t>(113 - 2 * index);
    if (index == 48u)
        return 18u;
    if (index == 49u)
        return 17u;
    if (index <= 61u)
        return static_cast<std::uint8_t>(29 - (index + 2) / 4);
    return static_cast<std::uint8_t>(
        std::max(1, 26 - (static_cast<int>(index) + 3) / 5));
}

std::uint16_t truncatedDecayProduct(std::uint16_t value,
                                    std::uint16_t coefficient) noexcept
{
    const std::uint16_t valueHigh = value >> 8u;
    const std::uint16_t valueLow = value & 0xffu;
    const std::uint16_t coefficientHigh = coefficient >> 8u;
    const std::uint16_t coefficientLow = coefficient & 0xffu;
    return static_cast<std::uint16_t>(
        coefficientHigh * valueHigh
        + ((coefficientLow * valueHigh) >> 8u)
        + ((coefficientHigh * valueLow) >> 8u));
}
} // namespace

std::uint16_t YouKnowEngine::storedControlAlignedWord(float panelPosition) noexcept
{
    return static_cast<std::uint16_t>(storedControlByte(panelPosition)) << 7u;
}

std::uint16_t YouKnowEngine::storedControlDacCode(float panelPosition) noexcept
{
    return static_cast<std::uint16_t>(storedControlByte(panelPosition)) << 5u;
}

std::uint16_t YouKnowEngine::envelopeAttackIncrement(
    float panelPosition) noexcept
{
    return attackIncrementForByte(storedControlByte(panelPosition));
}

std::uint16_t YouKnowEngine::envelopeDecayReleaseMultiplier(
    float panelPosition) noexcept
{
    return decayReleaseMultiplierForByte(storedControlByte(panelPosition));
}

std::uint16_t YouKnowEngine::lfoRateIncrement(float panelPosition) noexcept
{
    return lfoRateIncrementForByte(storedControlByte(panelPosition));
}

std::uint16_t YouKnowEngine::lfoDelayFadeIncrement(
    float panelPosition) noexcept
{
    return lfoDelayFadeIncrementForByte(storedControlByte(panelPosition));
}

std::uint8_t YouKnowEngine::portamentoIncrement(float panelPosition) noexcept
{
    const auto raw = controlAdcByte(panelPosition);
    return raw == 0u ? 0u : portamentoIncrementForIndex(raw >> 1u);
}

std::uint16_t YouKnowEngine::envelopeAttackLevel(
    std::uint16_t level, std::uint16_t increment) noexcept
{
    const std::uint32_t next = static_cast<std::uint32_t>(level) + increment;
    return static_cast<std::uint16_t>(
        std::min<std::uint32_t>(next, envelopePeak));
}

std::uint16_t YouKnowEngine::envelopeDecayLevel(
    std::uint16_t level, std::uint16_t sustain,
    std::uint16_t multiplier) noexcept
{
    const auto target = std::min(sustain, envelopePeak);
    const auto current = std::min(level, envelopePeak);
    if (current <= target)
        return target;
    const auto distance = static_cast<std::uint16_t>(current - target);
    return static_cast<std::uint16_t>(
        target + truncatedDecayProduct(distance, multiplier));
}

std::uint16_t YouKnowEngine::envelopeReleaseLevel(
    std::uint16_t level, std::uint16_t multiplier) noexcept
{
    const auto current = static_cast<std::uint16_t>(std::min(level, envelopePeak));
    return truncatedDecayProduct(current, multiplier);
}

float YouKnowEngine::envelopeDacFraction(std::uint16_t level) noexcept
{
    constexpr float inverseDacPeak = 1.0f / 4095.0f;
    const auto limited = static_cast<std::uint16_t>(
        std::min(level, envelopePeak));
    return static_cast<float>(limited >> 2u) * inverseDacPeak;
}

float YouKnowEngine::envelopeAttackSeconds(float panelPosition) noexcept
{
    const int increment = envelopeAttackIncrement(panelPosition);
    const int passes = (envelopePeak + increment - 1) / increment;
    return static_cast<float>(passes / controlScanHz);
}

float YouKnowEngine::envelopeDecaySeconds(float panelPosition) noexcept
{
    // Keep the conventional -20 dB display point, but reach it through the
    // exact integer helper rather than a continuous exponential estimate.
    const auto multiplier = envelopeDecayReleaseMultiplier(panelPosition);
    std::uint16_t level = envelopePeak;
    const std::uint16_t threshold = envelopePeak / 10u;
    int passes = 0;
    while (level > threshold && passes < 100000)
    {
        level = envelopeReleaseLevel(level, multiplier);
        ++passes;
    }
    return static_cast<float>(passes / controlScanHz);
}

float YouKnowEngine::envelopeReleaseSeconds(float panelPosition) noexcept
{
    const auto multiplier = envelopeDecayReleaseMultiplier(panelPosition);
    std::uint16_t level = envelopePeak;
    int passes = 0;
    while (level != 0u && passes < 100000)
    {
        level = envelopeReleaseLevel(level, multiplier);
        ++passes;
    }
    return static_cast<float>(passes / controlScanHz);
}

float YouKnowEngine::lfoRateHz(float panelPosition) noexcept
{
    // The rate the accumulator mechanism actually produces: whole passes per
    // half-sweep, so fast settings land on the B-2 state machine's quantised
    // grid.
    const int coefficient = lfoRateIncrement(panelPosition);
    const int passes = (8192 + coefficient - 1) / coefficient;
    return static_cast<float>(controlScanHz) / (4.0f * passes);
}

float YouKnowEngine::lfoDelaySeconds(float panelPosition) noexcept
{
    // Firmware completion time: the pass that crosses the silent-hold
    // threshold also performs the fade's first add, so those two stages share
    // one pass rather than being concatenated end to end.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L577-L607
    const int holdIncrement = envelopeAttackIncrement(panelPosition);
    const int fadeIncrement = lfoDelayFadeIncrement(panelPosition);
    const int holdPasses = (16384 + holdIncrement - 1) / holdIncrement;
    const int fadePasses = (65536 + fadeIncrement - 1) / fadeIncrement;
    return static_cast<float>((holdPasses + fadePasses - 1) / controlScanHz);
}

namespace
{
// Bender-board PORTAMENTO network, Service Notes p. 16 (2026-08-20 read):
// VR2 50KB linear track across +5 V, wiper through SW1 into R16 47 kOhm to
// ground at the slave ADC node. Both mapping directions share these values.
constexpr float portamentoTrackOhms = 50000.0f;
constexpr float portamentoLoadOhms = 47000.0f;
} // namespace

float YouKnowEngine::portamentoTravelAdcFraction(float travel) noexcept
{
    // With lower-section resistance x*R_T loaded by R_L, the wiper divider
    // solves to x*R_L / (R_L + x*(1-x)*R_T): exact 0 and 1 at the track
    // ends, 0.39496 at half travel.
    const float x = clamp01(sanitised(travel, 0.0f));
    return x * portamentoLoadOhms
         / (portamentoLoadOhms + x * (1.0f - x) * portamentoTrackOhms);
}

float YouKnowEngine::portamentoTravelForAdcFraction(float fraction) noexcept
{
    // The forward law rearranges to the quadratic
    // f*R_T*x^2 + (R_L - f*R_T)*x - f*R_L = 0, whose positive root inverts
    // it exactly; f = 0 short-circuits the division at a = 0.
    const float f = clamp01(sanitised(fraction, 0.0f));
    if (f == 0.0f)
        return 0.0f;
    const float a = f * portamentoTrackOhms;
    const float b = portamentoLoadOhms - f * portamentoTrackOhms;
    const float c = -f * portamentoLoadOhms;
    const float discriminant = b * b - 4.0f * a * c;
    const float root = (-b + std::sqrt(std::max(discriminant, 0.0f)))
                     / (2.0f * a);
    return clamp01(root);
}

float YouKnowEngine::portamentoSeconds(float panelPosition) noexcept
{
    // The performance pot is read as an eight-bit ADC value. Raw zero is the
    // explicit Off path and raw one selects the observed zero/immediate entry;
    // the active raw pairs select indices 1..127 through raw>>1.
    const int stepUnits = portamentoIncrement(panelPosition);
    if (stepUnits == 0)
        return 0.0f;
    const int passes = (3072 + stepUnits - 1) / stepUnits;
    return static_cast<float>(passes / controlScanHz);
}

float YouKnowEngine::outputReferenceGain(float referenceRmsVolts) noexcept
{
    if (!(referenceRmsVolts > 0.0f) || !std::isfinite(referenceRmsVolts))
        return 1.0f;
    return internalVoltsPerUnit * minus18DbfsAmplitude / referenceRmsVolts;
}

float YouKnowEngine::outputSummerClip(float value) noexcept
{
    static_assert(outputSummerClipExponent == 8.0f);
    if (!std::isfinite(value))
        return 0.0f;

    constexpr float asymptoteUnits =
        outputSummerSwingAsymptoteVolts / internalVoltsPerUnit;
    // The exponent is fixed at eight, so multiplies and three square roots
    // preserve the same algebraic curve without two general-purpose powers.
    // Double keeps even FLT_MAX's normalized eighth power finite, letting an
    // extreme input approach the asymptote instead of folding back to zero.
    const double normalised = std::abs(static_cast<double>(value))
                            / static_cast<double>(asymptoteUnits);
    const double squared = normalised * normalised;
    const double fourth = squared * squared;
    const double eighth = fourth * fourth;
    const double denominator = std::sqrt(std::sqrt(std::sqrt(1.0 + eighth)));
    return static_cast<float>(static_cast<double>(value) / denominator);
}

const std::array<YouKnowEngine::ConverterWrite,
                 YouKnowEngine::converterWritesPerPass>&
YouKnowEngine::converterWriteOrder() noexcept
{
    static constexpr std::array<ConverterWrite, converterWritesPerPass> order {{
        { ConverterDestination::Resonance, -1 },
        { ConverterDestination::CommonVca, -1 },
        { ConverterDestination::Sub, -1 },
        { ConverterDestination::Pitch, 0 },
        { ConverterDestination::Pitch, 1 },
        { ConverterDestination::Pitch, 2 },
        { ConverterDestination::Pitch, 3 },
        { ConverterDestination::Pitch, 4 },
        { ConverterDestination::Pitch, 5 },
        { ConverterDestination::Pwm, -1 },
        { ConverterDestination::Vcf, 0 },
        { ConverterDestination::VoiceVca, 0 },
        { ConverterDestination::Vcf, 1 },
        { ConverterDestination::VoiceVca, 1 },
        { ConverterDestination::Vcf, 2 },
        { ConverterDestination::VoiceVca, 2 },
        { ConverterDestination::Vcf, 3 },
        { ConverterDestination::VoiceVca, 3 },
        { ConverterDestination::Vcf, 4 },
        { ConverterDestination::VoiceVca, 4 },
        { ConverterDestination::Vcf, 5 },
        { ConverterDestination::VoiceVca, 5 },
        { ConverterDestination::Noise, -1 }
    }};
    return order;
}

std::array<double, YouKnowEngine::converterWritesPerPass>
YouKnowEngine::converterEventPhases(ConverterTimingProfile profile) noexcept
{
    std::array<double, converterWritesPerPass> phases {};
    // A traced pass has no data-independent layout: reset/refresh installs
    // its actual offsets from the supplied CPU state, not these zeroes.
    if (profile == ConverterTimingProfile::PhaseZeroDiagnostic
        || profile == ConverterTimingProfile::FirmwareControlNoInterrupt)
        return phases;

    if (profile == ConverterTimingProfile::FirmwareDcoNoInterrupt)
    {
        phases = converterEventPhases(ConverterTimingProfile::MeasuredChartGeometry);
        // Preserve the unresolved first-DCO and non-DCO anchors. The relative
        // five gaps have a stronger source than drafting proportions: the B-2
        // path 0493 -> 0496..04a1 -> 041c..0493 is 867 states for an unclamped,
        // running voice, not the chart's 223.7..227.1 us. Actual branch costs
        // are installed by refreshFirmwareDcoTiming at each logical pass.
        // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L681-L763
        // NEC uPD7810/11 instruction table pp. 17-26 (states, not clocks):
        // https://datasheet4u.com/pdf/298676/UPD7810.pdf#page=17
        // AuditDcoFirmwareTiming independently walks the instruction path and
        // also checks the previously recovered T-334/-323/-389 PIT anchors.
        for (std::size_t ordinal = 4; ordinal < 9; ++ordinal)
            phases[ordinal] = phases[ordinal - 1]
                + firmwareDcoInterWriteStates(false, 60)
                    * controlScanHz / voiceCpuStateHz;
        return phases;
    }

    if (profile == ConverterTimingProfile::MeasuredChartGeometry)
    {
        // The Service Notes p. 8 "D/A & S/H TIMING CHART", measured from the
        // 400 dpi print on 2026-08-20: stroke-center x coordinates of the 24
        // slot boundaries, in page pixels, with the 4.2 ms arrow spanning the
        // leading NOISE write (x 4165.5) to the trailing one (x 6052), i.e.
        // 1886.5 px per pass. The chart is NOISE-first; this queue starts at
        // RESONANCE (x 4216), so each phase is (x - 4216) / 1886.5 with the
        // pass-closing NOISE write taken from the next pass's leading stroke.
        // Boundary uncertainty is +/-3 px (+/-7 us); the widths quantize onto
        // a 10:7:5 drafting grid, so these are the figure's deliberate
        // proportions, not calibrated hardware timestamps.
        constexpr double resonanceStrokePixel = 4216.0;
        constexpr double passSpanPixels = 1886.5;
        constexpr std::array<double, converterWritesPerPass> strokePixels {
            4216.0,  // RESONANCE
            4313.5,  // VCA LEVEL
            4412.5,  // SUB
            4515.5, 4616.5, 4718.5, 4819.5, 4920.0, 5021.0, // DCO CV CH1-6
            5121.0,  // PWM
            5172.0, 5240.5,  // VCF1 / VCA1
            5309.0, 5379.0,  // VCF2 / VCA2
            5450.5, 5521.5,  // VCF3 / VCA3
            5594.0, 5664.5,  // VCF4 / VCA4
            5735.5, 5803.0,  // VCF5 / VCA5
            5877.0, 5982.0,  // VCF6 / VCA6
            6052.0   // NOISE (the chart's next-pass leading stroke)
        };
        for (std::size_t ordinal = 0; ordinal < phases.size(); ++ordinal)
            phases[ordinal] = (strokePixels[ordinal] - resonanceStrokePixel)
                            / passSpanPixels;
        return phases;
    }

    // The service chart establishes sequential activity spread across the
    // pass, but its drawing is not a calibrated timing capture. A normalized
    // ordinal grid is therefore an explicit compatibility profile: enough to
    // preserve non-simultaneous DCO programming without promoting made-up
    // microseconds to JUNO-106 facts.
    for (std::size_t ordinal = 0; ordinal < phases.size(); ++ordinal)
        phases[ordinal] = static_cast<double>(ordinal)
                        / static_cast<double>(phases.size());
    return phases;
}

namespace
{
// Several panel controls have a monotonic law, mapping 0..maxByte hardware
// travel to a realised time or rate, with no closed-form inverse. This
// brute-force nearest-byte search is shared by all of them: it matches in log
// space, proportionally, which is what makes a control's inverse agree with
// its own displayed value at every byte rather than only at the two ends of
// its range. `law` and the loop bounds/divisor still vary per control, so the
// search itself is the only part pulled out.
template <typename Law>
float nearestBytePositionByLogRatio(int minByte, int maxByte, float divisor,
                                    float target, Law law) noexcept
{
    int bestByte = minByte;
    float bestError = std::numeric_limits<float>::infinity();
    for (int byte = minByte; byte <= maxByte; ++byte)
    {
        const float realised = law(static_cast<float>(byte) / divisor);
        const float error = std::abs(std::log(realised / target));
        if (error < bestError)
        {
            bestError = error;
            bestByte = byte;
        }
    }
    return static_cast<float>(bestByte) / divisor;
}
} // namespace

float YouKnowEngine::panelPositionForAttack(float seconds) noexcept
{
    if (!(seconds > 0.0f) || !std::isfinite(seconds))
        return 0.0f;
    if (seconds <= envelopeAttackSeconds(0.0f))
        return 0.0f;
    if (seconds >= envelopeAttackSeconds(1.0f))
        return 1.0f;

    return nearestBytePositionByLogRatio(0, 127, 127.0f, seconds,
                                         envelopeAttackSeconds);
}

float YouKnowEngine::panelPositionForDecay(float seconds) noexcept
{
    if (!(seconds > 0.0f) || !std::isfinite(seconds))
        return 0.0f;
    if (seconds <= envelopeDecaySeconds(0.0f))
        return 0.0f;
    if (seconds >= envelopeDecaySeconds(1.0f))
        return 1.0f;

    return nearestBytePositionByLogRatio(0, 127, 127.0f, seconds,
                                         envelopeDecaySeconds);
}

float YouKnowEngine::panelPositionForRelease(float seconds) noexcept
{
    if (!(seconds > 0.0f) || !std::isfinite(seconds))
        return 0.0f;
    if (seconds <= envelopeReleaseSeconds(0.0f))
        return 0.0f;
    if (seconds >= envelopeReleaseSeconds(1.0f))
        return 1.0f;

    return nearestBytePositionByLogRatio(0, 127, 127.0f, seconds,
                                         envelopeReleaseSeconds);
}

float YouKnowEngine::panelPositionForLfoRate(float hertz) noexcept
{
    if (!(hertz > 0.0f) || !std::isfinite(hertz))
        return 0.0f;
    if (hertz <= lfoRateHz(0.0f))
        return 0.0f;
    if (hertz >= lfoRateHz(1.0f))
        return 1.0f;

    return nearestBytePositionByLogRatio(0, 127, 127.0f, hertz, lfoRateHz);
}

float YouKnowEngine::panelPositionForLfoDelay(float seconds) noexcept
{
    if (!(seconds > 0.0f) || !std::isfinite(seconds))
        return 0.0f;
    if (seconds >= lfoDelaySeconds(1.0f))
        return 1.0f;

    int bestByte = 0;
    float bestError = std::numeric_limits<float>::infinity();
    for (int byte = 0; byte <= 127; ++byte)
    {
        const float realised = lfoDelaySeconds(static_cast<float>(byte) / 127.0f);
        const float error = std::abs(realised - seconds);
        if (error < bestError)
        {
            bestError = error;
            bestByte = byte;
        }
    }
    return static_cast<float>(bestByte) / 127.0f;
}

float YouKnowEngine::panelPositionForPortamento(float secondsPerOctave) noexcept
{
    if (!(secondsPerOctave > 0.0f) || !std::isfinite(secondsPerOctave))
        return 0.0f;
    if (secondsPerOctave >= portamentoSeconds(1.0f))
        return 1.0f;
    if (secondsPerOctave <= portamentoSeconds(2.0f / 255.0f))
        return 2.0f / 255.0f;

    // The realised law has repeated values: raw pairs address one coefficient.
    // The shared search returns the first canonical ADC code producing the
    // closest displayed seconds-per-octave value.
    return nearestBytePositionByLogRatio(2, 255, 255.0f, secondsPerOctave,
                                         portamentoSeconds);
}

float YouKnowEngine::panelPositionForCutoff(float hertz) noexcept
{
    if (!(hertz > 0.0f) || !std::isfinite(hertz))
        return 0.0f;
    const float counts = vcfCountsPerOctave
                       * std::log2(std::max(hertz, vcfBaseFrequencyHz) / vcfBaseFrequencyHz);
    // The travel is read as a 0..127 byte driving the converter 128 counts at
    // a time, so this is the inverse of that quantisation, not of a continuum.
    return std::clamp(counts / (127.0f * 128.0f), 0.0f, 1.0f);
}

std::uint16_t YouKnowEngine::pwmDacCode(
    float panelPosition, PwmSource source, std::uint16_t lfoAccumulator,
    bool positivePolarity) noexcept
{
    // FF47 is the stored byte doubled to 0..254. The two partial MULs at
    // 0773..077B are exactly floor(P * Q / 256); subtracting that from 0x3fff
    // and dropping loadDac's two unused low bits is equivalently
    // 0x0fff - floor(P * Q / 1024).
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L298-L313
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1144-L1197
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1292-L1302
    const std::uint32_t depth = 2u * storedControlByte(panelPosition);
    const std::uint32_t accumulator = std::min<std::uint32_t>(
        lfoAccumulator, 0x1fffu);
    const std::uint32_t modulation = source == PwmSource::Manual
        ? 0x3fffu
        : (positivePolarity ? 0x2000u + accumulator
                            : 0x2000u - accumulator);
    return static_cast<std::uint16_t>(
        0x0fffu - ((depth * modulation) >> 10u));
}

float YouKnowEngine::pwmDacVolts(std::uint16_t code) noexcept
{
    // Roland calibrates code 0x0fff to +6 V / 50% duty. Square Off makes B-2
    // save zero, which the same linear converter path presents as the printed
    // -0.8 V comparator-pinning state. The +0.6 V / 95% row is the loaded
    // physical slider's nominal top, not the seven-bit data format's endpoint.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=9
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=19
    //
    // Checked on 2026-09-22 against #439522's original DCOs, whose manual
    // sweep steps the stored byte through 0..105 (Tools/AnalyzeHardwarePwm.py,
    // hash-pinned): 49.4 % at byte 0 and 0.455 %/byte there, against 50.0 %
    // and 0.443 %/byte here. The two agree within 1.0 % duty at every byte
    // (RMS 0.54 %); 95 % falls at byte 100.0 on the unit and 101.6 here. Where
    // the pot stops is a separate question. Roland's factory bank stores no
    // PWM byte above 105 and holds ten tones exactly on it, while every other
    // control reaches 127, so Roland's own programming unit stopped at 105:
    // 96.5 % on this law, 97.2 % on #439522, at the top of p. 19's 93-97 %.
    const double fraction = static_cast<double>(std::min<std::uint16_t>(
        code, 0x0fffu)) / 4095.0;
    return static_cast<float>(-0.8 + 6.8 * fraction);
}

// Where the comparator's two edges sit within one cycle, as fractions of the
// period. The threshold is the ramp voltage the rise crosses on the way up, and
// the comparator holds until the descending reset passes back through the same
// voltage -- not until the reset begins, where the ramp is still at its
// positive rail. At a high note the reset is a noticeable part of the period,
// so dropping at its start would shorten the high interval by about
// duty * reset: a 95% pulse asked for at the top of the 4' range would come out
// nearer 90%.
float YouKnowEngine::rampSegmentVoltage(float risePosition) noexcept
{
    return 2.0f * clamp01(risePosition) - 1.0f;
}

namespace
{
// Both comparator edges share this reset/rise pair. Callers that need both
// edges -- renderVoice's end-of-sample reconciliation calls both back to
// back with the same arguments -- would otherwise clamp and re-derive the
// same two floats twice per voice per internal sample for no reason.
struct PulseRampGeometry
{
    float reset;
    float rise;
};

PulseRampGeometry pulseRampGeometry(float resetFraction) noexcept
{
    const float reset = std::clamp(resetFraction, 0.0f, 0.25f);
    return { reset, std::max(1.0f - reset, 1.0e-4f) };
}
} // namespace

float YouKnowEngine::pulseRisePhase(float duty, float resetFraction) noexcept
{
    const auto geometry = pulseRampGeometry(resetFraction);
    return geometry.rise * (1.0f - std::clamp(duty, 0.0f, 1.0f));
}

float YouKnowEngine::pulseFallPhase(float duty, float resetFraction) noexcept
{
    const auto geometry = pulseRampGeometry(resetFraction);
    // The ramp runs 0..1 over the rise and falls linearly back over the reset,
    // both mapped to -1..+1. Solving the falling segment for the rise's
    // threshold gives the fraction of the reset spent still above it.
    const float threshold = 1.0f - std::clamp(duty, 0.0f, 1.0f);
    return geometry.rise
         + geometry.reset * std::clamp(1.0f - threshold, 0.0f, 1.0f);
}

float YouKnowEngine::pwmDutyCycle(float controlVolts) noexcept
{
    return pwmDutyCycle(controlVolts, 1.0f);
}

float YouKnowEngine::pwmDutyCycle(float controlVolts,
                                     float rampAmplitudeScale) noexcept
{
    return pwmDutyCycle(controlVolts, rampAmplitudeScale, 6.0f);
}

float YouKnowEngine::pwmDutyCycle(float controlVolts,
                                     float rampAmplitudeScale,
                                     float holdCeilingVolts) noexcept
{
    // Pulse Off writes -0.8 V. That sits below the ramp and leaves the
    // comparator permanently high while the oscillator itself keeps running.
    if (std::isfinite(controlVolts) && controlVolts < 0.0f)
        return 1.0f;

    // The nominal ramp spans 12 V peak to peak. Its compensation hold lags a
    // pitch step, and the optional card-current error changes the same ramp's
    // slope; both therefore move the comparator crossing as well as the saw.
    // The resistor-limited physical slider normally stops near +0.6 V, but
    // B-2 accepts all seven-bit SysEx values. That digital overrange can ask
    // for 0..+0.6 V before finally crossing below zero and pinning the output,
    // so retain the established +6 V / 50% floor but move the lower clamp from
    // the physical slider stop to the comparator ramp's zero-volt rail. The
    // floor is the shared hold's; a card threshold carrying its own offset
    // passes the ceiling that offset moves it to.
    const float ceiling = std::max(sanitised(holdCeilingVolts, 6.0f), 0.0f);
    const float volts = std::clamp(
        sanitised(controlVolts, 6.0f), 0.0f, ceiling);
    const float scale = std::clamp(
        sanitised(rampAmplitudeScale, 1.0f), 0.25f, 4.0f);
    return std::clamp(1.0f - volts / (12.0f * scale), 0.0f, 1.0f);
}

const VcaControlCircuit& YouKnowEngine::voiceVcaControlCircuit() noexcept
{
    static const VcaControlCircuit circuit {
        thermalVoltage,
        VoiceVcaControlLaw::controlFullScaleVolts,
        VoiceVcaControlLaw::turnOnVolts / VoiceVcaControlLaw::controlFullScaleVolts };
    return circuit;
}

const std::array<float, YouKnowEngine::VoiceVcaControlLaw::tableSteps + 1>&
YouKnowEngine::VoiceVcaControlLaw::exactGainTable()
{
    // y + ln y = v solved by Newton in double precision, then normalised on
    // the full-scale entry. From y0 = e^v (v <= 1) or v - ln v (v > 1) the
    // first step lands at or below the root and every later one climbs
    // monotonically towards it, so the iterate never leaves y > 0; a dozen
    // steps are far more than the quadratic tail needs. Built once, off the
    // audio thread, by prepare().
    static const std::array<float, tableSteps + 1> table = []
    {
        std::array<double, tableSteps + 1> solved {};
        constexpr double spanVolts = static_cast<double>(controlFullScaleVolts);
        for (int i = 0; i <= tableSteps; ++i)
        {
            const double v = (static_cast<double>(i) / tableSteps * spanVolts
                              - turnOnVolts) / static_cast<double>(thermalVoltage);
            double y = v > 1.0 ? v - std::log(v) : std::exp(v);
            for (int step = 0; step < 12; ++step)
            {
                const double delta = (y + std::log(y) - v) * y / (y + 1.0);
                y -= delta;
                if (std::abs(delta) <= 1.0e-15 * y)
                    break;
            }
            solved[static_cast<std::size_t>(i)] = y;
        }
        std::array<float, tableSteps + 1> result {};
        const double fullScale = solved[tableSteps];
        for (int i = 0; i <= tableSteps; ++i)
            result[static_cast<std::size_t>(i)] = static_cast<float>(
                solved[static_cast<std::size_t>(i)] / fullScale);
        return result;
    }();
    return table;
}

float YouKnowEngine::VoiceVcaControlLaw::gain(float control) noexcept
{
    const float level = clamp01(sanitised(control, 0.0f));
    if (level <= deadband)
        return 0.0f;
    const auto& table = exactGainTable();
    const float position = level * static_cast<float>(tableSteps);
    // Control 1 is entry tableSteps itself; the clamp keeps its upper lerp
    // neighbour inside the array.
    const int index = std::min(static_cast<int>(position), tableSteps - 1);
    const auto at = static_cast<std::size_t>(index);
    const float fraction = position - static_cast<float>(index);
    return table[at] + (table[at + 1] - table[at]) * fraction;
}

float YouKnowEngine::VoiceVcaControlLaw::softplusGain(float control) noexcept
{
    // The former stand-in: a smooth approximation to the grounded-base
    // stage's shape, normalised so full control is unity gain, with the same
    // exponential tail as the exact law and a slightly fuller knee. Kept
    // verbatim so the comparison switch is bit-exact.
    const float level = clamp01(sanitised(control, 0.0f));
    if (level <= deadband)
        return 0.0f;
    const float x = (level - softplusTurnOn) / knee;
    // log1p(exp(x)) is x to the last bit long before x reaches thirty, and the
    // exponential would overflow well after that; take the limit early so the
    // linear region costs one comparison rather than two transcendentals.
    const float softplus = x > 30.0f ? x : std::log1p(std::exp(x));
    return knee * softplus / (1.0f - softplusTurnOn);
}

float YouKnowEngine::commonVcaControlVolts(float dacFraction) noexcept
{
    const float position = clamp01(dacFraction);
    const float converterVolts = commonVcaDacReferenceVolts
        * commonVcaMaximumDacCode * position / commonVcaDacSteps;
    const float holdVolts = commonVcaBufferOffsetVolts
                          + commonVcaBufferGain * converterVolts;

    // At DC C7 is open, so the held voltage sees R30+R32 in series. GC1 is
    // additionally tied to ground by R31 and biased from +15 V by R165.
    constexpr float holdSeriesOhms = commonVcaR30Ohms + commonVcaR32Ohms;
    return (holdVolts / holdSeriesOhms
            + commonVcaBiasVolts / commonVcaR165Ohms)
         / (1.0f / holdSeriesOhms
            + 1.0f / commonVcaR31Ohms
            + 1.0f / commonVcaR165Ohms);
}

float YouKnowEngine::commonVcaHoldTimeConstantSeconds() noexcept
{
    // With ideal voltage sources AC-grounded, C7 sees R30 in parallel with
    // R32 plus the GC1 bias network. Nominal: 908.249 ohms * 10 uF = 9.08249 ms.
    constexpr float gcBiasOhms =
        commonVcaR31Ohms * commonVcaR165Ohms
        / (commonVcaR31Ohms + commonVcaR165Ohms);
    constexpr float farSideOhms = commonVcaR32Ohms + gcBiasOhms;
    constexpr float theveninOhms =
        commonVcaR30Ohms * farSideOhms
        / (commonVcaR30Ohms + farSideOhms);
    return theveninOhms * commonVcaC7Farads;
}

float YouKnowEngine::patchLevelGain(float dacFraction) noexcept
{
    return patchLevelGain(dacFraction, 25.0f);
}

float YouKnowEngine::patchLevelGain(float dacFraction,
                                    float jackBoardCelsius) noexcept
{
    // NEC's typical control constant is linear in dB and, being two thermal
    // voltages per decibel (commonVcaControlVoltsPerDecibel), proportional to
    // absolute temperature: a stored level's decibels shrink by 298.15 K / T
    // as the jack board warms, towards the 0 dB the part gives at Vc = 0,
    // which no temperature moves. At 25 C the ratio is exactly one and the
    // law is the data book's. Installed rail, resistor, capacitor and IC
    // spread remain measurement questions; they are not replaced here by
    // synthetic random offsets.
    const float voltsPerDecibel = commonVcaControlVoltsPerDecibel
        * ((jackBoardCelsius + 273.15f) / 298.15f);
    const float decibels = commonVcaControlVolts(dacFraction) / voltsPerDecibel;
    return std::pow(10.0f, decibels / 20.0f);
}

float YouKnowEngine::highPassCornerHz(HighPassMode mode) noexcept
{
    // Four legs of a switched network, selected by a CMOS multiplexer: a
    // shelving boost, a straight-through leg, and two progressively higher
    // corners. Each cut leg is its own series capacitor -- C10 15 nF, C11
    // 4.7 nF -- against the same 47 kOhm feed into the summing amplifier's
    // virtual earth, with a 1 MOhm bleed to ground. The 47 kOhm is the timing
    // resistance; the 1 MOhm is far too high to be.
    //
    // Two earlier revisions got this wrong in different ways, and the second
    // was wrong in a way that concealed itself: 15 kOhm against 47 nF has the
    // same product as 47 kOhm against 15 nF, so position 2 came out right by
    // coincidence while position 3 stayed 13 Hz off. Reading the schematic's
    // own designators is what separated them.
    //
    // The boost's corner is the branch's own dominant pole, read at designator
    // level from a complete 300 dpi scan of p. 15 (2026-08-07): Y3 crosses
    // C9 47 nF in parallel with R22 47 kOhm into the node C8 10 nF shunts to
    // ground, so the pole is R22*(C9+C8), or 59.41 Hz, and the section's
    // 72.05 Hz zero all but cancels the 72.34 Hz pole of IC4b's C6-bypassed
    // feedback -- which is why one corner describes a two-stage branch to
    // within 0.016 dB.
    switch (mode)
    {
        case HighPassMode::Boost: return 59.4083f; // R22 x (C9 + C8)
        case HighPassMode::Two:   return 225.8f;   // 47 kOhm x 15 nF
        case HighPassMode::Three: return 720.5f;   // 47 kOhm x 4.7 nF
        case HighPassMode::One:
        default:                  return 59.4083f;
    }
}

float YouKnowEngine::highPassShelfGain(HighPassMode mode) noexcept
{
    // How much of the low band the leg returns. The boost position's DC gain
    // is derived from the branch: the dry R25 leg at unity plus IC4b's
    // DC-coupled leg -- R22 passes DC around C9, C6 leaves the full
    // 1 + R18/R19 = 11 stage gain, and R24 220 kOhm reaches the R29 47 kOhm
    // summing bus -- for 1 + (47/220)*11 = 3.35, or +10.50 dB. A hardware
    // noise sweep independently landed on the same figure, far more than the
    // +3 dB an earlier account reported. The straight-through leg returns the
    // low band untouched, and the two cutting legs discard it.
    switch (mode)
    {
        case HighPassMode::Boost: return 1.0f + (47.0f / 220.0f) * 11.0f;
        case HighPassMode::One:   return 1.0f;
        case HighPassMode::Two:
        case HighPassMode::Three:
        default:                  return 0.0f;
    }
}

float YouKnowEngine::highPassHighGain(HighPassMode mode) noexcept
{
    // The boost leg lifts the high band a little too. Above the corner C9
    // carries the branch and C8 divides it -- C9/(C9+C8) = 47/57 -- while C6
    // shorts IC4b's feedback to unity gain, so the plateau is
    // 1 + (47/220)*(47/57) = 1.17616, or +1.41 dB, exactly where the hardware
    // noise sweep settled. Every other leg passes the high band at unity.
    switch (mode)
    {
        case HighPassMode::Boost:
            return 1.0f + (47.0f / 220.0f) * (47.0f / 57.0f);
        case HighPassMode::One:
        case HighPassMode::Two:
        case HighPassMode::Three:
        default:                  return 1.0f;
    }
}

float YouKnowEngine::outputCouplingCornerHz() noexcept
{
    return rcCornerHz(outputCouplingCapacitanceF,
                       outputCouplingSeriesOhms + outputCouplingPotOhms);
}

float YouKnowEngine::voiceBusCouplingCornerHz() noexcept
{
    return rcCornerHz(voiceBusCouplingCapacitanceF,
                       voiceBusCouplingResistanceOhms);
}

float YouKnowEngine::voiceBusCouplingCornerHz(HighPassMode mode) noexcept
{
    float selectedInputOhms = 0.0f;
    switch (mode)
    {
        case HighPassMode::Boost:
        case HighPassMode::One:
            selectedInputOhms = 47000.0f;
            break;
        case HighPassMode::Two:
        case HighPassMode::Three:
            selectedInputOhms = highPassCutBleedResistanceOhms;
            break;
        default:
            return voiceBusCouplingCornerHz();
    }
    const float parallelOhms =
        voiceBusCouplingResistanceOhms * selectedInputOhms
        / (voiceBusCouplingResistanceOhms + selectedInputOhms);
    return rcCornerHz(voiceBusCouplingCapacitanceF, parallelOhms);
}

float YouKnowEngine::commonVcaInputCouplingCornerHz() noexcept
{
    return rcCornerHz(commonVcaInputCapacitanceF,
                       commonVcaInputResistanceOhms);
}

float YouKnowEngine::moduleCouplingCornerHz() noexcept
{
    return rcCornerHz(moduleCouplingCapacitanceF, moduleCouplingResistanceOhms);
}

float YouKnowEngine::vcaInputCouplingCornerHz() noexcept
{
    return rcCornerHz(vcaInputCouplingCapacitanceF,
                       vcaInputCouplingResistanceOhms);
}

float YouKnowEngine::noiseSourceHighPassHz() noexcept
{
    return rcCornerHz(noiseCouplingCapacitanceF, noiseCouplingLoadOhms);
}

float YouKnowEngine::noiseSourceLowPassHz() noexcept
{
    return rcCornerHz(noiseOtaLoadCapacitanceF, noiseOtaLoadResistanceOhms);
}

float YouKnowEngine::outputBoundaryGain() noexcept
{
    // One internal unit is internalVoltsPerUnit. The provisional summer
    // asymptote through the volume wiper's maximum passband gain defines this
    // model's steady-state 0 dBFS policy; OQ-05 still owns the physical swing.
    const float fullScaleVolts = outputSummerSwingAsymptoteVolts
                               * outputCouplingHighGain(1.0f);
    if (!(fullScaleVolts > 0.0f) || !std::isfinite(fullScaleVolts))
        return 1.0f;
    // Expressed through the same Vref helper every other boundary question
    // uses: the reference is the RMS that lands on -18 dBFS once full scale is
    // the model asymptote, so the two cannot drift apart. The product level
    // policy then scales the whole output by the headroom the factory audit
    // measured as unused; see outputLevelPolicyDb.
    return outputLevelPolicyGain
         * outputReferenceGain(minus18DbfsAmplitude * fullScaleVolts);
}

float YouKnowEngine::outputSummerResistorNoiseDensity() noexcept
{
    // Each input resistor's own voltage noise is multiplied by its inverting
    // signal gain Rf/Rin. The feedback resistor appears directly at the
    // output. Uncorrelated sources add as powers, never as amplitudes.
    const float equivalentResistance = outputSummerFeedbackOhms
        + outputSummerFeedbackOhms * outputSummerFeedbackOhms
            / outputSummerDryInputOhms
        + outputSummerFeedbackOhms * outputSummerFeedbackOhms
            / outputSummerWetInputOhms;
    return std::sqrt(4.0f * boltzmannConstant * outputNoiseTemperatureKelvin
                     * equivalentResistance);
}

float YouKnowEngine::outputWiperNoiseResistance(
    float volumePosition) noexcept
{
    // At thermal equilibrium the complete passive network's open-circuit
    // voltage noise is 4kTR_th. With IC6's ideal small-signal output grounded,
    // the wiper sees its upper track plus R54/R57, its lower track, the selector
    // ladder and the headphone-amplifier input as parallel paths to ground.
    const float position = clamp01(sanitised(volumePosition, 0.0f));
    const float upper = outputCouplingSeriesOhms
                      + (1.0f - position) * outputCouplingPotOhms;
    const float lower = position * outputCouplingPotOhms;
    if (lower == 0.0f)
        return 0.0f;
    float conductance = 1.0f / upper + 1.0f / outputWiperInternalLoadOhms;
    conductance += 1.0f / lower;
    return 1.0f / conductance;
}

float YouKnowEngine::outputCouplingHighGain() noexcept
{
    return outputCouplingPotOhms
         / (outputCouplingSeriesOhms + outputCouplingPotOhms);
}

float YouKnowEngine::outputCouplingCornerHz(float volumePosition) noexcept
{
    return rcCornerHz(
        outputCouplingCapacitanceF,
        outputCouplingWiperNetworkFor(volumePosition).resistance);
}

float YouKnowEngine::outputCouplingHighGain(float volumePosition) noexcept
{
    const auto network = outputCouplingWiperNetworkFor(volumePosition);
    if (!(network.loadedLower > 0.0f))
        return 0.0f;
    return network.loadedLower / network.resistance;
}

float YouKnowEngine::outputJackCornerHz(float volumePosition) noexcept
{
    return outputJackCornerHzFor(outputCouplingWiperNetworkFor(volumePosition));
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

double YouKnowEngine::midiToHz(double midiNote) noexcept
{
    return 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
}

std::uint32_t YouKnowEngine::hash32(std::uint32_t value) noexcept
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

std::uint32_t YouKnowEngine::xorshift32(std::uint32_t state) noexcept
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float YouKnowEngine::bipolarFromState(std::uint32_t state) noexcept
{
    return static_cast<float>(state & 0xffffffu) * (2.0f / 16777215.0f) - 1.0f;
}

float YouKnowEngine::gaussianFromNoiseState() noexcept
{
    // Tr21's reverse-biased emitter-base junction is an avalanche source:
    // each internal sample sums a great many independent carrier events, so
    // the rail's amplitude statistics are Gaussian by the central limit
    // theorem -- the same reading the TP8 crest-factor interpretation in the
    // decision log already assumes. A uniform draw has a crest factor that
    // the C42/C41 shaping then moves with the internal rate (about 3.1 at
    // 44.1 kHz/1x against 4.35 at 192 kHz/4x for the same RMS), so the peak
    // reaching the transconductor and BA662 pairs changed with the quality
    // rung. Marsaglia's polar method draws pairs of uniforms through the
    // same xorshift32 state; the result is unit-variance and scaled below to
    // the uniform's 1/sqrt(3) RMS so every established level coordinate,
    // rate normalisation and RMS-based calibration stays put. Clamped at six
    // sigma (two draws in a billion) so no finite quantity downstream sees an
    // unbounded sample.
    if (noiseGaussianSpareValid_)
    {
        noiseGaussianSpareValid_ = false;
        return noiseGaussianSpare_;
    }
    constexpr float unitVarianceToUniformRms = 0.57735027f; // 1/sqrt(3)
    constexpr float clamp = 6.0f * unitVarianceToUniformRms;
    for (;;)
    {
        noiseState_ = xorshift32(noiseState_);
        const float u = bipolarFromState(noiseState_);
        noiseState_ = xorshift32(noiseState_);
        const float v = bipolarFromState(noiseState_);
        const float radius = u * u + v * v;
        if (radius >= 1.0f || radius <= 1.0e-12f)
            continue;
        const float scale = std::sqrt(-2.0f * std::log(radius) / radius)
                          * unitVarianceToUniformRms;
        noiseGaussianSpare_ = std::clamp(v * scale, -clamp, clamp);
        noiseGaussianSpareValid_ = true;
        return std::clamp(u * scale, -clamp, clamp);
    }
}

float YouKnowEngine::hashBipolar(std::uint32_t value) noexcept
{
    return bipolarFromState(hash32(value));
}

float YouKnowEngine::resetFraction(double periodSeconds) noexcept
{
    if (!(periodSeconds > 0.0))
        return 0.25f;
    const double fraction = static_cast<double>(rampResetSeconds) / periodSeconds;
    return static_cast<float>(std::clamp(fraction, 1.0e-6, 0.25));
}

double YouKnowEngine::dcoPositiveBaseRail(
    double totalRampScale) noexcept
{
    // The stored ramp state x is mapped to physical volts as
    // V = 6 * totalScale * (x + 1). Solve V=+15 V for x rather than clamping
    // x itself: compensation and card-current scale are part of the same
    // physical ramp and therefore move its base-coordinate supply crossing.
    const double safeScale = std::max(totalRampScale, 1.0e-12);
    return static_cast<double>(dcoPositiveRailVolts)
         / (0.5 * static_cast<double>(rampAmplitudeVolts) * safeScale)
         - 1.0;
}

// ---------------------------------------------------------------------------
// Bandlimiting
// ---------------------------------------------------------------------------

void YouKnowEngine::BandlimitedTrack::reset() noexcept
{
    ring.fill(0.0f);
    delay.fill(0.0f);
    base = 0;
    primed = false;
}

void YouKnowEngine::BandlimitedTrack::prime(float value) noexcept
{
    if (primed)
        return;
    delay.fill(value);
    primed = true;
}

float YouKnowEngine::BandlimitedTrack::advance(float naive) noexcept
{
    prime(naive);

    const int delayIndex = base % correctionHalfWidth;
    const float output = delay[static_cast<std::size_t>(delayIndex)]
                       + ring[static_cast<std::size_t>(base)];
    ring[static_cast<std::size_t>(base)] = 0.0f;
    delay[static_cast<std::size_t>(delayIndex)] = naive;
    base = base + 1 < correctionRing ? base + 1 : 0;

    return output;
}

const YouKnowEngine::CorrectionTables&
YouKnowEngine::correctionTables() noexcept
{
    static const CorrectionTables tables = [] {
        // Integrate a Blackman-windowed sinc to obtain the continuous
        // bandlimited step. Integrating once more gives the bandlimited ramp.
        // The ideal step is deliberately not subtracted here: its residual has
        // a unit jump at t=0, and interpolating that discontinuous table made
        // the lookup interval before zero emit a premature fractional edge.
        // The slope residual is continuous at zero, so it remains stored
        // directly and retains precision near the kernel boundary.
        constexpr int length = correctionTableLength;
        constexpr double step = 1.0 / correctionOversample;

        // A finite reconstruction kernel needs a transition band of its own.
        // Centring the ideal cutoff exactly on Nyquist left the newly recovered
        // B-2 note-84/8' grid's 24.109 kHz pulse harmonic only 66.1 dB down
        // when it folded to 19.991 kHz at a 44.1 kHz host. A 1.5%-of-Nyquist
        // guard restores more than 80 dB rejection there while retaining the
        // complete 0..15 kHz strict band and staying within 0.15 dB at 20 kHz.
        constexpr double cutoff = 0.985;
        std::array<double, length> impulse {};
        for (int i = 0; i < length; ++i)
        {
            const double t = static_cast<double>(i) * step
                           - correctionHalfWidth;
            const double sinc = std::abs(t) < 1.0e-12
                ? cutoff
                : std::sin(3.14159265358979323846 * cutoff * t)
                    / (3.14159265358979323846 * t);
            const double phase = static_cast<double>(i)
                               / static_cast<double>(length - 1);
            const double window = 0.42
                - 0.5 * std::cos(2.0 * 3.14159265358979323846 * phase)
                + 0.08 * std::cos(4.0 * 3.14159265358979323846 * phase);
            impulse[static_cast<std::size_t>(i)] = sinc * window;
        }

        // Trapezoidal running integral, normalised so the step ends at one.
        std::array<double, length> stepResponse {};
        double accumulator = 0.0;
        for (int i = 1; i < length; ++i)
        {
            accumulator += 0.5 * step
                         * (impulse[static_cast<std::size_t>(i - 1)]
                            + impulse[static_cast<std::size_t>(i)]);
            stepResponse[static_cast<std::size_t>(i)] = accumulator;
        }
        const double total = stepResponse[length - 1];
        if (std::abs(total) > 1.0e-12)
            for (auto& value : stepResponse)
                value /= total;

        std::array<double, length> rampResponse {};
        accumulator = 0.0;
        for (int i = 1; i < length; ++i)
        {
            accumulator += 0.5 * step
                         * (stepResponse[static_cast<std::size_t>(i - 1)]
                            + stepResponse[static_cast<std::size_t>(i)]);
            rampResponse[static_cast<std::size_t>(i)] = accumulator;
        }

        CorrectionTables result;
        for (int i = 0; i < length; ++i)
        {
            result.stepResponse[static_cast<std::size_t>(i)] =
                static_cast<float>(stepResponse[static_cast<std::size_t>(i)]);
            const double t = static_cast<double>(i) * step
                           - correctionHalfWidth;
            const double idealRamp = t >= 0.0 ? t : 0.0;
            result.slopeResidual[static_cast<std::size_t>(i)] =
                static_cast<float>(rampResponse[static_cast<std::size_t>(i)]
                                   - idealRamp);
        }
        return result;
    }();
    return tables;
}

// The linear-interpolated read of an oversampled correction table at ring
// sample `ringIndex`, `offset` subsamples into it. addStep and addSlope both
// walk their own table this same way, sample for sample -- factored out so
// neither repeats the identical clamp/lerp arithmetic for every one of
// correctionRing samples of every event.
float YouKnowEngine::interpolatedCorrectionSample(
    const std::array<float, correctionTableLength>& table,
    int ringIndex, float offset) noexcept
{
    const float position = (static_cast<float>(ringIndex) + offset)
                         * static_cast<float>(correctionOversample);
    const int lower = std::clamp(static_cast<int>(position), 0,
                                 correctionTableLength - 2);
    const float fraction = std::clamp(
        position - static_cast<float>(lower), 0.0f, 1.0f);
    return table[static_cast<std::size_t>(lower)]
         + (table[static_cast<std::size_t>(lower + 1)]
            - table[static_cast<std::size_t>(lower)]) * fraction;
}

// `samplesAgo` is how far back inside the sample just rendered the event sits,
// in [0, 1]. Output sample `j` of the correction ring is `j - halfWidth`
// samples away from the sample just rendered, so the residual is read at
// `j - halfWidth + samplesAgo` and the table is offset by the half width.
//
// The continuous response table is read with linear interpolation, not nearest
// neighbour. The ideal step is then evaluated exactly at the query time.
// Keeping the discontinuity out of the interpolated data is essential: even a
// dense table otherwise blends across the unit jump immediately before t=0.
//
// A query time of exactly zero is the naive sample sitting on the event, and
// which side of the step that sample holds depends on where the event was
// found. Inside the sample just rendered (samplesAgo < 1) that sample already
// carries the new level, so the ideal step is subtracted from it. At
// samplesAgo == 1 -- eventSamplesAgo(0.0), the left-boundary comparator
// reconciliation and the two control-word sub flips -- the event sits on the
// previous sample's instant, and that sample was rendered before the event at
// the old level: subtracting the step there wrote h * (0.5 - 1) into slot
// halfWidth - 1 instead of h * 0.5, a full-swing one-sample spike on every
// such edge (-2.0 against -0.002 one 1/64 grid step earlier for a +2 step).
void YouKnowEngine::addStep(BandlimitedTrack& track, float height,
                               float samplesAgo) const noexcept
{
    if (!(std::abs(height) > 0.0f))
        return;
    const auto& table = correctionTables().stepResponse;
    const float offset = std::clamp(samplesAgo, 0.0f, 1.0f);
    // `track.base + j` only ever wraps the ring once as j runs 0..correctionRing-1,
    // since track.base already sits in [0, correctionRing). Walking `slot` forward
    // with the same increment-or-reset the ring's own writer uses (see
    // BandlimitedTrack::advance) reaches the identical index every iteration
    // without a modulo by the non-power-of-two ring size on each of them.
    int slot = track.base;
    for (int j = 0; j < correctionRing; ++j)
    {
        const float response = interpolatedCorrectionSample(table, j, offset);
        const float queryTime = static_cast<float>(j - correctionHalfWidth)
                              + offset;
        const bool afterEvent = queryTime > 0.0f
                             || (queryTime == 0.0f && offset < 1.0f);
        const float residual = response - (afterEvent ? 1.0f : 0.0f);
        track.ring[static_cast<std::size_t>(slot)] += height * residual;
        slot = slot + 1 < correctionRing ? slot + 1 : 0;
    }
}

void YouKnowEngine::addSlope(BandlimitedTrack& track, float slopeStep,
                                float samplesAgo) const noexcept
{
    if (!(std::abs(slopeStep) > 0.0f))
        return;
    const auto& table = correctionTables().slopeResidual;
    const float offset = std::clamp(samplesAgo, 0.0f, 1.0f);
    // See addStep's identical walk above for why this avoids a per-iteration
    // modulo.
    int slot = track.base;
    for (int j = 0; j < correctionRing; ++j)
    {
        const float residual = interpolatedCorrectionSample(table, j, offset);
        track.ring[static_cast<std::size_t>(slot)] += slopeStep * residual;
        slot = slot + 1 < correctionRing ? slot + 1 : 0;
    }
}

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

void YouKnowEngine::Envelope::reset() noexcept
{
    stage = EnvelopeStage::Idle;
    level = 0u;
    value = 0.0f;
    attackPhase = decayPhase = phase = gate = running = false;
}

void YouKnowEngine::Envelope::noteOn() noexcept
{
    // 0115..013B sets gate/attack and clears FF33 only if FF11 was set.
    // The pending run snapshot and accumulator survive a voice command.
    gate = attackPhase = true;
    if (running)
        phase = false;
    stage = EnvelopeStage::Attack;
}

void YouKnowEngine::Envelope::noteOff(bool hold) noexcept
{
    // 009F..00B5 never clears FF07 or FF08. HOLD also preserves FF33.
    gate = false;
    if (!hold)
    {
        phase = false;
        if (stage != EnvelopeStage::Idle)
            stage = EnvelopeStage::Release;
    }
}

void YouKnowEngine::Envelope::latchGate(bool hold) noexcept
{
    // 02F2..02FF. Pedal release itself changes only the HOLD flag; FF11
    // changes here, not halfway through the following converter train.
    running = gate || (hold && running);
}

float YouKnowEngine::Envelope::tick(std::uint16_t attackIncrement,
                                   std::uint16_t decayMultiplier,
                                   std::uint16_t sustain,
                                   std::uint16_t releaseMultiplier) noexcept
{
    // B-2 0503..0590. The latches, not the display stage, choose the branch.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L825-L897
    const bool attack = running && !phase;
    const bool decay = (running && phase)
                    || (!running && attackPhase && decayPhase);
    if (attack)
    {
        decayPhase = false;
        const std::uint32_t next = static_cast<std::uint32_t>(level)
                                 + attackIncrement;
        // ONI A,$C0 tests overflow, not equality with 3FFF. Preserve FF07
        // after overflow: a note-off can still select the decay branch once.
        if (next > envelopePeak)
        {
            level = envelopePeak;
            phase = decayPhase = true;
            stage = EnvelopeStage::Decay;
        }
        else
        {
            level = static_cast<std::uint16_t>(next);
            stage = EnvelopeStage::Attack;
        }
    }
    else if (decay)
    {
        attackPhase = false;
        level = envelopeDecayLevel(level, sustain, decayMultiplier);
        stage = level <= sustain ? EnvelopeStage::Sustain : EnvelopeStage::Decay;
    }
    else
    {
        attackPhase = decayPhase = phase = false;
        level = envelopeReleaseLevel(level, releaseMultiplier);
        stage = level == 0u ? EnvelopeStage::Idle : EnvelopeStage::Release;
    }

    // The two low recurrence bits stay in RAM and influence the next pass,
    // but they do not reach the 12-bit converter on this pass.
    value = envelopeDacFraction(level);
    return value;
}


// ---------------------------------------------------------------------------
// Oscillator, filter and high-pass state
// ---------------------------------------------------------------------------

std::uint32_t YouKnowEngine::Dco::mode3HalfClocks(
    std::uint32_t count, bool outHigh) noexcept
{
    return outHigh ? (count + 1u) / 2u : count / 2u;
}

bool YouKnowEngine::Dco::programMode3(
    double clocksToNextInputEdge) noexcept
{
    // A mode word resets the CE and forces OUT high immediately. The selected
    // input clock keeps running while CE waits for the later LSB/MSB pair.
    // IC35/TP5 is one shared clock, so the engine supplies that global phase
    // rather than recovering six incompatible phases from the PIT counters.
    const double nextClock = std::isfinite(clocksToNextInputEdge)
                          && clocksToNextInputEdge > 0.0
        ? clocksToNextInputEdge : 1.0;

    const bool positiveEdge = !pitOutHigh;
    pitOutHigh = true;
    pendingDividerValid = false;
    pitState = PitState::awaitingCount;
    pitClocksToEvent = nextClock;
    return positiveEdge;
}

void YouKnowEngine::Dco::stageMode3Count(std::uint32_t count) noexcept
{
    // The count register has one value, so another complete pair before the
    // transfer replaces the older pending pair -- including a write equal to
    // the active CE count.
    pendingDivider = count;
    pendingDividerValid = true;
    if (pitState == PitState::awaitingCount)
        pitState = PitState::awaitingInitialLoad;
}

YouKnowEngine::Dco::PitEvent
YouKnowEngine::Dco::consumePitEvent() noexcept
{
    if (pitState == PitState::awaitingInitialLoad)
    {
        if (pendingDividerValid)
            divider = pendingDivider;
        pendingDividerValid = false;
        pitState = PitState::running;
        pitOutHigh = true;
        pitClocksToEvent = static_cast<double>(
            mode3HalfClocks(divider, true));
        return PitEvent::initialLoad;
    }

    pitOutHigh = !pitOutHigh;
    if (pendingDividerValid)
        divider = pendingDivider;
    pendingDividerValid = false;
    pitClocksToEvent = static_cast<double>(
        mode3HalfClocks(divider, pitOutHigh));
    return pitOutHigh ? PitEvent::risingEdge : PitEvent::fallingEdge;
}

void YouKnowEngine::Dco::reset() noexcept
{
    pendingDivider = divider;
    pendingDividerValid = false;
    pitState = PitState::stopped;
    pitOutHigh = true;
    pitWriteState = PitWriteState::idle;
    pitWriteDivider = divider;
    cpuStatesToWrite = 0.0;
    coldInitialLoadPending = false;
    pitClocksToEvent = 0.0;
    rampValue = -1.0;
    rampSlopePerSecond = 0.0;
    resetSecondsRemaining = 0.0;
    physicalResetActive = false;
    resetTargetValue = -1.0;
    resetTimeConstant = 1.0;
    resetSawCorrection.fill(0.0);
    positiveRailHeld = false;
    renderScale = 1.0f;
    pulseState = -1.0f;
    subState = 1.0f;
    saw.reset();
    pulse.reset();
    sub.reset();
}

void YouKnowEngine::beginDcoDischarge(
    Voice& voice, double samplesAgo, bool addCorrections) noexcept
{
    auto& dco = voice.dco;
    dco.positiveRailHeld = false;
    const double intervalSeconds = 1.0 / oversampledRate_;
    const double oldSlope = dcoCorrectionSlope(
        dco.rampSlopePerSecond * static_cast<double>(dco.renderScale) * voice.rampCurrentScale
        * intervalSeconds);
    const double periodSeconds = std::max(
        dco.periodSamples / oversampledRate_, 1.0e-12);
    const double resetSeconds = std::max(
        static_cast<double>(resetFraction(periodSeconds)) * periodSeconds,
        1.0e-12);
    dco.rampSlopePerSecond = (-1.0 - dco.rampValue) / resetSeconds;
    dco.resetSecondsRemaining = resetSeconds;
    if (dcoResetCircuitEnabled_)
    {
        dco.physicalResetActive = true;
        dco.resetSecondsRemaining = dcoResetCalibration_.gateSeconds;
        refreshDcoResetTrajectory(voice);
    }
    const double newSlope = dcoCorrectionSlope(
        dco.rampSlopePerSecond * static_cast<double>(dco.renderScale) * voice.rampCurrentScale
        * intervalSeconds);
    if (addCorrections && dco.saw.primed)
        addDcoSlope(voice, newSlope - oldSlope, samplesAgo);

    const float nextSub = -dco.subState;
    if (addCorrections && dco.sub.primed)
    {
        // Tr19's storage time delays the edge that ends its saturation -- the
        // start of the D6-conducting +1 half-cycle -- and nothing delays the
        // other. Later inside the sample just rendered is fewer samples ago;
        // addStep clamps at zero, so an edge closer than t_s to the sample
        // boundary (0.04 of a 192 kHz sample) keeps only part of its skew.
        const double stepSamplesAgo =
            nextSub > 0.0f && activeParameters_.enableSubStorageSkew
                ? samplesAgo - subSwitchStorageSeconds * oversampledRate_
                : samplesAgo;
        addStep(dco.sub, nextSub - dco.subState, static_cast<float>(stepSamplesAgo));
    }
    dco.subState = nextSub;
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(dcoSubTransitions, 1);
#endif
}

void YouKnowEngine::beginDcoCharge(
    Voice& voice, double samplesAgo, bool addCorrections) noexcept
{
    auto& dco = voice.dco;
    dco.positiveRailHeld = false;
    const double intervalSeconds = 1.0 / oversampledRate_;
    const double oldSlope = dcoCorrectionSlope(
        dco.rampSlopePerSecond * static_cast<double>(dco.renderScale) * voice.rampCurrentScale
        * intervalSeconds);
    const bool retainedReset = dco.physicalResetActive;
    dco.physicalResetActive = false;
    if (!retainedReset)
        dco.rampValue = -1.0;
    dco.resetSecondsRemaining = 0.0;
    if (!retainedReset)
        dco.renderScale = std::max(dcoLaunchScale(voice), 1.0e-12f);
    dco.rampSlopePerSecond = dcoChargingSlope(
        voice.dcoCv, activeParameters_.range) / dco.renderScale;
    const double newSlope = dcoCorrectionSlope(
        dco.rampSlopePerSecond * static_cast<double>(dco.renderScale) * voice.rampCurrentScale
        * intervalSeconds);
    if (addCorrections && dco.saw.primed)
        addDcoSlope(voice, newSlope - oldSlope, samplesAgo);
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(dcoCycleWraps, 1);
#endif
}

void YouKnowEngine::updateActiveDcoPeriod(
    Dco& dco, DcoRange range) noexcept
{
    const double frequency = dcoQuantisedFrequency(dco.divider, range);
    dco.periodSamples = frequency > 0.0
                      ? oversampledRate_ / frequency : 1.0e6;
}

void YouKnowEngine::beginRangeClockTransition(
    DcoRange previous, DcoRange next) noexcept
{
    if (previous == next)
        return;

    const double previousClockHz = actualRangeClockHz(previous);
    const double nextClockHz = actualRangeClockHz(next);
    const double rateRatio = nextClockHz / previousClockHz;
    const double clockTolerance = std::max(
        1.0e-15, (1.0 / oversampledRate_) * 1.0e-9) * previousClockHz;
    const double oldClocksToFalling = rangeClockClocksToFallingEdge_;
    double oldClocksToReload = rangeClockClocksToReload_;
    if (!rangeClockTransitionPending_)
    {
        const double rawTickClocks = rangeClockHz(previous) / masterClockHz;
        const double highClocks = 1.0 - rawTickClocks;
        // At exact reload equality, the old preset wins before the PF write.
        // The new preset is therefore captured at the following reload. This
        // compatibility policy is independent of PIT /WR ordering at falling.
        oldClocksToReload = oldClocksToFalling
                                > highClocks + clockTolerance
            ? oldClocksToFalling - highClocks
            : oldClocksToFalling + rawTickClocks;
    }

    const bool fallingBeforeReload =
        oldClocksToFalling + clockTolerance < oldClocksToReload;
    const double newClocksToReload = oldClocksToReload * rateRatio;
    const double newRawTickClocks = rangeClockHz(next) / masterClockHz;
    const double firstNewFallingAfterReload =
        newClocksToReload + 1.0 - newRawTickClocks;
    const double newClocksToFalling = fallingBeforeReload
        ? oldClocksToFalling * rateRatio
        : firstNewFallingAfterReload;
    for (auto& voice : voices_)
    {
        auto& dco = voice.dco;
        if (dco.rampSlopePerSecond > 0.0 && !dco.physicalResetActive)
        {
            // Preserve C54 charge and its frozen coordinate scale. Only
            // charging current changes; the discharge transistor and a
            // capacitor already held at its rail keep their state. Waiting
            // for the next PIT reset incorrectly integrated the old current
            // through the first part of a newly selected octave.
            const double oldSlope = dco.rampSlopePerSecond;
            dco.rampSlopePerSecond *= dcoChargingResistance(previous)
                                   / dcoChargingResistance(next);
            if (dco.saw.primed)
                addDcoSlope(voice, dcoCorrectionSlope(
                    (dco.rampSlopePerSecond - oldSlope)
                    * static_cast<double>(dco.renderScale) * voice.rampCurrentScale
                    / oversampledRate_), 1.0f);
        }
        if (dco.pitState == Dco::PitState::stopped
            || !(dco.pitClocksToEvent > 0.0))
            continue;

        // Preserve the number of TP5 falling/count edges still owed. A PF
        // write made during TP5's one-raw-tick low interval changes the very
        // next falling edge; a write made earlier leaves the imminent old
        // falling edge intact and changes the following interval.
        const double fallingEdgesAfterNext = std::max(
            0.0, std::round(
                dco.pitClocksToEvent - oldClocksToFalling));
        dco.pitClocksToEvent = newClocksToFalling
                             + fallingEdgesAfterNext;
    }

    rangeClockClocksToFallingEdge_ = newClocksToFalling;
    rangeClockClocksToReload_ = newClocksToReload;
    rangeClockTransitionPending_ = true;
}

double YouKnowEngine::rangeClockClocksToNextFallingEdge(
    double elapsedSeconds, DcoRange range) const noexcept
{
    const double clockHz = actualRangeClockHz(range);
    const double clocksAdvanced = std::max(0.0, elapsedSeconds) * clockHz;
    const double clockTolerance = std::max(
        1.0e-15, (1.0 / oversampledRate_) * 1.0e-9) * clockHz;
    const auto stablePhase = [clockTolerance](double clocks) {
        double phase = std::fmod(clocks, 1.0);
        if (phase < 0.0)
            phase += 1.0;
        if (phase <= clockTolerance || 1.0 - phase <= clockTolerance)
            phase = 1.0;
        return phase;
    };

    if (!rangeClockTransitionPending_)
        return stablePhase(
            rangeClockClocksToFallingEdge_ - clocksAdvanced);

    if (clocksAdvanced + clockTolerance < rangeClockClocksToReload_)
    {
        if (clocksAdvanced + clockTolerance
            < rangeClockClocksToFallingEdge_)
        {
            return rangeClockClocksToFallingEdge_ - clocksAdvanced;
        }

        // The old-modulus falling edge has happened, but its reload has not.
        // The following falling edge is one selected period minus one raw
        // 8 MHz tick after that pending reload.
        return 1.0 - std::max(
            0.0, clocksAdvanced - rangeClockClocksToFallingEdge_);
    }

    const double clocksAfterReload =
        std::max(0.0, clocksAdvanced - rangeClockClocksToReload_);
    return stablePhase(
        1.0 - rangeClockHz(range) / masterClockHz - clocksAfterReload);
}

void YouKnowEngine::advanceRangeClock(DcoRange range) noexcept
{
    const double clockHz = actualRangeClockHz(range);
    const double intervalSeconds = 1.0 / oversampledRate_;
    const double clockTolerance = std::max(
        1.0e-15, intervalSeconds * 1.0e-9) * clockHz;
    const double clocksAdvanced = clockHz * intervalSeconds;
    const double nextFalling = rangeClockClocksToNextFallingEdge(
        intervalSeconds, range);
    if (rangeClockTransitionPending_)
    {
        if (clocksAdvanced + clockTolerance < rangeClockClocksToReload_)
        {
            rangeClockClocksToReload_ -= clocksAdvanced;
            rangeClockClocksToFallingEdge_ = nextFalling;
            return;
        }

        rangeClockClocksToReload_ = 0.0;
        rangeClockTransitionPending_ = false;
    }
    rangeClockClocksToFallingEdge_ = nextFalling;
}

void YouKnowEngine::writeDcoMode3Control(
    Voice& voice, double clocksToNextInputEdge, double samplesAgo,
    bool addCorrections) noexcept
{
    auto& dco = voice.dco;
    const bool coldStart = dco.pitState == Dco::PitState::stopped;
    dco.coldInitialLoadPending = dco.coldInitialLoadPending || coldStart;

    // Mode programming forces OUT high. That is a physical C54/sub event only
    // when the previously stored output was low; an already-high output does
    // not acquire a fabricated edge or a fabricated sub polarity.
    if (dco.programMode3(clocksToNextInputEdge))
        beginDcoDischarge(voice, samplesAgo, addCorrections);
}

void YouKnowEngine::prestageDcoPitchTransaction(
    Voice& voice, double clocksToNextInputEdge, double samplesAgo,
    bool addCorrections) noexcept
{
    if (activeConverterTimingProfile_ == ConverterTimingProfile::PhaseZeroDiagnostic
        && nextConverterWrite_ == converterWritesPerPass
        && !converterNextPassPortamentoUpdated_)
    {
        // The deliberately collapsed diagnostic has no interval between SUB
        // and DCO CV, although PIT preparation precedes both. Bootstrap the
        // upcoming pass's six glide words together before its first prep and
        // let that pass's SUB consume the same update. This is an explicit
        // diagnostic policy, not a claim about physical B-2 instruction order.
        for (int card = 0; card < hardwareVoices; ++card)
            updateVoicePortamento(voices_[static_cast<std::size_t>(card)], activeParameters_);
        converterNextPassPortamentoUpdated_ = true;
    }
    auto& dco = voice.dco;
    const float previousCvTarget = voice.dcoCvTarget;
    const std::uint32_t count = updateVoicePitch(
        voice, activeParameters_);
    const float cvTarget = voice.dcoCvTarget;
    voice.dcoCvTarget = previousCvTarget;

    // FF00 is cleared at04B8 before the later DI/control instruction. The
    // continuing trace has already selected that branch even though its RAM
    // request is now clear; only a command restart abandons that local choice.
    const bool writesControlWord = activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt
        && voice.cardIndex < hardwareVoices
        ? (firmwarePassResetMask_ & (1u << voice.cardIndex)) != 0
        : voice.dcoResetPending;
    voice.dcoResetPending = false;
    voice.dcoPitchTransactionValid = true;
    voice.dcoPitchTransactionColdStart = writesControlWord
        && dco.pitState == Dco::PitState::stopped;
    voice.dcoPitchTransactionCvTarget = cvTarget;
    dco.pitWriteDivider = count;

    if (writesControlWord)
        writeDcoMode3Control(
            voice, clocksToNextInputEdge, samplesAgo, addCorrections);
    dco.pitWriteState = Dco::PitWriteState::awaitingLsb;
    dco.cpuStatesToWrite = pitControlToLsbStates;
}

void YouKnowEngine::programDcoCount(
    Voice& voice, std::uint32_t count, bool writesControlWord) noexcept
{
    auto& dco = voice.dco;
    dco.pitWriteDivider = count;
    if (!writesControlWord)
    {
        // Direct calls serve extension slots and the construction-only first
        // PhaseZeroDiagnostic pitch event, neither of which has a preceding
        // physical pre-stage. Preserve their established fallback anchor: LSB
        // now, then the recovered 11-state (2.75 us) spacing to MSB.
        // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L732-L741
        dco.pitWriteState = Dco::PitWriteState::awaitingMsb;
        dco.cpuStatesToWrite = pitLsbToMsbStates;
        return;
    }

    const bool coldStart = dco.pitState == Dco::PitState::stopped;
    if (coldStart)
    {
        // Engine construction has no earlier firmware scan from which to
        // inherit a charged DCO hold. Retain the established deterministic
        // initialization policy: the first programmed cell starts settled.
        voice.dcoCv = voice.dcoCvTarget;
    }
    writeDcoMode3Control(
        voice, rangeClockClocksToFallingEdge_, 1.0f, true);

    // Direct cold-start fallback anchors the control store here. IC29 B-2's
    // reset branch then reaches LSB after 55 CPU states, and both paths take
    // 11 more states to MSB. Ordinary physical transactions use their
    // recovered T-389/T-334/T-323 positions instead.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L783-L794
    // https://datasheet4u.com/pdf/298676/UPD7810.pdf#page=17
    dco.pitWriteState = Dco::PitWriteState::awaitingLsb;
    dco.cpuStatesToWrite = pitControlToLsbStates;
}

void YouKnowEngine::OtaCascade::reset() noexcept
{
    state.fill(0.0);
    inputHistory.fill(0.0);
    inputHistoryCount = 0;
    stageNoiseAt = {};
    stageNoiseHistory = {};
    stageNoiseHistoryCount = 0;
    previousOmegaStep = 0.0;
    previousFeedback = 0.0;
    previousHeadroom = 0.0;
    parameterHistoryPrimed = false;
}

void YouKnowEngine::OtaCascade::retime(float previousStep,
                                          float nextStep) noexcept
{
    // The capacitor voltages are physical and remain untouched. The three
    // older input endpoints are uniformly spaced on the old numerical grid;
    // they cannot be reinterpreted as samples on the new grid. Collapse them
    // to the one endpoint both grids share. updateProcessingRate calls this
    // only inside the existing zero-gain rate transition, and three new
    // internal samples refill the support before the fade is audible.
    inputHistory.fill(inputHistory[0]);
    inputHistoryCount = 0;
    for (auto& history : stageNoiseHistory)
        history.fill(history[0]);
    stageNoiseHistoryCount = 0;
    if (parameterHistoryPrimed)
    {
        const double previous = std::max(
            static_cast<double>(previousStep), 0.0);
        const double next = std::max(static_cast<double>(nextStep), 0.0);
        // Both arguments are the actual post-thermal, post-grid-cap
        // intervals on their respective grids. Scaling by their ratio maps
        // the last physical control endpoint exactly even when one side of a
        // rate change is capped. The zero branch is only reachable at reset.
        previousOmegaStep = std::clamp(
            previous > 0.0 ? previousOmegaStep * next / previous : next,
            0.0, maximumOmegaStep);
    }
}

void YouKnowEngine::OtaCascade::setStageNoise(
    const std::array<double, 4>& volts) noexcept
{
    // Only retain endpoints here: inactive cards have no solver-node reader.
    // The chosen tableau reconstructs exactly the nodes it consumes below.
    for (std::size_t stage = 0; stage < volts.size(); ++stage)
    {
        auto& history = stageNoiseHistory[stage];
        history[3] = history[2];
        history[2] = history[1];
        history[1] = history[0];
        history[0] = std::isfinite(volts[stage]) ? volts[stage] : 0.0;
    }
    stageNoiseHistoryCount = std::min(stageNoiseHistoryCount + 1, 3);
}

void YouKnowEngine::OtaCascade::prepareStageNoise(unsigned int nodeMask) noexcept
{
    // Same endpoint reconstruction as the signal, with startup of degree
    // one, two, then three. Endpoints are direct copies; the usual RK4 rung
    // needs only one weighted midpoint. No work is done at unread nodes.
    static constexpr auto weights = [] {
        std::array<std::array<double, 4>, controlNodePositions.size()> result {};
        for (std::size_t point = 0; point < result.size(); ++point)
        {
            const double t = controlNodePositions[point];
            result[point] = { t*(t+1.0)*(t+2.0)/6.0,
                -(t-1.0)*(t+1.0)*(t+2.0)/2.0,
                (t-1.0)*t*(t+2.0)/2.0, -(t-1.0)*t*(t+1.0)/6.0 };
        }
        return result;
    }();
    for (std::size_t stage = 0; stage < 4; ++stage)
    {
        const auto& h = stageNoiseHistory[stage];
        stageNoiseAt.front()[stage] = h[1];
        stageNoiseAt.back()[stage] = h[0];
    }
    for (std::size_t point = 1; point + 1 < stageNoiseAt.size(); ++point)
    {
        if ((nodeMask >> point & 1u) == 0u)
            continue;
        const double t = controlNodePositions[point];
        for (std::size_t stage = 0; stage < 4; ++stage)
        {
            const auto& h = stageNoiseHistory[stage];
            stageNoiseAt[point][stage] = stageNoiseHistoryCount <= 1
                ? t*h[0] + (1.0-t)*h[1]
                : stageNoiseHistoryCount == 2
                    ? 0.5*t*(t+1.0)*h[0] + (1.0-t*t)*h[1] + 0.5*t*(t-1.0)*h[2]
                    : weights[point][0]*h[0] + weights[point][1]*h[1]
                        + weights[point][2]*h[2] + weights[point][3]*h[3];
        }
    }
}

double YouKnowEngine::OtaCascade::reconstructInput(
    double current, const std::array<double, 3>& history,
    double intervalPosition) noexcept
{
    // Lagrange interpolation through endpoint coordinates
    // { current@1, previous@0, previous2@-1, previous3@-2 }.
    // It is exact for every polynomial through degree three, uses no future
    // endpoint and adds no delay. Its worst coefficient L1 norm on [0,1] is
    // 1.631131; the circuit suite fences both that finite gain and the
    // deliberately exposed Nyquist-alternating overshoot.
    constexpr std::array<double, 4> nodes {
        1.0, 0.0, -1.0, -2.0
    };
    const std::array<double, 4> samples {
        current, history[0], history[1], history[2]
    };
    double result = 0.0;
    for (std::size_t point = 0; point < nodes.size(); ++point)
    {
        double weight = 1.0;
        for (std::size_t other = 0; other < nodes.size(); ++other)
            if (other != point)
                weight *= (intervalPosition - nodes[other])
                        / (nodes[point] - nodes[other]);
        result += weight * samples[point];
    }
    return result;
}

double YouKnowEngine::OtaCascade::clampOmegaStep(double value) noexcept
{
    return std::clamp(std::isfinite(value) ? value : 0.0,
                      0.0, maximumOmegaStep);
}

YouKnowEngine::VcfHoldInterval
YouKnowEngine::exactVcfHoldInterval(
    float state, float target, bool hasEvent, double eventPosition,
    float eventTarget, double intervalSeconds) noexcept
{
    VcfHoldInterval result;
    const double initial = std::isfinite(state)
        ? static_cast<double>(state) : 0.0;
    const double initialTarget = std::isfinite(target)
        ? static_cast<double>(target) : initial;
    const double nextTarget = std::isfinite(eventTarget)
        ? static_cast<double>(eventTarget) : initialTarget;
    const double duration = std::isfinite(intervalSeconds)
        ? std::max(intervalSeconds, 0.0) : 0.0;
    const double event = std::clamp(
        std::isfinite(eventPosition) ? eventPosition : 1.0,
        0.0, 1.0);
    const double lambda = duration
        / static_cast<double>(vcfHoldSlewSeconds);

    for (std::size_t point = 0; point < result.value.size(); ++point)
    {
        const double position = OtaCascade::controlNodePositions[point];
        double value = initialTarget
            + (initial - initialTarget) * std::exp(-position * lambda);
        if (hasEvent && event <= position)
        {
            const double response = -std::expm1(
                -(position - event) * lambda);
            value += (nextTarget - initialTarget) * response;
        }
        result.value[point] = value;
    }
    result.endpoint = static_cast<float>(result.value.back());
    return result;
}

YouKnowEngine::VcfHoldInterval
YouKnowEngine::steppedHoldInterval(
    float held, bool hasEvent, double eventPosition,
    float eventTarget) noexcept
{
    VcfHoldInterval result;
    const double before = std::isfinite(held)
        ? static_cast<double>(held) : 0.0;
    const double after = hasEvent && std::isfinite(eventTarget)
        ? static_cast<double>(eventTarget) : before;
    const double event = std::clamp(
        std::isfinite(eventPosition) ? eventPosition : 1.0, 0.0, 1.0);

    for (std::size_t point = 0; point < result.value.size(); ++point)
    {
        const double position = OtaCascade::controlNodePositions[point];
        result.value[point] = (hasEvent && event <= position) ? after : before;
    }
    result.endpoint = static_cast<float>(result.value.back());
    return result;
}

double YouKnowEngine::exactOnePoleHoldEndpoint(
    double state, float target, bool hasEvent, double eventPosition,
    float eventTarget, double intervalSeconds, double timeConstantSeconds,
    double fullIntervalDecay) noexcept
{
    const double initial = std::isfinite(state) ? state : 0.0;
    const double initialTarget = std::isfinite(target)
        ? static_cast<double>(target) : initial;
    const double duration = std::isfinite(intervalSeconds)
        ? std::max(intervalSeconds, 0.0) : 0.0;
    const double timeConstant = std::isfinite(timeConstantSeconds)
                                    && timeConstantSeconds > 0.0
        ? timeConstantSeconds : 1.0;
    const double decay = std::isfinite(fullIntervalDecay)
        ? std::clamp(fullIntervalDecay, 0.0, 1.0)
        : std::exp(-duration / timeConstant);
    double endpoint = initialTarget + (initial - initialTarget) * decay;
    if (hasEvent)
    {
        const double event = std::clamp(
            std::isfinite(eventPosition) ? eventPosition : 1.0,
            0.0, 1.0);
        const double nextTarget = std::isfinite(eventTarget)
            ? static_cast<double>(eventTarget) : initialTarget;
        // Superposition needs only the response of the target step over the
        // suffix. At p=0 it is the whole interval; at p=1 expm1(0) is exactly
        // zero, so a right-endpoint write cannot influence preceding time.
        const double suffixResponse = -std::expm1(
            -(1.0 - event) * duration / timeConstant);
        endpoint += (nextTarget - initialTarget) * suffixResponse;
    }
    return endpoint;
}

YouKnowEngine::PwmHoldCoefficients
YouKnowEngine::pwmHoldCoefficients(double intervalSeconds) noexcept
{
    const double duration = std::isfinite(intervalSeconds)
        ? std::max(intervalSeconds, 0.0) : 0.0;
    const double firstTime = static_cast<double>(pwmHoldFirstPoleSeconds);
    const double secondTime = static_cast<double>(pwmHoldSecondPoleSeconds);
    const double firstDecay = std::exp(-duration / firstTime);
    const double secondDecay = std::exp(-duration / secondTime);
    return {
        firstDecay,
        secondDecay,
        firstTime / (firstTime - secondTime)
            * (firstDecay - secondDecay)
    };
}

YouKnowEngine::PwmHoldState YouKnowEngine::advancePwmHold(
    PwmHoldState state, double target,
    const PwmHoldCoefficients& coefficients) noexcept
{
    const double initialFirst = state.first;
    state.first = coefficients.firstDecay * initialFirst
                + (1.0 - coefficients.firstDecay) * target;
    state.second = coefficients.firstToSecond * initialFirst
                 + coefficients.secondDecay * state.second
                 + (1.0 - coefficients.secondDecay
                    - coefficients.firstToSecond) * target;
    return state;
}

YouKnowEngine::PwmHoldState YouKnowEngine::exactPwmHoldEndpoint(
    PwmHoldState state, float target, bool hasEvent,
    double eventPosition, float eventTarget, double intervalSeconds,
    const PwmHoldCoefficients& fullIntervalCoefficients) noexcept
{
    const double initialTarget = std::isfinite(target)
        ? static_cast<double>(target) : state.first;
    if (!hasEvent)
        return advancePwmHold(state, initialTarget, fullIntervalCoefficients);

    const double duration = std::isfinite(intervalSeconds)
        ? std::max(intervalSeconds, 0.0) : 0.0;
    const double event = std::clamp(
        std::isfinite(eventPosition) ? eventPosition : 1.0,
        0.0, 1.0);
    const double nextTarget = std::isfinite(eventTarget)
        ? static_cast<double>(eventTarget) : initialTarget;
    state = advancePwmHold(
        state, initialTarget, pwmHoldCoefficients(event * duration));
    return advancePwmHold(
        state, nextTarget,
        pwmHoldCoefficients((1.0 - event) * duration));
}

namespace
{
constexpr double vcfTanhFineLimit = 5.0;
constexpr double vcfTanhLimit = 19.0;
constexpr std::size_t vcfTanhFineIntervals = 160;
constexpr std::size_t vcfTanhTailIntervals = 56;
constexpr double vcfTanhFineWidth = 1.0 / 32.0;
constexpr double vcfTanhTailWidth = 1.0 / 4.0;
constexpr double vcfTanhFineScale = 32.0;
constexpr double vcfTanhTailScale = 4.0;

struct VcfTanhHermiteCoefficient
{
    double constant {};
    double linear {};
    double quadratic {};
    double cubic {};
};

// Each table is built once, off the audio path, from libm's exact-mode values
// and analytic slopes. The fine table covers every argument in the profiled
// single-note and six-note fixtures; the coarse tail preserves the prior
// far-tail saturation behaviour without occupying the hot table's footprint.
template <std::size_t intervals>
std::array<VcfTanhHermiteCoefficient, intervals> makeVcfTanhHermiteTable(
    double start, double width)
{
    struct Node
    {
        double value {};
        double slope {};
    };
    std::array<Node, intervals + 1u> nodes {};
    for (std::size_t index = 0; index < nodes.size(); ++index)
    {
        const double value = std::tanh(
            start + width * static_cast<double>(index));
        nodes[index] = { value, 1.0 - value * value };
    }
    for (std::size_t index = 0; index < intervals; ++index)
        if (nodes[index + 1u].value == nodes[index].value)
        {
            nodes[index].slope = 0.0;
            nodes[index + 1u].slope = 0.0;
        }

    std::array<VcfTanhHermiteCoefficient, intervals> table {};
    for (std::size_t index = 0; index < table.size(); ++index)
    {
        const Node left = nodes[index];
        const Node right = nodes[index + 1u];
        // Once adjacent exact-mode nodes round to the same double, the
        // representable function is flat. Their shared node slopes were
        // zeroed above so neighbouring intervals meet it continuously;
        // make the plateau itself explicit too.
        if (right.value == left.value)
        {
            table[index] = { left.value, 0.0, 0.0, 0.0 };
            continue;
        }
        const double delta = right.value - left.value;
        const double leftSlope = width * left.slope;
        const double rightSlope = width * right.slope;
        table[index] = {
            left.value,
            leftSlope,
            3.0 * delta - 2.0 * leftSlope - rightSlope,
            -2.0 * delta + leftSlope + rightSlope
        };
    }
    return table;
}

const auto vcfTanhFineTable = makeVcfTanhHermiteTable<vcfTanhFineIntervals>(
    0.0, vcfTanhFineWidth);
const auto vcfTanhTailTable = makeVcfTanhHermiteTable<vcfTanhTailIntervals>(
    vcfTanhFineLimit, vcfTanhTailWidth);

// The body of `zonedHermiteTanhUnchecked`, force-inlined into the solver's
// right-hand side. As an outlined call it was the single largest consumer in
// the whole-engine profile (~10M calls/s at the default rung); inlining keeps
// the identical expression tree -- same table, same polynomial, same rounding
// -- while letting the nine independent evaluations per RHS overlap.
[[gnu::always_inline]] inline double zonedHermiteTanhImpl(
    double value) noexcept
{
    const double magnitude = std::abs(value);
    if (magnitude < vcfTanhFineLimit)
    {
        const double position = magnitude * vcfTanhFineScale;
        const std::size_t interval = static_cast<std::size_t>(position);
        const double fraction = position - static_cast<double>(interval);
        const auto& coefficient = vcfTanhFineTable[interval];
        const double result = ((coefficient.cubic * fraction
                              + coefficient.quadratic) * fraction
                              + coefficient.linear) * fraction
                              + coefficient.constant;
        return std::copysign(result, value);
    }
    if (magnitude >= vcfTanhLimit)
        return std::copysign(1.0, value);

    const double position = (magnitude - vcfTanhFineLimit)
                          * vcfTanhTailScale;
    const std::size_t interval = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(interval);
    const auto& coefficient = vcfTanhTailTable[interval];
    const double result = ((coefficient.cubic * fraction
                          + coefficient.quadratic) * fraction
                          + coefficient.linear) * fraction
                          + coefficient.constant;
    return std::copysign(result, value);
}

// The PolyZoned rung's inner zone: tanh(x)/x on |x| < 1 as a degree-five
// polynomial in u = x^2 (Chebyshev fit of tanh(sqrt(u))/sqrt(u) on [0, 1];
// max |x*Q(x^2) - tanh(x)| = 4.31e-7 over the zone, an order below the 5e-6
// the measured inner-zone candidates already admitted). The kernel sits on
// the solver's serial dependency chain -- k1 feeds k2 feeds k3 -- so it is
// written in Estrin form: independent first-order pairs combined through
// u^2, four fused steps deep where Horner needs six. Wider alternatives
// were measured and rejected here: a Pade [9/8] core pays a divide on that
// chain, and a two-piece degree-13 fit pays still more depth plus a select;
// both lost to this shallower kernel end to end. |x| < 1 covers 95..99.8%
// of the arguments the profiled scenarios produce; the rest fall through to
// the established zoned Hermite tables.
[[gnu::always_inline]] inline double vcfInnerTanhFactor(double u) noexcept
{
    const double uSquared = u * u;
    const double top = -0.00305822903759879 * u + 0.016720855179576926;
    const double high = -0.051585988404218436 * u + 0.1328072064598552;
    const double low = -0.3332895137021704 * u + 0.9999993948006496;
    return (top * uSquared + high) * uSquared + low;
}

// Full-range scalar form of the PolyZoned nonlinearity: the inner
// polynomial where it is valid, the established zoned Hermite tables beyond.
[[gnu::always_inline]] inline double polyZonedTanhImpl(double value) noexcept
{
    const double u = value * value;
    if (u < 1.0)
        return value * vcfInnerTanhFactor(u);
    return zonedHermiteTanhImpl(value);
}

// Evaluates the zoned nonlinearity across four independent arguments -- the
// four stage nonlinearities of one right-hand-side evaluation, whose
// arguments depend only on the state vector and never on each other's
// outputs, so the four scalar chains overlap in flight.
[[gnu::always_inline]] inline std::array<double, 4> polyZonedTanhBatch(
    const std::array<double, 4>& argument) noexcept
{
    return { polyZonedTanhImpl(argument[0]),
             polyZonedTanhImpl(argument[1]),
             polyZonedTanhImpl(argument[2]),
             polyZonedTanhImpl(argument[3]) };
}
} // namespace

double YouKnowEngine::OtaCascade::zonedHermiteTanh(double value) noexcept
{
    if (std::isnan(value))
        return value;
    const double magnitude = std::abs(value);
    // Direct callers retain the denormal bypass. The integrated Fast path
    // starts from validated finite state; its table polynomial is bit-exact
    // for subnormals, so it need not repeat this cold guard 90 times per card.
    if (magnitude < std::numeric_limits<double>::min())
        return value;
    return zonedHermiteTanhUnchecked(value);
}

double YouKnowEngine::OtaCascade::zonedHermiteTanhUnchecked(
    double value) noexcept
{
    return zonedHermiteTanhImpl(value);
}

double YouKnowEngine::OtaCascade::cubicEarlyTanh(double value) noexcept
{
    const double magnitude = std::abs(value);
    if (magnitude >= 1.5)
        return std::copysign(1.0, value);
    return value * (1.0 - (4.0 / 27.0) * value * value);
}

float YouKnowEngine::VoiceVcaSignalLaw::shape(float volts) noexcept
{
    constexpr double inverseHeadroom =
        1.0 / static_cast<double>(headroomVolts);
    const double drive = static_cast<double>(volts) * inverseHeadroom;
    const double u = drive * drive;
    if (u < 1.0)
        return static_cast<float>(
            headroomVolts * (drive * vcfInnerTanhFactor(u)));
    // A non-finite input also lands here; the checked table call passes it
    // through for finishVoiceFilter's own guard to zero.
    return static_cast<float>(
        headroomVolts * OtaCascade::zonedHermiteTanh(drive));
}

float YouKnowEngine::VoiceVcaSignalLaw::serviceGain() noexcept
{
    // Roland's consecutive p.19 adjustments use the SAME bank-3 C4 sine:
    // TP19 = 4.8 Vp-p, then VR27 sets TP8 = 6 Vp-p. The headroom derivation
    // above uses both figures to establish distortion, but H*tanh(V/H) has
    // unity small-signal gain and cannot itself satisfy that output target.
    // Restore the missing fixed voltage gain. No external-output reference
    // or unknown mixer voltage enters this ratio.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=19
    // The control law is normalized on ENV peak code4095, while full stored
    // SUSTAIN uses code4064. Its gain at that service point is 0.992216;
    // the complete fixed correction is about 1.2790 (+2.138 dB), not merely
    // 6/4.8. The negligible C59 loss at 248 Hz is left inside that physical
    // coupling rather than absorbed into another gain adjustment.
    static const float gain = trimOutputPeakVolts
        / (shape(trimFilterPeakVolts)
           * VoiceVcaControlLaw::gain(4064.0f / 4095.0f));
    return gain;
}

double YouKnowEngine::OtaCascade::closedLoopSpectralFactor(
    double feedback) noexcept
{
    // Four identical one-poles closed through `feedback` put the loop roots at
    // s/w = -1 + feedback^(1/4) * exp(i*(pi + 2*pi*m)/4). The farthest of the
    // four from the origin is the one whose exponential lands in the third or
    // fourth quadrant, at distance sqrt((1 + q)^2 + q^2) with
    // q = feedback^(1/4) / sqrt(2). At the sanitized feedback ceiling of eight
    // that is 2.494; with the loop open it is exactly one.
    const double bounded = std::clamp(
        std::isfinite(feedback) ? feedback : 0.0, 0.0, 8.0);
    const double q = std::sqrt(std::sqrt(bounded)) * 0.70710678118654752440;
    const double real = 1.0 + q;
    const double factor = std::sqrt(real * real + q * q);
    // `planTableau`'s short circuit trusts the declared ceiling, so hold the
    // formula to it here rather than restating the constant in a comment. The
    // clamp is the identity for every admissible feedback.
    return std::min(factor, maximumClosedLoopSpectralFactor);
}

// Advance the continuous four-stage OTA equations over one internal interval.
// The default rung's two fixed half-interval Merson steps use five
// right-hand-side evaluations each; the two cheaper rungs run classic RK4 over
// the same interval, and `planTableau` decides which of the three this
// interval can take. The only circuit state is capacitor voltage, and the
// causal cubic supplies input between the two known sample endpoints.
// The compatibility profile closes a circuit-shaped nonlinear resonance
// return, Hfb*tanh(V4/Hfb), so the loop remains bounded beyond oscillation.
template <bool useCubicEarly>
float YouKnowEngine::OtaCascade::process(float input, float omegaStep,
                                            float feedback,
                                            float headroom,
                                            bool enableEarlyEffect,
                                            float calibration,
                                            const ControlTrajectory* trajectory,
                                            VcfTanhMode tanhMode,
                                            VcfSolverMode solverMode) noexcept
{
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(vcfSteps, 1);
    if (trajectory != nullptr)
    {
        YOUKNOW_COUNT_DOMAIN_WORK(vcfExactControlIntervals, 1);
        YOUKNOW_COUNT_DOMAIN_WORK(vcfExactControlNodes,
                                     controlNodePositions.size());
    }
#endif
    const double resonanceCompensation =
        static_cast<double>(inputCompensationCoefficient);
    const double resonanceOffset =
        static_cast<double>(resonanceOffsetVolts);
    const double currentOmega = clampOmegaStep(
        static_cast<double>(omegaStep));
    const double currentFeedback = std::clamp(
        std::isfinite(feedback) ? static_cast<double>(feedback) : 0.0,
        0.0, 8.0);
    const double currentHeadroom = std::max(
        std::isfinite(headroom) ? static_cast<double>(headroom) : 0.0,
        1.0e-5);
    const double currentInput = std::isfinite(input)
        ? static_cast<double>(input) : 0.0;
    const double currentCalibration = std::clamp(
        std::isfinite(calibration) ? static_cast<double>(calibration) : 0.0,
        0.0, static_cast<double>(EngineParameters::calibrationCeiling));

    if (!parameterHistoryPrimed)
    {
        previousOmegaStep = currentOmega;
        previousFeedback = currentFeedback;
        previousHeadroom = currentHeadroom;
        parameterHistoryPrimed = true;
    }

    // A bounded state, the finite histories and card trims maintained by the
    // engine, and the sanitized controls can only form finite Merson arguments.
    // Recover a hostile standalone state once here so Fast does not repeat the
    // helper's NaN check at every nonlinear evaluation.
    const bool useFastTanh = useCubicEarly
                          || tanhMode != VcfTanhMode::Exact;
    if (useFastTanh
        && !std::all_of(state.begin(), state.end(), [](double value) {
               return std::abs(value) <= 64.0;
           }))
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(vcfRecoveries, 1);
#endif
        state.fill(0.0);
        inputHistory.fill(currentInput);
        inputHistoryCount = 2;
        previousOmegaStep = currentOmega;
        previousFeedback = currentFeedback;
        previousHeadroom = currentHeadroom;
        return 0.0f;
    }

    struct IntegrationNode
    {
        double position {};
        std::array<double, 2> linear {};
        std::array<double, 3> quadratic {};
        std::array<double, 4> cubic {};
    };
    static constexpr auto makeNodes = []<std::size_t count>(
        const std::array<double, count>& positions) {
        std::array<IntegrationNode, count> result {};
        for (std::size_t point = 0; point < count; ++point)
        {
            const double t = positions[point];
            result[point] = {
                t,
                { t, 1.0 - t },
                { 0.5 * t * (t + 1.0), 1.0 - t * t,
                  0.5 * t * (t - 1.0) },
                { t * (t + 1.0) * (t + 2.0) / 6.0,
                  -(t - 1.0) * (t + 1.0) * (t + 2.0) / 2.0,
                  (t - 1.0) * t * (t + 2.0) / 2.0,
                  -(t - 1.0) * t * (t + 1.0) / 6.0 }
            };
        }
        return result;
    };
    static constexpr auto nodes = makeNodes(controlNodePositions);
    constexpr std::size_t pointCount = nodes.size();
    constexpr double substep = 0.5;

    // Which tableau this interval runs. `MersonHalfSteps` is unconditional and
    // pays nothing for the bound; the two RK4 rungs each name the cheapest
    // tableau they will accept and let `planTableau` decide from the largest
    // stage pole and the largest loop gain the interval can present -- both
    // can be escalated, and both fall back to Merson where classic RK4's
    // stability region ends. The trajectory's interior nodes carry the hold's
    // own curvature and can leave the endpoint interval, so they are scanned
    // when one is supplied.
    const Tableau plannedTableau = [&] {
        if (solverMode == VcfSolverMode::MersonHalfSteps)
            return Tableau::MersonHalf;
        double planOmega = std::max(previousOmegaStep, currentOmega);
        double planFeedback = std::max(previousFeedback, currentFeedback);
        if (trajectory != nullptr)
            for (std::size_t point = 0; point < pointCount; ++point)
            {
                planOmega = std::max(
                    planOmega, clampOmegaStep(trajectory->omegaStep[point]));
                const double candidate = trajectory->feedback[point];
                if (std::isfinite(candidate))
                    planFeedback = std::max(
                        planFeedback, std::clamp(candidate, 0.0, 8.0));
            }
        // The stage capacitor spread scales each pole independently, and the
        // Early effect scales every stage rate by at most 1 + earlyAmount.
        double largestScale = 0.0;
        for (const float scale : gScale)
            largestScale = std::max(largestScale,
                                    static_cast<double>(std::abs(scale)));
        const double earlyCeiling = enableEarlyEffect
            ? 1.0 + static_cast<double>(otaEarlyEffectCoefficient)
                        * currentCalibration
            : 1.0;
        return planTableau(solverMode,
                           planOmega * largestScale * earlyCeiling,
                           planFeedback);
    }();
    // A full-interval rung reads three of the seven control nodes. Skipping
    // the rest is not an approximation: the tableau never evaluates there, so
    // the reconstruction, the control interpolation and the per-stage omega
    // product at those ordinals have no reader.
    const unsigned int nodeMask = tableauNodeMask(plannedTableau);
    prepareStageNoise(nodeMask);

    std::array<double, pointCount> inputAt {};
    std::array<double, pointCount> omegaAt {};
    std::array<double, pointCount> feedbackAt {};
    std::array<double, pointCount> headroomAt {};
    // A settled interval -- both endpoints equal, no hold trajectory -- is
    // most of what an instrument does, and there the node interpolation
    // below is arithmetically the identity: a + p * (b - a) with b == a is
    // exactly a for every finite a. Broadcasting the endpoint is therefore
    // not an approximation, just the same values without the per-node
    // arithmetic.
    const bool settledControls = trajectory == nullptr
        && previousOmegaStep == currentOmega
        && previousFeedback == currentFeedback
        && previousHeadroom == currentHeadroom;
    const double settledHeadroom = std::max(currentHeadroom, 1.0e-5);
    for (std::size_t point = 0; point < pointCount; ++point)
    {
        if ((nodeMask >> point & 1u) == 0u)
            continue;
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(vcfInputReconstructions, 1);
#endif
        if (inputHistoryCount == 0)
            inputAt[point] = nodes[point].linear[0] * currentInput
                           + nodes[point].linear[1] * inputHistory[0];
        else if (inputHistoryCount == 1)
            inputAt[point] = nodes[point].quadratic[0] * currentInput
                + nodes[point].quadratic[1] * inputHistory[0]
                + nodes[point].quadratic[2] * inputHistory[1];
        else
            inputAt[point] = nodes[point].cubic[0] * currentInput
                + nodes[point].cubic[1] * inputHistory[0]
                + nodes[point].cubic[2] * inputHistory[1]
                + nodes[point].cubic[3] * inputHistory[2];
        if (settledControls)
        {
            omegaAt[point] = currentOmega;
            feedbackAt[point] = currentFeedback;
            headroomAt[point] = settledHeadroom;
        }
        else if (trajectory != nullptr)
        {
            omegaAt[point] = clampOmegaStep(
                trajectory->omegaStep[point]);
            feedbackAt[point] = std::clamp(
                std::isfinite(trajectory->feedback[point])
                    ? trajectory->feedback[point] : 0.0,
                0.0, 8.0);
            headroomAt[point] = std::max(
                std::isfinite(trajectory->headroom[point])
                    ? trajectory->headroom[point] : 0.0,
                1.0e-5);
        }
        else
        {
            const double position = nodes[point].position;
            omegaAt[point] = previousOmegaStep
                + position * (currentOmega - previousOmegaStep);
            feedbackAt[point] = previousFeedback
                + position * (currentFeedback - previousFeedback);
            headroomAt[point] = std::max(
                previousHeadroom
                    + position * (currentHeadroom - previousHeadroom),
                1.0e-5);
        }
    }
    // The resonance return's own headroom at each node: the stage headroom
    // through the return's divider when the card's temperature moves both,
    // the 25 C constant otherwise (EngineParameters::
    // enableResonanceHeadroomTemperature). Its reciprocal is taken here,
    // once per node, so the fast-reciprocal kernels keep their multiply.
    std::array<double, pointCount> loopHeadroomAt {};
    std::array<double, pointCount> inverseLoopHeadroomAt {};
    for (std::size_t point = 0; point < pointCount; ++point)
    {
        if ((nodeMask >> point & 1u) == 0u)
            continue;
        loopHeadroomAt[point] = resonanceHeadroomFollowsStage
            ? resonanceHeadroomFor(headroomAt[point])
            : static_cast<double>(
                  VoicedResonanceCompatibilityProfile::loopHeadroomVolts);
        inverseLoopHeadroomAt[point] = 1.0 / loopHeadroomAt[point];
    }

    const auto advanceOne = [](const std::array<double, 4>& origin,
                               const std::array<double, 4>& slope,
                               double distance) {
        std::array<double, 4> result {};
        for (std::size_t stage = 0; stage < result.size(); ++stage)
            result[stage] = origin[stage] + distance * slope[stage];
        return result;
    };
    const auto advanceTwo = [](const std::array<double, 4>& origin,
                               double step,
                               const std::array<double, 4>& a, double wa,
                               const std::array<double, 4>& b, double wb) {
        std::array<double, 4> result {};
        for (std::size_t stage = 0; stage < result.size(); ++stage)
            result[stage] = origin[stage] + step
                * (wa * a[stage] + wb * b[stage]);
        return result;
    };
    const auto advanceThree = [](const std::array<double, 4>& origin,
                                 double step,
                                 const std::array<double, 4>& a, double wa,
                                 const std::array<double, 4>& b, double wb,
                                 const std::array<double, 4>& c, double wc) {
        std::array<double, 4> result {};
        for (std::size_t stage = 0; stage < result.size(); ++stage)
            result[stage] = origin[stage] + step
                * (wa * a[stage] + wb * b[stage] + wc * c[stage]);
        return result;
    };
    const auto advanceFour = [](const std::array<double, 4>& origin,
                                double step,
                                const std::array<double, 4>& a, double wa,
                                const std::array<double, 4>& b, double wb,
                                const std::array<double, 4>& c, double wc,
                                const std::array<double, 4>& d, double wd) {
        std::array<double, 4> result {};
        for (std::size_t stage = 0; stage < result.size(); ++stage)
            result[stage] = origin[stage] + step
                * (wa * a[stage] + wb * b[stage] + wc * c[stage]
                   + wd * d[stage]);
        return result;
    };

    const bool applyEarlyEffect = enableEarlyEffect
                               && currentCalibration > 0.0;
    const double earlyAmount =
        static_cast<double>(otaEarlyEffectCoefficient) * currentCalibration;
    std::array<double, 4> stageScale {};
    std::array<std::array<double, 4>, pointCount> stageOffset {};
    for (std::size_t stage = 0; stage < stageScale.size(); ++stage)
    {
        stageScale[stage] = static_cast<double>(gScale[stage]);
        for (std::size_t point = 0; point < pointCount; ++point)
            if ((nodeMask >> point & 1u) != 0u)
                stageOffset[point][stage] = static_cast<double>(offsetVoltage[stage])
                    + stageNoiseAt[point][stage];
    }
    std::array<std::array<double, 4>, pointCount> stageOmegaAt {};
    for (std::size_t point = 0; point < pointCount; ++point)
    {
        if ((nodeMask >> point & 1u) == 0u)
            continue;
        for (std::size_t stage = 0; stage < stageScale.size(); ++stage)
            stageOmegaAt[point][stage] = omegaAt[point] * stageScale[stage];
    }

    // The tableau walks are shared by every right-hand side: they advance
    // `state` through the closure with whichever derivative the dispatch
    // below built, so the ladder exists once rather than once per kernel.
    const auto integrateSteps = [&]<Tableau tableau>(
                                    const auto& derivative) {
        if constexpr (tableau == Tableau::MersonHalf)
        {
            for (int step = 0; step < integrationSubsteps; ++step)
            {
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(vcfIntegrationSubsteps, 1);
#endif
                const std::size_t origin = static_cast<std::size_t>(3 * step);
                const auto k1 = derivative(state, inputAt[origin], origin);
                const auto k2 = derivative(
                    advanceOne(state, k1, substep / 3.0),
                    inputAt[origin + 1u], origin + 1u);
                const auto k3 = derivative(
                    advanceTwo(state, substep, k1, 1.0 / 6.0,
                               k2, 1.0 / 6.0),
                    inputAt[origin + 1u], origin + 1u);
                const auto k4 = derivative(
                    advanceTwo(state, substep, k1, 1.0 / 8.0,
                               k3, 3.0 / 8.0),
                    inputAt[origin + 2u], origin + 2u);
                const auto k5 = derivative(
                    advanceThree(state, substep, k1, 1.0 / 2.0,
                                 k3, -3.0 / 2.0, k4, 2.0),
                    inputAt[origin + 3u], origin + 3u);
                state = advanceThree(state, substep, k1, 1.0 / 6.0,
                                     k4, 2.0 / 3.0, k5, 1.0 / 6.0);
            }
        }
        else
        {
            // Classic RK4: one node at the start of the sub-interval, two
            // at its midpoint and one at its end. The half-step rung walks
            // that shape twice over the established 0/1/4/1/2 and 1/2/3/4/1
            // ordinals; the full-interval rung walks it once over 0/1/2/1.
            constexpr bool halfSteps = tableau == Tableau::Rk4Half;
            constexpr int steps = halfSteps ? 2 : 1;
            constexpr double stepSize = halfSteps ? 0.5 : 1.0;
            for (int step = 0; step < steps; ++step)
            {
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(vcfIntegrationSubsteps, 1);
#endif
                const std::size_t start = halfSteps
                    ? static_cast<std::size_t>(3 * step) : 0u;
                const std::size_t middle = halfSteps
                    ? static_cast<std::size_t>(2 + 3 * step) : 3u;
                const std::size_t end = halfSteps
                    ? static_cast<std::size_t>(3 + 3 * step) : 6u;
                const auto k1 = derivative(state, inputAt[start], start);
                const auto k2 = derivative(
                    advanceOne(state, k1, 0.5 * stepSize),
                    inputAt[middle], middle);
                const auto k3 = derivative(
                    advanceOne(state, k2, 0.5 * stepSize),
                    inputAt[middle], middle);
                const auto k4 = derivative(
                    advanceOne(state, k3, stepSize), inputAt[end], end);
                state = advanceFour(state, stepSize,
                                    k1, 1.0 / 6.0, k2, 1.0 / 3.0,
                                    k3, 1.0 / 3.0, k4, 1.0 / 6.0);
            }
        }
    };

    // Fast amortizes normalization over the nodes this tableau actually reads
    // -- seven on the default rung, five or three on the cheaper ones. In the
    // Character-on path, those reciprocals replace 90 RHS divisions per card
    // interval on the default rung. Exact keeps the established division
    // expressions and their frozen rounding behavior.
    const auto integrate = [&]<bool useReciprocal, Tableau tableau>(
                               const auto& nonlinear) {
        std::array<double, pointCount> inverseHeadroomAt {};
        if constexpr (useReciprocal)
            for (std::size_t point = 0; point < pointCount; ++point)
                if ((tableauNodeMask(tableau) >> point & 1u) != 0u)
                    inverseHeadroomAt[point] = 1.0 / headroomAt[point];

        // Four doubles are an arm64 HFA: by value keeps RK states in d0-d3;
        // a const reference forces every temporary through addressable memory.
        const auto derivative = [&](std::array<double, 4> value,
                                    double drive, std::size_t point) {
            std::array<double, 4> result {};
            const double runningHeadroom = headroomAt[point];
            const auto normalise = [&](double input) {
                if constexpr (useReciprocal)
                    return input * inverseHeadroomAt[point];
                return input / runningHeadroom;
            };
#if defined(YOUKNOW_WORK_AUDIT)
            YOUKNOW_COUNT_DOMAIN_WORK(vcfRhsEvaluations, 1);
            YOUKNOW_COUNT_DOMAIN_WORK(vcfFeedbackEvaluations, 1);
            if (applyEarlyEffect)
                YOUKNOW_COUNT_DOMAIN_WORK(vcfEarlyEvaluations,
                                             result.size());
#endif
            // One pair, one tanh, of the difference of its two divided
            // inputs. `resonanceCompensation` is zero when the legacy split
            // is selected, which makes this the former argument exactly.
            const double differentialInput =
                value[3] - resonanceCompensation * drive + resonanceOffset;
            const double feedbackArgument = [&] {
                if constexpr (useReciprocal)
                    return differentialInput * inverseLoopHeadroomAt[point];
                return differentialInput / loopHeadroomAt[point];
            }();
            double previous = drive - feedbackAt[point] * loopHeadroomAt[point]
                * nonlinear(feedbackArgument);
            for (std::size_t stage = 0; stage < result.size(); ++stage)
            {
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(vcfStageEvaluations, 1);
#endif
                const double early = [&] {
                    if (!applyEarlyEffect)
                        return 1.0;
                    if constexpr (useCubicEarly)
                        return 1.0 + earlyAmount
                            * cubicEarlyTanh(normalise(value[stage]));
                    return 1.0 + earlyAmount
                        * nonlinear(normalise(value[stage]));
                }();
                result[stage] = stageOmegaAt[point][stage]
                    * early * runningHeadroom
                    * nonlinear(normalise(
                        previous - value[stage]
                        + stageOffset[point][stage]));
                previous = value[stage];
            }
            return result;
        };

        integrateSteps.template operator()<tableau>(derivative);
    };

    // The PolyZoned kernel: the same derivative expressions with the four
    // stage nonlinearities batched. Their arguments depend only on the state
    // vector, never on each other's outputs, so the inner polynomial can be
    // evaluated unconditionally across the batch -- branch-free and
    // load-free, which the scalar zoned kernel cannot be -- and the rare
    // out-of-zone lane is patched afterwards through the established Hermite
    // tables. The feedback return stays scalar: stage zero's argument needs
    // its result.
    const auto integratePoly = [&]<Tableau tableau> {
        std::array<double, pointCount> inverseHeadroomAt {};
        for (std::size_t point = 0; point < pointCount; ++point)
            if ((tableauNodeMask(tableau) >> point & 1u) != 0u)
                inverseHeadroomAt[point] = 1.0 / headroomAt[point];

        const auto derivative = [&](std::array<double, 4> value,
                                    double drive, std::size_t point) {
            const double inverseHeadroom = inverseHeadroomAt[point];
            const double runningHeadroom = headroomAt[point];
#if defined(YOUKNOW_WORK_AUDIT)
            YOUKNOW_COUNT_DOMAIN_WORK(vcfRhsEvaluations, 1);
            YOUKNOW_COUNT_DOMAIN_WORK(vcfFeedbackEvaluations, 1);
            YOUKNOW_COUNT_DOMAIN_WORK(vcfStageEvaluations, 4);
            if (applyEarlyEffect)
                YOUKNOW_COUNT_DOMAIN_WORK(vcfEarlyEvaluations, 4);
#endif
            // With the resonance loop open the return term is exactly zero
            // whatever the fourth capacitor holds, and its evaluation is the
            // one nonlinearity stage zero's argument has to wait for -- so
            // an open loop skips it rather than computing a value only to
            // multiply it away. Identical arithmetic either way.
            const double loopReturn = feedbackAt[point] == 0.0
                ? drive
                : drive - feedbackAt[point] * loopHeadroomAt[point]
                    * polyZonedTanhImpl(
                        (value[3] - resonanceCompensation * drive
                         + resonanceOffset)
                        * inverseLoopHeadroomAt[point]);

            const std::array<double, 4> stageArg {
                (loopReturn - value[0] + stageOffset[point][0]) * inverseHeadroom,
                (value[0] - value[1] + stageOffset[point][1]) * inverseHeadroom,
                (value[1] - value[2] + stageOffset[point][2]) * inverseHeadroom,
                (value[2] - value[3] + stageOffset[point][3]) * inverseHeadroom
            };
            const std::array<double, 4> stageTanh =
                polyZonedTanhBatch(stageArg);

            std::array<double, 4> early {};
            if (!applyEarlyEffect)
                early.fill(1.0);
            else if constexpr (useCubicEarly)
                for (std::size_t stage = 0; stage < early.size(); ++stage)
                    early[stage] = 1.0 + earlyAmount
                        * cubicEarlyTanh(value[stage] * inverseHeadroom);
            else
            {
                const std::array<double, 4> earlyTanh = polyZonedTanhBatch({
                    value[0] * inverseHeadroom, value[1] * inverseHeadroom,
                    value[2] * inverseHeadroom, value[3] * inverseHeadroom });
                for (std::size_t stage = 0; stage < early.size(); ++stage)
                    early[stage] = 1.0 + earlyAmount * earlyTanh[stage];
            }

            std::array<double, 4> result {};
            for (std::size_t stage = 0; stage < result.size(); ++stage)
                result[stage] = stageOmegaAt[point][stage]
                    * early[stage] * runningHeadroom * stageTanh[stage];
            return result;
        };

        integrateSteps.template operator()<tableau>(derivative);
    };

    // One switch per interval over a value that is constant for the whole
    // parameter snapshot. Each arm instantiates only the integration shell;
    // the derivative it calls is shared, so the ladder does not clone the hot
    // right-hand side -- an experiment that did clone one measured 6% slower.
    const auto integrateWithTableau = [&]<bool useReciprocal>(
                                          const auto& nonlinear) {
        switch (plannedTableau)
        {
            case Tableau::Rk4Half:
                integrate.template operator()<useReciprocal, Tableau::Rk4Half>(
                    nonlinear);
                return;
            case Tableau::Rk4Full:
                integrate.template operator()<useReciprocal, Tableau::Rk4Full>(
                    nonlinear);
                return;
            case Tableau::MersonHalf:
                break;
        }
        integrate.template operator()<useReciprocal, Tableau::MersonHalf>(
            nonlinear);
    };

    if (tanhMode == VcfTanhMode::PolyZoned)
        switch (plannedTableau)
        {
            case Tableau::Rk4Half:
                integratePoly.template operator()<Tableau::Rk4Half>();
                break;
            case Tableau::Rk4Full:
                integratePoly.template operator()<Tableau::Rk4Full>();
                break;
            case Tableau::MersonHalf:
            default:
                integratePoly.template operator()<Tableau::MersonHalf>();
                break;
        }
    else if constexpr (useCubicEarly)
        integrateWithTableau.template operator()<true>(
            [](double value) noexcept {
                return zonedHermiteTanhImpl(value);
            });
    else
        switch (tanhMode)
        {
            case VcfTanhMode::ZonedHermite:
                integrateWithTableau.template operator()<true>(
                    [](double value) noexcept {
                        return zonedHermiteTanhImpl(value);
                    });
                break;
            case VcfTanhMode::Exact:
            default:
                integrateWithTableau.template operator()<false>(
                    [](double value) noexcept {
                        return std::tanh(value);
                    });
                break;
        }

    for (std::size_t point = inputHistory.size() - 1u; point > 0u; --point)
        inputHistory[point] = inputHistory[point - 1u];
    inputHistory[0] = currentInput;
    inputHistoryCount = std::min(inputHistoryCount + 1, 2);
    previousOmegaStep = currentOmega;
    previousFeedback = currentFeedback;
    previousHeadroom = currentHeadroom;

    // The comparison also rejects NaNs and infinities.  A separate isfinite
    // predicate was redundant and measurably enlarged this hot loop.
    const bool valid = std::all_of(
        state.begin(), state.end(), [](double value) {
            return std::abs(value) <= 64.0;
        });
    if (!valid)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(vcfRecoveries, 1);
#endif
        state.fill(0.0);
        inputHistory.fill(currentInput);
        inputHistoryCount = 2;
        return 0.0f;
    }
    return static_cast<float>(state[3]);
}

#if defined(YOUKNOW_HAS_VCF_PAIR_SIMD)
bool YouKnowEngine::OtaCascade::tryProcessSettledRk4Pair(
    OtaCascade& first, float firstInput, float firstOmegaStep,
    float firstFeedback, float firstHeadroom,
    OtaCascade& second, float secondInput, float secondOmegaStep,
    float secondFeedback, float secondHeadroom,
    bool enableEarlyEffect, float calibration,
    float& firstOutput, float& secondOutput) noexcept
{
    const double currentCalibration = std::clamp(
        std::isfinite(calibration) ? static_cast<double>(calibration) : 0.0,
        0.0, static_cast<double>(EngineParameters::calibrationCeiling));
    const bool applyEarlyEffect = enableEarlyEffect
                               && currentCalibration > 0.0;
    const double earlyAmount =
        static_cast<double>(otaEarlyEffectCoefficient) * currentCalibration;
    const double earlyCeiling = applyEarlyEffect ? 1.0 + earlyAmount : 1.0;

    struct Lane
    {
        OtaCascade* cascade {};
        double input {};
        double omega {};
        double feedback {};
        double headroom {};
        double inverseHeadroom {};
        double loopHeadroom {};
        double inverseLoopHeadroom {};
        Tableau tableau { Tableau::MersonHalf };
        std::array<double, 5> drive {};
        std::array<double, 4> stageOmega {};
        std::array<std::array<double, 4>, 7> stageOffset {};
    };

    const auto prepareLane = [&](OtaCascade& cascade, float input,
                                 float omegaStep, float feedback,
                                 float headroom, Lane& lane) {
        const double currentOmega = clampOmegaStep(
            static_cast<double>(omegaStep));
        const double currentFeedback = std::clamp(
            std::isfinite(feedback) ? static_cast<double>(feedback) : 0.0,
            0.0, 8.0);
        const double currentHeadroom = std::max(
            std::isfinite(headroom) ? static_cast<double>(headroom) : 0.0,
            1.0e-5);
        if (!cascade.parameterHistoryPrimed
            || cascade.inputHistoryCount != 2
            || cascade.previousOmegaStep != currentOmega
            || cascade.previousFeedback != currentFeedback
            || cascade.previousHeadroom != currentHeadroom
            || !std::all_of(
                cascade.state.begin(), cascade.state.end(), [](double value) {
                    return std::abs(value) <= 64.0;
                }))
            return false;

        double largestScale = 0.0;
        for (const float scale : cascade.gScale)
            largestScale = std::max(
                largestScale, static_cast<double>(std::abs(scale)));
        const Tableau tableau = planTableau(
            VcfSolverMode::Rk4Single,
            currentOmega * largestScale * earlyCeiling, currentFeedback);
        if (tableau == Tableau::MersonHalf)
            return false;

        lane.cascade = &cascade;
        lane.input = std::isfinite(input) ? static_cast<double>(input) : 0.0;
        lane.omega = currentOmega;
        lane.feedback = currentFeedback;
        lane.headroom = currentHeadroom;
        lane.inverseHeadroom = 1.0 / currentHeadroom;
        lane.loopHeadroom = cascade.resonanceHeadroomFollowsStage
            ? resonanceHeadroomFor(currentHeadroom)
            : static_cast<double>(
                  VoicedResonanceCompatibilityProfile::loopHeadroomVolts);
        lane.inverseLoopHeadroom = 1.0 / lane.loopHeadroom;
        lane.tableau = tableau;

        const auto reconstruct = [&](double currentWeight,
                                     double firstWeight,
                                     double secondWeight,
                                     double thirdWeight) {
            return currentWeight * lane.input
                + firstWeight * cascade.inputHistory[0]
                + secondWeight * cascade.inputHistory[1]
                + thirdWeight * cascade.inputHistory[2];
        };
        // Both RK4 tableaux read 0, 1/2 and 1. The half-step pair alone also
        // reads 1/4 and 3/4. These are the identical cubic weights produced
        // by `makeNodes` in `process`.
        lane.drive[0] = reconstruct(0.0,    1.0,    0.0,     0.0);
        lane.drive[2] = reconstruct(0.3125, 0.9375, -0.3125, 0.0625);
        lane.drive[4] = reconstruct(1.0,    0.0,    0.0,     0.0);
        if (tableau == Tableau::Rk4Half)
        {
            lane.drive[1] = reconstruct(
                0.1171875, 1.0546875, -0.2109375, 0.0390625);
            lane.drive[3] = reconstruct(
                0.6015625, 0.6015625, -0.2578125, 0.0546875);
        }
        const unsigned int noiseNodeMask = tableauNodeMask(tableau);
        cascade.prepareStageNoise(noiseNodeMask);
        for (std::size_t stage = 0; stage < lane.stageOmega.size(); ++stage)
        {
            lane.stageOmega[stage] = currentOmega
                * static_cast<double>(cascade.gScale[stage]);
            for (std::size_t point = 0; point < 7; ++point)
                if ((noiseNodeMask >> point & 1u) != 0u)
                    lane.stageOffset[point][stage] =
                        static_cast<double>(cascade.offsetVoltage[stage])
                        + cascade.stageNoiseAt[point][stage];
        }
        return true;
    };

    Lane lanes[2];
    if (!prepareLane(first, firstInput, firstOmegaStep, firstFeedback,
                     firstHeadroom, lanes[0])
        || !prepareLane(second, secondInput, secondOmegaStep, secondFeedback,
                        secondHeadroom, lanes[1]))
        return false;
    if (lanes[0].tableau != lanes[1].tableau)
        return false;

#if defined(__aarch64__) && defined(__ARM_NEON)
    using Pair = float64x2_t;
    const auto pack = [](double low, double high) {
        return vsetq_lane_f64(high, vdupq_n_f64(low), 1);
    };
#define pairLow(value) vgetq_lane_f64((value), 0)
#define pairHigh(value) vgetq_lane_f64((value), 1)
#define pairSplat(value) vdupq_n_f64(value)
#define pairAdd(first, second) vaddq_f64((first), (second))
#define pairSubtract(first, second) vsubq_f64((first), (second))
#define pairMultiply(first, second) vmulq_f64((first), (second))
#define pairMultiplyScalar(value, scalar) vmulq_n_f64((value), (scalar))
#define pairMultiplyAdd(addend, first, second) \
    vfmaq_f64((addend), (first), (second))
#define pairMultiplyAddScalar(addend, value, scalar) \
    vfmaq_n_f64((addend), (value), (scalar))
#elif defined(__x86_64__) && defined(__SSE2__)
    using Pair = __m128d;
    const auto pack = [](double low, double high) {
        return _mm_set_pd(high, low);
    };
#define pairLow(value) _mm_cvtsd_f64(value)
#define pairHigh(value) _mm_cvtsd_f64(_mm_unpackhi_pd((value), (value)))
#define pairSplat(value) _mm_set1_pd(value)
#define pairAdd(first, second) _mm_add_pd((first), (second))
#define pairSubtract(first, second) _mm_sub_pd((first), (second))
#define pairMultiply(first, second) _mm_mul_pd((first), (second))
#define pairMultiplyScalar(value, scalar) \
    _mm_mul_pd((value), _mm_set1_pd(scalar))
    // Baseline x86_64 has SSE2 but not FMA. Keep the scalar path's separate
    // multiply and add rounding rather than changing the solve along with its
    // scheduling.
#define pairMultiplyAdd(addend, first, second) \
    _mm_add_pd((addend), _mm_mul_pd((first), (second)))
#define pairMultiplyAddScalar(addend, value, scalar) \
    _mm_add_pd((addend), _mm_mul_pd((value), _mm_set1_pd(scalar)))
#endif
    using PairState = std::array<Pair, 4>;
    const auto polyTanhPair = [&](Pair value) {
        const Pair u = pairMultiply(value, value);
        if (pairLow(u) >= 1.0 || pairHigh(u) >= 1.0)
            return pack(polyZonedTanhImpl(pairLow(value)),
                        polyZonedTanhImpl(pairHigh(value)));

        const Pair uSquared = pairMultiply(u, u);
        const Pair top = pairMultiplyAddScalar(
            pairSplat(0.016720855179576926), u,
            -0.00305822903759879);
        const Pair high = pairMultiplyAddScalar(
            pairSplat(0.1328072064598552), u,
            -0.051585988404218436);
        const Pair low = pairMultiplyAddScalar(
            pairSplat(0.9999993948006496), u,
            -0.3332895137021704);
        const Pair upper = pairMultiplyAdd(high, top, uSquared);
        const Pair factor = pairMultiplyAdd(low, upper, uSquared);
        return pairMultiply(value, factor);
    };
    const auto cubicEarlyPair = [&](Pair value) {
        if (std::abs(pairLow(value)) >= 1.5
            || std::abs(pairHigh(value)) >= 1.5)
            return pack(cubicEarlyTanh(pairLow(value)),
                        cubicEarlyTanh(pairHigh(value)));

        const Pair scaled = pairMultiplyScalar(value, -(4.0 / 27.0));
        const Pair factor = pairMultiplyAdd(pairSplat(1.0), scaled, value);
        return pairMultiply(value, factor);
    };

    PairState state;
    PairState stageOmega;
    std::array<PairState, 7> stageOffset;
    for (std::size_t stage = 0; stage < state.size(); ++stage)
    {
        state[stage] = pack(first.state[stage], second.state[stage]);
        stageOmega[stage] = pack(lanes[0].stageOmega[stage],
                                 lanes[1].stageOmega[stage]);
        for (std::size_t point = 0; point < 7; ++point)
            if ((tableauNodeMask(lanes[0].tableau) >> point & 1u) != 0u)
                stageOffset[point][stage] = pack(lanes[0].stageOffset[point][stage],
                                               lanes[1].stageOffset[point][stage]);
    }
    const Pair inverseHeadroom = pack(lanes[0].inverseHeadroom,
                                      lanes[1].inverseHeadroom);
    const Pair runningHeadroom = pack(lanes[0].headroom, lanes[1].headroom);

    // Read per cascade rather than broadcast: the coefficient is uniform
    // across voices in practice, but two plug-in instances can differ.
    const Pair resonanceCompensation = pack(
        static_cast<double>(first.inputCompensationCoefficient),
        static_cast<double>(second.inputCompensationCoefficient));
    const Pair resonanceOffset = pack(
        static_cast<double>(first.resonanceOffsetVolts),
        static_cast<double>(second.resonanceOffsetVolts));
    const Pair inverseLoopHeadroom = pack(
        lanes[0].inverseLoopHeadroom, lanes[1].inverseLoopHeadroom);
    const auto derivative = [&](const PairState& value, Pair drive, std::size_t point) {
        const Pair feedbackArgument = pairMultiply(
            pairAdd(pairSubtract(value[3],
                                 pairMultiply(resonanceCompensation, drive)),
                    resonanceOffset),
            inverseLoopHeadroom);
        const Pair feedbackTanh = polyTanhPair(feedbackArgument);
        const double firstLoopReturn = lanes[0].feedback == 0.0
            ? pairLow(drive)
            : pairLow(drive)
                - lanes[0].feedback * lanes[0].loopHeadroom
                    * pairLow(feedbackTanh);
        const double secondLoopReturn = lanes[1].feedback == 0.0
            ? pairHigh(drive)
            : pairHigh(drive)
                - lanes[1].feedback * lanes[1].loopHeadroom
                    * pairHigh(feedbackTanh);
        const Pair loopReturn = pack(firstLoopReturn, secondLoopReturn);

        PairState stageArgument {
            pairMultiply(pairAdd(pairSubtract(loopReturn, value[0]),
                                 stageOffset[point][0]), inverseHeadroom),
            pairMultiply(pairAdd(pairSubtract(value[0], value[1]),
                                 stageOffset[point][1]), inverseHeadroom),
            pairMultiply(pairAdd(pairSubtract(value[1], value[2]),
                                 stageOffset[point][2]), inverseHeadroom),
            pairMultiply(pairAdd(pairSubtract(value[2], value[3]),
                                 stageOffset[point][3]), inverseHeadroom)
        };
        PairState stageTanh;
        for (std::size_t stage = 0; stage < stageTanh.size(); ++stage)
            stageTanh[stage] = polyTanhPair(stageArgument[stage]);

        PairState early;
        if (!applyEarlyEffect)
        {
            for (auto& valueAtStage : early)
                valueAtStage = pairSplat(1.0);
        }
        else
        {
            for (std::size_t stage = 0; stage < early.size(); ++stage)
            {
                const Pair earlyTanh = cubicEarlyPair(
                    pairMultiply(value[stage], inverseHeadroom));
                early[stage] = pairMultiplyAddScalar(
                    pairSplat(1.0), earlyTanh, earlyAmount);
            }
        }

        PairState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            result[stage] = pairMultiply(stageOmega[stage], early[stage]);
            result[stage] = pairMultiply(result[stage], runningHeadroom);
            result[stage] = pairMultiply(result[stage], stageTanh[stage]);
        }
        return result;
    };
    const auto advanceOne = [](const PairState& origin,
                               const PairState& slope, double distance) {
        PairState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
            result[stage] = pairMultiplyAddScalar(
                origin[stage], slope[stage], distance);
        return result;
    };
    const auto finishRk4 = [](const PairState& origin,
                              const PairState& k1,
                              const PairState& k2,
                              const PairState& k3,
                              const PairState& k4, double stepSize) {
        PairState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            Pair weighted = pairMultiplyScalar(k2[stage], 1.0 / 3.0);
            weighted = pairMultiplyAddScalar(
                weighted, k1[stage], 1.0 / 6.0);
            weighted = pairMultiplyAddScalar(
                weighted, k3[stage], 1.0 / 3.0);
            weighted = pairMultiplyAddScalar(
                weighted, k4[stage], 1.0 / 6.0);
            result[stage] = pairMultiplyAddScalar(
                origin[stage], weighted, stepSize);
        }
        return result;
    };

    const auto advanceRk4 = [&](std::size_t start, std::size_t middle,
                                std::size_t end, double stepSize) {
        const PairState origin = state;
        const Pair k1Drive = pack(lanes[0].drive[start],
                                  lanes[1].drive[start]);
        const Pair middleDrive = pack(lanes[0].drive[middle],
                                      lanes[1].drive[middle]);
        const Pair endDrive = pack(lanes[0].drive[end],
                                   lanes[1].drive[end]);
        static constexpr std::array<std::size_t, 5> node { 0, 2, 3, 5, 6 };
        const PairState k1 = derivative(origin, k1Drive, node[start]);
        const PairState k2 = derivative(
            advanceOne(origin, k1, 0.5 * stepSize), middleDrive, node[middle]);
        const PairState k3 = derivative(
            advanceOne(origin, k2, 0.5 * stepSize), middleDrive, node[middle]);
        const PairState k4 = derivative(
            advanceOne(origin, k3, stepSize), endDrive, node[end]);
        state = finishRk4(origin, k1, k2, k3, k4, stepSize);
    };
    if (lanes[0].tableau == Tableau::Rk4Full)
        advanceRk4(0u, 2u, 4u, 1.0);
    else
    {
        advanceRk4(0u, 1u, 2u, 0.5);
        advanceRk4(2u, 3u, 4u, 0.5);
    }

    for (std::size_t stage = 0; stage < state.size(); ++stage)
    {
        first.state[stage] = pairLow(state[stage]);
        second.state[stage] = pairHigh(state[stage]);
    }

#undef pairLow
#undef pairHigh
#undef pairSplat
#undef pairAdd
#undef pairSubtract
#undef pairMultiply
#undef pairMultiplyScalar
#undef pairMultiplyAdd
#undef pairMultiplyAddScalar

    const auto finishLane = [](Lane& lane, float& output) {
        auto& cascade = *lane.cascade;
        for (std::size_t point = cascade.inputHistory.size() - 1u;
             point > 0u; --point)
            cascade.inputHistory[point] = cascade.inputHistory[point - 1u];
        cascade.inputHistory[0] = lane.input;
        cascade.inputHistoryCount = std::min(cascade.inputHistoryCount + 1, 2);
        cascade.previousOmegaStep = lane.omega;
        cascade.previousFeedback = lane.feedback;
        cascade.previousHeadroom = lane.headroom;

        const bool valid = std::all_of(
            cascade.state.begin(), cascade.state.end(), [](double value) {
                return std::abs(value) <= 64.0;
            });
        if (!valid)
        {
            cascade.state.fill(0.0);
            cascade.inputHistory.fill(lane.input);
            cascade.inputHistoryCount = 2;
            output = 0.0f;
            return;
        }
        output = static_cast<float>(cascade.state[3]);
    };
    finishLane(lanes[0], firstOutput);
    finishLane(lanes[1], secondOutput);
    return true;
}

#if defined(_MSC_VER)
#define YOUKNOW_MERSON_NOINLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__)
#define YOUKNOW_MERSON_NOINLINE __attribute__((noinline))
#else
#define YOUKNOW_MERSON_NOINLINE
#endif
YOUKNOW_MERSON_NOINLINE
bool YouKnowEngine::OtaCascade::tryProcessSettledMersonPair(
    OtaCascade& first, float firstInput, float firstOmegaStep,
    float firstFeedback, float firstHeadroom,
    OtaCascade& second, float secondInput, float secondOmegaStep,
    float secondFeedback, float secondHeadroom,
    bool enableEarlyEffect, float calibration,
    float& firstOutput, float& secondOutput) noexcept
{
    const double currentCalibration = std::clamp(
        std::isfinite(calibration) ? static_cast<double>(calibration) : 0.0,
        0.0, static_cast<double>(EngineParameters::calibrationCeiling));
    const bool applyEarlyEffect = enableEarlyEffect
                               && currentCalibration > 0.0;
    const double earlyAmount =
        static_cast<double>(otaEarlyEffectCoefficient) * currentCalibration;
    const double earlyCeiling = applyEarlyEffect ? 1.0 + earlyAmount : 1.0;

    struct Lane
    {
        OtaCascade* cascade {};
        double input {};
        double omega {};
        double feedback {};
        double headroom {};
        double inverseHeadroom {};
        double loopHeadroom {};
        double inverseLoopHeadroom {};
        std::array<double, 7> drive {};
        std::array<double, 4> stageOmega {};
        std::array<std::array<double, 4>, 7> stageOffset {};
    };

    const auto prepareLane = [&](OtaCascade& cascade, float input,
                                 float omegaStep, float feedback,
                                 float headroom, Lane& lane) {
        const double currentOmega = clampOmegaStep(
            static_cast<double>(omegaStep));
        const double currentFeedback = std::clamp(
            std::isfinite(feedback) ? static_cast<double>(feedback) : 0.0,
            0.0, 8.0);
        const double currentHeadroom = std::max(
            std::isfinite(headroom) ? static_cast<double>(headroom) : 0.0,
            1.0e-5);
        if (!cascade.parameterHistoryPrimed
            || cascade.inputHistoryCount != 2
            || cascade.previousOmegaStep != currentOmega
            || cascade.previousFeedback != currentFeedback
            || cascade.previousHeadroom != currentHeadroom
            || !std::all_of(
                cascade.state.begin(), cascade.state.end(), [](double value) {
                    return std::abs(value) <= 64.0;
                }))
            return false;

        double largestScale = 0.0;
        for (const float scale : cascade.gScale)
            largestScale = std::max(
                largestScale, static_cast<double>(std::abs(scale)));
        if (planTableau(
                VcfSolverMode::Rk4Single,
                currentOmega * largestScale * earlyCeiling,
                currentFeedback) != Tableau::MersonHalf)
            return false;

        lane.cascade = &cascade;
        lane.input = std::isfinite(input) ? static_cast<double>(input) : 0.0;
        lane.omega = currentOmega;
        lane.feedback = currentFeedback;
        lane.headroom = currentHeadroom;
        lane.inverseHeadroom = 1.0 / currentHeadroom;
        lane.loopHeadroom = cascade.resonanceHeadroomFollowsStage
            ? resonanceHeadroomFor(currentHeadroom)
            : static_cast<double>(
                  VoicedResonanceCompatibilityProfile::loopHeadroomVolts);
        lane.inverseLoopHeadroom = 1.0 / lane.loopHeadroom;

        const auto reconstruct = [&](double currentWeight,
                                     double firstWeight,
                                     double secondWeight,
                                     double thirdWeight) {
            return currentWeight * lane.input
                + firstWeight * cascade.inputHistory[0]
                + secondWeight * cascade.inputHistory[1]
                + thirdWeight * cascade.inputHistory[2];
        };
        // These are the seven causal-cubic values produced by `makeNodes` in
        // `process`, in Merson's 0, 1/6, 1/4, 1/2, 2/3, 3/4 and 1 order.
        lane.drive[0] = reconstruct(0.0,    1.0,    0.0,     0.0);
        lane.drive[1] = reconstruct(
            0x1.1f9add3c0ca45p-4, 0x1.0da12f684bda1p+0,
            -0x1.3425ed097b426p-3, 0x1.ba781948b0fcfp-6);
        lane.drive[2] = reconstruct(
            0.1171875, 1.0546875, -0.2109375, 0.0390625);
        lane.drive[3] = reconstruct(
            0.3125, 0.9375, -0.3125, 0.0625);
        lane.drive[4] = reconstruct(
            0x1.f9add3c0ca457p-2, 0x1.7b425ed097b42p-1,
            -0x1.2f684bda12f68p-2, 0x1.f9add3c0ca458p-5);
        lane.drive[5] = reconstruct(
            0.6015625, 0.6015625, -0.2578125, 0.0546875);
        lane.drive[6] = reconstruct(1.0,    0.0,    0.0,     0.0);
        cascade.prepareStageNoise(tableauNodeMask(Tableau::MersonHalf));
        for (std::size_t stage = 0; stage < lane.stageOmega.size(); ++stage)
        {
            lane.stageOmega[stage] = currentOmega
                * static_cast<double>(cascade.gScale[stage]);
            for (std::size_t point = 0; point < 7; ++point)
                lane.stageOffset[point][stage] =
                    static_cast<double>(cascade.offsetVoltage[stage])
                    + cascade.stageNoiseAt[point][stage];
        }
        return true;
    };

    Lane lanes[2];
    if (!prepareLane(first, firstInput, firstOmegaStep, firstFeedback,
                     firstHeadroom, lanes[0])
        || !prepareLane(second, secondInput, secondOmegaStep, secondFeedback,
                        secondHeadroom, lanes[1]))
        return false;

#if defined(__aarch64__) && defined(__ARM_NEON)
    using Pair = float64x2_t;
    const auto pack = [](double low, double high) {
        return vsetq_lane_f64(high, vdupq_n_f64(low), 1);
    };
#define pairLow(value) vgetq_lane_f64((value), 0)
#define pairHigh(value) vgetq_lane_f64((value), 1)
#define pairSplat(value) vdupq_n_f64(value)
#define pairAdd(first, second) vaddq_f64((first), (second))
#define pairSubtract(first, second) vsubq_f64((first), (second))
#define pairMultiply(first, second) vmulq_f64((first), (second))
#define pairMultiplyScalar(value, scalar) vmulq_n_f64((value), (scalar))
#define pairMultiplyAdd(addend, first, second) \
    vfmaq_f64((addend), (first), (second))
#define pairMultiplyAddScalar(addend, value, scalar) \
    vfmaq_n_f64((addend), (value), (scalar))
#elif defined(__x86_64__) && defined(__SSE2__)
    using Pair = __m128d;
    const auto pack = [](double low, double high) {
        return _mm_set_pd(high, low);
    };
#define pairLow(value) _mm_cvtsd_f64(value)
#define pairHigh(value) _mm_cvtsd_f64(_mm_unpackhi_pd((value), (value)))
#define pairSplat(value) _mm_set1_pd(value)
#define pairAdd(first, second) _mm_add_pd((first), (second))
#define pairSubtract(first, second) _mm_sub_pd((first), (second))
#define pairMultiply(first, second) _mm_mul_pd((first), (second))
#define pairMultiplyScalar(value, scalar) \
    _mm_mul_pd((value), _mm_set1_pd(scalar))
    // Baseline x86_64 has SSE2 but not FMA. Keep the scalar path's separate
    // multiply and add rounding rather than changing the solve along with its
    // scheduling.
#define pairMultiplyAdd(addend, first, second) \
    _mm_add_pd((addend), _mm_mul_pd((first), (second)))
#define pairMultiplyAddScalar(addend, value, scalar) \
    _mm_add_pd((addend), _mm_mul_pd((value), _mm_set1_pd(scalar)))
#endif
    using PairState = std::array<Pair, 4>;
    const auto polyTanhPair = [&](Pair value) {
        const Pair u = pairMultiply(value, value);
        if (pairLow(u) >= 1.0 || pairHigh(u) >= 1.0)
            return pack(polyZonedTanhImpl(pairLow(value)),
                        polyZonedTanhImpl(pairHigh(value)));

        const Pair uSquared = pairMultiply(u, u);
        const Pair top = pairMultiplyAddScalar(
            pairSplat(0.016720855179576926), u,
            -0.00305822903759879);
        const Pair high = pairMultiplyAddScalar(
            pairSplat(0.1328072064598552), u,
            -0.051585988404218436);
        const Pair low = pairMultiplyAddScalar(
            pairSplat(0.9999993948006496), u,
            -0.3332895137021704);
        const Pair upper = pairMultiplyAdd(high, top, uSquared);
        const Pair factor = pairMultiplyAdd(low, upper, uSquared);
        return pairMultiply(value, factor);
    };
    const auto cubicEarlyPair = [&](Pair value) {
        if (std::abs(pairLow(value)) >= 1.5
            || std::abs(pairHigh(value)) >= 1.5)
            return pack(cubicEarlyTanh(pairLow(value)),
                        cubicEarlyTanh(pairHigh(value)));

        const Pair scaled = pairMultiplyScalar(value, -(4.0 / 27.0));
        const Pair factor = pairMultiplyAdd(pairSplat(1.0), scaled, value);
        return pairMultiply(value, factor);
    };

    PairState state;
    PairState stageOmega;
    std::array<PairState, 7> stageOffset;
    for (std::size_t stage = 0; stage < state.size(); ++stage)
    {
        state[stage] = pack(first.state[stage], second.state[stage]);
        stageOmega[stage] = pack(lanes[0].stageOmega[stage],
                                 lanes[1].stageOmega[stage]);
        for (std::size_t point = 0; point < 7; ++point)
            stageOffset[point][stage] = pack(lanes[0].stageOffset[point][stage],
                                           lanes[1].stageOffset[point][stage]);
    }
    const Pair inverseHeadroom = pack(lanes[0].inverseHeadroom,
                                      lanes[1].inverseHeadroom);
    const Pair runningHeadroom = pack(lanes[0].headroom, lanes[1].headroom);

    // Read per cascade rather than broadcast: the coefficient is uniform
    // across voices in practice, but two plug-in instances can differ.
    const Pair resonanceCompensation = pack(
        static_cast<double>(first.inputCompensationCoefficient),
        static_cast<double>(second.inputCompensationCoefficient));
    const Pair resonanceOffset = pack(
        static_cast<double>(first.resonanceOffsetVolts),
        static_cast<double>(second.resonanceOffsetVolts));
    const Pair inverseLoopHeadroom = pack(
        lanes[0].inverseLoopHeadroom, lanes[1].inverseLoopHeadroom);
    const auto derivative = [&](const PairState& value, Pair drive, std::size_t point) {
        const Pair feedbackArgument = pairMultiply(
            pairAdd(pairSubtract(value[3],
                                 pairMultiply(resonanceCompensation, drive)),
                    resonanceOffset),
            inverseLoopHeadroom);
        const Pair feedbackTanh = polyTanhPair(feedbackArgument);
        const double firstLoopReturn = lanes[0].feedback == 0.0
            ? pairLow(drive)
            : pairLow(drive)
                - lanes[0].feedback * lanes[0].loopHeadroom
                    * pairLow(feedbackTanh);
        const double secondLoopReturn = lanes[1].feedback == 0.0
            ? pairHigh(drive)
            : pairHigh(drive)
                - lanes[1].feedback * lanes[1].loopHeadroom
                    * pairHigh(feedbackTanh);
        const Pair loopReturn = pack(firstLoopReturn, secondLoopReturn);

        PairState stageArgument {
            pairMultiply(pairAdd(pairSubtract(loopReturn, value[0]),
                                 stageOffset[point][0]), inverseHeadroom),
            pairMultiply(pairAdd(pairSubtract(value[0], value[1]),
                                 stageOffset[point][1]), inverseHeadroom),
            pairMultiply(pairAdd(pairSubtract(value[1], value[2]),
                                 stageOffset[point][2]), inverseHeadroom),
            pairMultiply(pairAdd(pairSubtract(value[2], value[3]),
                                 stageOffset[point][3]), inverseHeadroom)
        };
        PairState stageTanh;
        for (std::size_t stage = 0; stage < stageTanh.size(); ++stage)
            stageTanh[stage] = polyTanhPair(stageArgument[stage]);

        PairState early;
        if (!applyEarlyEffect)
        {
            for (auto& valueAtStage : early)
                valueAtStage = pairSplat(1.0);
        }
        else
        {
            for (std::size_t stage = 0; stage < early.size(); ++stage)
            {
                const Pair earlyTanh = cubicEarlyPair(
                    pairMultiply(value[stage], inverseHeadroom));
                early[stage] = pairMultiplyAddScalar(
                    pairSplat(1.0), earlyTanh, earlyAmount);
            }
        }

        PairState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            result[stage] = pairMultiply(stageOmega[stage], early[stage]);
            result[stage] = pairMultiply(result[stage], runningHeadroom);
            result[stage] = pairMultiply(result[stage], stageTanh[stage]);
        }
        return result;
    };
    const auto advanceOne = [](const PairState& origin,
                               const PairState& slope, double distance) {
        PairState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
            result[stage] = pairMultiplyAddScalar(
                origin[stage], slope[stage], distance);
        return result;
    };
    const auto advanceTwo = [](const PairState& origin, double stepSize,
                               const PairState& firstSlope,
                               double firstWeight,
                               const PairState& secondSlope,
                               double secondWeight) {
        PairState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            Pair weighted = pairMultiplyScalar(
                secondSlope[stage], secondWeight);
            weighted = pairMultiplyAddScalar(
                weighted, firstSlope[stage], firstWeight);
            result[stage] = pairMultiplyAddScalar(
                origin[stage], weighted, stepSize);
        }
        return result;
    };
    const auto advanceThree = [](const PairState& origin, double stepSize,
                                 const PairState& firstSlope,
                                 double firstWeight,
                                 const PairState& secondSlope,
                                 double secondWeight,
                                 const PairState& thirdSlope,
                                 double thirdWeight) {
        PairState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            Pair weighted = pairMultiplyScalar(
                secondSlope[stage], secondWeight);
            weighted = pairMultiplyAddScalar(
                weighted, firstSlope[stage], firstWeight);
            weighted = pairMultiplyAddScalar(
                weighted, thirdSlope[stage], thirdWeight);
            result[stage] = pairMultiplyAddScalar(
                origin[stage], weighted, stepSize);
        }
        return result;
    };

    for (const std::size_t start : { 0u, 3u })
    {
        const PairState origin = state;
        const Pair k1Drive = pack(lanes[0].drive[start],
                                  lanes[1].drive[start]);
        const Pair sharedDrive = pack(lanes[0].drive[start + 1u],
                                      lanes[1].drive[start + 1u]);
        const Pair k4Drive = pack(lanes[0].drive[start + 2u],
                                  lanes[1].drive[start + 2u]);
        const Pair k5Drive = pack(lanes[0].drive[start + 3u],
                                  lanes[1].drive[start + 3u]);
        const PairState k1 = derivative(origin, k1Drive, start);
        const PairState k2 = derivative(
            advanceOne(origin, k1, 1.0 / 6.0), sharedDrive, start + 1u);
        const PairState k3 = derivative(
            advanceTwo(origin, 0.5, k1, 1.0 / 6.0,
                       k2, 1.0 / 6.0), sharedDrive, start + 1u);
        const PairState k4 = derivative(
            advanceTwo(origin, 0.5, k1, 1.0 / 8.0,
                       k3, 3.0 / 8.0), k4Drive, start + 2u);
        const PairState k5 = derivative(
            advanceThree(origin, 0.5, k1, 1.0 / 2.0,
                         k3, -3.0 / 2.0, k4, 2.0), k5Drive, start + 3u);
        state = advanceThree(origin, 0.5, k1, 1.0 / 6.0,
                             k4, 2.0 / 3.0, k5, 1.0 / 6.0);
    }

    for (std::size_t stage = 0; stage < state.size(); ++stage)
    {
        first.state[stage] = pairLow(state[stage]);
        second.state[stage] = pairHigh(state[stage]);
    }

#undef pairLow
#undef pairHigh
#undef pairSplat
#undef pairAdd
#undef pairSubtract
#undef pairMultiply
#undef pairMultiplyScalar
#undef pairMultiplyAdd
#undef pairMultiplyAddScalar

    const auto finishLane = [](Lane& lane, float& output) {
        auto& cascade = *lane.cascade;
        for (std::size_t point = cascade.inputHistory.size() - 1u;
             point > 0u; --point)
            cascade.inputHistory[point] = cascade.inputHistory[point - 1u];
        cascade.inputHistory[0] = lane.input;
        cascade.inputHistoryCount = std::min(cascade.inputHistoryCount + 1, 2);
        cascade.previousOmegaStep = lane.omega;
        cascade.previousFeedback = lane.feedback;
        cascade.previousHeadroom = lane.headroom;

        const bool valid = std::all_of(
            cascade.state.begin(), cascade.state.end(), [](double value) {
                return std::abs(value) <= 64.0;
            });
        if (!valid)
        {
            cascade.state.fill(0.0);
            cascade.inputHistory.fill(lane.input);
            cascade.inputHistoryCount = 2;
            output = 0.0f;
            return;
        }
        output = static_cast<float>(cascade.state[3]);
    };
    finishLane(lanes[0], firstOutput);
    finishLane(lanes[1], secondOutput);
    return true;
}

YOUKNOW_MERSON_NOINLINE
bool YouKnowEngine::OtaCascade::tryProcessSettledMersonQuad(
    const std::array<OtaCascade*, 4>& cascades,
    const std::array<float, 4>& inputs,
    const std::array<float, 4>& omegaSteps,
    const std::array<float, 4>& feedbacks,
    const std::array<float, 4>& headrooms,
    bool enableEarlyEffect, float calibration,
    std::array<float, 4>& outputs) noexcept
{
    const double currentCalibration = std::clamp(
        std::isfinite(calibration) ? static_cast<double>(calibration) : 0.0,
        0.0, static_cast<double>(EngineParameters::calibrationCeiling));
    const bool applyEarlyEffect = enableEarlyEffect
                               && currentCalibration > 0.0;
    const double earlyAmountDouble =
        static_cast<double>(otaEarlyEffectCoefficient)
        * currentCalibration;
    const float earlyAmount = static_cast<float>(earlyAmountDouble);
    const double earlyCeiling = applyEarlyEffect
        ? 1.0 + earlyAmountDouble : 1.0;

    struct Lane
    {
        OtaCascade* cascade {};
        double input {};
        double omega {};
        double feedback {};
        double headroom {};
        float inverseHeadroom {};
        float loopHeadroom {};
        float inverseLoopHeadroom {};
        std::array<float, 7> drive {};
        std::array<float, 4> stageOmega {};
        std::array<std::array<float, 4>, 7> stageOffset {};
    };
    std::array<Lane, 4> lanes;
    for (std::size_t laneIndex = 0; laneIndex < lanes.size(); ++laneIndex)
    {
        auto& cascade = *cascades[laneIndex];
        auto& lane = lanes[laneIndex];
        const double currentOmega = clampOmegaStep(
            static_cast<double>(omegaSteps[laneIndex]));
        const double currentFeedback = std::clamp(
            std::isfinite(feedbacks[laneIndex])
                ? static_cast<double>(feedbacks[laneIndex]) : 0.0,
            0.0, 8.0);
        const double currentHeadroom = std::max(
            std::isfinite(headrooms[laneIndex])
                ? static_cast<double>(headrooms[laneIndex]) : 0.0,
            1.0e-5);
        if (!cascade.parameterHistoryPrimed
            || cascade.inputHistoryCount != 2
            || cascade.previousOmegaStep != currentOmega
            || cascade.previousFeedback != currentFeedback
            || cascade.previousHeadroom != currentHeadroom
            || !std::all_of(
                cascade.state.begin(), cascade.state.end(), [](double value) {
                    return std::abs(value) <= 64.0;
                }))
            return false;

        double largestScale = 0.0;
        for (const float scale : cascade.gScale)
            largestScale = std::max(
                largestScale, static_cast<double>(std::abs(scale)));
        if (planTableau(
                VcfSolverMode::Rk4Single,
                currentOmega * largestScale * earlyCeiling,
                currentFeedback) != Tableau::MersonHalf)
            return false;

        lane.cascade = &cascade;
        lane.input = std::isfinite(inputs[laneIndex])
            ? static_cast<double>(inputs[laneIndex]) : 0.0;
        lane.omega = currentOmega;
        lane.feedback = currentFeedback;
        lane.headroom = currentHeadroom;
        lane.inverseHeadroom = static_cast<float>(1.0 / currentHeadroom);
        lane.loopHeadroom = static_cast<float>(
            cascade.resonanceHeadroomFollowsStage
                ? resonanceHeadroomFor(currentHeadroom)
                : static_cast<double>(
                      VoicedResonanceCompatibilityProfile::loopHeadroomVolts));
        lane.inverseLoopHeadroom = 1.0f / lane.loopHeadroom;
        const auto reconstruct = [&](double currentWeight,
                                     double firstWeight,
                                     double secondWeight,
                                     double thirdWeight) {
            return static_cast<float>(
                currentWeight * lane.input
                + firstWeight * cascade.inputHistory[0]
                + secondWeight * cascade.inputHistory[1]
                + thirdWeight * cascade.inputHistory[2]);
        };
        lane.drive[0] = reconstruct(0.0,    1.0,    0.0,     0.0);
        lane.drive[1] = reconstruct(
            0x1.1f9add3c0ca45p-4, 0x1.0da12f684bda1p+0,
            -0x1.3425ed097b426p-3, 0x1.ba781948b0fcfp-6);
        lane.drive[2] = reconstruct(
            0.1171875, 1.0546875, -0.2109375, 0.0390625);
        lane.drive[3] = reconstruct(
            0.3125, 0.9375, -0.3125, 0.0625);
        lane.drive[4] = reconstruct(
            0x1.f9add3c0ca457p-2, 0x1.7b425ed097b42p-1,
            -0x1.2f684bda12f68p-2, 0x1.f9add3c0ca458p-5);
        lane.drive[5] = reconstruct(
            0.6015625, 0.6015625, -0.2578125, 0.0546875);
        lane.drive[6] = reconstruct(1.0,    0.0,    0.0,     0.0);
        cascade.prepareStageNoise(tableauNodeMask(Tableau::MersonHalf));
        for (std::size_t stage = 0; stage < lane.stageOmega.size(); ++stage)
        {
            lane.stageOmega[stage] = static_cast<float>(
                currentOmega * static_cast<double>(cascade.gScale[stage]));
            for (std::size_t point = 0; point < 7; ++point)
                lane.stageOffset[point][stage] = static_cast<float>(
                    cascade.offsetVoltage[stage] + cascade.stageNoiseAt[point][stage]);
        }
    }

#if defined(__aarch64__) && defined(__ARM_NEON)
    using Quad = float32x4_t;
    const auto quadLoad = [](const std::array<float, 4>& values) {
        return vld1q_f32(values.data());
    };
    const auto quadStore = [](Quad value, std::array<float, 4>& values) {
        vst1q_f32(values.data(), value);
    };
#define quadSplat(value) vdupq_n_f32(value)
#define quadAdd(first, second) vaddq_f32((first), (second))
#define quadSubtract(first, second) vsubq_f32((first), (second))
#define quadMultiply(first, second) vmulq_f32((first), (second))
#define quadMultiplyScalar(value, scalar) vmulq_n_f32((value), (scalar))
#define quadMultiplyAdd(addend, first, second) \
    vfmaq_f32((addend), (first), (second))
#define quadMultiplyAddScalar(addend, value, scalar) \
    vfmaq_n_f32((addend), (value), (scalar))
#define quadAnyGreaterEqual(value, limit) \
    (vmaxvq_f32(vabsq_f32(value)) >= (limit))
#elif defined(__x86_64__) && defined(__SSE2__)
    using Quad = __m128;
    const auto quadLoad = [](const std::array<float, 4>& values) {
        return _mm_loadu_ps(values.data());
    };
    const auto quadStore = [](Quad value, std::array<float, 4>& values) {
        _mm_storeu_ps(values.data(), value);
    };
#define quadSplat(value) _mm_set1_ps(value)
#define quadAdd(first, second) _mm_add_ps((first), (second))
#define quadSubtract(first, second) _mm_sub_ps((first), (second))
#define quadMultiply(first, second) _mm_mul_ps((first), (second))
#define quadMultiplyScalar(value, scalar) \
    _mm_mul_ps((value), _mm_set1_ps(scalar))
#define quadMultiplyAdd(addend, first, second) \
    _mm_add_ps((addend), _mm_mul_ps((first), (second)))
#define quadMultiplyAddScalar(addend, value, scalar) \
    _mm_add_ps((addend), _mm_mul_ps((value), _mm_set1_ps(scalar)))
#define quadAnyGreaterEqual(value, limit) \
    (_mm_movemask_ps(_mm_cmpge_ps( \
        _mm_andnot_ps(_mm_set1_ps(-0.0f), (value)), \
        _mm_set1_ps(limit))) != 0)
#endif
    using QuadState = std::array<Quad, 4>;
    const auto packField = [&](const auto& member, std::size_t point) {
        std::array<float, 4> values;
        for (std::size_t lane = 0; lane < lanes.size(); ++lane)
            values[lane] = member(lanes[lane], point);
        return quadLoad(values);
    };
    // Keep these small kernels in the RHS to avoid calls spilling its live SIMD
    // state. The polynomial and fallback are unchanged.
    const auto polyTanhQuad = [&](Quad value) __attribute__((always_inline)) {
        const Quad u = quadMultiply(value, value);
        const Quad uSquared = quadMultiply(u, u);
        const Quad top = quadMultiplyAddScalar(
            quadSplat(0.016720855179576926f), u,
            -0.00305822903759879f);
        const Quad high = quadMultiplyAddScalar(
            quadSplat(0.1328072064598552f), u,
            -0.051585988404218436f);
        const Quad low = quadMultiplyAddScalar(
            quadSplat(0.9999993948006496f), u,
            -0.3332895137021704f);
        const Quad upper = quadMultiplyAdd(high, top, uSquared);
        const Quad factor = quadMultiplyAdd(low, upper, uSquared);
        Quad result = quadMultiply(value, factor);
        if (quadAnyGreaterEqual(value, 1.0f))
        {
            std::array<float, 4> values;
            std::array<float, 4> results;
            quadStore(value, values);
            quadStore(result, results);
            for (std::size_t lane = 0; lane < values.size(); ++lane)
                if (std::abs(values[lane]) >= 1.0f)
                    results[lane] = static_cast<float>(
                        polyZonedTanhImpl(
                            static_cast<double>(values[lane])));
            result = quadLoad(results);
        }
        return result;
    };
    const auto cubicEarlyQuad = [&](Quad value) __attribute__((always_inline)) {
        const Quad scaled = quadMultiplyScalar(value, -(4.0f / 27.0f));
        const Quad factor = quadMultiplyAdd(quadSplat(1.0f), scaled, value);
        Quad result = quadMultiply(value, factor);
        if (quadAnyGreaterEqual(value, 1.5f))
        {
            std::array<float, 4> values;
            std::array<float, 4> results;
            quadStore(value, values);
            quadStore(result, results);
            for (std::size_t lane = 0; lane < values.size(); ++lane)
                if (std::abs(values[lane]) >= 1.5f)
                    results[lane] = static_cast<float>(
                        cubicEarlyTanh(values[lane]));
            result = quadLoad(results);
        }
        return result;
    };

    QuadState state;
    QuadState stageOmega;
    std::array<QuadState, 7> stageOffset;
    for (std::size_t stage = 0; stage < state.size(); ++stage)
    {
        std::array<float, 4> values;
        for (std::size_t lane = 0; lane < lanes.size(); ++lane)
            values[lane] = static_cast<float>(lanes[lane].cascade->state[stage]);
        state[stage] = quadLoad(values);
        stageOmega[stage] = packField(
            [](const Lane& lane, std::size_t point) {
                return lane.stageOmega[point];
            }, stage);
        for (std::size_t point = 0; point < 7; ++point)
            stageOffset[point][stage] = packField(
                [point](const Lane& lane, std::size_t index) {
                    return lane.stageOffset[point][index];
                }, stage);
    }
    const Quad inverseHeadroom = packField(
        [](const Lane& lane, std::size_t) {
            return lane.inverseHeadroom;
        }, 0u);
    const Quad runningHeadroom = packField(
        [](const Lane& lane, std::size_t) {
            return static_cast<float>(lane.headroom);
        }, 0u);
    const Quad feedbackGain = packField(
        [](const Lane& lane, std::size_t) {
            return static_cast<float>(lane.feedback) * lane.loopHeadroom;
        }, 0u);
    const Quad inverseLoopHeadroom = packField(
        [](const Lane& lane, std::size_t) {
            return lane.inverseLoopHeadroom;
        }, 0u);
    // Read per cascade rather than broadcast: the coefficient is uniform
    // across voices in practice, but two plug-in instances can differ.
    const Quad resonanceCompensation = quadLoad({
        cascades[0]->inputCompensationCoefficient,
        cascades[1]->inputCompensationCoefficient,
        cascades[2]->inputCompensationCoefficient,
        cascades[3]->inputCompensationCoefficient });
    const Quad resonanceOffset = quadLoad({
        cascades[0]->resonanceOffsetVolts,
        cascades[1]->resonanceOffsetVolts,
        cascades[2]->resonanceOffsetVolts,
        cascades[3]->resonanceOffsetVolts });
    std::array<Quad, 7> drives;
    for (std::size_t point = 0; point < drives.size(); ++point)
        drives[point] = packField(
            [](const Lane& lane, std::size_t index) {
                return lane.drive[index];
            }, point);

    // A value parameter lets the ARM ABI pass the four vectors in registers.
    const auto derivative = [&](QuadState value, Quad drive, std::size_t point) {
        const Quad feedbackArgument = quadMultiply(
            quadAdd(quadSubtract(value[3],
                                 quadMultiply(resonanceCompensation, drive)),
                    resonanceOffset),
            inverseLoopHeadroom);
        const Quad loopReturn = quadSubtract(
            drive, quadMultiply(feedbackGain,
                                polyTanhQuad(feedbackArgument)));
        QuadState stageArgument {
            quadMultiply(quadAdd(quadSubtract(loopReturn, value[0]),
                                 stageOffset[point][0]), inverseHeadroom),
            quadMultiply(quadAdd(quadSubtract(value[0], value[1]),
                                 stageOffset[point][1]), inverseHeadroom),
            quadMultiply(quadAdd(quadSubtract(value[1], value[2]),
                                 stageOffset[point][2]), inverseHeadroom),
            quadMultiply(quadAdd(quadSubtract(value[2], value[3]),
                                 stageOffset[point][3]), inverseHeadroom)
        };
        QuadState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            Quad early = quadSplat(1.0f);
            if (applyEarlyEffect)
                early = quadMultiplyAddScalar(
                    early,
                    cubicEarlyQuad(quadMultiply(
                        value[stage], inverseHeadroom)),
                    earlyAmount);
            result[stage] = quadMultiply(stageOmega[stage], early);
            result[stage] = quadMultiply(result[stage], runningHeadroom);
            result[stage] = quadMultiply(
                result[stage], polyTanhQuad(stageArgument[stage]));
        }
        return result;
    };
    const auto advanceOne = [](const QuadState& origin,
                               const QuadState& slope, float distance) {
        QuadState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
            result[stage] = quadMultiplyAddScalar(
                origin[stage], slope[stage], distance);
        return result;
    };
    const auto advanceTwo = [](const QuadState& origin, float stepSize,
                               const QuadState& firstSlope,
                               float firstWeight,
                               const QuadState& secondSlope,
                               float secondWeight) {
        QuadState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            Quad weighted = quadMultiplyScalar(
                secondSlope[stage], secondWeight);
            weighted = quadMultiplyAddScalar(
                weighted, firstSlope[stage], firstWeight);
            result[stage] = quadMultiplyAddScalar(
                origin[stage], weighted, stepSize);
        }
        return result;
    };
    const auto advanceThree = [](const QuadState& origin, float stepSize,
                                 const QuadState& firstSlope,
                                 float firstWeight,
                                 const QuadState& secondSlope,
                                 float secondWeight,
                                 const QuadState& thirdSlope,
                                 float thirdWeight) {
        QuadState result;
        for (std::size_t stage = 0; stage < result.size(); ++stage)
        {
            Quad weighted = quadMultiplyScalar(
                secondSlope[stage], secondWeight);
            weighted = quadMultiplyAddScalar(
                weighted, firstSlope[stage], firstWeight);
            weighted = quadMultiplyAddScalar(
                weighted, thirdSlope[stage], thirdWeight);
            result[stage] = quadMultiplyAddScalar(
                origin[stage], weighted, stepSize);
        }
        return result;
    };
    for (const std::size_t start : { 0u, 3u })
    {
        const QuadState origin = state;
        const QuadState k1 = derivative(origin, drives[start], start);
        const QuadState k2 = derivative(
            advanceOne(origin, k1, 1.0f / 6.0f), drives[start + 1u], start + 1u);
        const QuadState k3 = derivative(
            advanceTwo(origin, 0.5f, k1, 1.0f / 6.0f,
                       k2, 1.0f / 6.0f), drives[start + 1u], start + 1u);
        const QuadState k4 = derivative(
            advanceTwo(origin, 0.5f, k1, 1.0f / 8.0f,
                       k3, 3.0f / 8.0f), drives[start + 2u], start + 2u);
        const QuadState k5 = derivative(
            advanceThree(origin, 0.5f, k1, 0.5f,
                         k3, -1.5f, k4, 2.0f), drives[start + 3u], start + 3u);
        state = advanceThree(origin, 0.5f, k1, 1.0f / 6.0f,
                             k4, 2.0f / 3.0f, k5, 1.0f / 6.0f);
    }

    for (std::size_t stage = 0; stage < state.size(); ++stage)
    {
        std::array<float, 4> values;
        quadStore(state[stage], values);
        for (std::size_t lane = 0; lane < lanes.size(); ++lane)
            lanes[lane].cascade->state[stage] = values[lane];
    }

#undef quadSplat
#undef quadAdd
#undef quadSubtract
#undef quadMultiply
#undef quadMultiplyScalar
#undef quadMultiplyAdd
#undef quadMultiplyAddScalar
#undef quadAnyGreaterEqual

    for (std::size_t laneIndex = 0; laneIndex < lanes.size(); ++laneIndex)
    {
        auto& lane = lanes[laneIndex];
        auto& cascade = *lane.cascade;
        for (std::size_t point = cascade.inputHistory.size() - 1u;
             point > 0u; --point)
            cascade.inputHistory[point] = cascade.inputHistory[point - 1u];
        cascade.inputHistory[0] = lane.input;
        cascade.inputHistoryCount = std::min(cascade.inputHistoryCount + 1, 2);
        cascade.previousOmegaStep = lane.omega;
        cascade.previousFeedback = lane.feedback;
        cascade.previousHeadroom = lane.headroom;
        const bool valid = std::all_of(
            cascade.state.begin(), cascade.state.end(), [](double value) {
                return std::abs(value) <= 64.0;
            });
        if (!valid)
        {
            cascade.state.fill(0.0);
            cascade.inputHistory.fill(lane.input);
            cascade.inputHistoryCount = 2;
            outputs[laneIndex] = 0.0f;
        }
        else
        {
            outputs[laneIndex] = static_cast<float>(cascade.state[3]);
        }
    }
    return true;
}
#undef YOUKNOW_MERSON_NOINLINE
#endif

void YouKnowEngine::HighPass::reset() noexcept
{
    state = 0.0;
}

float YouKnowEngine::HighPass::process(float input, float g,
                                          float shelfGain,
                                          float highGain) noexcept
{
    // Topology-preserving single pole. The cutting legs pass the high band at
    // unity and discard the low band; the boost leg is a real shelf, lifting
    // the low band strongly and the high band slightly, as the derived
    // branch does.
    const double v = (static_cast<double>(input) - state)
                   * static_cast<double>(g)
                   / (1.0 + static_cast<double>(g));
    const double low = v + state;
    state = low + v;
    if (!std::isfinite(state))
        state = 0.0;
    const double high = static_cast<double>(input) - low;
    return static_cast<float>(static_cast<double>(highGain) * high
                            + static_cast<double>(shelfGain) * low);
}

void YouKnowEngine::BoostBranch::reset() noexcept
{
    vN = 0.0;
    vC9 = 0.0;
    vC6 = 0.0;
    linkState = 0.0;
    c6State = 0.0;
    selected = false;
}

void YouKnowEngine::updateBoostBranchCoefficients() noexcept
{
    const double h = static_cast<double>(inverseOversampledRate_);
    auto& c = boostBranchCoefficients_;
    // Driven configuration: with Y3 held at the bus, C9 is across a known
    // voltage and the only free state is w = vN - share * u, a single pole
    // at R22 (C8 + C9) -- the same 59.41 Hz corner the collapsed shelf used,
    // bilinear like every other one-pole in this file.
    const double linkTau = boostLinkOhms * (boostLinkFarads + boostShuntFarads);
    c.linkG = std::tan(0.5 * h / linkTau);
    // IC4b's feedback pole, R18 C6 (72.34 Hz), driven by R18/R19 times the
    // voltage on its inverting input.
    const double c6Tau = boostFeedbackOhms * boostFeedbackFarads;
    c.c6G = std::tan(0.5 * h / c6Tau);
    // Undriven configuration, exact trapezoidal step of the linear pair
    //   C8  dvN/dt  = -(vN + vC9) / R25
    //   C9  dvC9/dt = -vC9 / R22 - (vN + vC9) / R25
    // with Y3 = vN + vC9 hanging on R25 into the virtual earth.
    const double a00 = -1.0 / (boostSumOhms * boostShuntFarads);
    const double a01 = a00;
    const double a10 = -1.0 / (boostSumOhms * boostLinkFarads);
    const double a11 = -(1.0 / boostLinkOhms + 1.0 / boostSumOhms)
                     / boostLinkFarads;
    // (I - hA/2)^-1 (I + hA/2)
    const double p00 = 1.0 - 0.5 * h * a00, p01 = -0.5 * h * a01;
    const double p10 = -0.5 * h * a10,      p11 = 1.0 - 0.5 * h * a11;
    const double q00 = 1.0 + 0.5 * h * a00, q01 = 0.5 * h * a01;
    const double q10 = 0.5 * h * a10,       q11 = 1.0 + 0.5 * h * a11;
    const double det = p00 * p11 - p01 * p10;
    const double i00 = p11 / det, i01 = -p01 / det;
    const double i10 = -p10 / det, i11 = p00 / det;
    c.m00 = i00 * q00 + i01 * q10;
    c.m01 = i00 * q01 + i01 * q11;
    c.m10 = i10 * q00 + i11 * q10;
    c.m11 = i10 * q01 + i11 * q11;
}

float YouKnowEngine::processBoostBranch(float coupled,
                                           bool selected) noexcept
{
    auto& b = boostBranch_;
    const auto& c = boostBranchCoefficients_;
    const double u = static_cast<double>(coupled);

    if (selected)
    {
        if (!b.selected)
        {
            // Re-selection puts the bus step across C9 in series with C8;
            // the charge redistributes and N jumps by C9/(C8+C9) of it. The
            // integrator behind the driven pole restarts at rest on the new
            // node voltage.
            b.vN += boostLinkShare * (u - b.vN - b.vC9);
            b.linkState = b.vN - boostLinkShare * u;
            b.selected = true;
        }
        // Bilinear one-pole on w = vN - share*u with input (1 - share)*u:
        // v = (x - s) g/(1+g), w = v + s, s <- w + v.
        const double v = ((1.0 - boostLinkShare) * u - b.linkState) * c.linkG
                       / (1.0 + c.linkG);
        const double w = v + b.linkState;
        b.linkState = w + v;
        b.vN = w + boostLinkShare * u;
        b.vC9 = u - b.vN;
    }
    else
    {
        b.selected = false;
        const double vN = c.m00 * b.vN + c.m01 * b.vC9;
        const double vC9 = c.m10 * b.vN + c.m11 * b.vC9;
        b.vN = vN;
        b.vC9 = vC9;
    }

    // IC4b: non-inverting, its inverting input tracks N while the output is
    // inside its swing; C6 charges from R19's current less its own R18 leak,
    // so its voltage settles to (R18/R19) times the inverting input.
    // The output bound is the shared summer-stage swing policy (OQ-05): a
    // x11 stage inside +/-15 V rails cannot pass more than about 1.2 V of
    // low band linearly, so on a loud bass chord in Boost this is the first
    // stage in the chain to run out of rail.
    const auto stepC6 = [&](double inverting, double& stateOut) {
        const double x = (boostAmplifierDcGain - 1.0) * inverting;
        const double v = (x - b.c6State) * c.c6G / (1.0 + c.c6G);
        const double y = v + b.c6State;
        stateOut = y + v;
        return y;
    };
    double c6State = b.c6State;
    double vC6 = stepC6(b.vN, c6State);
    double linear = b.vN + vC6;
    double out = static_cast<double>(outputSummerClip(static_cast<float>(linear)));
    if (std::abs(out - linear) > 1.0e-6 * std::max(1.0, std::abs(linear)))
    {
        // Saturated: the inverting input is what the clipped output leaves
        // across the divider, not N. One corrected step is enough at these
        // corners (940 us and slower against a 5 us grid).
        vC6 = stepC6(out - vC6, c6State);
        linear = b.vN + vC6;
        out = static_cast<double>(outputSummerClip(static_cast<float>(linear)));
    }
    b.vC6 = vC6;
    b.c6State = c6State;
    if (!std::isfinite(b.vN) || !std::isfinite(b.vC9) || !std::isfinite(b.vC6)
        || !std::isfinite(b.linkState) || !std::isfinite(b.c6State))
        b.reset();

    const double y3 = selected ? u : b.vN + b.vC9;
    return static_cast<float>(boostDirectGain * y3 + boostReturnGain * out);
}

void YouKnowEngine::HalfbandDecimator::reset() noexcept
{
    left.fill(0.0f);
    right.fill(0.0f);
    writeIndex = 0;
}

// ---------------------------------------------------------------------------
// Construction and preparation
// ---------------------------------------------------------------------------

YouKnowEngine::YouKnowEngine() noexcept
{
    buildHalfbandKernel();
    (void) correctionTables();
    // Function-local statics are thread-safe, but their first-use guards and
    // exponentials do not belong in the first audio callback.
    (void) chassisGradientMeanCelsius();
    (void) SubLevelDiodeLaw::table();
    buildVoiceCards();
    refreshVoiceCardThermalScales();
    clearHeldNotes();
}

void YouKnowEngine::buildHalfbandKernel() noexcept
{
    // Kaiser-windowed half-band. The historical sixty-three-tap
    // Blackman-Harris boundary had a deep far stopband but spent too many taps
    // reaching it: its main lobe put the last decimation stage's transition at
    // roughly 18 to 30 kHz. At a 44.1 kHz host that left the top of the audio
    // band inside the transition -- 0.85 dB down at 20 kHz, and content
    // folding onto 19.1 kHz rejected by only 31.7 dB.
    //
    // Kaiser trades stopband depth for transition width continuously. At 95
    // taps the selected common-host design passes the expanded 44.1/48 kHz DCO
    // audit: the shorter 63-tap boundary leaked the 25.1 kHz sixth pulse
    // harmonic back onto 19.0 kHz at 44.1 kHz. The longer boundary keeps that
    // line below the declared -70 dBc numerical-fidelity gate while retaining
    // the 20 kHz passband contract.
    //
    // The Bessel function is written out below rather than taken from the
    // standard special-function header, which is not available on every
    // toolchain this project builds with -- the reason the window was
    // originally avoided.
    const auto besselI0 = [](double x) noexcept {
        double sum = 1.0;
        double term = 1.0;
        for (int k = 1; k < 64; ++k)
        {
            const double ratio = x / (2.0 * static_cast<double>(k));
            term *= ratio * ratio;
            sum += term;
            if (term < 1.0e-18 * sum)
                break;
        }
        return sum;
    };
    // The B-2 pitch grid also places that same 24.109 kHz line just inside the
    // last 44.1 kHz decimator's transition. Beta 7.7 narrows that transition
    // enough to retain about 1.5 dB of margin over the -70 dBc gate while the
    // measured far stopband remains approximately 80 dB. Tap count, latency
    // and hot-loop work are unchanged.
    constexpr double kaiserBeta = 7.7;
    const double besselDenominator = besselI0(kaiserBeta);

    constexpr int centre = (halfbandTaps - 1) / 2;
    for (int n = 0; n < halfbandTaps; ++n)
    {
        const float offset = static_cast<float>(n - centre);
        float ideal;
        if (std::abs(offset) < 1.0e-6f)
        {
            ideal = 0.5f;
        }
        else if (((n - centre) & 1) == 0)
        {
            // Every other non-centre half-band tap is analytically zero.
            // Spelling that out avoids platform-dependent sin(k*pi) crumbs
            // and lets downsamplePair skip those multiplies exactly.
            ideal = 0.0f;
        }
        else
        {
            const float x = pi * offset * 0.5f;
            ideal = 0.5f * std::sin(x) / x;
        }

        const double t = 2.0 * static_cast<double>(n)
                             / static_cast<double>(halfbandTaps - 1) - 1.0;
        const double window =
            besselI0(kaiserBeta * std::sqrt(std::max(0.0, 1.0 - t * t)))
            / besselDenominator;
        halfbandKernel_[static_cast<std::size_t>(n)] =
            ideal * static_cast<float>(window);
    }

    // The ideal and window equations are symmetric, but C++ does not require
    // independent libm evaluations at +/-x to round identically. The paired
    // hot loop needs bit-equal coefficients, so make that design invariant
    // explicit before normalisation instead of merely observing it locally.
    for (int tap = 0; tap < centre; ++tap)
        halfbandKernel_[static_cast<std::size_t>(halfbandTaps - 1 - tap)] =
            halfbandKernel_[static_cast<std::size_t>(tap)];

    double sum = 0.0;
    for (const float tap : halfbandKernel_)
        sum += static_cast<double>(tap);

    // Normalise to exactly unity gain at DC so decimation cannot shift level.
    if (sum > 1.0e-9)
    {
        for (auto& tap : halfbandKernel_)
            tap = static_cast<float>(static_cast<double>(tap) / sum);

        double normalisedSum = 0.0;
        for (const float tap : halfbandKernel_)
            normalisedSum += static_cast<double>(tap);
        halfbandKernel_[static_cast<std::size_t>(centre)] +=
            static_cast<float>(1.0 - normalisedSum);
    }

    // Compact the taps the decimator will actually accumulate. Normalisation
    // above divides every entry by one positive scalar and then corrects the
    // centre, so the analytic zeros are still exactly zero here and this
    // reproduces the same set the old per-tap branch selected, in the same
    // order.
    halfbandActiveTapCount_ = 0;
    for (int tap = 0; tap < halfbandTaps; ++tap)
    {
        const float coefficient = halfbandKernel_[static_cast<std::size_t>(tap)];
        if (coefficient == 0.0f)
            continue;
        auto& active = halfbandActiveTaps_[
            static_cast<std::size_t>(halfbandActiveTapCount_++)];
        active.coefficient = coefficient;
        active.tap = tap;
    }
}

void YouKnowEngine::buildVoiceCards() noexcept
{
    // One draw per real dispersion mechanism. The envelopes themselves carry
    // no draw: they are computed digitally in the shared processor, so every
    // voice's envelope is identical and what disperses is the analogue chain
    // each one drives.
    for (int index = 0; index < maxVoices; ++index)
    {
        auto& card = cards_[static_cast<std::size_t>(index)];
        const std::uint32_t seed = static_cast<std::uint32_t>(index) * 2654435761u + 17u;
        // Preserve the previous card draw's direction of charging-current
        // error while expressing the physical inverse capacitance relation.
        card.dcoComponents.capacitorDraw = -hashBipolar(seed);
        for (std::size_t range = 0; range < 3; ++range)
            card.dcoComponents.resistorDraw[range] =
                hashBipolar(seed + 40u + static_cast<std::uint32_t>(range));
        card.comparatorOffset = hashBipolar(seed + 1u);
        card.cutoffOffsetError = hashBipolar(seed + 2u);
        card.resonanceError = hashBipolar(seed + 3u);
        card.vcaControlOffset = hashBipolar(seed + 4u);
        card.cutoffScaleError = hashBipolar(seed + 5u);
        card.subLevelError = hashBipolar(seed + 6u);
        card.driftPhase = 0.5f * (hashBipolar(seed + 7u) + 1.0f);
        card.vcaGainError = hashBipolar(seed + 8u);
        card.noiseLevelError = hashBipolar(seed + 9u);
        card.agingWeight = 0.5f * (hashBipolar(seed + 30u) + 1.0f);
        card.agingCutoffCounts = 0.0f;
        for (std::size_t stage = 0; stage < 4; ++stage)
        {
            card.vcfStageOffsets[stage] =
                0.0015f * hashBipolar(seed + 10u + static_cast<std::uint32_t>(stage));
            card.vcfStageGErrors[stage] =
                hashBipolar(seed + 20u + static_cast<std::uint32_t>(stage));
        }
        card.resonanceOtaOffset = 0.0015f * hashBipolar(seed + 14u);
        card.driftValue = 0.0f;
        card.driftState = seed | 1u;
    }
}

void YouKnowEngine::refreshAgedUnitState() noexcept
{
    // The aged-unit extension precomputes here -- called only where `aging`
    // can actually change (setParameters and reset), never on the per-note
    // path -- so the render paths stay pure: the per-card flatward cutoff
    // shift in converter counts, and one shared noise-trim gain. Both are
    // exactly inert at aging zero.
    const float aging = activeParameters_.aging;
    for (auto& card : cards_)
        card.agingCutoffCounts =
            aging > 0.0f ? agingCutoffDriftCents / 1200.0f * vcfCountsPerOctave
                               * aging * card.agingWeight
                         : 0.0f;
    agedNoiseGain_ =
        aging > 0.0f
            ? std::pow(10.0f, agingNoiseDriftDecibels * aging / 20.0f)
            : 1.0f;
}

void YouKnowEngine::refreshVoiceCardStageTrims() noexcept
{
    // A differential pair's input offset has a population mean of zero: there
    // is no nominal V_os a calibrated model could carry, and no service step
    // trims one. The draw above is signed and unbiased, so the whole term is
    // card-to-card spread and belongs entirely to Unit Character -- at zero,
    // the calibrated nominal model, it must be absent rather than merely
    // small. The card array itself stays the unscaled physical draw, as every
    // other card field does.
    const float amount = activeParameters_.enableVcfStageOffsets
                       ? activeParameters_.calibration : 0.0f;
    const float resonanceAmount = activeParameters_.enableResonanceOtaOffset
                                ? activeParameters_.calibration : 0.0f;
    for (auto& voice : voices_)
    {
        const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
        // The same pair-to-node conversion, through the resonance OTA's own
        // 100k/1.5k divider rather than the stages' 560/68560 one.
        voice.filter.resonanceOffsetVolts = card.resonanceOtaOffset
            * VoicedResonanceCompatibilityProfile::loopDividerRatio
            * resonanceAmount;
        for (std::size_t stage = 0; stage < 4; ++stage)
        {
            // The draw is volts at the pair; the cascade sums it with
            // module-node voltages that reach the pair through the anchored
            // 560/68560 divider, so the node-coordinate offset is the draw
            // divided by that attenuation. Handing the pair value to the node
            // unconverted -- as a previous revision did -- scales the
            // mechanism down 122x and mutes it.
            voice.filter.offsetVoltage[stage] =
                card.vcfStageOffsets[stage] / stageAttenuation * amount;
            // Each stage integrates into its own capacitor, so the four poles
            // do not coincide the way one shared `g` makes them. Four
            // mathematically identical poles give a resonance peak and a
            // self-oscillation more symmetric and purer than any real
            // four-section filter produces. Like the offsets, this is signed
            // and unbiased -- there is no nominal mismatch, so at Unit
            // Character zero all four collapse to unity.
            voice.filter.gScale[stage] =
                1.0f + card.vcfStageGErrors[stage] * vcfStageCapacitorTolerance
                           * amount;
        }
    }
}

void YouKnowEngine::refreshVoiceCardThermalScales() noexcept
{
    for (int index = 0; index < maxVoices; ++index)
        cards_[static_cast<std::size_t>(index)].thermalFilterOmegaScale =
            thermalFilterOmegaScaleFor(activeParameters_, index,
                                       thermalWarmupFraction_);
    refreshCardJohnsonTemperatureScales();
}

void YouKnowEngine::refreshCardJohnsonTemperatureScales() noexcept
{
    // Thermal noise power is proportional to absolute temperature. Cache the
    // amplitude ratio on the existing ~375 Hz wall-clock control cadence,
    // rather than performing a square root for every card on every audio
    // sample. Even the accelerated 15 C / 3 s warm-up changes this ratio by
    // less than 0.000023 between updates. The cadence survives quality changes.
    for (int index = 0; index < maxVoices; ++index)
    {
        const float kelvin = voiceCardCelsius(
            activeParameters_, index, thermalWarmupFraction_) + 273.15f;
        cards_[static_cast<std::size_t>(index)].johnsonTemperatureScale =
            std::sqrt(kelvin / outputNoiseTemperatureKelvin);
    }
}

void YouKnowEngine::refreshVoiceCardServiceTrims() noexcept
{
    // Roland p. 19 trims EACH card after at least ten minutes, repeating
    // FREQ/WIDTH to +/-10 cents. Drawing a final residual and then adding
    // untrimmed capacitor and temperature errors counts those errors twice.
    // Reuse the cascade's harmonic balance to set one fixed FREQ adjustment;
    // it never follows a played note, resonance edit or the running drift.
    // The software's accelerated warm-up is settled by the service procedure's
    // ten-minute reference. Re-trim at that settled temperature, rather than
    // retaining the old 900-second model's partly warmed calibration point.
    const auto& parameters = activeParameters_;
    const double serviceWarmupFraction = 1.0
        - std::exp(-600.0 / thermalWarmupTimeConstantSeconds);
    static const LimitCycle nominalCycle = [] {
        std::array<double, 4> gains { 1.0, 1.0, 1.0, 1.0 };
        return limitCycleFor(2.4, otaHeadroomVolts,
            VoicedResonanceCompatibilityProfile::loopHeadroomVolts, gains);
    }();
    for (int index = 0; index < maxVoices; ++index)
    {
        auto& card = cards_[static_cast<std::size_t>(index)];
        card.vcfServiceTrimCounts = 0.0f;
        card.vcfServiceCvScale = 1.0f;
        card.vcfServiceCvOffset = 0.0f;
        card.vcfServiceResonanceScale = 1.0f;
        if (parameters.calibration == 0.0f)
            continue;
        // WIDTH sees the converter's physical voltage, including carry
        // error. C6's 8558-count sum is quantised to 8556 at the DAC.
        const float low = vcfFreqTrimAnchorCounts
            + vcfConverterCarryCounts(vcfFreqTrimAnchorCounts)
                  * parameters.calibration;
        constexpr float highCode = 8556.0f;
        const float high = highCode
            + vcfConverterCarryCounts(highCode) * parameters.calibration;
        const float targetSpan = highCode - vcfFreqTrimAnchorCounts
            + (vcfWidthTrimSpanCounts - (highCode - vcfFreqTrimAnchorCounts))
                  * parameters.calibration;
        card.vcfServiceCvScale = targetSpan / (high - low);
        card.vcfServiceCvOffset = vcfFreqTrimAnchorCounts
            - low * card.vcfServiceCvScale;
        std::array<double, 4> poles {};
        for (std::size_t stage = 0; stage < poles.size(); ++stage)
            poles[stage] = voices_[static_cast<std::size_t>(index)]
                               .filter.gScale[stage];
        const auto serviceFraction = static_cast<float>(serviceWarmupFraction);
        const double gradient = parameters.enableSpatialThermalGradient
            ? chassisGradientCelsius(index) * parameters.calibration : 0.0;
        // Both rises ride the warm-up clock (voiceCardCelsius), read here at
        // the service reference, where the fraction is one to double
        // precision.
        const double temperatureRise =
            (gradient + 15.0 * parameters.calibration) * serviceWarmupFraction;
        const double headroom = otaHeadroomVolts
            * (1.0 + temperatureRise / 298.15);
        // The return's headroom at the same temperature, by the law the
        // kernels run (resonanceHeadroomFor), or its 25 C value when the
        // comparison switch holds the kernels there.
        const double returnHeadroom =
            parameters.enableResonanceHeadroomTemperature
                ? resonanceHeadroomFor(headroom)
                : static_cast<double>(
                      VoicedResonanceCompatibilityProfile::loopHeadroomVolts);
        // The service procedure fixes 4.8 Vpp before adjusting frequency.
        // One harmonic-balance evaluation at that amplitude is sufficient;
        // there is no root search in the automatable Character setter.
        auto gains = poles;
        const auto cycle = limitCycleFor(
            2.4, headroom, returnHeadroom, gains, poles);
        // The RES adjustment first: the loop gain that sustains the
        // procedure's 2.4 V peak on this card, as a ratio to the nominal
        // card's own solve so the endpoint constant, which was set against
        // the rendered nominal limit cycle, is what a nominal card keeps.
        if (parameters.enableResonanceServiceTrim)
            card.vcfServiceResonanceScale = static_cast<float>(
                cycle.loopGain / nominalCycle.loopGain);
        // Then FREQ. The RES adjustment moves the loop gain the render reads
        // the frequency-trim table at, so the ratio of that reading to the
        // nominal one is carried; the gradient's cutoff factor is taken at
        // the service temperature, not the running one. The voiced post-trim
        // residual (resonanceError) stays out of this solve as before: its
        // effect on the amplitude and on the table reading partly cancel,
        // and accounting for both needs the amplitude it realises, an
        // inverse solve the automatable Character setter does not run.
        const double correctionRatio = static_cast<double>(
            VoicedResonanceCompatibilityProfile::frequencyTrim(
                VoicedResonanceCompatibilityProfile::maximumFeedback
                * card.vcfServiceResonanceScale))
            / static_cast<double>(
                VoicedResonanceCompatibilityProfile::frequencyTrim(
                    VoicedResonanceCompatibilityProfile::maximumFeedback));
        const double settledThermalScale = thermalFilterOmegaScaleFor(
            parameters, index, serviceFraction);
        card.vcfServiceTrimCounts = static_cast<float>(
            -vcfCountsPerOctave * std::log2(
                cycle.droop * settledThermalScale * correctionRatio
                / nominalCycle.droop));
    }
}

void YouKnowEngine::prepare(double sampleRate, int maxBlockSize,
                               bool oversamplingEnabled)
{
    prepare(sampleRate, maxBlockSize,
            oversamplingEnabled ? maximumOversampleFactor
                                : minimumOversampleFactor);
}

void YouKnowEngine::prepare(double sampleRate, int /*maxBlockSize*/,
                               int requestedFactor)
{
    // The frequency correction's limit-cycle table is solved on first use.
    // Touch it here, where blocking is allowed, so the first audio callback
    // never pays for it.
    (void) VoicedResonanceCompatibilityProfile::frequencyTrim(0.0f);
    (void) VoiceVcaControlLaw::exactGainTable();
    (void) VoiceVcaSignalLaw::serviceGain();

    // A host that has not negotiated a rate yet, or one reporting a nonsense
    // one, must not be able to put a zero, a negative or a NaN on the internal
    // grid: every coefficient below divides by it. Zero, negative, NaN and
    // infinite reports all mean "no rate yet", so they share one 48 kHz
    // default; an earlier revision clamped zero to the 8 kHz floor and built
    // a 32 kHz internal grid (VCF ceiling 3.6 kHz) that the panel readout
    // published until the real prepare arrived. A positive finite rate
    // outside the supported span is clamped into it.
    sampleRate_ = std::isfinite(sampleRate) && sampleRate > 0.0
                    ? std::clamp(sampleRate, minimumSupportedSampleRate,
                                 maximumSupportedSampleRate)
                    : 48000.0;
    inverseSampleRate_ = static_cast<float>(1.0 / sampleRate_);
    oversamplingRequested_ = sanitiseOversampleFactor(requestedFactor);
    oversamplingApplied_ = oversamplingRequested_;
    chorus_.prepareSupportRates(sampleRate_);
    activeConverterTimingProfile_ = converterTimingProfile_;
    updateProcessingRate();
    prepared_ = true;
    reset();
}

void YouKnowEngine::updateProcessingRate(bool preserveFreeRunningState) noexcept
{
    // Prepare the control circuit's table before entering the audio callback.
    (void) voiceVcaControlCircuit();
    const double previousProcessingRate = oversampledRate_;
    oversampling_ = effectiveOversampleFactor(oversamplingApplied_);

    oversampledRate_ = sampleRate_ * oversampling_;
    inverseOversampledRate_ = static_cast<float>(1.0 / oversampledRate_);
    noiseRateScale_ = static_cast<float>(
        std::sqrt(oversampledRate_ / noiseReferenceRateHz));
    const auto slewFor = [this](float seconds) {
        return 1.0f - std::exp(-inverseOversampledRate_ / seconds);
    };
    processingCoefficients_.vcfSlew = slewFor(vcfHoldSlewSeconds);
    processingCoefficients_.internalIntervalSeconds = 1.0 / oversampledRate_;
    processingCoefficients_.voiceVcaDecay = std::exp(
        -processingCoefficients_.internalIntervalSeconds
        / static_cast<double>(voiceVcaHoldSlewSeconds));
    processingCoefficients_.commonVcaTime =
        static_cast<double>(commonVcaHoldTimeConstantSeconds());
    processingCoefficients_.commonVcaDecay = std::exp(
        -processingCoefficients_.internalIntervalSeconds
        / processingCoefficients_.commonVcaTime);
    processingCoefficients_.subDecay = std::exp(
        -processingCoefficients_.internalIntervalSeconds
        / static_cast<double>(subHoldSlewSeconds));
    processingCoefficients_.pwmFullInterval = pwmHoldCoefficients(
        processingCoefficients_.internalIntervalSeconds);
    voiceEnergyFollower_ = slewFor(voiceEnergyFollowerSeconds);
    processingCoefficients_.outputGlide =
        1.0f - std::exp(-inverseSampleRate_ / panelGlideSeconds);
    processingCoefficients_.scanPhasePerInternalSample =
        controlScanHz / oversampledRate_;
    processingCoefficients_.outputBoundaryGain = outputBoundaryGain();
    processingCoefficients_.outputSlewMaxStep =
        static_cast<float>(outputSummerSlewRateVoltsPerSecond
                           * voltsToSample / oversampledRate_);
    processingCoefficients_.outputSummerBandwidthBlend = 1.0f - std::exp(
        -2.0f * std::numbers::pi_v<float> * outputSummerBandwidthHz()
        / oversampledRate_);
    // bipolarFromState() is uniform [-1,1] with RMS 1/sqrt(3). Integrating a
    // one-sided V/sqrt(Hz) density to the host Nyquist frequency therefore
    // needs sqrt(3*Fs/2). Noise is generated after decimation: doing it in
    // every quality domain would change its physical bandwidth and waste CPU.
    processingCoefficients_.outputSummerNoiseScale =
        outputSummerResistorNoiseDensity()
        * std::sqrt(1.5f * static_cast<float>(sampleRate_))
        * voltsToSample;
    // IC5's datasheet floor is a band RMS, folded to a white-equivalent
    // density over NEC's 10 Hz-20 kHz filter. It joins the bus ahead of the
    // chorus split and the decimators, so it is generated at the internal
    // rate with sqrt(3*Fint/2) rather than the host rate above: after
    // decimation its RMS over NEC's band is again the datasheet's 19.95 uV
    // (about 8 % more over a full 0-24 kHz host band).
    const float commonVcaNoiseDensity =
        std::pow(10.0f, commonVcaOutputNoiseDbv / 20.0f)
        / std::sqrt(commonVcaOutputNoiseBandwidthHz);
    processingCoefficients_.commonVcaNoiseScale = commonVcaNoiseDensity
        * std::sqrt(1.5f * static_cast<float>(oversampledRate_))
        * voltsToSample;

    // C14's load and the following HPF are selected by one panel switch.  Their
    // coefficients move only with that mode or this internal rate.
    updateSharedHighPass(activeParameters_);
    if (highPassSwitchResistance_ > 0)
        highPassSwitch_.prepare(oversampledRate_, highPassSwitchResistance_);
    // The two cut legs' undriven corners: each leg's own passband corner
    // scaled by highPassDepartRatio. 225.8 Hz -> 10.14 Hz leaving Two,
    // 720.5 Hz -> 32.34 Hz leaving Three. Both sit far below every supported
    // internal Nyquist, so they need no design-corner clamp.
    highPassTwoDepartG_ = std::tan(
        pi * highPassCornerHz(HighPassMode::Two) * highPassDepartRatio
        * inverseOversampledRate_);
    highPassThreeDepartG_ = std::tan(
        pi * highPassCornerHz(HighPassMode::Three) * highPassDepartRatio
        * inverseOversampledRate_);
    updateBoostBranchCoefficients();
    const float moduleCouplingCorner = rcCornerHz(
        moduleCouplingCapacitanceF,
        static_cast<float>(moduleInputCouplingResistanceOhms()));
    moduleCouplingG_ = std::tan(
        pi * moduleCouplingCorner * inverseOversampledRate_);
    vcaInputCouplingG_ = std::tan(
        pi * vcaInputCouplingCornerHz() * inverseOversampledRate_);
    commonVcaInputCouplingG_ = std::tan(
        pi * commonVcaInputCouplingCornerHz() * inverseOversampledRate_);
    noiseSourceHighPassG_ = std::tan(
        pi * noiseSourceHighPassHz() * inverseOversampledRate_);
    // C41/R79's physical 4.82 kHz corner lies above Nyquist on the supported
    // 8--9.6 kHz HQ-off endpoint grids. Feeding that frequency straight to
    // tan() makes the TPT coefficient negative and its state recursion
    // unstable. Keep the component-derived helper unchanged, but limit this
    // numerical design corner on the actual internal grid, as the other TPT
    // support filters do. Standard rates and 8 kHz HQ (32 kHz internal) remain
    // on the exact component corner.
    const float noiseSourceLowPassDesignHz = std::min(
        noiseSourceLowPassHz(), static_cast<float>(oversampledRate_) * 0.45f);
    noiseSourceLowPassG_ = std::tan(
        pi * noiseSourceLowPassDesignHz * inverseOversampledRate_);
    outputCouplingG_ = std::tan(
        pi * outputCouplingCornerHz() * inverseSampleRate_);
    const double deepest = totalLatencySamples(maximumOversampleFactor);
    const double running = totalLatencySamples(oversampling_);
    latencyPadSamples_ = std::clamp(
        static_cast<int>(std::floor(deepest - running + 0.5)),
        0, latencyPadRingSize - 1);
    latencyPadLeft_.fill(0.0f);
    latencyPadRight_.fill(0.0f);
    latencyPadWriteIndex_ = 0;
    oversamplingQuietSamples_ =
        std::max(1, static_cast<int>(sampleRate_ * outputPathQuietSeconds));
    rateTransitionStep_ = 1.0f / std::max(
        1.0f, static_cast<float>(sampleRate_ * rateTransitionSeconds));
    // A live quality change is a numerical implementation detail, not a power
    // cycle of the assigner or modulator. Their phases are stored in passes or
    // normalized cycles, so they survive unchanged. Only a first prepare/reset
    // starts a new scan. DCO periodSamples is only a coordinate for a physical
    // period already in flight: scale that retained period with the grid rather
    // than reconstructing it from a newly moved RANGE switch before the next
    // PIT edge. Preserve the remaining drift interval in seconds too.
    if (preserveFreeRunningState && previousProcessingRate > 0.0)
    {
        const double rateRatio = oversampledRate_ / previousProcessingRate;
        for (auto& voice : voices_)
            voice.dco.periodSamples *= rateRatio;
    }
    if (preserveFreeRunningState && driftControlCountdown_ > 0
        && previousProcessingRate > 0.0)
    {
        driftControlCountdown_ = std::max(
            1, static_cast<int>(std::llround(
                   driftControlCountdown_ * oversampledRate_
                   / previousProcessingRate)));
    }
    else if (!preserveFreeRunningState)
    {
        driftControlCountdown_ = 0;
    }
    // The cutoff chain's memo is keyed on counts and loop gain, both of which
    // survive a quality change untouched, while the coefficient it produces
    // is measured in internal samples and does not. Retire it here so a card
    // whose holds have settled exactly cannot be handed back the old grid's
    // answer.
    for (auto& voice : voices_)
    {
        voice.cutoffChainCounts = -1.0e30f;
        voice.cutoffChainFeedback = -1.0e30f;
    }
    chorus_.prepare(oversampledRate_, preserveFreeRunningState);
}

int YouKnowEngine::effectiveOversampleFactor(int requestedFactor) const noexcept
{
    // The deepest rung worth running at this host rate: the bandlimiting target
    // is minimumHqProcessingRate, and going past it buys nothing but CPU. The
    // player's request is then capped by that, so selecting 4x on a 192 kHz
    // host quietly runs at 1x rather than at 768 kHz.
    int ceilingFactor;
    if (sampleRate_ >= minimumHqProcessingRate)
        ceilingFactor = 1;
    else if (sampleRate_ >= minimumHqProcessingRate / 2.0)
        ceilingFactor = 2;
    else
        ceilingFactor = maximumOversampleFactor;

    int result = std::min(sanitiseOversampleFactor(requestedFactor), ceilingFactor);
    if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
        while (sampleRate_ * result < 32000.0 && result < maximumOversampleFactor)
            result *= 2;
    return result;
}

bool YouKnowEngine::setOversamplingEnabled(bool enabled) noexcept
{
    return setOversamplingFactor(enabled ? maximumOversampleFactor
                                         : minimumOversampleFactor);
}

bool YouKnowEngine::setOversamplingFactor(int factor) noexcept
{
    oversamplingRequested_ = sanitiseOversampleFactor(factor);
    return applyPendingOversamplingIfIdle();
}

bool YouKnowEngine::applyPendingOversamplingIfIdle() noexcept
{
    // Nothing here is about the rung that was asked for; it is about the grid
    // the engine would actually run. Two different rungs resolve to the same
    // internal rate whenever the host is already fast enough -- 4x and 2x both
    // run at 2x on a 96 kHz host -- and rebuilding the whole output path to
    // arrive back at the rate it is already on would spend a safety fade on a
    // change nobody can hear. Adopt the request and leave the grid alone.
    if (effectiveOversampleFactor(oversamplingRequested_) == oversampling_)
    {
        oversamplingApplied_ = oversamplingRequested_;
        // A request can also be withdrawn while the old path is fading. Bring
        // it back without touching any rate-dependent state.
        if (rateTransition_ == RateTransition::FadingOut)
            rateTransition_ = RateTransition::FadingIn;
        return true;
    }
    // The last voice retiring is not the same as the instrument being quiet.
    // The delay lines still hold up to their longest setting and the
    // decimators have their own group delay, so changing the rate waits until
    // what is left in them has gone.
    if (anyVoiceActive_)
    {
        if (rateTransition_ == RateTransition::FadingOut)
            rateTransition_ = RateTransition::FadingIn;
        return false;
    }
    if (oversamplingIdleSamples_ < oversamplingQuietSamples_)
        return false;

    // Once a safety fade begins, finish it even if Chorus or its optional
    // noise control changes meanwhile. Re-evaluating the source here could
    // turn the gain back to one halfway down and expose the very state reset
    // the fade protects. Applying this to every idle quality change also
    // covers a wet-mute or filter tail whose current control already reads
    // silent, at a fixed and negligible five-millisecond cost.
    if (rateTransition_ != RateTransition::FadingOut)
    {
        rateTransition_ = RateTransition::FadingOut;
        return false;
    }
    if (rateTransitionGain_ > 0.0f)
        return false;

    // A physical pitch transaction is anchored to the current internal grid:
    // its PIT preparation starts at T-389 CPU states and its paired converter
    // hold is committed at T. Let that sub-sample transaction retire before a
    // quality rebuild moves T to a different grid. The physical transaction
    // itself spans at most 97.25 us; because this guard is polled at process
    // boundaries, the already-muted rebuild can occur at the next host block.
    for (int slot = 0; slot < hardwareVoices; ++slot)
    {
        const auto& voice = voices_[static_cast<std::size_t>(slot)];
        if (voice.dcoPitchTransactionValid
            || voice.dco.pitWriteState != Dco::PitWriteState::idle)
            return false;
    }

    oversamplingApplied_ = oversamplingRequested_;
    updateProcessingRate(true);
    rebuildRateDependentVoiceState();
    clearRateDependentOutputPath(true);
    rateTransitionGain_ = 0.0f;
    rateTransition_ = RateTransition::FadingIn;
    return true;
}

// Sample-grid histories downstream of the voices: delay lines, decimation
// stages and latency pad. A live HQ change preserves the physical C14, HPF and
// C12 states while recomputing their coefficients for the new rate; a hard
// reset clears them. The final C17/C20 capacitors run at the host rate and
// always survive a live HQ rebuild, or asymmetric PWM would make a large step.
void YouKnowEngine::clearRateDependentOutputPath(
    bool preserveFreeRunningState) noexcept
{
    firstDecimator_.reset();
    secondDecimator_.reset();
    // updateProcessingRate() has already replaced the chorus transitions,
    // retained its BBD buckets/free-running phases, and reinitialised the
    // sample-grid support histories and continuous-support coordinates under
    // this zero-gain boundary. A hard reset also clears the physical/free-
    // running effect state.
    if (!preserveFreeRunningState)
        chorus_.reset(false);
    if (!preserveFreeRunningState)
    {
        voiceBusCoupling_.reset();
        highPassSwitch_.reset();
        highPass_.reset();
        highPassTwoLeg_.reset();
        highPassThreeLeg_.reset();
        boostBranch_.reset();
        commonVcaInputCoupling_.reset();
        noiseSourceHighPass_.reset();
        noiseSourceLowPass_.reset();
    }
    latencyPadLeft_.fill(0.0f);
    latencyPadRight_.fill(0.0f);
    latencyPadWriteIndex_ = 0;
}

void YouKnowEngine::clearOutputPath() noexcept
{
    clearRateDependentOutputPath(false);
    outputCouplingLeft_.reset();
    outputCouplingRight_.reset();
    // IC6's own output node. It was the one mutable state in the output path
    // this did not clear, so `reset()` did not put the engine in one state:
    // what it rendered next depended on what it had been rendering before. The
    // leak is tiny -- the slew limit is 1.0 V/us, so the integrator collapses
    // within an internal sample or two of a stop -- but a reset that does not
    // reset is not something a deterministic re-render can rely on.
    outputSlewStateLeft_ = 0.0f;
    outputSlewStateRight_ = 0.0f;
    outputBandwidthStateLeft_ = 0.0f;
    outputBandwidthStateRight_ = 0.0f;
    outputJackLeft_.reset();
    outputJackRight_.reset();
    outputNoiseStateLeft_ = 0x91e10da5u;
    outputNoiseStateRight_ = 0xd1b54a35u;
    outputWiperNoiseStateLeft_ = 0x94d049bbu;
    outputWiperNoiseStateRight_ = 0x8538ecadu;
    commonVcaNoiseState_ = 0x7f4a7c15u;
}

void YouKnowEngine::rebuildRateDependentVoiceState() noexcept
{
    for (auto& voice : voices_)
    {
        const float previousFilterOmegaStep = voice.filterOmegaStep;
        const float previousEffectiveFilterOmegaStep =
            boundedThermalFilterOmegaStep(
                previousFilterOmegaStep, activeParameters_, voice.cardIndex,
                thermalWarmupFraction_);
        // Residual kernels are measured in internal samples. The safety fade
        // has reached zero, so discard their old-rate tails and prime the new
        // timeline at the continuing capacitor voltage. PIT countdown is held
        // in selected-clock periods and the ramp slope in volts per second, so
        // neither physical state is retimed by this sample-grid rebuild.
        const float saw = static_cast<float>(
            (voice.dco.rampValue + 1.0) * static_cast<double>(voice.dco.renderScale)
            * voice.rampCurrentScale - 1.0);
        voice.dco.saw.reset();
        voice.dco.resetSawCorrection.fill(0.0);
        voice.dco.pulse.reset();
        voice.dco.sub.reset();
        voice.dco.saw.prime(saw);
        voice.dco.pulse.prime(voice.dco.pulseState);
        voice.dco.sub.prime(voice.dco.subState);

        if (voice.active || voice.cardIndex < hardwareVoices)
        {
            updateVoiceAudio(voice, activeParameters_);
            const float nextEffectiveFilterOmegaStep =
                boundedThermalFilterOmegaStep(
                    voice.filterOmegaStep, activeParameters_, voice.cardIndex,
                    thermalWarmupFraction_);
            voice.filter.retime(previousEffectiveFilterOmegaStep,
                                nextEffectiveFilterOmegaStep);
        }
    }
}

double YouKnowEngine::totalLatencySamples(int factor) noexcept
{
    const int limited = std::max(1, factor);
    // The oscillator's residual tracks delay by their own half width, and that
    // delay is in internal samples -- so it shrinks, in output samples, as the
    // factor grows, while the decimators' delay grows. Both have to be counted,
    // or the two configurations do not line up.
    double latency = static_cast<double>(correctionHalfWidth)
                   / static_cast<double>(limited);
    // downsamplePair() writes the pair and reads its kernel back from the
    // second sample, so the 47-tap half width is measured from one input
    // sample after the pair's first, and each stage delays the first sample
    // of its pair by 46 of its inputs, not 47. 4x therefore sits at 34.5 host
    // samples before the residual delay (40.5 with it), which is what
    // testDecimatorGroupDelayMatchesTheLatencyFormula measures; the report
    // still rounds to 41 and the 2x/1x pads stay 6 and 17.
    constexpr double half = (halfbandTaps - 1) / 2.0 - 1.0;
    for (int step = limited; step > 1; step /= 2)
        latency += half / static_cast<double>(step);
    return latency;
}

int YouKnowEngine::getProcessingLatencySamples() const noexcept
{
    // Always the deepest configuration's figure, whatever is running. The
    // quality setting can change while the host is playing, and a plug-in that
    // renegotiated its latency mid-transport would make the host re-align
    // everything around it; padding the shallower settings by at most 17 host
    // samples keeps the number the host was told true.
    return static_cast<int>(
        std::floor(totalLatencySamples(maximumOversampleFactor) + 0.5));
}

void YouKnowEngine::applyLatencyPad(float& left, float& right) noexcept
{
    if (latencyPadSamples_ <= 0)
        return;

    latencyPadLeft_[static_cast<std::size_t>(latencyPadWriteIndex_)] = left;
    latencyPadRight_[static_cast<std::size_t>(latencyPadWriteIndex_)] = right;
    const int readIndex =
        (latencyPadWriteIndex_ - latencyPadSamples_ + latencyPadRingSize)
        % latencyPadRingSize;
    left = latencyPadLeft_[static_cast<std::size_t>(readIndex)];
    right = latencyPadRight_[static_cast<std::size_t>(readIndex)];
    latencyPadWriteIndex_ = (latencyPadWriteIndex_ + 1) % latencyPadRingSize;
}

void YouKnowEngine::reset()
{
    for (auto& hold : envelopeHolds_)
        hold.reset(VoiceVcaSignalLaw::holdStandoffVolts);
    voiceBoardCommandReplayActive_ = voiceBoardCommandReplayRequested_;
    for (auto& voice : voices_)
    {
        voice = Voice {};
        voice.dco.reset();
        voice.filter.reset();
        voice.moduleCoupling.reset();
        voice.coupledMixer.reset();
        voice.vcaInputCoupling.reset();
        voice.vcaInputVolts = 0.0f;
        voice.envelope.reset();
    }
    for (int index = 0; index < maxVoices; ++index)
    {
        auto& voice = voices_[static_cast<std::size_t>(index)];
        voice.cardIndex = index;
        // Microscopic filter excitation belongs to the continuously powered
        // card, so seed it once per slot rather than once per MIDI assignment.
        voice.noiseState = hash32(static_cast<std::uint32_t>(index)
                                  * 2246822519u + 1u) | 1u;
    }
    refreshVoiceRampCurrentScales();
    // `voice = Voice {}` above zeroed the offsets, and the cards outlive a
    // reset, so put them back before anything can render a symmetric filter.
    refreshVoiceCardStageTrims();
    refreshVoiceCardServiceTrims();
    refreshAgedUnitState();

    clearOutputPath();
    clearHeldNotes();
    rateTransition_ = RateTransition::Idle;
    rateTransitionGain_ = 1.0f;

    thermalWarmupSeconds_ = 0.0;
    thermalWarmupFraction_ = thermalStartsSettled_ ? 1.0f : 0.0f;
    refreshVoiceCardThermalScales();
    refreshJackBoardTemperature(activeParameters_);
    refreshDcoMasterClock();
    powerSupplyDroop_ = 0.0f;
    railRipplePhase_ = 0.0;
    railRippleVolts_ = 0.0f;
    lfoAccumulator_ = 0u;
    lfoRising_ = true;
    lfoPolarity_ = 1.0f;
    lfoValue_ = 0.0f;
    lfoDelayLevel_ = 0.0f;
    lfoDelayByte_ = 0u;
    updateSharedScan(activeParameters_);
    resonanceCv_ = resonanceCvTarget_;
    sharedVca_ = sharedVcaTarget_;
    pwmVoltsFirstPole_ = pwmVoltsTarget_;
    pwmVolts_ = pwmVoltsTarget_;
    subCv_ = subCvTarget_;
    noiseCv_ = noiseCvTarget_;
    primeStartupVoiceWaveNodes(activeParameters_);
    // The hold has to go with the level: a note arriving at the very first
    // sample of a new run gives the scan no idle pass in which to clear it, so
    // a hold left over from the previous run would be skipped.
    lfoDelayHoldoff_ = 0u;
    lfoDelayFade_ = 0u;
    // The two oscillators have no recoverable startup relation. One full
    // selected period is the deterministic arbitrary IC35 phase; unlike the
    // old card-local policy it is one phase shared by all six PIT inputs.
    rangeClockClocksToFallingEdge_ = 1.0;
    rangeClockClocksToReload_ = 0.0;
    rangeClockTransitionPending_ = false;
    controlScanPhase_ = 1.0;
    activeConverterTimingProfile_ = converterTimingProfile_;
    converterEventPhases_ = converterEventPhases(converterTimingProfile_);
    nextConverterWrite_ = 0;
    converterPassEnvelopeUpdated_.fill(false);
    converterPassPortamentoUpdated_ = false;
    converterNextPassPortamentoUpdated_ = false;
    passiveHoldEventLatch_ = {};
    exactVcfControlInterval_.fill(false);
    assignmentRescanPending_ = false;
    assignmentRescanPassArmed_ = false;
    rescanPreviousUnisonMembers_.fill(false);
    rescanUnisonMidiValid_ = false;
    rescanUnisonMidi_ = 60.0f;
    noiseState_ = 0x6d2b79f5u;
    noiseGaussianSpare_ = 0.0f;
    noiseGaussianSpareValid_ = false;
    // Both the live value and the target, or a run that stopped with the bender
    // pushed over would start the next one there: hosts are not obliged to
    // resend a neutral controller when the transport restarts, and nothing
    // would bring it back until the player touched the wheel.
    pitchBendTarget_ = 0.0f;
    modWheelTarget_ = 0.0f;
    dcoPitchBendWord_ = 0;
    vcfBendCountsWord_ = 0;
    dcoLfoPitchWord_ = 0;
    vcfLfoCountsWord_ = 0;
    sustainPedalDown_ = false;
    generation_ = 0;
    activeVoiceCount_ = 0;
    anyVoiceActive_ = false;
    displayEnvelope_ = 0.0f;
    displayLfo_ = 0.0f;
    displayVoiceMask_ = 0;
    driftControlCountdown_ = 0;
    panelGlidePrimed_ = false;
    // A reset leaves nothing in the output path, so a quality change asked for
    // before the first block does not have to wait for one that never comes.
    oversamplingIdleSamples_ = oversamplingQuietSamples_;
    converterPassEndPhase_ = 1.0;
    for (std::size_t i = 0; i < converterWritesPerPass; ++i)
        converterInhibitPhases_[i] = std::max(0.0, converterEventPhases_[i]
            - 75.0 * controlScanHz / voiceCpuStateHz);
    refreshFirmwareControlTrace(true);
    controlScanPhase_ = activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt
        ? 0.0 : converterPassEndPhase_;
}

void YouKnowEngine::resetForHostStop()
{
    // The chassis does not return to ambient because the transport stopped.
    // The warm-up timer and the fraction derived from it are the whole of the
    // free-running physical state `reset()` clears -- the voice-card trims
    // outlive it already, and the rail droop is an instantaneous load measure
    // of voices this call is about to silence, so zero is its correct value
    // once they are gone rather than a cold supply beside a warm chassis.
    //
    // This is a decision about what a host reset means, not a measurement. It
    // moves a boundary the engine draws elsewhere: `reset()` is written as a
    // power cycle -- the comment on the quality switch says a live rate change
    // is not one "because a prepare/reset is" -- and on that reading a host
    // that resets on every transport stop is simply asking for a power cycle
    // each time. The reading taken here is that a transport stop is not one:
    // the modelled instrument is not switched off when the player stops the
    // song. Even the accelerated software warm-up must survive transport stops.
    // `prepare()` remains the cold path, and it is the one a rate change,
    // a device change and a fresh instance all go through.
    const double warmupSeconds = thermalWarmupSeconds_;
    const float warmupFraction = thermalWarmupFraction_;
    reset();
    thermalWarmupSeconds_ = warmupSeconds;
    thermalWarmupFraction_ = warmupFraction;
    refreshVoiceCardThermalScales();
    refreshJackBoardTemperature(activeParameters_);
    refreshDcoMasterClock();
    // reset() primes the cleared voice nodes; prime again at the retained
    // temperature so a host stop cannot leave cold-clock capacitor means.
    primeStartupVoiceWaveNodes(activeParameters_);
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

EngineParameters YouKnowEngine::sanitise(const EngineParameters& parameters) noexcept
{
    EngineParameters result = parameters;

    const auto fix01 = [](float& value, float fallback) {
        value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
    };

    fix01(result.lfoRate, 0.42f);
    fix01(result.lfoDelay, 0.0f);
    fix01(result.dcoLfoDepth, 0.0f);
    fix01(result.pwmDepth, 0.30f);
    fix01(result.subLevel, 0.0f);
    fix01(result.noiseLevel, 0.0f);
    fix01(result.cutoff, 0.62f);
    fix01(result.resonance, 0.10f);
    fix01(result.envDepth, 0.35f);
    fix01(result.vcfLfoDepth, 0.0f);
    fix01(result.keyFollow, 0.50f);
    fix01(result.vcaLevel, 0.80f);
    fix01(result.attack, 0.0f);
    fix01(result.decay, 0.45f);
    fix01(result.sustain, 0.70f);
    fix01(result.release, 0.30f);
    fix01(result.portamento, 0.0f);
    fix01(result.benderDcoDepth, 0.30f);
    fix01(result.benderVcfDepth, 0.0f);
    result.mainNoiseLevelScale = std::isfinite(result.mainNoiseLevelScale)
        ? std::clamp(result.mainNoiseLevelScale, 0.0f, 4.0f)
        : 1.0f;
    fix01(result.benderLfoDepth, 0.0f);
    fix01(result.volume, 0.80f);
    fix01(result.velocityDepth, 0.0f);
    fix01(result.aging, 0.0f);
    // Unlike every other 0-1 control, Unit Character extends to 2: 0 is the
    // digital reference, 1 matches real hardware, and the headroom to 2
    // extrapolates every blended mechanism past its physical draw. The old
    // 0-100 range this comment once described is gone -- the comparison
    // tools stopped using it, and past 1 the affine blends walk through
    // their nominals -- so the ceiling lives in `calibrationCeiling`.
    // Clamping to fix01's 0-1 would still silently discard the top half.
    result.calibration = std::isfinite(result.calibration)
        ? std::clamp(result.calibration, 0.0f, EngineParameters::calibrationCeiling) : 1.0f;
    fix01(result.chorusNoise, Chorus::defaultNoiseScale);

    result.masterTuneCents = std::isfinite(result.masterTuneCents)
                           ? std::clamp(result.masterTuneCents, -50.0f, 50.0f)
                           : 0.0f;
    result.keyTranspose = std::clamp(result.keyTranspose, -12, 12);
    result.polyphony = std::clamp(result.polyphony, 1, maxVoices);
    if (result.vcfTanhMode != VcfTanhMode::Exact
        && result.vcfTanhMode != VcfTanhMode::ZonedHermite
        && result.vcfTanhMode != VcfTanhMode::PolyZoned)
        result.vcfTanhMode = VcfTanhMode::Exact;
    if (result.vcfFastEarlyMode != VcfFastEarlyMode::Hermite
        && result.vcfFastEarlyMode != VcfFastEarlyMode::Cubic)
        result.vcfFastEarlyMode = VcfFastEarlyMode::Hermite;
    if (result.vcfSolverMode != VcfSolverMode::MersonHalfSteps
        && result.vcfSolverMode != VcfSolverMode::Rk4HalfSteps
        && result.vcfSolverMode != VcfSolverMode::Rk4Single)
        result.vcfSolverMode = VcfSolverMode::MersonHalfSteps;
    return result;
}

// The shared high-pass, whose parts are one network rather than six. Recomputed
// where the switch is read rather than per voice, which is also what stops six
// voices each writing the same three values.
void YouKnowEngine::updateSharedHighPass(const EngineParameters& parameters) noexcept
{
    voiceBusCouplingG_ = std::tan(
        pi * voiceBusCouplingCornerHz(parameters.highPass)
        * inverseOversampledRate_);
    const float corner = highPassCornerHz(parameters.highPass);
    highPassG_ =
        std::tan(pi * std::min(corner, static_cast<float>(oversampledRate_) * 0.45f)
                 * inverseOversampledRate_);
    highPassShelf_ = highPassShelfGain(parameters.highPass);
    highPassHigh_ = highPassHighGain(parameters.highPass);
}

float YouKnowEngine::processMainNoiseSource(
    float rawNoise, float level, bool levelBeforeC41) noexcept
{
    // Module board p. 13 is ordered, not merely a pair of commuting fixed
    // filters: Tr21 crosses C42 into the BA662 level OTA, then C41/R79 loads
    // that OTA's output. At a fixed level the old post-C41 scalar has the same
    // transfer, but during a scanned level move it exposes the wrong stored
    // charge. Keep C42 running unconditionally and drive the existing C41
    // state with the controlled OTA output; no extra pole or reset is added.
    const float coupled = noiseSourceHighPass_.process(
        rawNoise, noiseSourceHighPassG_, 0.0f, 1.0f);
    // On coarse HQ-off grids below about 19.3 kHz, C41's 33 us memory is
    // shorter than one sample while the established bilinear support pole is
    // negative. Exposing that state would turn a physical monotonic discharge
    // into an invented alternating tail. Preserve the qualified fixed-level
    // filter there and treat the sub-sample control memory as instantaneous.
    const bool resolvedC41Memory =
        levelBeforeC41 && noiseSourceLowPassG_ <= 1.0f;
    const float otaOutput = coupled * (resolvedC41Memory ? level : 1.0f);
    const float shaped = noiseSourceLowPass_.process(
        otaOutput, noiseSourceLowPassG_, 1.0f, 0.0f);
    const float rail = shaped * noiseMixVolts;
    return resolvedC41Memory ? rail : rail * level;
}

bool YouKnowEngine::configureCoupledMixer(
    const CoupledSubMixer::Calibration& calibration) noexcept
{
    if (prepared_ || moduleInputCouplingResistanceOverrideOhms_ > 0.0
        || oscillatorLevelScale_ != 1.0f || !calibration.valid())
        return false;
    coupledMixerCalibration_ = calibration;
    coupledMixerEnabled_ = true;
    return true;
}

bool YouKnowEngine::configureModuleInputCouplingResistanceOhms(
    double totalResistanceOhms) noexcept
{
    if (prepared_ || coupledMixerEnabled_ || !std::isfinite(totalResistanceOhms)
        || totalResistanceOhms < 1000.0 || totalResistanceOhms > 1000000.0)
        return false;
    moduleInputCouplingResistanceOverrideOhms_ = totalResistanceOhms;
    return true;
}

double YouKnowEngine::moduleInputCouplingResistanceOhms() const noexcept
{
    return moduleInputCouplingResistanceOverrideOhms_ > 0.0
        ? moduleInputCouplingResistanceOverrideOhms_
        : static_cast<double>(moduleCouplingResistanceOhms);
}

bool YouKnowEngine::configureOscillatorLevelScale(float scale) noexcept
{
    if (prepared_ || coupledMixerEnabled_ || !std::isfinite(scale)
        || scale < 0.25f || scale > 2.0f)
        return false;
    oscillatorLevelScale_ = scale;
    return true;
}

bool YouKnowEngine::configureEnvelopeHolds(
    const std::array<EnvelopeHoldCircuit::Configuration, 6>& configuration) noexcept
{
    if (prepared_ || !std::all_of(configuration.begin(), configuration.end(),
                                  [](const auto& value) { return value.valid(); }))
        return false;
    envelopeHoldConfiguration_ = configuration;
    envelopeHoldsConfigured_ = true;
    return true;
}

bool YouKnowEngine::configureDcoMasterClockHz(double frequencyHz) noexcept
{
    if (prepared_ || !std::isfinite(frequencyHz)
        || frequencyHz < 0.9 * masterClockHz
        || frequencyHz > 1.1 * masterClockHz)
        return false;
    dcoReferenceClockRatio_ = frequencyHz / masterClockHz;
    dcoClockTemperatureCelsius_ = -1000.0f;
    refreshDcoMasterClock();
    return true;
}

bool YouKnowEngine::configureDcoResetCircuit(
    const DcoResetCircuit::Calibration& calibration) noexcept
{
    if (prepared_ || !calibration.valid())
        return false;
    dcoResetCalibration_ = calibration;
    dcoResetCircuitEnabled_ = true;
    return true;
}

bool YouKnowEngine::configureDcoTemperatureProxy(
    bool enabled, double referenceCelsius) noexcept
{
    if (prepared_ || !Csa8MtzTemperatureProxy::supports(referenceCelsius))
        return false;
    dcoTemperatureProxyEnabled_ = enabled;
    dcoTemperatureReferenceFactor_ =
        Csa8MtzTemperatureProxy::frequencyFactor(referenceCelsius);
    dcoClockTemperatureCelsius_ = -1000.0f;
    refreshDcoMasterClock();
    return true;
}

bool YouKnowEngine::configureThermalStart(bool settled) noexcept
{
    if (prepared_)
        return false;
    thermalStartsSettled_ = settled;
    thermalWarmupSeconds_ = 0.0;
    thermalWarmupFraction_ = settled ? 1.0f : 0.0f;
    refreshVoiceCardThermalScales();
    refreshJackBoardTemperature(activeParameters_);
    refreshDcoMasterClock();
    return true;
}

void YouKnowEngine::refreshDcoMasterClock() noexcept
{
    if (!dcoTemperatureProxyEnabled_)
    {
        dcoMasterClockRatio_ = dcoReferenceClockRatio_;
        return;
    }
    // One common chassis temperature approximates the ONE master resonator's
    // local temperature. Voice-card spatial offsets must never produce six
    // independent DCO clocks. The real IC38 thermal location is unmeasured.
    const float temperature = jackBoardCelsius(activeParameters_);
    if (temperature == dcoClockTemperatureCelsius_)
        return;
    dcoClockTemperatureCelsius_ = temperature;
    dcoMasterClockRatio_ = dcoReferenceClockRatio_
        * (Csa8MtzTemperatureProxy::frequencyFactor(temperature)
           / dcoTemperatureReferenceFactor_);
    // Change frequency only: PIT/IC35 clocks-remaining, C54 charge/current,
    // finite reset seconds and all correction histories stay continuous.
}

bool YouKnowEngine::configureHighPassSwitch(double resistance) noexcept
{
    if (prepared_ || !std::isfinite(resistance) || resistance < 50 || resistance > 1000)
        return false;
    highPassSwitchResistance_ = resistance;
    return true;
}

void YouKnowEngine::setParameters(const EngineParameters& parameters)
{
    // Before the first valid prepared audio interval, even an equal snapshot has
    // the one-shot responsibility of priming the physical holds below.  Once
    // audio time has begun, an equal complete image has no ordered converter
    // write or assignment side effect and can return exactly.
    const bool startupSnapshot = !prepared_ || !panelGlidePrimed_;
    if (!startupSnapshot && parameters == activeParameters_
        && parameters == targetParameters_)
        return;

    const auto next = sanitise(parameters);
    if (!startupSnapshot && next == activeParameters_
        && next == targetParameters_)
        return;

    const bool assignModeChanged = next.keyMode != activeParameters_.keyMode;
    const bool unisonVoiceCountChanged = next.polyphony != activeParameters_.polyphony
                                      && (next.keyMode == KeyMode::Unison
                                          || activeParameters_.keyMode
                                                 == KeyMode::Unison);
    const bool highPassChanged = next.highPass != activeParameters_.highPass;
    const bool rangeChanged = next.range != activeParameters_.range;
    const bool stageTrimsChanged =
        next.calibration != activeParameters_.calibration
        || next.enableVcfStageOffsets
               != activeParameters_.enableVcfStageOffsets
        || next.enableResonanceOtaOffset
               != activeParameters_.enableResonanceOtaOffset;
    const bool thermalScalesChanged = startupSnapshot
        || next.calibration != activeParameters_.calibration
        || next.enableSpatialThermalGradient
               != activeParameters_.enableSpatialThermalGradient;
    const bool rampCurrentScalesChanged = startupSnapshot
        || next.calibration != activeParameters_.calibration || rangeChanged;
    const bool agingChanged = next.aging != activeParameters_.aging;
    if (next.useFixedVcfServiceFrequencyTrim
            != activeParameters_.useFixedVcfServiceFrequencyTrim
        || next.useServiced439522VcfCalibration
            != activeParameters_.useServiced439522VcfCalibration)
        for (auto& voice : voices_)
            voice.cutoffChainCounts = -1.0e30f;
    // Before the first valid prepared audio interval, a host snapshot is the
    // power-up image rather than a timed panel move. `panelGlidePrimed_` is
    // already the exact one-shot marker for that boundary: invalid/zero calls
    // return before setting it, and reset clears it. Do not use output-path
    // silence here. Once audio time has started, the hardware scanner keeps
    // running through ordinary silence and after panic.
    targetParameters_ = next;
    if (rangeChanged && !startupSnapshot)
    {
        // IC35 is one shared 8 MHz synchronous divider. PF7/PF6 present
        // 14/12/8 while its inverted carry feeds both /LD and every PIT CLK:
        // a switch cannot truncate the count already in progress. Retain the
        // old-cycle remainder on every card, then use the new modulus after
        // that common reload edge.
        // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13
        // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=17
        // https://toshiba.semicon-storage.com/info/TC74HC161AF_datasheet_en_20140301.pdf?did=10907&prodName=TC74HC161AF#page=2
        beginRangeClockTransition(activeParameters_.range, next.range);
    }
    else if (startupSnapshot)
    {
        // A restored pre-audio panel image is the power-up selection, not a
        // timed PF write from the constructor's default range. Adopt its
        // modulus without manufacturing an old-range cycle first.
        rangeClockClocksToFallingEdge_ = 1.0;
        rangeClockClocksToReload_ = 0.0;
        rangeClockTransitionPending_ = false;
    }
    // Switch positions land immediately. Main VOLUME is the only continuous
    // panel control applied outside the scanned converter path; it glides in
    // the render loop so host automation cannot make a block-boundary step.
    activeParameters_ = targetParameters_;
    refreshDcoMasterClock();
    useCubicEarly_ =
        activeParameters_.vcfTanhMode != VcfTanhMode::Exact
        && activeParameters_.vcfFastEarlyMode == VcfFastEarlyMode::Cubic;
    // Unit Character scales the stage offsets, so they follow the panel; the
    // aged-unit precompute follows only panel edits and reset, not note-ons.
    if (stageTrimsChanged)
        refreshVoiceCardStageTrims();
    if (thermalScalesChanged)
        refreshVoiceCardThermalScales();
    if (startupSnapshot || stageTrimsChanged || thermalScalesChanged)
        refreshVoiceCardServiceTrims();
    if (rampCurrentScalesChanged)
        refreshVoiceRampCurrentScales();
    if (agingChanged)
        refreshAgedUnitState();
    if (highPassChanged)
        updateSharedHighPass(activeParameters_);

    // A host may deliver its saved snapshot before prepare(), after prepare(),
    // or more than once while restoring state. Until audio time begins, prime
    // every shared hold from the newest complete snapshot instead of letting
    // the first attack hear the constructor's stale patch. Once any valid
    // prepared interval has run, every edit takes the normal ordered 23-write
    // converter path, even if the instrument has been quiet long enough for a
    // quality switch or has just received panic.
    if (startupSnapshot)
    {
        // No audio interval can own a pending physical write before startup.
        // A newer restore snapshot therefore supersedes any speculative latch
        // assembled by a test/host sequence that has not yet processed audio.
        passiveHoldEventLatch_ = {};
        exactVcfControlInterval_.fill(false);
        updateSharedScan(next);
        resonanceCv_ = resonanceCvTarget_;
        sharedVca_ = sharedVcaTarget_;
        pwmVoltsFirstPole_ = pwmVoltsTarget_;
        pwmVolts_ = pwmVoltsTarget_;
        subCv_ = subCvTarget_;
        noiseCv_ = noiseCvTarget_;
        primeStartupVoiceWaveNodes(next);
        refreshFirmwareControlTrace();
    }

    // The original assigner handles either POLY-button transition by gating
    // the six assignments, clearing its tables, and rescanning held keys. A
    // mutable plug-in voice-count has no hardware counterpart, but rebuilding
    // a live Unison stack is the only coherent equivalent when that count
    // changes.
    if (prepared_ && !voiceBoardCommandReplayActive_
        && (assignModeChanged || unisonVoiceCountChanged))
        beginVoiceAssignmentRescan();
}

// Constant rate in pitch: a wider leap takes proportionally longer, rather than
// every glide finishing in the same time. Zero means the control is off, and a
// note steps straight to its pitch.
float YouKnowEngine::glideStepPerScan(float portamento) noexcept
{
    const int stepUnits = portamentoIncrement(portamento);
    if (stepUnits == 0)
        return 0.0f;
    return static_cast<float>(stepUnits) / 256.0f;
}

float YouKnowEngine::resolveGlideStepPerScan(float portamento) noexcept
{
    if (portamento != glideLawPortamento_)
    {
        glideLawPortamento_ = portamento;
        glideLawStepPerScan_ = glideStepPerScan(portamento);
    }
    return glideLawStepPerScan_;
}

int YouKnowEngine::voiceLimit() const noexcept
{
    return std::clamp(activeParameters_.polyphony, 1, maxVoices);
}

// ---------------------------------------------------------------------------
// Note handling
// ---------------------------------------------------------------------------

bool YouKnowEngine::rememberHeldNote(int midiNote, float velocity) noexcept
{
    if (midiNote < 0 || midiNote > 127)
        return false;
    const auto index = static_cast<std::size_t>(midiNote);
    const bool firstPress = heldNoteCounts_[index] == 0;
    if (firstPress)
        heldNoteVelocities_[index] = velocity;
    if (heldNoteCounts_[index] < std::numeric_limits<std::uint16_t>::max())
        ++heldNoteCounts_[index];
    return firstPress;
}

bool YouKnowEngine::forgetHeldNote(int midiNote) noexcept
{
    if (midiNote < 0 || midiNote > 127)
        return false;
    const auto index = static_cast<std::size_t>(midiNote);
    if (heldNoteCounts_[index] == 0)
        return false;
    --heldNoteCounts_[index];
    return heldNoteCounts_[index] == 0;
}

void YouKnowEngine::clearHeldNotes() noexcept
{
    heldNoteVelocities_.fill(0.0f);
    heldNoteCounts_.fill(0);
}

int YouKnowEngine::highestHeldNote() const noexcept
{
    for (int note = 127; note >= 0; --note)
        if (heldNoteCounts_[static_cast<std::size_t>(note)] > 0)
            return note;
    return -1;
}

void YouKnowEngine::releaseVoiceKey(Voice& voice) noexcept
{
    voice.keyDown = false;
    voice.releaseStamp = ++generation_;
    voice.envelope.noteOff(sustainPedalDown_);
    if (sustainPedalDown_)
    {
        voice.sustained = true;
    }
    else
    {
        voice.releasing = true;
    }
}

void YouKnowEngine::dropFromUnison(Voice& voice) noexcept
{
    voice.unisonMember = false;
    if (voice.keyDown)
        releaseVoiceKey(voice);
}

bool YouKnowEngine::anyVoiceRunning() const noexcept
{
    // Use B-2's FF11 snapshot, not the assigner's current key/HOLD state.
    // HOLD-off at 00BF clears only FF1E bit 0 and returns to the interrupted
    // pass. Until 02FD..02FF next runs, a released voice still has FF11 set;
    // another Voice On in that interval must not restart the LFO onset.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L155-L160
    for (const auto& voice : voices_)
        if (voice.envelope.running)
            return true;
    return false;
}

bool YouKnowEngine::anyVoiceSounding() const noexcept
{
    for (const auto& voice : voices_)
        if (voice.active)
            return true;
    return false;
}

void YouKnowEngine::beginVoiceAssignmentRescan() noexcept
{
    // The POLY-button handler gates every current assignment and clears the
    // assigner's note/voice tables. Its keyboard pass is separate, so do not
    // collapse the gate-off and the replacement Note On into one host event.
    if (!assignmentRescanPending_)
    {
        rescanPreviousUnisonMembers_.fill(false);
        rescanUnisonMidiValid_ = false;
        for (int slot = 0; slot < maxVoices; ++slot)
        {
            const auto& voice = voices_[static_cast<std::size_t>(slot)];
            if (!voice.active || !voice.unisonMember)
                continue;
            rescanPreviousUnisonMembers_[static_cast<std::size_t>(slot)] = true;
            if (!rescanUnisonMidiValid_)
            {
                rescanUnisonMidi_ = voice.currentMidi;
                rescanUnisonMidiValid_ = true;
            }
        }
    }

    assignmentRescanPending_ = true;
    // A handler may arrive after some voice writes in the current pass. Only a
    // wholly subsequent pass can guarantee that all six CPUs observed gate-off.
    assignmentRescanPassArmed_ = false;
    const bool sentVoiceOff = std::any_of(
        voices_.begin(), voices_.end(), [](const Voice& voice) {
            return voice.active && voice.keyDown;
        });
    if (sentVoiceOff)
        finishProtectedPitWritesBeforeSerialVoiceCommand();
    for (auto& voice : voices_)
    {
        voice.unisonMember = false;
        if (voice.active && voice.keyDown)
            releaseVoiceKey(voice);
        // Only the assigner loses this. The voice CPU's last pitch byte, DCO
        // phase and portamento accumulator all survive the table clear.
        voice.hasAllocatorHistory = false;
        voice.lastRootMidi = -1;
        voice.releaseStamp = 0;
    }
    generation_ = 0;
    updateActiveVoiceCount();
    if (sentVoiceOff)
    {
        restartVoiceBoardScanAfterSerialVoiceCommand();
        // The restart begins a wholly subsequent pass at RESONANCE. That pass
        // is therefore the one that may prove every card observed gate-off;
        // waiting to arm at its following wrap would add a fictitious pass.
        assignmentRescanPassArmed_ = true;
    }
}

void YouKnowEngine::completeVoiceAssignmentRescan() noexcept
{
    if (!assignmentRescanPending_)
        return;

    // The physical matrix is scanned from its high address down. Solo Unison
    // consumes the first set bit once; the poly modes continue descending and
    // therefore give a limited pool to the highest held keys.
    if (activeParameters_.keyMode == KeyMode::Unison)
    {
        const int note = highestHeldNote();
        if (note >= 0)
            assignHeldNote(note,
                           heldNoteVelocities_[static_cast<std::size_t>(note)]);
    }
    else
    {
        for (int note = 127; note >= 0; --note)
        {
            const auto index = static_cast<std::size_t>(note);
            if (heldNoteCounts_[index] != 0)
                assignHeldNote(note, heldNoteVelocities_[index]);
        }
    }

    assignmentRescanPending_ = false;
    assignmentRescanPassArmed_ = false;
    rescanPreviousUnisonMembers_.fill(false);
    rescanUnisonMidiValid_ = false;
    updateActiveVoiceCount();
}

// The delay is a hold followed by a fade. Both start again for a new phrase.
void YouKnowEngine::rearmLfoDelay() noexcept
{
    lfoDelayHoldoff_ = 0u;
    lfoDelayFade_ = 0u;
    lfoDelayLevel_ = 0.0f;
    lfoDelayByte_ = 0u;
}

int YouKnowEngine::findVoiceForNote(int midiNote) const noexcept
{
    const int limit = voiceLimit();
    for (int slot = 0; slot < limit; ++slot)
    {
        const auto& voice = voices_[static_cast<std::size_t>(slot)];
        if (voice.active && voice.keyDown && voice.rootMidi == midiNote)
            return slot;
    }
    return -1;
}

// The key assigner never steals a sounding key. With every voice held, a
// further note is simply dropped -- which is the assigner firmware's own
// policy, and the reason dense chords lose notes on it. A voice whose key has
// been let go is available again even while its release rings.
int YouKnowEngine::allocateVoice(int midiNote) noexcept
{
    const int limit = voiceLimit();
    const auto available = [this](int slot) {
        const auto& voice = voices_[static_cast<std::size_t>(slot)];
        // Sustain belongs to the voice CPU, not the key assigner. A key-up
        // frees the slot immediately even while the pedal keeps its old tail
        // audible; the next assignment is allowed to retrigger that slot.
        return !voice.active || !voice.keyDown;
    };

    if (activeParameters_.keyMode == KeyMode::Poly2)
    {
        // A plain linear scan from the first voice up, so low slots are
        // reused immediately and their release tails chopped; only the most
        // recent notes keep a full release. That truncation is the point of
        // the mode, and it is also what makes its per-voice glide musical.
        for (int slot = 0; slot < limit; ++slot)
            if (available(slot))
                return slot;
        return -1;
    }

    // Note memory first: a free voice whose *last* note -- even one whose
    // release has long finished -- matches the incoming pitch is taken, so a
    // repeated note lands on the voice already sitting at its pitch and
    // filter state. Otherwise the free voice that has been released longest
    // is taken, which is what preserves the freshest tails when keys come up
    // out of order.
    for (int slot = 0; slot < limit; ++slot)
    {
        const auto& voice = voices_[static_cast<std::size_t>(slot)];
        if (available(slot) && voice.hasAllocatorHistory
            && voice.lastRootMidi == midiNote)
            return slot;
    }

    int best = -1;
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    for (int slot = 0; slot < limit; ++slot)
    {
        const auto& voice = voices_[static_cast<std::size_t>(slot)];
        if (!available(slot))
            continue;
        // A slot that has never played counts as released at the dawn of
        // time, so an empty instrument fills from the first voice up.
        const std::uint64_t stamp = voice.hasAllocatorHistory
                                  ? voice.releaseStamp : 0;
        if (stamp < oldest || best < 0)
        {
            oldest = stamp;
            best = slot;
        }
    }
    return best;
}

void YouKnowEngine::initialiseVoice(Voice& voice, int slot, int midiNote,
                                       float velocity) noexcept
{
    const auto& parameters = activeParameters_;
    const bool wasSounding = voice.active;
    const int voiceMidi = midiNote + parameters.keyTranspose;
    const bool resetDco = pitchChangeRequestsDcoReset(voice, voiceMidi);

    // reset() permanently binds array slot N to card N. Every caller passes the
    // voice from that same slot, so this defensive assignment is a no-op and the
    // card trims already installed by reset()/setParameters() remain current.
    // In particular, a note-on must not recompute all 16 cards' four stages.
    voice.cardIndex = slot;
    // An idle extension slot represents no powered analogue card: silenceVoice
    // deliberately clears it. Construct its new virtual WAVE node at the
    // current settled mean before opening the VCA, rather than treating that
    // construction as a zero-to-DC capacitor step.
    if (!wasSounding && slot >= hardwareVoices)
        primeVoiceWaveNode(voice, parameters);
    // Wake from the freewheel by resuming, not flushing: frozen reconstruction
    // and filter state measured closer to the always-rendered reference than a
    // from-silence rebuild (retrigger nulls improved about 6 dB when nothing
    // was reset). C56/C50 is advanced separately by the cheap path below.
    // Everything is bounded and finite, the comparator reconciles against its
    // memoryless truth, and the VCA is still closed while the few stale
    // reconstruction samples flush through.
    voice.freewheeling = false;
    voice.active = true;
    voice.keyDown = true;
    voice.sustained = false;
    voice.releasing = false;
    voice.rootMidi = midiNote;
    voice.velocity = velocity;
    voice.generation = ++generation_;
    voice.energy = 0.0f;

    const float target = static_cast<float>(voiceMidi);
    voice.targetMidi = target;

    voice.glideSemitonesPerScan = resolveGlideStepPerScan(
        portamentoTravelAdcFraction(parameters.portamento));
    if (voice.glideSemitonesPerScan > 0.0f)
    {
        // The glide integrator is per voice and survives retirement, so a
        // reassigned voice slides from its retained word in the shared CPU --
        // notes several allocator assignments back, matching poly-glide
        // behavior. Only a slot with no earlier pitch starts at the new note.
        if (!wasSounding)
            voice.currentMidi = voice.hasVoicePitchHistory
                              ? voice.currentMidi : target;
    }
    else
    {
        voice.currentMidi = target;
    }
    voice.lastRootMidi = midiNote;
    voice.hasAllocatorHistory = true;
    voice.lastVoiceMidi = voiceMidi;
    voice.hasVoicePitchHistory = true;

    // A running note timer takes the count-only path for a legato pitch message.
    // A different pitch on a free/releasing voice requests the Mode-3
    // control-word path, but the voice CPU consumes it only on that voice's
    // next scan update. Explicit OUT polarity then decides whether forcing high
    // produces the positive C54/sub edge.
    if (resetDco)
        voice.dcoResetPending = true;

    if (!wasSounding)
        voice.vca = 0.0f;
    voice.envelope.noteOn();
}

void YouKnowEngine::silenceVoice(Voice& voice) noexcept
{
    voice.active = false;
    voice.keyDown = false;
    voice.sustained = false;
    voice.releasing = false;
    voice.unisonMember = false;
    voice.rootMidi = -1;
    voice.vca = 0.0f;
    voice.vcaControlTarget = 0.0f;
    voice.vcaControl = 0.0f;
    voice.energy = 0.0f;
    voice.envelope.reset();
    if (voice.cardIndex >= hardwareVoices)
    {
        // Product-extension slots have no powered hardware card while idle.
        // Since renderVoice deliberately stops advancing them, retaining an
        // old resonant filter or oscillator timeline here would freeze it in
        // amber and resurrect it on a later assignment. Keep the intended
        // digital note/portamento memories below, but reconstruct the virtual
        // audio cell from silence next time it is used.
        voice.dco.reset();
        voice.dcoResetPending = true;
        voice.pulseThresholdPrimed = false;
        voice.filter.reset();
        voice.moduleCoupling.reset();
        voice.coupledMixer.reset();
        voice.vcaInputCoupling.reset();
        voice.vcaInputVolts = 0.0f;
        voice.noiseState = hash32(
            static_cast<std::uint32_t>(voice.cardIndex) * 2246822519u + 1u) | 1u;
    }
    // Physical slots deliberately keep their free-running DCO, filter and
    // card-noise state. Every slot keeps both digital note memories,
    // currentMidi and releaseStamp for the assigner/portamento policy.
}

bool YouKnowEngine::serviceVoiceBoardNoteOn(int card, int boardPitchByte) noexcept
{
    if (!prepared_ || !voiceBoardCommandReplayActive_
        || activeConverterTimingProfile_ != ConverterTimingProfile::FirmwareControlNoInterrupt
        || card < 0 || card >= hardwareVoices || boardPitchByte < 0 || boardPitchByte > 127)
        return false;

    // The service point means the logical handler has completed. As with the
    // normal host command policy, finish only the already-protected PIT byte
    // pair; its duration and the actual ISR entry latency are not reconstructed.
    finishProtectedPitWritesBeforeSerialVoiceCommand();
    auto& voice = voices_[static_cast<std::size_t>(card)];
    const bool wasRunning = voice.envelope.running; // actual FF11 snapshot
    const bool changedPitch = firmwareControlState_.ram[9 + card] != boardPitchByte;
    const bool alreadyPending = voice.dcoResetPending
        || (firmwareControlState_.ram[0] & (1u << card)) != 0;
    const float currentWord = voice.currentMidi;
    initialiseVoice(voice, card, boardPitchByte - activeParameters_.keyTranspose, 1.0f);
    // Voice On changes FF09, gate and phase latches, never FF71's glide word.
    // Even PORTAMENTO off transfers the new byte only at the later 03EC store.
    // Using -1 also keeps later host transpose edits out of an already-decoded
    // board byte; the diagnostic's next service command owns the next byte.
    voice.rootMidi = -1;
    voice.targetMidi = static_cast<float>(boardPitchByte);
    voice.currentMidi = currentWord;
    voice.lastVoiceMidi = boardPitchByte;
    voice.dcoResetPending = alreadyPending || (changedPitch && !wasRunning);
    voice.unisonMember = false;
    // 0128→0144 bypasses FF00 on an equal byte, even when FF11 is clear.
    // A changed byte tests FF11 at 012E, not the just-written gate at 0118.
    // Envelope::noteOn implements the separate 0132/0145 phase-latch branches.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L204-L245
    updateActiveVoiceCount();
    restartVoiceBoardScanAfterSerialVoiceCommand();
    return true;
}

bool YouKnowEngine::serviceVoiceBoardNoteOff(int card) noexcept
{
    if (!prepared_ || !voiceBoardCommandReplayActive_
        || activeConverterTimingProfile_ != ConverterTimingProfile::FirmwareControlNoInterrupt
        || card < 0 || card >= hardwareVoices)
        return false;
    finishProtectedPitWritesBeforeSerialVoiceCommand();
    // Every decoded 80..85 command restarts, including a duplicate voice off.
    // It clears the addressed gate and, unless HOLD is set, FF33; pitch RAM,
    // FF07/08, FF11 and the capacitor/timer phase all survive the service.
    releaseVoiceKey(voices_[static_cast<std::size_t>(card)]);
    updateActiveVoiceCount();
    restartVoiceBoardScanAfterSerialVoiceCommand();
    return true;
}

void YouKnowEngine::noteOn(int midiNote, float velocity)
{
    if (voiceBoardCommandReplayActive_)
        return;
    if (midiNote < 0 || midiNote > 127)
        return;
    noteOnInternal(midiNote, std::clamp(velocity, 0.0f, 1.0f));
}

void YouKnowEngine::noteOnInternal(int midiNote, float velocity) noexcept
{
    // The assigner consumes changes in the keyboard bitfield, not repeated
    // writes of a bit that is already high. Keep the count balanced for MIDI
    // streams that overlap equal notes, but do not retrigger on that repeat.
    if (!rememberHeldNote(midiNote, velocity))
        return;

    // A POLY handler has cleared the allocator and is waiting for the keyboard
    // scan. This new bit will be discovered by that same descending pass.
    if (assignmentRescanPending_)
        return;
    assignHeldNote(midiNote, velocity);
}

void YouKnowEngine::finishProtectedPitWritesBeforeSerialVoiceCommand() noexcept
{
    // A fractional passive-hold event has already written its DAC/mux and
    // advanced the physical capacitor inside the preceding sample. Its public
    // target is deferred only until the next boundary poll. Preserve that
    // completed port store before the non-returning handler discards the poll.
    if (passiveHoldEventLatch_.valid)
    {
        const float target = passiveHoldEventLatch_.target;
        performConverterWrite(
            passiveHoldEventLatch_.write, activeParameters_, &target);
        passiveHoldEventLatch_ = {};
    }

    // Finish everything protected by DI before the Voice On/Off handler is
    // allowed to mutate note state. The reset path enters DI four states before
    // its T-389 control store, so that narrow interval must capture the old
    // voice payload here; doing it after assignment would program the new note.
    // The running path reaches DI 13 states after T-389, leaving 42 states to
    // its LSB. Every awaiting-MSB state is protected. Exact boundary ties use
    // the declared PIT/DI-first policy.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L732-L741
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L783-L794
    for (int slot = 0; slot < hardwareVoices; ++slot)
    {
        auto& voice = voices_[static_cast<std::size_t>(slot)];
        auto& dco = voice.dco;
        const bool pitchScanWouldRequestReset = voice.rootMidi >= 0
            && pitchChangeRequestsDcoReset(
                voice, voice.rootMidi + activeParameters_.keyTranspose);
        const bool capturedResetBranch = activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt
            ? (firmwarePassResetMask_ & (1u << slot)) != 0
            : voice.dcoResetPending || pitchScanWouldRequestReset;
        const bool resetPrestageIsProtected =
            dco.pitWriteState
                    == Dco::PitWriteState::awaitingPitchPrestage
            && capturedResetBranch
            && dco.cpuStatesToWrite <= pitResetDiToControlStates;
        if (resetPrestageIsProtected)
            prestageDcoPitchTransaction(
                voice, rangeClockClocksToFallingEdge_, 1.0f, true);

        const bool lsbIsProtected =
            dco.pitWriteState == Dco::PitWriteState::awaitingLsb
            && (dco.pitState == Dco::PitState::awaitingCount
                || dco.cpuStatesToWrite <= pitDiToLsbStates);
        const bool msbIsProtected =
            dco.pitWriteState == Dco::PitWriteState::awaitingMsb;
        if (lsbIsProtected || msbIsProtected)
            dco.stageMode3Count(dco.pitWriteDivider);

        voice.dcoPitchTransactionValid = false;
        voice.dcoPitchTransactionColdStart = false;
        dco.pitWriteState = Dco::PitWriteState::idle;
        dco.cpuStatesToWrite = 0.0;
    }
}

void YouKnowEngine::restartVoiceBoardScanAfterSerialVoiceCommand() noexcept
{
    if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
    {
        controlScanPhase_ = 0.0;
        nextConverterWrite_ = 0;
        converterPassEnvelopeUpdated_.fill(false);
        converterPassPortamentoUpdated_ = converterNextPassPortamentoUpdated_ = false;
        passiveHoldEventLatch_ = {};
        refreshFirmwareControlTrace();
        return;
    }

    // The recovered B-2 serial ISR does not RETI from Voice On/Off. It loads
    // SP with $ffff and jumps through $02eb to the beginning of the main loop,
    // so an interrupted converter pass resumes at RESONANCE rather than at its
    // former ordinal. Treat the engine's logical note command as that completed
    // handler boundary. The 31.25-kbaud arrival phase and the installed NMOS
    // uPD7810's automatic entry latency remain unmeasured and are deliberately
    // not turned into random timing here.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L204-L244
    for (auto& voice : voices_)
        voice.envelope.latchGate(sustainPedalDown_);
    const bool passBoundaryWasAlreadyDue = controlScanPhase_ >= 1.0;
    if (!passBoundaryWasAlreadyDue)
    {
        // The jump begins a fresh loop, so its early onset calculation runs
        // again. The restart itself does not rerun the late LFO/PWM update;
        // preserve that state and refresh only what 030D-03A1 derives from the
        // already-stored oscillator value.
        // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L526-L607
        advanceLfoDelay(activeParameters_);
        dcoLfoPitchWord_ = dcoLfoPitchWordOffset(
            lfoAccumulator_, lfoPolarity_ >= 0.0f, lfoDelayByte_,
            storedControlByte(activeParameters_.dcoLfoDepth),
            storedControlByte(modWheelTarget_),
            controlAdcByte(activeParameters_.benderLfoDepth));
        vcfLfoCountsWord_ = vcfLfoCountsWord(
            lfoAccumulator_, lfoPolarity_ >= 0.0f, lfoDelayByte_,
            storedControlByte(activeParameters_.vcfLfoDepth));
    }
    controlScanPhase_ = passBoundaryWasAlreadyDue ? 1.0 : 0.0;
    nextConverterWrite_ = 0;
    converterPassEnvelopeUpdated_.fill(false);
    converterPassPortamentoUpdated_ = false;
    converterNextPassPortamentoUpdated_ = false;
    passiveHoldEventLatch_ = {};
    refreshFirmwareDcoTiming();
}

void YouKnowEngine::refreshFirmwareControlTrace(bool initialise) noexcept
{
    if (activeConverterTimingProfile_ != ConverterTimingProfile::FirmwareControlNoInterrupt)
        return;
    static const auto tables = [] {
        FirmwareControlTrace::Tables result;
        for (unsigned i = 0; i < 128; ++i)
        {
            result.attack[i] = attackIncrementForByte(static_cast<std::uint8_t>(i));
            result.portamento[i] = portamentoIncrementForIndex(static_cast<std::uint8_t>(i));
        }
        for (int i = 0; i < 104; ++i)
        {
            result.pitchCv[static_cast<std::size_t>(i)] = derivedDcoCvAnchor(i);
            result.pitchDivider[static_cast<std::size_t>(i)] =
                static_cast<std::uint16_t>(derivedDcoDividerAnchor(i));
        }
        return result;
    }();
    auto& ram = firmwareControlState_.ram;
    if (initialise)
    {
        firmwareControlState_ = {};
        ram[0x1e] = 8; // idle-pass hangtime latch; note command can rearm it
        ram[0x4a] = 0;
    }
    const auto putWord = [&ram](unsigned address, unsigned value) {
        ram[address] = static_cast<std::uint8_t>(value);
        ram[address + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto& p = activeParameters_;
    // Explicit host adapter: panel/serial controls are held at the pass-start
    // snapshot. The ADC ISR is absent from this nominal trace; its input bank,
    // four raw samples and preceding hysteresis memory are supplied separately.
    // With no capture configured, use a frozen lower-bank ADC snapshot. Its
    // calculated outputs are overwritten by the next host snapshot. This is a
    // repeatable nominal path, not a claim about the real ADC interrupt phase.
    const auto tune = masterTunePitchWordOffset(p.masterTuneCents);
    const auto bend = dcoBendCommand(pitchBendTarget_);
    ram[0x1e] = static_cast<std::uint8_t>((ram[0x1e] & 0x0e)
        | (sustainPedalDown_ ? 1 : 0) | (p.pulseEnabled ? 0x40 : 0)
        | (tune < 0 ? 0x80 : 0) | (bend < 0 ? 0x20 : 0));
    ram[0x37] = static_cast<std::uint8_t>((p.pwmSource == PwmSource::Lfo ? 1 : 0)
        | (p.envPolarity == EnvPolarity::Normal ? 2 : 0)
        | (p.vcaMode == VcaMode::Envelope ? 4 : 0));
    putWord(0x21, envelopeDecayReleaseMultiplier(p.decay));
    putWord(0x23, storedControlByte(p.sustain) * 128u);
    putWord(0x25, envelopeDecayReleaseMultiplier(p.release));
    putWord(0x39, storedControlByte(p.noiseLevel) * 128u);
    putWord(0x3b, storedControlByte(p.subLevel) * 128u);
    putWord(0x3d, storedControlByte(p.cutoff) * 128u);
    putWord(0x3f, storedControlByte(p.resonance) * 128u);
    putWord(0x43, storedControlByte(p.vcaLevel) * 128u);
    ram[0x41] = static_cast<std::uint8_t>(storedControlByte(p.envDepth) * 2u);
    ram[0x42] = static_cast<std::uint8_t>(storedControlByte(p.keyFollow) * 2u);
    ram[0x45] = storedControlByte(p.attack);
    ram[0x47] = static_cast<std::uint8_t>(storedControlByte(p.pwmDepth) * 2u);
    ram[0x48] = static_cast<std::uint8_t>(storedControlByte(p.vcfLfoDepth) * 2u);
    ram[0x49] = dcoLfoDepthScale(storedControlByte(p.dcoLfoDepth));
    putWord(0x4b, lfoRateIncrement(p.lfoRate));
    putWord(0x58, envelopeAttackIncrement(p.lfoDelay));
    putWord(0x6c, lfoDelayFadeIncrement(p.lfoDelay));
    ram[0x61] = static_cast<std::uint8_t>(std::abs(tune));
    ram[0x63] = static_cast<std::uint8_t>(2u * storedControlByte(modWheelTarget_));
    ram[0x64] = static_cast<std::uint8_t>((ram[0x63] * controlAdcByte(p.benderLfoDepth)) >> 8);
    putWord(0x65, static_cast<unsigned>(std::abs(vcfBendCountsWord(bend, controlAdcByte(p.benderVcfDepth)))));
    putWord(0x68, static_cast<unsigned>(std::abs(dcoBendWordForCommand(bend, controlAdcByte(p.benderDcoDepth)))));
    ram[0x7d] = portamentoIncrement(portamentoTravelAdcFraction(p.portamento));
    ram[0x07] = ram[0x08] = ram[0x10] = ram[0x11] = ram[0x33] = 0;
    for (int card = 0; card < hardwareVoices; ++card)
    {
        auto& voice = voices_[static_cast<std::size_t>(card)];
        const auto bit = static_cast<std::uint8_t>(1u << card);
        const auto& env = voice.envelope;
        if (voice.dcoResetPending) ram[0] |= bit;
        if (env.attackPhase) ram[7] |= bit;
        if (env.decayPhase) ram[8] |= bit;
        if (env.gate) ram[0x10] |= bit;
        if (env.running) ram[0x11] |= bit;
        if (env.phase) ram[0x33] |= bit;
        const float target = voice.rootMidi >= 0
            ? static_cast<float>(voice.rootMidi + p.keyTranspose) : voice.targetMidi;
        // The optional host transpose extends below the board's unsigned byte;
        // this profile clamps that extension at zero, explicitly like RAM.
        ram[9 + card] = static_cast<std::uint8_t>(std::clamp(std::lround(target), 0L, 255L));
        putWord(0x71 + 2 * card, static_cast<unsigned>(std::clamp(
            std::lround(voice.currentMidi * 256.0f), 0L, 65535L)));
        putWord(0x27 + 2 * card, env.level);
    }
    ram[0x5c] = firmwareAdcSnapshot_.upperBank ? 8 : 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        ram[0x5d + i] = firmwareAdcSnapshot_.raw[i];
        ram[0x80 + (firmwareAdcSnapshot_.upperBank ? 4 : 0) + i] =
            firmwareAdcSnapshot_.previous[i];
    }
    firmwareControlState_.adcComplete = firmwareAdcSnapshot_.conversionComplete;
    firmwarePassResetMask_ = ram[0];
    firmwareControlTrace_ = FirmwareControlTrace::run(firmwareControlState_, tables);
    firmwareControlTraceValid_ = firmwareControlTrace_.valid;
    nextFirmwareControlEvent_ = 0;
    if (!firmwareControlTraceValid_)
        return;
    constexpr double phasePerState = controlScanHz / voiceCpuStateHz;
    converterPassEndPhase_ = firmwareControlTrace_.states * phasePerState;
    for (std::size_t i = 0; i < firmwareControlTrace_.count; ++i)
    {
        const auto& event = firmwareControlTrace_.events[i];
        if (event.kind == FirmwareControlTrace::EventKind::Converter)
        {
            converterEventPhases_[event.card] = event.states * phasePerState;
            firmwareConverterCodes_[event.card] = event.value;
        }
        else if (event.kind == FirmwareControlTrace::EventKind::Inhibit)
            converterInhibitPhases_[event.card] = event.states * phasePerState;
    }
}

void YouKnowEngine::advanceFirmwareControlEvents(double phase) noexcept
{
    if (activeConverterTimingProfile_ != ConverterTimingProfile::FirmwareControlNoInterrupt
        || !firmwareControlTraceValid_)
        return;
    const double elapsedStates = phase * voiceCpuStateHz / controlScanHz;
    auto& ram = firmwareControlState_.ram;
    const auto word = [&ram](unsigned address) {
        return static_cast<unsigned>(ram[address]) + 256u * ram[address + 1];
    };
    while (nextFirmwareControlEvent_ < firmwareControlTrace_.count)
    {
        const auto& event = firmwareControlTrace_.events[nextFirmwareControlEvent_];
        if (event.states > elapsedStates + 1.0e-7)
            break;
        ++nextFirmwareControlEvent_;
        if (event.kind == FirmwareControlTrace::EventKind::RamByte)
        {
            ram[event.card] = static_cast<std::uint8_t>(event.value);
            if (event.card == 0)
                for (int card = 0; card < hardwareVoices; ++card)
                    voices_[static_cast<std::size_t>(card)].dcoResetPending =
                        (ram[0] & (1u << card)) != 0;
            if (event.card == 7 || event.card == 8 || event.card == 0x10
                || event.card == 0x11 || event.card == 0x33)
                for (int card = 0; card < hardwareVoices; ++card)
                {
                    auto& env = voices_[static_cast<std::size_t>(card)].envelope;
                    env.attackPhase = (ram[7] & (1u << card)) != 0;
                    env.decayPhase = (ram[8] & (1u << card)) != 0;
                    env.gate = (ram[0x10] & (1u << card)) != 0;
                    env.running = (ram[0x11] & (1u << card)) != 0;
                    env.phase = (ram[0x33] & (1u << card)) != 0;
                }
        }
        else if (event.kind == FirmwareControlTrace::EventKind::Envelope)
        {
            auto& env = voices_[event.card].envelope;
            env.level = event.value;
            env.value = static_cast<float>(env.level >> 2u) / 4095.0f;
            env.stage = !env.running ? (env.level == 0 ? EnvelopeStage::Idle : EnvelopeStage::Release)
                : !env.phase ? EnvelopeStage::Attack
                : env.level <= word(0x23) ? EnvelopeStage::Sustain : EnvelopeStage::Decay;
            converterPassEnvelopeUpdated_[event.card] = true;
        }
        else if (event.kind == FirmwareControlTrace::EventKind::Portamento)
        {
            voices_[event.card].currentMidi = static_cast<float>(event.value) / 256.0f;
            converterPassPortamentoUpdated_ = event.card == 5;
        }
    }
    lfoAccumulator_ = static_cast<std::uint16_t>(word(0x4d));
    lfoRising_ = (ram[0x4a] & 1u) == 0;
    lfoPolarity_ = (ram[0x4a] & 2u) == 0 ? 1.0f : -1.0f;
    lfoValue_ = lfoPolarity_ * static_cast<float>(lfoAccumulator_) / 8191.0f;
    lfoDelayHoldoff_ = word(0x56);
    lfoDelayFade_ = (ram[0x1e] & 4) ? 65536u : word(0x5a);
    lfoDelayByte_ = (ram[0x1e] & 4) ? 255 : static_cast<std::uint8_t>(lfoDelayFade_ >> 8);
    lfoDelayLevel_ = static_cast<float>(lfoDelayByte_) / 255.0f;
    displayLfo_ = lfoValue_ * lfoDelayLevel_;
    dcoLfoPitchWord_ = static_cast<std::int32_t>(word(0x51)) * (lfoPolarity_ > 0 ? 1 : -1);
    vcfLfoCountsWord_ = static_cast<std::int32_t>(word(0x53)) * (lfoPolarity_ > 0 ? 1 : -1);
}

float YouKnowEngine::firmwareConverterTarget(const ConverterWrite& write) const noexcept
{
    const auto& order = converterWriteOrder();
    std::size_t ordinal = 0;
    for (; ordinal < order.size(); ++ordinal)
        if (order[ordinal].destination == write.destination && order[ordinal].voice == write.voice)
            break;
    if (ordinal == order.size()) return 0;
    const float code = static_cast<float>(firmwareConverterCodes_[ordinal]);
    if (write.destination == ConverterDestination::Pwm)
        return pwmDacVolts(firmwareConverterCodes_[ordinal]);
    if (write.destination == ConverterDestination::Vcf)
    {
        // Keep the optional velocity path an explicit product extension.
        if (activeParameters_.velocityDepth != 0)
            return voiceVcfTarget(voices_[static_cast<std::size_t>(write.voice)], activeParameters_);
        const float counts = code * 4.0f;
        return counts + vcfConverterCarryCounts(counts)
            * (activeParameters_.useServiced439522VcfCalibration ? 1.0f : activeParameters_.calibration);
    }
    if (write.destination == ConverterDestination::VoiceVca)
    {
        const auto& voice = voices_[static_cast<std::size_t>(write.voice)];
        const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
        return clamp01(code / 4095.0f * velocityGain(activeParameters_, voice)
            + card.vcaControlOffset * 0.004f * activeParameters_.calibration);
    }
    return code / 4095.0f;
}

void YouKnowEngine::refreshFirmwareDcoTiming() noexcept
{
    if (activeConverterTimingProfile_
        != ConverterTimingProfile::FirmwareDcoNoInterrupt)
        return;

    // This is deliberately an opt-in *partial* no-interrupt candidate. The
    // first DCO/other destinations and 4.2 ms pass retain chart policy. B-2's
    // full loop has data-dependent work and no timer wait at 07b5; replacing
    // the whole scheduler requires a complete instruction trace, not a rescale
    // of these gaps. Serial wire/entry time, pin edges and mux acquisition
    // remain unmeasured. No latency is added to incoming host events.
    //
    // Predict only the next pitch/reset branch from the logical pass snapshot.
    // A host edit after this snapshot can change the later transaction's data;
    // its branch-time effect is outside this candidate's qualification. This
    // explicit limit is why the shipping profile does not select it yet.
    const auto& p = activeParameters_;
    const float glide = resolveGlideStepPerScan(
        portamentoTravelAdcFraction(p.portamento));
    const std::int32_t controlOffset = masterTunePitchWordOffset(p.masterTuneCents)
        + dcoPitchBendWord_ + dcoLfoPitchWord_;
    for (int slot = 1; slot < hardwareVoices; ++slot)
    {
        const auto& voice = voices_[static_cast<std::size_t>(slot)];
        const float target = voice.rootMidi >= 0
            ? static_cast<float>(voice.rootMidi + p.keyTranspose)
            : voice.targetMidi;
        const float nextMidi = glide > 0.0f
            ? voice.currentMidi + std::clamp(target - voice.currentMidi, -glide, glide)
            : target;
        const auto word = aggregatePitchWord(nextMidi, controlOffset);
        const bool reset = voice.dcoResetPending
            || (voice.rootMidi >= 0 && pitchChangeRequestsDcoReset(
                    voice, voice.rootMidi + p.keyTranspose));
        const auto ordinal = static_cast<std::size_t>(slot + 3);
        converterEventPhases_[ordinal] = converterEventPhases_[ordinal - 1]
            + firmwareDcoInterWriteStates(reset, static_cast<std::uint8_t>(word >> 8u))
                * controlScanHz / voiceCpuStateHz;
    }
}

void YouKnowEngine::assignHeldNote(int midiNote, float velocity) noexcept
{
    // The LFO delay re-arms on the first voice-on that follows a pass with no
    // voice running, not on the first key of a phrase. B-2 rebuilds
    // $FF11_voiceRun each pass as the gate mask, OR-ed with its previous value
    // while the sustain flag is set (0x02f2..0x02ff); a pass that finds it
    // zero sets the hangtime flag (0x030d, 0x036b), and the next pass that
    // finds it non-zero with that flag set clears the holdoff and the fade
    // (0x0312..0x0320). Three consequences the key mask cannot express: with
    // HOLD down and every key lifted the sustained voices keep voiceRun
    // non-zero, so a new key does not restart the delay; a Solo Unison key-up
    // or a POLY press gates all six off, so the re-trigger that follows does;
    // and a seventh key still held after the six sounding ones are released
    // does too. A releasing voice has its gate bit clear and so does not
    // count, which is the behaviour the key mask already had.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L510-L535
    if (!anyVoiceRunning())
        rearmLfoDelay();

    if (activeParameters_.keyMode == KeyMode::Unison)
    {
        finishProtectedPitWritesBeforeSerialVoiceCommand();
        // Every voice takes the same note, and every note timer divides the
        // same reference by the same integer, so there is no pitch spread at
        // steady state. Ordered count writes can temporarily select different
        // counts during modulation; prior counter state also sets phase. Analog
        // R/C variation changes waveform shape, not that timer division. Each
        // slot's glide starts from that slot's own pitch history,
        // as its per-voice integrator does. Entering from Poly can retain
        // different stored glide words. Only the product extension that
        // widens an established stack adopts its existing position.
        const int limit = voiceLimit();
        float stackMidi = 0.0f;
        bool haveStackMidi = false;
        for (int slot = 0; slot < limit && !haveStackMidi; ++slot)
        {
            const auto& voice = voices_[static_cast<std::size_t>(slot)];
            if (voice.active && voice.unisonMember)
            {
                stackMidi = voice.currentMidi;
                haveStackMidi = true;
            }
        }
        if (!haveStackMidi && assignmentRescanPending_
            && rescanUnisonMidiValid_)
        {
            stackMidi = rescanUnisonMidi_;
            haveStackMidi = true;
        }
        for (int slot = 0; slot < limit; ++slot)
        {
            auto& voice = voices_[static_cast<std::size_t>(slot)];
            const bool joiningWidenedStack = assignmentRescanPending_
                && !rescanPreviousUnisonMembers_[static_cast<std::size_t>(slot)];
            const bool freshVoiceCpu = !voice.hasVoicePitchHistory;
            initialiseVoice(voice, slot, midiNote, velocity);
            if ((freshVoiceCpu || joiningWidenedStack) && haveStackMidi
                && voice.glideSemitonesPerScan > 0.0f)
                voice.currentMidi = stackMidi;
            voice.unisonMember = true;
        }
        // A voice count lowered while a wider stack was sounding leaves slots
        // above the new count still keyed to the old note. They leave the stack
        // here rather than holding that note against the new one, which would
        // turn Unison into a chord.
        for (int slot = limit; slot < maxVoices; ++slot)
        {
            auto& voice = voices_[static_cast<std::size_t>(slot)];
            if (voice.active && voice.unisonMember)
                dropFromUnison(voice);
        }
        updateActiveVoiceCount();
        restartVoiceBoardScanAfterSerialVoiceCommand();
        return;
    }

    const int existing = findVoiceForNote(midiNote);
    const int slot = existing >= 0 ? existing : allocateVoice(midiNote);
    if (slot < 0)
        return; // Every key is held: the note is dropped, as on the hardware.

    finishProtectedPitWritesBeforeSerialVoiceCommand();
    auto& voice = voices_[static_cast<std::size_t>(slot)];
    initialiseVoice(voice, slot, midiNote, velocity);
    voice.unisonMember = false;
    updateActiveVoiceCount();
    restartVoiceBoardScanAfterSerialVoiceCommand();
}

void YouKnowEngine::noteOff(int midiNote)
{
    if (voiceBoardCommandReplayActive_)
        return;
    if (midiNote < 0 || midiNote > 127)
        return;
    noteOffInternal(midiNote);
}

void YouKnowEngine::reassertKeyMode() noexcept
{
    if (prepared_ && !voiceBoardCommandReplayActive_)
        beginVoiceAssignmentRescan();
}

void YouKnowEngine::noteOffInternal(int midiNote) noexcept
{
    // No high-to-low bit transition means no firmware handler. In particular,
    // an unmatched Note Off must not make Solo Unison gate and rescan.
    if (!forgetHeldNote(midiNote))
        return;

    const int remaining = highestHeldNote();

    // The old assignments have already been gated. Only the held-key table
    // needs changing before the pending descending scan reaches it.
    if (assignmentRescanPending_)
        return;

    if (activeParameters_.keyMode == KeyMode::Unison)
    {
        // Any physical key-up makes the Solo Unison handler gate the stack,
        // clear its note table and rescan the keyboard. The highest remaining
        // key therefore wins, and every surviving note is a fresh envelope
        // attack rather than a legato hand-off.
        if (remaining >= 0)
        {
            beginVoiceAssignmentRescan();
            return;
        }

        finishProtectedPitWritesBeforeSerialVoiceCommand();
        for (auto& voice : voices_)
            if (voice.active && voice.unisonMember && voice.keyDown)
                releaseVoiceKey(voice);
        restartVoiceBoardScanAfterSerialVoiceCommand();
        return;
    }

    const bool sentVoiceOff = std::any_of(
        voices_.begin(), voices_.end(), [midiNote](const Voice& voice) {
            return voice.active && voice.rootMidi == midiNote
                && voice.keyDown;
        });
    if (sentVoiceOff)
        finishProtectedPitWritesBeforeSerialVoiceCommand();
    for (auto& voice : voices_)
    {
        if (!voice.active || voice.rootMidi != midiNote || !voice.keyDown)
            continue;
        releaseVoiceKey(voice);
    }
    if (sentVoiceOff)
        restartVoiceBoardScanAfterSerialVoiceCommand();
}

void YouKnowEngine::releaseAllNotes()
{
    clearHeldNotes();
    assignmentRescanPending_ = false;
    assignmentRescanPassArmed_ = false;
    rescanPreviousUnisonMembers_.fill(false);
    rescanUnisonMidiValid_ = false;
    const bool sentVoiceOff = std::any_of(
        voices_.begin(), voices_.end(), [](const Voice& voice) {
            return voice.active && voice.keyDown;
        });
    if (sentVoiceOff)
        finishProtectedPitWritesBeforeSerialVoiceCommand();
    for (auto& voice : voices_)
    {
        if (!voice.active || !voice.keyDown)
            continue;
        releaseVoiceKey(voice);
    }
    if (sentVoiceOff)
        restartVoiceBoardScanAfterSerialVoiceCommand();
}

void YouKnowEngine::allNotesOff()
{
    clearHeldNotes();
    assignmentRescanPending_ = false;
    assignmentRescanPassArmed_ = false;
    rescanPreviousUnisonMembers_.fill(false);
    rescanUnisonMidiValid_ = false;
    sustainPedalDown_ = false;
    for (auto& voice : voices_)
        silenceVoice(voice);
    // A hard stop has to be silent now, not once the delay lines have run out.
    // Cutting the voices alone would leave the chorus playing back the last
    // few milliseconds of a held chord after the panic.
    clearOutputPath();
    oversamplingIdleSamples_ = oversamplingQuietSamples_;
    updateActiveVoiceCount();
}

void YouKnowEngine::setPitchBend(float normalisedBipolar) noexcept
{
    pitchBendTarget_ = std::clamp(sanitised(normalisedBipolar, 0.0f), -1.0f, 1.0f);
}

void YouKnowEngine::setModWheel(float amount) noexcept
{
    modWheelTarget_ = clamp01(sanitised(amount, 0.0f));
}

void YouKnowEngine::setSustainPedal(bool down) noexcept
{
    if (sustainPedalDown_ == down)
        return;
    sustainPedalDown_ = down;
    if (down)
        return;

    for (auto& voice : voices_)
        if (voice.active && voice.sustained && !voice.keyDown)
        {
            voice.sustained = false;
            voice.releasing = true;
            // B-2 sustain-off (00BF) does not mutate envelope phase bits.
            // The next FF11 snapshot makes this voice stop running.
        }
}

void YouKnowEngine::updateActiveVoiceCount() noexcept
{
    int count = 0;
    int mask = 0;
    for (int slot = 0; slot < maxVoices; ++slot)
        if (voices_[static_cast<std::size_t>(slot)].active)
        {
            ++count;
            if (slot < 16)
                mask |= 1 << slot;
        }
    activeVoiceCount_ = count;
    anyVoiceActive_ = count > 0;
    displayVoiceMask_ = mask;
}

// ---------------------------------------------------------------------------
// Modulation
// ---------------------------------------------------------------------------

void YouKnowEngine::advanceLfo(const EngineParameters& parameters) noexcept
{
    // The modulator is firmware: it advances once per converter scan and holds
    // its value in between. That staircase is audible as a faint roughness on
    // deep, slow vibrato, and smoothing it away would be modelling a different
    // instrument. The caller invokes this exactly once per pass.

    // A clamped accumulator, not a phase: the rate coefficient is added until
    // the span clamps, the direction flips there, and a polarity flip at the
    // bottom folds the two sweeps into a bipolar triangle. The clamp discards
    // whatever the last step overshot by, so fast settings quantise onto
    // whole passes per sweep.
    const std::uint16_t coefficient = lfoRateIncrement(parameters.lfoRate);
    if (lfoRising_)
    {
        const std::uint32_t next =
            static_cast<std::uint32_t>(lfoAccumulator_) + coefficient;
        if (next >= 0x2000u)
        {
            lfoAccumulator_ = 0x1fffu;
            lfoRising_ = false;
        }
        else
            lfoAccumulator_ = static_cast<std::uint16_t>(next);
    }
    else
    {
        if (coefficient > lfoAccumulator_)
        {
            lfoAccumulator_ = 0u;
            lfoRising_ = true;
            lfoPolarity_ = -lfoPolarity_;
        }
        else
            lfoAccumulator_ = static_cast<std::uint16_t>(
                lfoAccumulator_ - coefficient);
    }
    lfoValue_ = lfoPolarity_
              * static_cast<float>(lfoAccumulator_) / 8191.0f;

    advanceLfoDelay(parameters);
}

void YouKnowEngine::advanceLfoDelay(
    const EngineParameters& parameters) noexcept
{
    // Delay: a silent hold that advances at the attack table's own rate, then
    // the stepped fade. The pair is re-armed the moment a note starts with no
    // key down -- release tails still ringing keep their vibrato, because the
    // firmware's retrigger latch watches the keys, not the envelopes.
    if (lfoDelayHoldoff_ < 0x4000u)
    {
        lfoDelayHoldoff_ = std::min<std::uint32_t>(
            0x4000u,
            lfoDelayHoldoff_ + envelopeAttackIncrement(parameters.lfoDelay));
        lfoDelayByte_ = 0u;
    }

    // OFFI branches straight into DADDNC when the just-stored holdoff crosses
    // 0x3fff. Keep the two tests independent so that crossing pass also makes
    // the fade's first step instead of inserting one extra 4.2 ms hold.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L577-L607
    if (lfoDelayHoldoff_ >= 0x4000u && lfoDelayFade_ < 0x10000u)
    {
        lfoDelayFade_ = std::min<std::uint32_t>(
            0x10000u,
            lfoDelayFade_ + lfoDelayFadeIncrement(parameters.lfoDelay));
        lfoDelayByte_ = lfoDelayFade_ >= 0x10000u
            ? 255u : static_cast<std::uint8_t>(lfoDelayFade_ >> 8u);
    }
    else if (lfoDelayFade_ >= 0x10000u)
        lfoDelayByte_ = 255u;

    lfoDelayLevel_ = static_cast<float>(lfoDelayByte_) / 255.0f;
    displayLfo_ = lfoValue_ * lfoDelayLevel_;
}

void YouKnowEngine::updateVoiceCardDrift(VoiceCard& card) noexcept
{
    // A voiced residual wander of the analogue control chain, independent of
    // the temperature state. At 375 Hz this AR(1) prior has a 3.332 s
    // correlation time and about 2.425 cents RMS cutoff movement at Character
    // 1. Neither number is measured Juno thermal behavior. Do not add a second
    // thermal wander on top without separating compensated cutoff response
    // from this existing prior. The DCOs have a separate, shared clock.
    card.driftState = xorshift32(card.driftState);
    const float excitation =
        static_cast<float>(card.driftState & 0xffffu) * (2.0f / 65535.0f) - 1.0f;
    card.driftValue = card.driftValue * 0.9992f + excitation * 0.004f;
}

std::uint32_t YouKnowEngine::updateVoiceScan(
    Voice& voice, const EngineParameters& parameters) noexcept
{
    updateVoiceEnvelope(voice, parameters);
    updateVoicePortamento(voice, parameters);
    const std::uint32_t count = updateVoicePitch(
        voice, parameters);
    updateVoiceVcfTarget(voice, parameters);
    updateVoiceVcaTarget(voice, parameters);
    return count;
}

bool YouKnowEngine::pitchChangeRequestsDcoReset(
    const Voice& voice, int voiceMidi) noexcept
{
    // A changed pitch branches on FF11 at 012E, before the new pass copies
    // FF10 into it. In particular, lifting HOLD has not yet cleared FF11.
    // Reading immediate keyDown/sustained flags here reset the note timer
    // spuriously when a new note followed HOLD-off within the current pass.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L216-L227
    return (!voice.hasVoicePitchHistory || voice.lastVoiceMidi != voiceMidi)
        && !voice.envelope.running;
}

void YouKnowEngine::updateVoiceEnvelope(
    Voice& voice, const EngineParameters& parameters) noexcept
{
    // --- Envelope ---------------------------------------------------------
    // A linear attack and multiplicative falling segments, advanced once per
    // scan. The published minimum of 1.5 ms is shorter than one scan pass, so
    // the shortest attack the instrument can actually produce is one pass
    // long. There is no per-voice rate error here: the generator is the
    // shared processor, so six voices' envelopes are digitally identical and
    // only the analogue chain after them disperses.
    // Recurrence, widths and coefficient laws are resolved for the supplied,
    // hash-matched B-2 image. All three are the same shared-processor answer
    // for every voice, so each is resolved only when its panel position has
    // actually moved since the last voice asked -- see the note on the
    // envelopeLaw* members.
    if (parameters.attack != envelopeLawAttack_)
    {
        envelopeLawAttack_ = parameters.attack;
        envelopeLawAttackIncrement_ = envelopeAttackIncrement(parameters.attack);
    }
    if (parameters.decay != envelopeLawDecay_)
    {
        envelopeLawDecay_ = parameters.decay;
        envelopeLawDecayMultiplier_ =
            envelopeDecayReleaseMultiplier(parameters.decay);
    }
    if (parameters.release != envelopeLawRelease_)
    {
        envelopeLawRelease_ = parameters.release;
        envelopeLawReleaseMultiplier_ =
            envelopeDecayReleaseMultiplier(parameters.release);
    }
    voice.attackIncrement = envelopeLawAttackIncrement_;
    voice.decayMultiplier = envelopeLawDecayMultiplier_;
    voice.releaseMultiplier = envelopeLawReleaseMultiplier_;

    voice.envelope.tick(voice.attackIncrement, voice.decayMultiplier,
                        storedControlAlignedWord(parameters.sustain),
                        voice.releaseMultiplier);
}

void YouKnowEngine::updateEnvelopeBeforeConverterWrite(
    const ConverterWrite& write, const EngineParameters& parameters) noexcept
{
    if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
        return;

    const int envelopeCard = write.destination == ConverterDestination::Pwm
        ? 0 : write.destination == ConverterDestination::VoiceVca
            && write.voice >= 0 && write.voice < hardwareVoices - 1
        ? write.voice + 1 : -1;
    if (envelopeCard < 0)
        return;
    auto& updated = converterPassEnvelopeUpdated_[
        static_cast<std::size_t>(envelopeCard)];
    if (updated)
        return;

    // B-2 leaves the entire DCO loop at 04A3 before computing any envelope
    // (0503..0590). VCF n then consumes ENV n at 05C5; VCA n follows using
    // that stored envelope, while the next loop has already computed ENV n+1.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L800-L943
    // Resolve at the earliest following physical write on the selected grid:
    // ENV 0 before PWM, ENV n before VCA n-1. These are proven ordinal bounds,
    // not the unpublished instruction timestamps of the earlier calculation.
    // Abandoning the DCO train must not advance ENV, but an interrupt between
    // PWM / previous-card VCA and this card's VCF must retain the stored word.
    // The fractional peek and later public poll are one write and share this
    // guard. A restart clears the guard without undoing completed ENV work.
    updateVoiceEnvelope(voices_[static_cast<std::size_t>(envelopeCard)], parameters);
    updated = true;
}

void YouKnowEngine::updateVoicePortamento(
    Voice& voice, const EngineParameters& parameters) noexcept
{
    if (voice.rootMidi >= 0)
        voice.targetMidi = static_cast<float>(
            voice.rootMidi + parameters.keyTranspose);

    // The performance ADC selects one integer step shared by all six voices.
    // Read its current control once per glide pass rather than caching it at
    // note-on: turning PORTAMENTO off lands every word on its target on the
    // next pass. The memoized resolver retains the existing pot/ADC table law.
    voice.glideSemitonesPerScan = resolveGlideStepPerScan(
        portamentoTravelAdcFraction(parameters.portamento));

    if (voice.glideSemitonesPerScan > 0.0f)
    {
        const float distance = voice.targetMidi - voice.currentMidi;
        const float step = std::min(std::abs(distance), voice.glideSemitonesPerScan);
        voice.currentMidi += distance < 0.0f ? -step : step;
    }
    else
    {
        voice.currentMidi = voice.targetMidi;
    }
}

void YouKnowEngine::updatePortamentoBeforeConverterWrite(
    const ConverterWrite& write, const EngineParameters& parameters) noexcept
{
    if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
        return;

    if (write.destination != ConverterDestination::Sub
        || converterPassPortamentoUpdated_)
        return;

    // B-2 03E0..0406 advances all six unsigned 8.8 glide words, then writes
    // SUB at 0410; only after that does 041C start the six DCO transactions.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L635-L685
    // The following SUB write is a proven ordinal bound for completed work,
    // not an instruction timestamp for each earlier RAM store. An interrupt
    // during DCO output therefore retains every card's glide update, while
    // the next loop can advance all six again. Fractional SUB peeks and their
    // public polls share one guard. The ten extension slots keep their own
    // complete pass update and are never advanced by this physical-board loop.
    for (int card = 0; card < hardwareVoices; ++card)
        updateVoicePortamento(voices_[static_cast<std::size_t>(card)], parameters);
    converterPassPortamentoUpdated_ = true;
}

std::uint32_t YouKnowEngine::updateVoicePitch(
    Voice& voice, const EngineParameters& parameters) noexcept
{
    // --- Pitch ------------------------------------------------------------
    // Recomputed from the key rather than cached at note-on, so moving the
    // transpose control takes a held note with it.
    if (voice.rootMidi >= 0)
    {
        const int voiceMidi = voice.rootMidi + parameters.keyTranspose;
        voice.targetMidi = static_cast<float>(voiceMidi);
        // Transpose is part of the pitch byte delivered to the voice CPU, not
        // of the assigner's physical-key memory. A held-note transpose change
        // therefore becomes the CPU's new reset-comparison history too. If
        // this stayed at the note-on value, replaying the same resulting board
        // pitch after release would falsely restart the free-running DCO.
        // With either run bit set this is a legato pitch message and the DCO
        // stays free-running. During an ordinary release both bits are clear,
        // so the voice CPU applies its normal different-pitch reset rule.
        if (pitchChangeRequestsDcoReset(voice, voiceMidi))
            voice.dcoResetPending = true;
        voice.lastVoiceMidi = voiceMidi;
        voice.hasVoicePitchHistory = true;
    }

    // The six stored glide words were already advanced before SUB. A live
    // product transpose edit can update the target/reset history here, as it
    // did before, but its current pitch word remains that earlier snapshot
    // until the next glide pass. No late edit adds a second per-card step.

    const std::int32_t controlOffset = activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt
        ? static_cast<std::int32_t>(firmwareControlState_.ram[0x6f]
            + 256u * firmwareControlState_.ram[0x70]) - 0x1818
        : static_cast<std::int32_t>(masterTunePitchWordOffset(parameters.masterTuneCents))
            + dcoPitchBendWord_ + dcoLfoPitchWord_;

    const DcoPitchPair pitch = dcoPitchPair(aggregatePitchWord(
        static_cast<double>(voice.currentMidi), controlOffset));
    // Roland explicitly says DCO CV contains no RANGE data: RANGE changes the
    // timer clock and selects the ramp resistor, not this compensation hold.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=9
    // This is the unshifted 12-bit code the firmware writes for the same 8.8
    // pitch coordinate. The paired count is staged below by the converter
    // destination. A
    // running M82C53 count-only write leaves the active CE/period untouched
    // until the next OUT transition. The code reaches the integrator through
    // the hold capacitor's slew; code*active-count is the relative ramp charge
    // rather than a fabricated hertz proxy.
    voice.dcoCvTarget = static_cast<float>(pitch.cvCode);
    return pitch.divider;
}

void YouKnowEngine::updateVoiceVcfTarget(
    Voice& voice, const EngineParameters& parameters) noexcept
{
    voice.cutoffCountsTarget = voiceVcfTarget(voice, parameters);
}

float YouKnowEngine::voiceVcfTarget(
    const Voice& voice, const EngineParameters& parameters) const noexcept
{
    // --- Filter cutoff, summed in converter counts ------------------------
    float counts = vcfPanelCounts(parameters.cutoff);
    const float envelopeSign =
        parameters.envPolarity == EnvPolarity::Normal ? 1.0f : -1.0f;
    // The velocity extension rides here rather than on a curve of its own.
    // ENV into the VCF is the only path this instrument has from the envelope
    // to the cutoff, so scaling its amount by the same gain the amplifier
    // applies makes a quieter note one whose filter envelope opened less far
    // -- with no new law and no new constant. The panel byte is still the
    // byte the firmware stored; the extension multiplies what that byte asks
    // for, exactly as it multiplies the amplifier's own control.
    counts += envelopeSign * static_cast<float>(vcfEnvelopeCountsWord(
                  voice.envelope.level, storedControlByte(parameters.envDepth)))
            * velocityGain(parameters, voice);
    // The LFO term is the pass-held B-2 word, not a fraction of its maximum:
    // the doubled panel byte and the onset byte truncate to one depth byte
    // before the accumulator multiply, so panel byte 1 reaches 15 counts
    // where a proportional 4047/127 would give 32.
    counts += static_cast<float>(vcfLfoCountsWord_);
    // Likewise the bender: the assigner's command times the sensitivity ADC,
    // formed once per pass, so the filter is still inside the two-bin centre
    // dead zone where the old 255-step magnitude already added 16 counts.
    counts += static_cast<float>(vcfBendCountsWord_);
    counts += static_cast<float>(vcfKeyFollowCountsWord(
        static_cast<std::int32_t>(std::lround(voice.currentMidi * 256.0f)),
        storedControlByte(parameters.keyFollow)));
    // The firmware clamps the sum to its 14-bit accumulator -- so the digital
    // part of the control voltage can never ask for less than the law's base
    // frequency -- and hands the converter the top twelve bits, so it moves
    // in 4-count steps. The analogue trims and drift ride on top of this at
    // the audio grid, below the converter's own resolution, exactly where
    // the hardware's trimmers sit.
    counts = std::clamp(counts, 0.0f, vcfCountsCeiling);
    const float code = vcfDacCountStep * std::floor(counts / vcfDacCountStep);
    // Apply the retained effective cutoff boundary offsets after digital
    // quantization, so the hold slews toward that persistent target. The
    // nominal count conversion assigns the mid-scale boundary 23.31 cents
    // of excess; the later service slope and current limit affect the actual
    // frequency step. The approved serviced-card fit fixes its strength at 1;
    // the legacy profile retains its existing Unit Character scaling.
    return code + vcfConverterCarryCounts(code)
        * (parameters.useServiced439522VcfCalibration ? 1.0f : parameters.calibration);
}

void YouKnowEngine::updateVoiceVcaTarget(
    Voice& voice, const EngineParameters& parameters) noexcept
{
    voice.vcaControlTarget = voiceVcaTarget(voice, parameters);
}

float YouKnowEngine::voiceVcaTarget(
    const Voice& voice, const EngineParameters& parameters) const noexcept
{
    const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
    const float tolerance = parameters.calibration;
    const float envelope = voice.envelope.value;

    // --- Amplifier control ------------------------------------------------
    const float control = parameters.vcaMode == VcaMode::Envelope
                        ? envelope
                        : (voice.envelope.running ? 1.0f : 0.0f);
    return clamp01(
        control * velocityGain(parameters, voice)
        + card.vcaControlOffset * 0.004f * tolerance);
}

float YouKnowEngine::velocityGain(const EngineParameters& parameters,
                                     const Voice& voice) noexcept
{
    return 1.0f - parameters.velocityDepth * (1.0f - voice.velocity);
}

void YouKnowEngine::updateSharedScan(
    const EngineParameters& parameters) noexcept
{
    resonanceCvTarget_ = converterDacFraction(parameters.resonance);
    sharedVcaTarget_ = converterDacFraction(parameters.vcaLevel);
    subCvTarget_ = converterDacFraction(parameters.subLevel);
    noiseCvTarget_ = converterDacFraction(parameters.noiseLevel);

    // A pre-audio snapshot is the one exceptional direct prime. Once the scan
    // runs, this same code is formed beside advanceLfo and held for the PWM
    // destination just as B-2 holds FF4F.
    converterPassPwmDacCode_ = parameters.pulseEnabled
        ? pwmDacCode(parameters.pwmDepth, parameters.pwmSource,
                     lfoAccumulator_, lfoPolarity_ >= 0.0f)
        : 0u;
    pwmVoltsTarget_ = pwmDacVolts(converterPassPwmDacCode_);
}

void YouKnowEngine::performConverterWrite(
    const ConverterWrite& write, const EngineParameters& parameters,
    const float* passiveHoldTargetOverride) noexcept
{
    const bool traced = activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt;
    const auto targetFor = [&](float legacyTarget) {
        return passiveHoldTargetOverride != nullptr ? *passiveHoldTargetOverride
            : traced ? firmwareConverterTarget(write) : legacyTarget;
    };

#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(converterWrites, 1);
#endif
    const auto validPhysicalVoice = [&write] {
        return write.voice >= 0 && write.voice < hardwareVoices;
    };

    // A fractional enable has already advanced the physical capacitor in the
    // previous interval. Its later software target commit must not inhibit,
    // inject charge or reopen that same acquisition window a second time.
    if (envelopeHoldsConfigured_ && passiveHoldTargetOverride == nullptr)
        inhibitEnvelopeHold();

    // Do not add glitches to a target that this same write replaces. The
    // explicitly configured envelope comparison puts any supplied turn-off
    // charge on the independent 10 nF capacitor at inhibit. With no supplied
    // circuit configuration there is no injection or acquisition parameter;
    // the original installed parasitics remain unmeasured (OQ-07/08).
    switch (write.destination)
    {
        case ConverterDestination::Resonance:
            resonanceCvTarget_ = targetFor(converterDacFraction(parameters.resonance));
            break;
        case ConverterDestination::CommonVca:
            sharedVcaTarget_ = targetFor(converterDacFraction(parameters.vcaLevel));
            break;
        case ConverterDestination::Sub:
            subCvTarget_ = targetFor(converterDacFraction(parameters.subLevel));
            break;
        case ConverterDestination::Pitch:
            if (validPhysicalVoice())
            {
                auto& voice = voices_[static_cast<std::size_t>(write.voice)];
                if (voice.dcoPitchTransactionValid)
                {
                    voice.dcoCvTarget =
                        voice.dcoPitchTransactionCvTarget;
                    voice.dcoPitchTransactionValid = false;
                    voice.dcoPitchTransactionColdStart = false;
                }
                else
                {
                    // The first PhaseZeroDiagnostic T has no preceding
                    // modelled pass in which T-389 could exist. Keep that
                    // construction-only/direct-test fallback deterministic;
                    // every subsequent physical transaction is pre-staged.
                    const std::uint32_t count = updateVoicePitch(
                        voice, parameters);
                    programDcoCount(
                        voice, count, voice.dcoResetPending);
                    voice.dcoResetPending = false;
                }
            }
            break;
        case ConverterDestination::Pwm:
            // B-2 computes FF4F directly from the raw LFO accumulator and PWM
            // depth, independently of the onset byte used by DCO and VCF, then
            // writes that saved word on the following envelope loop.
            // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L900-L905
            // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1144-L1197
            if (passiveHoldTargetOverride != nullptr)
            {
                pwmVoltsTarget_ = *passiveHoldTargetOverride;
                break;
            }
            pwmVoltsTarget_ = targetFor(pwmDacVolts(converterPassPwmDacCode_));
            break;
        case ConverterDestination::Vcf:
            if (validPhysicalVoice())
            {
                auto& voice =
                    voices_[static_cast<std::size_t>(write.voice)];
                if (passiveHoldTargetOverride != nullptr)
                    voice.cutoffCountsTarget = *passiveHoldTargetOverride;
                else if (traced)
                    voice.cutoffCountsTarget = firmwareConverterTarget(write);
                else
                    updateVoiceVcfTarget(voice, parameters);
            }
            break;
        case ConverterDestination::VoiceVca:
            if (validPhysicalVoice())
            {
                auto& voice = voices_[static_cast<std::size_t>(write.voice)];
                if (passiveHoldTargetOverride != nullptr)
                    voice.vcaControlTarget = *passiveHoldTargetOverride;
                else if (traced)
                    voice.vcaControlTarget = firmwareConverterTarget(write);
                else
                    updateVoiceVcaTarget(voice, parameters);
                if (envelopeHoldsConfigured_ && passiveHoldTargetOverride == nullptr)
                    beginEnvelopeHoldAcquisition(write.voice, voice.vcaControlTarget);
            }
            break;
        case ConverterDestination::Noise:
            noiseCvTarget_ = activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt
                ? firmwareConverterTarget(write) : converterDacFraction(parameters.noiseLevel);
            break;
    }
}

void YouKnowEngine::inhibitEnvelopeHold() noexcept
{
    for (std::size_t index = 0; index < envelopeHolds_.size(); ++index)
        envelopeHolds_[index].inhibit(envelopeHoldConfiguration_[index]);
}

void YouKnowEngine::beginEnvelopeHoldAcquisition(int slot, float target) noexcept
{
    inhibitEnvelopeHold();
    if (slot >= 0 && slot < hardwareVoices)
        envelopeHolds_[static_cast<std::size_t>(slot)].select(
            VoiceVcaSignalLaw::holdStandoffVolts
            + static_cast<double>(target) * VoiceVcaControlLaw::controlFullScaleVolts);
}

double YouKnowEngine::envelopeMuxInhibitPhase(std::size_t ordinal) const noexcept
{
    if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
        return converterInhibitPhases_[ordinal];
    // All normal B-2 loadDac callers enable immediately after its RET.
    // 082F..083C: 20+7+10+4+10+4+10+10 = 75 nominal CPU states.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1295-L1304
    // Tools/AuditControlDacTiming.py independently audits the sequence and NEC
    // timing. These are instruction-start conventions, not measured PA edges.
    // Existing chart profiles retain their diagnostic enable anchors: adding
    // the known routine duration does NOT qualify them as execution traces.
    // The first inhibit lies before phase-zero RES enable in the old grids;
    // at bootstrap/restart performConverterWrite owns the boundary fallback.
    return converterEventPhases_[ordinal] - 75.0 * controlScanHz / voiceCpuStateHz;
}

void YouKnowEngine::advanceEnvelopeHolds(
    double seconds, const EngineParameters& parameters) noexcept
{
    if (!(seconds > 0.0))
        return;
    constexpr double span = VoiceVcaControlLaw::controlFullScaleVolts;
    constexpr double offset = VoiceVcaSignalLaw::holdStandoffVolts;
    for (std::size_t index = 0; index < envelopeHolds_.size(); ++index)
    {
        auto& hold = envelopeHolds_[index];
        auto& control = voices_[index].vcaControl;
        const auto volts = hold.trajectory(envelopeHoldConfiguration_[index]);
        const EnvelopeHoldCircuit::Trajectory input {
            (volts.constant - offset) / span, volts.exponential / span,
            volts.slope / span, volts.tau };
        if (!parameters.enableCoupledVoiceVcaControl)
        {
            control = input.throughOnePole(control, seconds, voiceVcaHoldSlewSeconds);
        }
        else
        {
            const auto& circuit = voiceVcaControlCircuit();
            // Resolve the upstream RC boundary layer independently of host
            // rate. After 16 time constants its residual is <1.13e-7 of the
            // initial step. Quarter-tau RK stages integrate that trajectory;
            // the remaining slow C58 interval uses eight stages as it does
            // near its nonlinear knee. The voltage itself remains exact.
            const double fast = std::abs(input.exponential) > 1.0e-12
                ? std::min(seconds, 16.0 * input.tau) : 0.0;
            if (fast > 0.0)
                control = circuit.advanceDriven(control, fast,
                    [&input](double t) { return input.at(t); },
                    std::max(1, static_cast<int>(std::ceil(4.0 * fast / input.tau))));
            const double rest = seconds - fast;
            if (rest > 0.0)
            {
                if (input.exponential == 0.0 && input.slope == 0.0)
                    control = circuit.advance(control, input.constant, rest);
                else
                    control = circuit.advanceDriven(control, rest,
                        [&input, fast](double t) { return input.at(t + fast); }, 8);
            }
        }
        hold.advance(volts, seconds);
    }
}

void YouKnowEngine::advanceEnvelopeHoldControls(
    double phase, double phaseStep, bool hasEnable, int slot, float target,
    double enablePosition, const EngineParameters& parameters) noexcept
{
    struct Edge { double position; bool enable; };
    std::array<Edge, converterWritesPerPass * 2 + 1> edges {};
    std::size_t count = 0;
    // Walk inhibit edges independently of enable peeks: a short internal
    // interval can contain the inhibit while the next enable is still later.
    // A serial restart abandons future edges, but leaves the capacitor and
    // selected channel alive until the replacement pass actually inhibits it.
    const auto appendInhibit = [&](double event) {
        if (event > phase + 1.0e-12 && event <= phase + phaseStep + 1.0e-12)
            edges[count++] = { std::clamp((event - phase) / phaseStep, 0.0, 1.0), false };
    };
    for (std::size_t ordinal = 0; ordinal < converterWritesPerPass; ++ordinal)
        appendInhibit(envelopeMuxInhibitPhase(ordinal));
    if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
    {
        // This profile's 32 kHz floor bounds one interval to 125 CPU states.
        // The next pass's first inhibit can be inside it (111 states after
        // entry without HOLD), although its first enable is still later.
        // HOLD moves those edges to 129/204 states instead of 111/186.
        // HOLD is frozen afresh at that pass's entry, so the old trace's first
        // offset cannot predict the new prefix after a pedal change. No other
        // next-pass inhibit can occur in this interval. Keep the actual pass
        // duration: the instruction trace is not a normalized 4.2 ms grid.
        appendInhibit(converterPassEndPhase_
            + firmwareFirstConverterInhibitStates(sustainPedalDown_)
                * controlScanHz / voiceCpuStateHz);
    }
    else
        for (std::size_t ordinal = 0; ordinal < converterWritesPerPass; ++ordinal)
            appendInhibit(converterPassEndPhase_ + envelopeMuxInhibitPhase(ordinal));
    if (hasEnable)
        edges[count++] = { enablePosition, true };
    std::sort(edges.begin(), edges.begin() + static_cast<std::ptrdiff_t>(count),
        [](const Edge& a, const Edge& b) {
            return a.position < b.position || (a.position == b.position && !a.enable && b.enable);
        });
    const double seconds = processingCoefficients_.internalIntervalSeconds;
    double previous = 0.0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto& edge = edges[index];
        advanceEnvelopeHolds((edge.position - previous) * seconds, parameters);
        if (edge.enable)
            beginEnvelopeHoldAcquisition(slot, target);
        else
            inhibitEnvelopeHold();
        previous = edge.position;
    }
    advanceEnvelopeHolds((1.0 - previous) * seconds, parameters);
}

bool YouKnowEngine::isPassiveHoldWrite(
    const ConverterWrite& write) noexcept
{
    switch (write.destination)
    {
        case ConverterDestination::Resonance:
        case ConverterDestination::CommonVca:
        case ConverterDestination::Sub:
        case ConverterDestination::Pwm:
            return true;
        case ConverterDestination::Vcf:
        case ConverterDestination::VoiceVca:
            return write.voice >= 0 && write.voice < hardwareVoices;
        case ConverterDestination::Pitch:
        case ConverterDestination::Noise:
            return false;
    }
    return false;
}

float YouKnowEngine::passiveHoldWriteTarget(
    const ConverterWrite& write,
    const EngineParameters& parameters) const noexcept
{
    if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
        return firmwareConverterTarget(write);

    switch (write.destination)
    {
        case ConverterDestination::Resonance:
            return converterDacFraction(parameters.resonance);
        case ConverterDestination::CommonVca:
            return converterDacFraction(parameters.vcaLevel);
        case ConverterDestination::Sub:
            return converterDacFraction(parameters.subLevel);
        case ConverterDestination::Pwm:
            return pwmDacVolts(converterPassPwmDacCode_);
        case ConverterDestination::Vcf:
            if (write.voice >= 0 && write.voice < hardwareVoices)
                return voiceVcfTarget(
                    voices_[static_cast<std::size_t>(write.voice)],
                    parameters);
            break;
        case ConverterDestination::VoiceVca:
            if (write.voice >= 0 && write.voice < hardwareVoices)
                return voiceVcaTarget(
                    voices_[static_cast<std::size_t>(write.voice)], parameters);
            break;
        case ConverterDestination::Pitch:
        case ConverterDestination::Noise:
            break;
    }
    return 0.0f;
}

void YouKnowEngine::scheduleUpcomingDcoPitchPrestages(
    double phase, double phasePerInternalSample) noexcept
{
    if (!(phasePerInternalSample > 0.0))
        return;

    constexpr std::size_t firstPitchOrdinal = 3u;
    const double cpuStatesPerInterval = voiceCpuStateHz / oversampledRate_;
    const double stateTolerance = std::max(
        1.0e-9, cpuStatesPerInterval * 1.0e-9);

    for (int slot = 0; slot < hardwareVoices; ++slot)
    {
        auto& voice = voices_[static_cast<std::size_t>(slot)];
        auto& dco = voice.dco;
        if (voice.dcoPitchTransactionValid
            || dco.pitWriteState != Dco::PitWriteState::idle)
            continue;

        const std::size_t ordinal = firstPitchOrdinal
                                  + static_cast<std::size_t>(slot);
        const bool remainsInCurrentPass =
            nextConverterWrite_ < converterWritesPerPass
            && ordinal >= nextConverterWrite_;
        const double nominalEventPhase = converterEventPhases_[ordinal]
                                       + (remainsInCurrentPass ? 0.0 : converterPassEndPhase_);

        // T is the converter event's actual left-boundary poll, rather than
        // the policy profile's ideal phase. Count those exact internal
        // boundaries ahead, then place the recovered preparation 389 CPU
        // states before T. Subtracting the converter poll's own aperture makes
        // a phase lying within 1e-12 of a boundary belong to that boundary,
        // exactly as the queue comparison does.
        const double intervalsToT = std::max(
            1.0, std::ceil(
                (nominalEventPhase - phase - 1.0e-12)
                / phasePerInternalSample));
        const double statesToPrestage =
            intervalsToT * cpuStatesPerInterval - dcoPitchPrestageStates;
        if (statesToPrestage < -stateTolerance
            || statesToPrestage > cpuStatesPerInterval + stateTolerance)
            continue;

        dco.pitWriteState = Dco::PitWriteState::awaitingPitchPrestage;
        dco.cpuStatesToWrite = std::clamp(
            statesToPrestage, 0.0, cpuStatesPerInterval);
    }
}

bool YouKnowEngine::latchUpcomingPassiveHoldEvent(
    double phase, double phasePerInternalSample,
    const EngineParameters& parameters) noexcept
{
    // prepare() clamps the host to at least 8 kHz. Even with HQ disabled this
    // is at most 5/168 of a pass, smaller than the normalized 1/23 ordinal
    // spacing, so a physical interval can contain no more than one converter
    // write. The complete firmware profile instead enforces a 32 kHz
    // internal floor. AuditFirmwareControlTrace proves a conservative minimum
    // of 216 states between enables, including NOISE→next RES; a 32 kHz
    // interval spans125 states. Thus it uses this same scalar latch safely.
    static_assert(5.0 / 168.0 < 1.0 / converterWritesPerPass);
    if (passiveHoldEventLatch_.valid || !(phasePerInternalSample > 0.0))
        return false;

    const auto& writes = converterWriteOrder();
    const double intervalEnd = phase + phasePerInternalSample;
    std::size_t ordinal = nextConverterWrite_;
    bool nextPass = false;
    double eventPhase = 0.0;
    if (ordinal < writes.size())
        eventPhase = converterEventPhases_[ordinal];
    else if (intervalEnd >= converterPassEndPhase_)
    {
        ordinal = 0u;
        nextPass = true;
        eventPhase = converterPassEndPhase_ + converterEventPhases_[ordinal];
    }
    else
        return false;

    // The normal boundary poll owns events at the left edge. Peeking is only
    // for the open/closed physical interval (phase, phase + delta].
    if (!(eventPhase > phase + 1.0e-12
          && eventPhase <= intervalEnd + 1.0e-12))
        return false;

    const auto& write = writes[ordinal];
    if (!isPassiveHoldWrite(write))
        return false;

    updatePortamentoBeforeConverterWrite(write, parameters);
    updateEnvelopeBeforeConverterWrite(write, parameters);
    passiveHoldEventLatch_.valid = true;
    passiveHoldEventLatch_.nextPass = nextPass;
    passiveHoldEventLatch_.ordinal = ordinal;
    passiveHoldEventLatch_.write = write;
    passiveHoldEventLatch_.target = passiveHoldWriteTarget(
        write, parameters);
    passiveHoldEventLatch_.eventPosition = std::clamp(
        (eventPhase - phase) / phasePerInternalSample, 0.0, 1.0);
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(passiveHoldFractionalEventPeeks, 1);
    if (write.destination == ConverterDestination::Resonance
        || write.destination == ConverterDestination::Vcf)
        YOUKNOW_COUNT_DOMAIN_WORK(vcfFractionalEventPeeks, 1);
#endif
    return true;
}

float YouKnowEngine::CircuitDerivedResonanceProfile::loopGain(
    float panelPosition) noexcept
{
    // Linear above the grounded-base stage's junction onset, zero below it,
    // sharing the voiced profile's amplitude-anchored endpoint. A nominal
    // card's full travel lands on maximumFeedback here; the caller applies
    // the per-card RES adjustment (VoiceCard::vcfServiceResonanceScale) that
    // puts a real card's full travel on the same 4.8 Vp-p.
    const float position = clamp01(panelPosition);
    const float active = std::max(0.0f, position - onsetTravel)
                       / (1.0f - onsetTravel);
    return VoicedResonanceCompatibilityProfile::maximumFeedback * active;
}

float YouKnowEngine::CircuitDerivedNoiseLevelProfile::drive(
    float dacFraction) noexcept
{
    // Tr22's collector current is linear in the hold voltage above the
    // junction drop plus R114's pull-down and zero below; normalised so the
    // full-travel level lands on the unchanged noiseMixVolts anchor.
    const float x = clamp01(dacFraction);
    return std::max(0.0f, x - onsetTravel) * spanReciprocal;
}

void YouKnowEngine::selectConverterTimingProfile(
    ConverterTimingProfile profile) noexcept
{
    converterTimingProfile_ = profile;
}

float YouKnowEngine::resonanceFeedbackFor(
    float resonanceCv, const VoiceCard& card, float calibration,
    bool circuitDerivedShape) noexcept
{
    // The regeneration control voltage is shared -- one converter output for
    // all six loops -- but each voice's loop amplifier has its own gain
    // spread.
    // Voiced, and back to being voiced. The service procedure trims each loop
    // to a 4.8 Vpp self-oscillation peak but states no tolerance on the result,
    // so what spread survives the adjustment is documented nowhere located. A
    // revision briefly anchored 5% to a source describing the *untrimmed*
    // component class, which is a different question: a trimmed mechanism's
    // residual is not its parts' tolerance.
    const float resonancePanel = clamp01(resonanceCv
        + card.resonanceError * 0.02f * calibration);
    const float loopGain = circuitDerivedShape
             ? CircuitDerivedResonanceProfile::loopGain(resonancePanel)
             : VoicedResonanceCompatibilityProfile::loopGain(resonancePanel);
    // The RES adjustment: the trimmer scales this card's whole return, so
    // its settled full-travel limit cycle is the procedure's 4.8 Vp-p
    // (refreshVoiceCardServiceTrims). Exactly 1 at Unit Character 0.
    return loopGain * card.vcfServiceResonanceScale;
}

float YouKnowEngine::cutoffAnalogCounts(
    float cutoffCounts, const VoiceCard& card, float calibration,
    float powerSupplyDroop) noexcept
{
    // Fixed FREQ/WIDTH trims absorb the line through the physical DAC
    // endpoints, while preserving its carry discontinuities between them.
    cutoffCounts = cutoffCounts * card.vcfServiceCvScale
        + card.vcfServiceCvOffset;
    // The analogue side of the cutoff chain: the two per-voice trimmers --
    // one scales the control voltage, one offsets it -- imperfectly set, and
    // the voiced residual wander, all riding below the converter's own
    // resolution on the slewed digital value. The final residual draws sit
    // within the service windows; their distribution remains voiced.
    // A sagging rail pulls the cutoff reference down with it, and the
    // rectifier ripple riding on the rail (advanceRailRipple) lifts and
    // lowers it 120 times a second through this same transfer. `calibration`
    // is applied here and only here: the droop state itself is a pure load
    // measure, so this mechanism scales linearly with Unit Character like its
    // eighteen siblings rather than quadratically.
    const float psuCutoffShift =
        -powerSupplyDroop * railToCutoffCountsPerVolt * calibration;
    // The trim residual is bounded by Roland's own printed acceptance
    // (p. 19 procedures 7/8: repeat "until within +/-10 cents" at both check
    // points): one +/-10-cent draw at the code-6272 FREQ point, one at the
    // WIDTH point two octaves up, the line through them elsewhere. The
    // former +/-0.07 octave and +/-5%-of-total-counts here were voiced with
    // no bounding source; a freshly calibrated card cannot legitimately
    // disperse past what the service procedure accepts at the points it
    // checks. Field drift beyond the windows is the separate `aging`
    // mechanism's, precomputed per card.
    const float trimSpanPosition =
        (cutoffCounts - vcfFreqTrimAnchorCounts) / vcfWidthTrimSpanCounts;
    const float trimResidualCounts =
        (card.cutoffOffsetError * (1.0f - trimSpanPosition)
         + card.cutoffScaleError * trimSpanPosition)
        * vcfTrimResidualOctaves * vcfCountsPerOctave * calibration;
    return cutoffCounts
        + trimResidualCounts
        + card.driftValue * 40.0f * calibration
        + psuCutoffShift
        + card.vcfServiceTrimCounts
        + card.agingCutoffCounts;
}

float YouKnowEngine::rampCurrentScaleFor(
    const VoiceCard& card, float calibration, DcoRange range) noexcept
{
    return card.dcoComponents.chargingScale(
        static_cast<std::size_t>(range), calibration);
}

void YouKnowEngine::refreshVoiceRampCurrentScales() noexcept
{
    for (auto& voice : voices_)
    {
        const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
        auto& dco = voice.dco;
        const float previous = voice.rampCurrentScale;
        const float current = rampCurrentScaleFor(
            card, activeParameters_.calibration, activeParameters_.range);
        voice.rampServiceScale = rampCurrentScaleFor(
            card, activeParameters_.calibration, DcoRange::Eight);
        if (current == previous)
        {
            if (dco.physicalResetActive)
            {
                const double oldSlope = dco.rampSlopePerSecond;
                refreshDcoResetTrajectory(voice);
                if (dco.saw.primed)
                    addDcoSlope(voice, (dco.rampSlopePerSecond - oldSlope)
                        * dco.renderScale * current / oversampledRate_, 1.0);
            }
            continue;
        }

        // These coordinates describe voltage, not charge itself. Reproject
        // them when the selected component scale changes so the physical
        // capacitor voltage remains continuous. Its NEW derivative follows
        // the new current/C; an existing linear reset retains its deadline.
        const double oldSlope = dco.rampSlopePerSecond * dco.renderScale * previous;
        dco.rampValue = (dco.rampValue + 1.0)
                      * static_cast<double>(previous) / current - 1.0;
        if (dco.resetSecondsRemaining > 0.0 && !dco.physicalResetActive)
            dco.rampSlopePerSecond *= static_cast<double>(previous) / current;
        voice.rampCurrentScale = current;
        if (dco.physicalResetActive)
            refreshDcoResetTrajectory(voice);
        if (dco.positiveRailHeld)
        {
            dco.rampValue = dcoPositiveBaseRail(
                static_cast<double>(dco.renderScale) * current);
            dco.rampSlopePerSecond = 0.0;
        }
        const double newSlope = dco.rampSlopePerSecond * dco.renderScale * current;
        if (dco.saw.primed && !voice.freewheeling && oldSlope != newSlope)
            addDcoSlope(voice, dcoCorrectionSlope((newSlope - oldSlope)
                                               / oversampledRate_), 1.0f);
    }
}

void YouKnowEngine::updateVoiceAudio(Voice& voice,
                                        const EngineParameters& parameters) noexcept
{
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(voiceAudioUpdates, 1);
#endif
    const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
    const float tolerance = parameters.calibration;

    voice.feedback = resonanceFeedbackFor(
        resonanceCv_, card, tolerance,
        parameters.useCircuitDerivedResonanceShape);
    voice.inputCompensation =
        VoicedResonanceCompatibilityProfile::inputCompensation(
            voice.feedback, parameters.resonanceCompensationShape);
    // The differential form carries the same coefficient inside the pair's
    // own tanh instead of ahead of it, so the cascade needs c rather than
    // 1 + c*k. Zero leaves the cascade arithmetic bit-identical to the split.
    voice.filter.inputCompensationCoefficient =
        parameters.enableDifferentialResonanceInput
            ? VoicedResonanceCompatibilityProfile::compensationCoefficient(
                  parameters.resonanceCompensationShape)
            : 0.0f;
    voice.filter.resonanceHeadroomFollowsStage =
        parameters.enableResonanceHeadroomTemperature;

    // The rail the cutoff reference reads: the load's sag less whatever the
    // rectifier ripple adds this sample (a rail above nominal is a negative
    // droop).
    const float analogCounts = cutoffAnalogCounts(
        voice.cutoffCounts, card, tolerance,
        powerSupplyDroop_ - railRippleVolts_);
    const float calibrationFeedback = parameters.useFixedVcfServiceFrequencyTrim
        ? resonanceFeedbackFor(1.0f, card, tolerance,
            parameters.useCircuitDerivedResonanceShape)
        : voice.feedback;
    // The chain from counts to the physical omega*dt interval costs an exp2
    // and two double pow calls per card, per internal sample -- and it is a
    // pure function of the two values compared here. A card whose hold has
    // settled and whose drift step has not landed presents bit-identical
    // inputs for thousands of samples in a row, which is most of what an idle
    // instrument does. The guard is exact equality, so the cache can only
    // return the value the chain would have recomputed.
    if (analogCounts != voice.cutoffChainCounts
        || calibrationFeedback != voice.cutoffChainFeedback)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(cutoffMemoMisses, 1);
#endif
        const float cutoffHz = vcfEffectiveCutoffHz(analogCounts, calibrationFeedback,
            parameters.useServiced439522VcfCalibration ? voice.cardIndex : -1);
        const float limited =
            std::min(cutoffHz, static_cast<float>(oversampledRate_) * 0.45f);
        voice.filterOmegaStep = twoPi * limited * inverseOversampledRate_;
        voice.cutoffChainCounts = analogCounts;
        voice.cutoffChainFeedback = calibrationFeedback;
    }
#if defined(YOUKNOW_WORK_AUDIT)
    else
    {
        YOUKNOW_COUNT_DOMAIN_WORK(cutoffMemoHits, 1);
    }
#endif

    // The retire check below the main scan loop asks this same law about
    // this same vcaControl a moment later, to see whether the card has
    // actually gone silent; cache the raw gain so it reads this value
    // instead of paying for another lookup.
    const auto vcaControl = static_cast<float>(voice.vcaControl);
    voice.vcaGain = parameters.useSoftplusVoiceVcaCompatibilityLaw
                        ? VoiceVcaControlLaw::softplusGain(vcaControl)
                        : VoiceVcaControlLaw::gain(vcaControl);
    voice.vca = voice.vcaGain;
    // The card's VCA GAIN spread is VR27's setting, and VR27 sits on the
    // input. Keeping it here rather than on the output leaves the small-signal
    // product identical and lets a hot card drive its own pair harder.
    voice.vcaInputTrim = 1.0f + card.vcaGainError * 0.03f * tolerance;

    // The IR3109 stage offsets used to be rewritten here, every audio sample,
    // from values that never change. They now live in
    // refreshVoiceCardStageTrims, called where the card or the panel moves.
}

void YouKnowEngine::updatePulseComparator(
    Voice& voice, const EngineParameters& parameters) noexcept
{
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(pulseComparatorUpdates, 1);
#endif
    const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
    // The event walk compares this threshold with retained capacitor voltage.
    // Held CV controls its derivative; changing current does not rescale the
    // voltage already integrated earlier in the cycle.
    const float cardCurrent = voice.rampServiceScale;
    // ADJUSTMENT s. 10 (p. 19) is a joint window: the one shared VR31 puts
    // CH1 at exactly 50 % with PWM at 5, and every other card is accepted
    // within 48-52 % as it stands, ramp error and comparator error together.
    // So CH1's net residual at the trim point is zero and the others' is a
    // draw inside +/-2 points; the comparator offset is whatever threshold
    // shift leaves that net after this card's own ramp error -- at
    // calibration 0 both vanish and the threshold is the shared hold alone.
    // An earlier revision drew +/-2 points on the threshold and +/-3 % on the
    // ramp independently, which put some cards outside the window Roland
    // ships them inside. Pulse Off remains separate at -0.8 V and pins the
    // comparator high even while this card's VCA is shut.
    // At the second service point, D(.6V) = D(6V) + .45/serviceScale.
    // Draw inside the intersection of BOTH acceptance windows. The bounded
    // deterministic population is a product prior, not measured statistics.
    const float halfWidth = pwmDutyAcceptanceHalfWidth * parameters.calibration;
    const float lowerResidual = std::max(-halfWidth, 0.45f - halfWidth - 0.45f / cardCurrent);
    const float upperResidual = std::min(halfWidth, 0.45f + halfWidth - 0.45f / cardCurrent);
    const float netDuty = voice.cardIndex == 0 ? 0.0f
        : lowerResidual + (0.5f + 0.5f * card.comparatorOffset)
                            * (upperResidual - lowerResidual);
    // duty = 1 - V_th / (12 V * scale) at the 6 V hold, so the threshold
    // that lands 0.5 + netDuty is 6 V * scale * (1 - 2 netDuty).
    const float thresholdOffset =
        0.5f * rampAmplitudeVolts * (cardCurrent * (1.0f - 2.0f * netDuty) - 1.0f);
    const float threshold = static_cast<float>(pwmVolts_) + thresholdOffset;
    voice.pulseThresholdVolts = sanitised(threshold, 6.0f);
    voice.pulsePinnedHigh = voice.pulseThresholdVolts < 0.0f;
    // The event walk crosses this threshold as it stands, so the duty it
    // reports has to be the one the walk solves. pwmDutyCycle's +6 V / 50 %
    // floor is the shared hold's; on a card whose offset lifts the threshold
    // above 6 V (6.12 V at rampCurrentScale 1.02) clamping the sum back to
    // 6 V reported 0.5098 where the render holds exactly 0.5, and
    // pulseWaveNodeMean then primed C56/C50 and drove the freewheel mean with
    // 0.118 V of DC the rendered comparator never carries -- a false C56 step
    // at note-on and on resume. The physical-mean helper therefore uses this
    // threshold directly, including a card offset above the shared 6 V hold.
    voice.pulseDuty = steadyDcoPulseDuty(voice);
}

YouKnowEngine::SteadyDcoCycle YouKnowEngine::steadyDcoCycle(
    const Voice& voice) const noexcept
{
    // Roland's DCO description (p.9) separates fixed-CV charging through the
    // RANGE resistor from timer-edge discharge and comparator threshold:
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=9
    // This is the settled mean of our existing finite-linear reset/+15 V
    // compatibility model, not a measured MC5534 reset shape. Count/CV writes
    // still use the physical event walk, never this periodic approximation.
    // A stopped/construction cell has no periodic waveform. Its default
    // sample-count scratch value is not an oscillator period. Prime the
    // virtual powered mean with the same coherent 0x5400 reference pair as
    // the current normalization, rather than interpreting that scratch value
    // as a tiny ramp and charging C56 from a fabricated near-DC step. This
    // construction policy does not alter any PIT/CV write or running charge.
    const bool construction = voice.dco.pitState == Dco::PitState::stopped;
    const double nominalPeriod = construction
        ? 7675.0 / rangeClockHz(activeParameters_.range)
        : std::max(voice.dco.periodSamples / oversampledRate_, 1.0e-12);
    const double reset = static_cast<double>(resetFraction(nominalPeriod))
                       * nominalPeriod;
    const double period = nominalPeriod / dcoMasterClockRatio_;
    const double slope = 0.5 * static_cast<double>(rampAmplitudeVolts)
        * dcoChargingSlope(construction ? 256.0f : voice.dcoCv, activeParameters_.range)
        * voice.rampCurrentScale;
    if (dcoResetCircuitEnabled_)
    {
        const double gate = std::min(dcoResetCalibration_.gateSeconds, period);
        const auto& parts = cards_[static_cast<std::size_t>(voice.cardIndex)].dcoComponents;
        const double tau = dcoResetCalibration_.dischargeOhms
                         * parts.capacitance(activeParameters_.calibration);
        const double target = dcoResetCalibration_.clampVolts + slope * tau;
        const double loss = -std::expm1(-gate / tau);
        // Periodic fixed point of discharge followed by constant-current
        // charge. Gate overlap means continuously active reset. The supply
        // bounds both the charging plateau and a high reset asymptote.
        const double peak = std::min(15.0, target + slope * (period - gate) / loss);
        const double trough = std::min(15.0,
            DcoResetCircuit::voltage(peak, target, tau, gate));
        return { period, gate, slope, peak, trough, target, tau };
    }
    return { period, reset, slope,
             std::min(15.0, slope * (period - reset)) };
}

float YouKnowEngine::steadyDcoPulseDuty(const Voice& voice) const noexcept
{
    const float threshold = voice.pulseThresholdVolts;
    if (threshold < 0.0f)
        return 1.0f;
    const auto cycle = steadyDcoCycle(voice);
    if (!(cycle.peakVolts > 0.0))
        return threshold <= 0.0f ? 1.0f : 0.0f;
    if (threshold > cycle.peakVolts)
        return 0.0f;
    if (dcoResetCircuitEnabled_)
    {
        const double rise = cycle.periodSeconds - cycle.resetSeconds;
        const double highRise = cycle.slopeVoltsPerSecond > 0.0
            ? std::clamp(rise - (threshold - cycle.troughVolts)
                                    / cycle.slopeVoltsPerSecond, 0.0, rise)
            : (threshold <= cycle.troughVolts ? rise : 0.0);
        double highReset = cycle.resetSeconds;
        if (threshold > cycle.troughVolts)
            highReset = std::clamp(cycle.resetTauSeconds * std::log(
                (cycle.peakVolts - cycle.resetTargetVolts)
                / (threshold - cycle.resetTargetVolts)), 0.0, cycle.resetSeconds);
        return static_cast<float>((highRise + highReset) / cycle.periodSeconds);
    }
    const double highSeconds = std::max(0.0, cycle.periodSeconds
        - cycle.resetSeconds - threshold / cycle.slopeVoltsPerSecond)
        + cycle.resetSeconds * (1.0 - threshold / cycle.peakVolts);
    return static_cast<float>(std::clamp(
        highSeconds / cycle.periodSeconds, 0.0, 1.0));
}

float YouKnowEngine::steadyDcoSawMean(const Voice& voice) const noexcept
{
    const auto cycle = steadyDcoCycle(voice);
    if (dcoResetCircuitEnabled_)
    {
        const double rise = cycle.periodSeconds - cycle.resetSeconds;
        const double charging = cycle.slopeVoltsPerSecond > 0.0
            ? std::clamp((cycle.peakVolts - cycle.troughVolts)
                            / cycle.slopeVoltsPerSecond, 0.0, rise) : 0.0;
        const double riseArea = 0.5 * (cycle.troughVolts + cycle.peakVolts) * charging
                              + cycle.peakVolts * (rise - charging);
        const double resetArea = cycle.troughVolts == cycle.peakVolts
            ? cycle.peakVolts * cycle.resetSeconds
            : DcoResetCircuit::integral(cycle.peakVolts, cycle.resetTargetVolts,
                                       cycle.resetTauSeconds, cycle.resetSeconds);
        return static_cast<float>(sawMixVolts
            * ((riseArea + resetArea) / (cycle.periodSeconds * 6.0) - 1.0));
    }
    if (!(cycle.slopeVoltsPerSecond > 0.0))
        return -sawMixVolts;
    const double riseSeconds = cycle.peakVolts / cycle.slopeVoltsPerSecond;
    // Triangle rise + finite linear fall + any supply-held plateau.
    const double meanVolts = cycle.peakVolts * (1.0
        - 0.5 * (riseSeconds + cycle.resetSeconds) / cycle.periodSeconds);
    return static_cast<float>(sawMixVolts
        * (meanVolts / (0.5 * rampAmplitudeVolts) - 1.0));
}

void YouKnowEngine::primeStartupVoiceWaveNodes(
    const EngineParameters& parameters) noexcept
{
    // A restored pre-audio snapshot describes a powered, already-settled
    // instrument. Prime the WAVE-node coupling capacitor at the periodic
    // source's DC mean so Pulse Off's documented constant-high comparator does
    // not become a fabricated power-on thump on the first note. The saw's
    // finite charge time can move its mean; sub is a half-wave current whose
    // mean equals its AC amplitude; pulse mean is level * (2*duty - 1), including +level
    // for the pinned-high off state.
    for (auto& voice : voices_)
        primeVoiceWaveNode(voice, parameters);
}

double YouKnowEngine::dcoChargingSlope(float heldCode, DcoRange range) noexcept
{
    // Roland p.9/p.13: dV/dt = -Vheld/(Rrange*C54), independent of PIT
    // count and clock. Use ONE 8' B-2 reference (0x5400: code256,count7675)
    // for the established approximately 12 V excursion. This fixes model
    // gain, not an unmeasured MC5534A volts-per-DAC-code specification.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=9
    constexpr double referenceRise = 7675.0 / 2000000.0
                                   - static_cast<double>(rampResetSeconds);
    const double code = std::isfinite(heldCode)
        ? std::clamp(static_cast<double>(heldCode), 0.0, 4095.0) : 0.0;
    return (2.0 / referenceRise) * (code / 256.0)
         * (200000.0 / dcoChargingResistance(range));
}

float YouKnowEngine::dcoLaunchScale(const Voice& voice) const noexcept
{
    const double period = std::max(
        voice.dco.periodSamples / oversampledRate_, 1.0e-12);
    const double reset = static_cast<double>(resetFraction(period)) * period;
    // Coordinate choice only: the nominal steady peak of the currently held
    // CV. No target or pending transaction may anticipate its converter write.
    return static_cast<float>(0.5 * dcoChargingSlope(
        voice.dcoCv, activeParameters_.range) * (period - reset));
}

void YouKnowEngine::updateDcoHeldCv(Voice& voice, float code) noexcept
{
    if (voice.dcoCv == code)
        return;
    voice.dcoCv = code;
    auto& dco = voice.dco;
    if (dco.physicalResetActive)
    {
        const double oldSlope = dco.rampSlopePerSecond;
        refreshDcoResetTrajectory(voice);
        if (dco.saw.primed)
            addDcoSlope(voice, (dco.rampSlopePerSecond - oldSlope)
                * dco.renderScale * voice.rampCurrentScale / oversampledRate_, 1.0);
        return;
    }
    if (dco.resetSecondsRemaining > 0.0 || dco.positiveRailHeld
        || dco.pitState == Dco::PitState::stopped)
        return;
    const double oldSlope = dco.rampSlopePerSecond;
    dco.rampSlopePerSecond = dcoChargingSlope(code, activeParameters_.range)
                          / static_cast<double>(dco.renderScale);
    // T remains the existing DCO converter boundary poll. Future captured
    // transaction data cannot affect the preceding capacitor trajectory.
    if (dco.saw.primed)
        addDcoSlope(voice, dcoCorrectionSlope(
            (dco.rampSlopePerSecond - oldSlope) * dco.renderScale
            * voice.rampCurrentScale / oversampledRate_), 1.0f);
}

void YouKnowEngine::refreshDcoResetTrajectory(Voice& voice) noexcept
{
    auto& dco = voice.dco;
    const auto& parts = cards_[static_cast<std::size_t>(voice.cardIndex)].dcoComponents;
    dco.resetTimeConstant = dcoResetCalibration_.dischargeOhms
                         * parts.capacitance(activeParameters_.calibration);
    const double voltsPerCoordinate = 0.5 * rampAmplitudeVolts
                                    * dco.renderScale * voice.rampCurrentScale;
    const double slopeVolts = 0.5 * rampAmplitudeVolts
        * dcoChargingSlope(voice.dcoCv, activeParameters_.range) * voice.rampCurrentScale;
    const double target = dcoResetCalibration_.clampVolts
                        + slopeVolts * dco.resetTimeConstant;
    dco.resetTargetValue = target / voltsPerCoordinate - 1.0;
    dco.positiveRailHeld = target >= dcoPositiveRailVolts
        && (dco.rampValue + 1.0) * voltsPerCoordinate >= dcoPositiveRailVolts - 1e-12;
    dco.rampSlopePerSecond = dco.positiveRailHeld ? 0.0
        : (dco.resetTargetValue - dco.rampValue) / dco.resetTimeConstant;
}

double YouKnowEngine::dcoCorrectionSlope(double slope) const noexcept
{
    // Preserve shipping float rounding while the physical path accumulates
    // sharp onset/curvature cancellation without a float precision loss.
    return dcoResetCircuitEnabled_ ? slope : static_cast<double>(static_cast<float>(slope));
}

void YouKnowEngine::addDcoSlope(Voice& voice, double slopeStep, double samplesAgo) noexcept
{
    auto& dco = voice.dco;
    if (!dcoResetCircuitEnabled_)
    {
        addSlope(dco.saw, static_cast<float>(slopeStep), static_cast<float>(samplesAgo));
        return;
    }
    if (voice.freewheeling || slopeStep == 0.0)
        return;
    const auto& table = correctionTables().slopeResidual;
    const double offset = std::clamp(samplesAgo, 0.0, 1.0);
    int slot = dco.saw.base;
    for (int j = 0; j < correctionRing; ++j)
    {
        const double position = (j + offset) * correctionOversample;
        const int lower = std::clamp(static_cast<int>(position), 0, correctionTableLength - 2);
        const double fraction = std::clamp(position - lower, 0.0, 1.0);
        const double a = table[static_cast<std::size_t>(lower)];
        const double residual = a + (table[static_cast<std::size_t>(lower + 1)] - a) * fraction;
        dco.resetSawCorrection[static_cast<std::size_t>(slot)] += slopeStep * residual;
        slot = slot + 1 < correctionRing ? slot + 1 : 0;
    }
}

void YouKnowEngine::addDcoResetCurvature(
    Voice& voice, double slopeAtStart, double elapsed, double seconds) noexcept
{
    auto& dco = voice.dco;
    if (!dco.saw.primed || slopeAtStart == 0.0 || voice.freewheeling)
        return;
    const double interval = 1.0 / oversampledRate_;
    const double cell = interval / correctionOversample;
    double consumed = 0.0;
    double slope = slopeAtStart * dco.renderScale * voice.rampCurrentScale;
    // Each correction-table cell is linear in event time. Its exact weighted
    // exponential centroid and total derivative change integrate that cell;
    // this repairs smooth reset curvature as well as the two slope corners.
    for (int piece = 0; piece <= correctionOversample && consumed < seconds; ++piece)
    {
        const double t = elapsed + consumed;
        const double nextCell = (std::floor(t / cell + 1e-10) + 1.0) * cell;
        const double dt = std::min(seconds - consumed, nextCell - t);
        if (!(dt > 0.0))
            break;
        const double change = slope * std::expm1(-dt / dco.resetTimeConstant);
        const double centroid = DcoResetCircuit::curvatureCentroid(dco.resetTimeConstant, dt);
        addDcoSlope(voice, change * interval, (interval - t - centroid) / interval);
        slope += change;
        consumed += dt;
    }
}

bool YouKnowEngine::pulseMixEnabled(
    bool requested, float duty, bool couplePinnedLevel) noexcept
{
    return couplePinnedLevel || (requested && duty < 1.0f);
}

float YouKnowEngine::pulseWaveNodeMean(
    const Voice& voice, const EngineParameters& parameters) noexcept
{
    return pulseMixEnabled(parameters.pulseEnabled, voice.pulseDuty,
                           parameters.enablePulseOffWaveNodeCoupling)
        ? pulseMixVolts * (2.0f * voice.pulseDuty - 1.0f)
        : 0.0f;
}

float YouKnowEngine::subWaveNodeMean(
    const Voice& voice, const EngineParameters& parameters) const noexcept
{
    // Module p. 13: Tr19's collector node feeds R101 27k -> D6 (cathode on
    // the pin-14 WAVE line) and R102 33k back to the SUB LEVEL rail, so the
    // rail's current enters the node on the half-cycle Tr19 is off and the
    // unipolar square's mean equals its AC amplitude -- the same subGain
    // prepareVoiceFilter applies. Zero with the switch off (bipolar square).
    if (!parameters.enableSubHalfWaveNodeCoupling)
        return 0.0f;
    const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
    const float subCurrent = parameters.enableSubDiodeControl
        ? SubLevelDiodeLaw::gain(subCv_) : static_cast<float>(subCv_);
    return subMixVolts * subCurrent
         * (1.0f + card.subLevelError * 0.03f * parameters.calibration);
}

void YouKnowEngine::primeVoiceWaveNode(
    Voice& voice, const EngineParameters& parameters) noexcept
{
    updatePulseComparator(voice, parameters);
    auto& dco = voice.dco;
    const double totalScale = static_cast<double>(dco.renderScale)
                            * static_cast<double>(voice.rampCurrentScale);
    const double rampVolts = 0.5 * static_cast<double>(rampAmplitudeVolts)
                           * totalScale * (dco.rampValue + 1.0);
    dco.pulseState = voice.pulsePinnedHigh
                  || rampVolts >= voice.pulseThresholdVolts
                   ? 1.0f : -1.0f;
    dco.pulse.reset();
    voice.previousPulseThresholdVolts = voice.pulseThresholdVolts;
    voice.previousPulsePinnedHigh = voice.pulsePinnedHigh;
    voice.pulseThresholdPrimed = true;
    voice.moduleCoupling.state = static_cast<double>(oscillatorLevelScale_
        * (pulseWaveNodeMean(voice, parameters)
           + subWaveNodeMean(voice, parameters)
           + (parameters.sawEnabled ? steadyDcoSawMean(voice) : 0.0f)));
    if (coupledMixerEnabled_)
    {
        const auto& c = coupledMixerCalibration_;
        // Same settled-mean startup policy as the compatibility network.
        // This is not a claim about power-on charge or the nonlinear periodic
        // mean; the audit allows settling before measuring steady windows.
        voice.coupledMixer.prime(c,
            c.sourceBiasVolts + c.sourceScale * (pulseWaveNodeMean(voice, parameters)
                + (parameters.sawEnabled ? steadyDcoSawMean(voice) : 0.0f)),
            CoupledSubMixer::railFullScaleVolts * subCv_, 0.5);
    }
}

// ---------------------------------------------------------------------------
// Voice rendering
// ---------------------------------------------------------------------------

void YouKnowEngine::advanceThermalWarmup() noexcept
{
    // Wall-clock seconds, not internal samples: the chassis warms at the same
    // rate whatever grid the model is solved on. The total is double so the
    // increment cannot round away against it -- see the member's declaration.
    thermalWarmupSeconds_ += inverseOversampledRate_;
    // The warm-up fraction is one chassis-wide number, advanced beside the
    // timer it derives from. Six voices asking six times per internal sample
    // for the same exponential of the same elapsed time is the same answer at
    // six times the price.
    thermalWarmupFraction_ =
        thermalStartsSettled_ ? 1.0f
            : 1.0f - std::exp(-static_cast<float>(thermalWarmupSeconds_)
                / static_cast<float>(thermalWarmupTimeConstantSeconds));
}

float YouKnowEngine::railRipplePeakVolts() noexcept
{
    const float sawtoothPeakToPeak = railSecondaryRatedAmps
        / (2.0f * mainsFrequencyHz * railReservoirFarads);
    const float fundamentalPeak = sawtoothPeakToPeak / (0.5f * twoPi);
    return fundamentalPeak
        * std::pow(10.0f, -railRegulatorRippleRejectionDb / 20.0f);
}

void YouKnowEngine::advanceRailRipple(
    const EngineParameters& parameters) noexcept
{
    if (!parameters.enableRailRipple)
    {
        railRippleVolts_ = 0.0f;
        return;
    }
    // Wall-clock phase, like the warm-up timer: the same 120 Hz whatever
    // grid the model is solved on.
    railRipplePhase_ += static_cast<double>(twoPi) * 2.0
        * static_cast<double>(mainsFrequencyHz)
        * static_cast<double>(inverseOversampledRate_);
    if (railRipplePhase_ >= static_cast<double>(twoPi))
        railRipplePhase_ -= static_cast<double>(twoPi);
    railRippleVolts_ = railRipplePeakVolts_
        * static_cast<float>(std::sin(railRipplePhase_));
}

float YouKnowEngine::converterHoldDroopVoltsPerSecond(
    float followerBiasAmps, float boardCelsius) noexcept
{
    const float bias = followerBiasAmps
        * std::exp2((boardCelsius - 25.0f) / fetBiasDoublingCelsius);
    return (hd14051OffLeakageAmps + bias) / converterHoldFarads;
}

void YouKnowEngine::applyConverterHoldDroop(
    const EngineParameters& parameters, float seconds) noexcept
{
    if (!parameters.enableConverterHoldDroop)
        return;
    // The muxes and their followers share the module board with the cards,
    // so the chassis rise the thermometer reports stands for their
    // temperature; the few degrees of gradient between cards are a few
    // percent of a ramp that is itself a hundredth of an LSB.
    const float boardCelsius =
        25.0f + 15.0f * parameters.calibration * thermalWarmupFraction_;
    // IC24 (DCO CV, SUB) sits behind TL08x followers; IC23 (VCF CV, PWM,
    // VCA LEVEL) and IC26 (ENV, RES, NOISE) behind TL064s.
    const float dcoBranchVolts = seconds
        * converterHoldDroopVoltsPerSecond(tl08xInputBiasAmps, boardCelsius);
    const float restVolts = seconds
        * converterHoldDroopVoltsPerSecond(tl064InputBiasAmps, boardCelsius);
    // IC28a's branch feeds IC24 and IC23 in DAC codes (a VCF count is a
    // quarter of one); IC27b's feeds IC26, whose RES and NOISE holds are
    // read as a fraction of the stored maximum code and whose ENV holds as
    // a fraction of the envelope's code-4095 span.
    const float dcoBranchCodes = dcoBranchVolts / invertingBranchLsbVolts;
    const float restCodes = restVolts / invertingBranchLsbVolts;
    const float ic26Fraction = restVolts
        / CircuitDerivedResonanceProfile::controlFullScaleVolts;
    const float envelopeFraction = restVolts
        / VoiceVcaControlLaw::controlFullScaleVolts;
    for (int slot = 0; slot < hardwareVoices; ++slot)
    {
        auto& voice = voices_[static_cast<std::size_t>(slot)];
        voice.dcoCvTarget -= dcoBranchCodes;
        voice.cutoffCountsTarget -= 4.0f * restCodes;
        // The explicit envelope-hold circuit carries its own configured
        // bias and leakage when a harness configures it.
        if (!envelopeHoldsConfigured_)
            voice.vcaControlTarget -= envelopeFraction;
    }
    subCvTarget_ -= dcoBranchCodes / 4064.0f;
    sharedVcaTarget_ -= restCodes / 4064.0f;
    pwmVoltsTarget_ -= restVolts;
    noiseCvTarget_ -= ic26Fraction;
    // The RES hold has no post-hold network: its state is the hold itself
    // and only a write moves it, so the ramp moves both.
    resonanceCvTarget_ -= ic26Fraction;
    resonanceCv_ -= ic26Fraction;
}

float YouKnowEngine::voiceCardCelsius(
    const EngineParameters& parameters, int cardIndex,
    float warmupFraction) noexcept
{
    const float psuThermalOffset = parameters.enableSpatialThermalGradient
        ? chassisGradientCelsius(cardIndex) * parameters.calibration
        : 0.0f;
    const float tempRise = 15.0f * parameters.calibration;
    // Both rises are the supply's heat: at power-on every card is at ambient
    // and the gradient, like the common rise, develops on the warm-up clock.
    // The service trims read this at the settled fraction, so a cold card
    // is mis-trimmed by the gradient it has not yet acquired, as a serviced
    // instrument is.
    return 25.0f + (psuThermalOffset + tempRise) * warmupFraction;
}

float YouKnowEngine::dynamicOtaHeadroomVolts(
    const EngineParameters& parameters, int cardIndex) const noexcept
{
    const float tempC = voiceCardCelsius(
        parameters, cardIndex, thermalWarmupFraction_);
    const float dynamicThermalVoltage = 0.026f * ((tempC + 273.15f) / 298.15f);
    return 2.0f * dynamicThermalVoltage / stageAttenuation;
}

float YouKnowEngine::voiceVcaThermalDriveScale(
    const EngineParameters& parameters, int cardIndex) const noexcept
{
    if (!parameters.enableVoiceVcaTemperature)
        return 1.0f;
    // The same local temperature drives the filter and VCA. Fix the service
    // reference at this card's settled temperature, including its spatial
    // offset. Never re-trim against the running temperature. Identical Kelvin
    // expressions make Character 0 and a settled start exactly unity.
    const float referenceKelvin =
        voiceCardCelsius(parameters, cardIndex, 1.0f) + 273.15f;
    const float actualKelvin =
        voiceCardCelsius(parameters, cardIndex, thermalWarmupFraction_) + 273.15f;
    return referenceKelvin / actualKelvin;
}

float YouKnowEngine::jackBoardCelsius(
    const EngineParameters& parameters) const noexcept
{
    // The 15 C rise the cards read, scaled by Unit Character as above, so
    // Character 0 holds the part at NEC's 25 C condition for the whole
    // session; no gradient term, because the jack board is not a card.
    return 25.0f + 15.0f * parameters.calibration * thermalWarmupFraction_;
}

void YouKnowEngine::refreshJackBoardTemperature(
    const EngineParameters& parameters) noexcept
{
    jackBoardCelsius_ = jackBoardCelsius(parameters);
    // Johnson's law on the output stage's resistor floors, the same
    // sqrt(T / 298.15 K) the cards' floors take; exactly 1 at 25 C. The
    // volume wiper sits on the panel board, whose own temperature is not
    // measured, so it reads the one chassis warm-up the model has, like
    // every part off the cards. IC5's data-sheet floor is a stated 25 C
    // band figure with no temperature coefficient, and is left at it.
    jackBoardJohnsonScale_ = std::sqrt(
        (jackBoardCelsius_ + 273.15f) / outputNoiseTemperatureKelvin);
}

void YouKnowEngine::advanceDcoPitAndRamp(
    Voice& voice, DcoRange range, float previousThresholdVolts,
    float thresholdVolts, bool previousPinnedHigh, bool pinnedHigh,
    bool addCorrections) noexcept
{
    auto& dco = voice.dco;
    const double intervalSeconds = 1.0 / oversampledRate_;
    // Treat analytically coincident ramp/PIT corners as one event. This is a
    // billionth of an internal interval, many orders below the 1/64-sample
    // correction-table grid, but large enough to absorb final-operation ULPs.
    const double eventToleranceSeconds = std::max(
        1.0e-15, intervalSeconds * 1.0e-9);
    const double pitClockHz = actualRangeClockHz(range);
    const double thresholdStart = std::isfinite(previousThresholdVolts)
        ? static_cast<double>(previousThresholdVolts) : 6.0;
    const double thresholdEnd = std::isfinite(thresholdVolts)
        ? static_cast<double>(thresholdVolts) : thresholdStart;
    const double thresholdSlope = intervalSeconds > 0.0
        ? (thresholdEnd - thresholdStart) / intervalSeconds : 0.0;
    const bool comparatorPinnedForInterval =
        previousPinnedHigh && pinnedHigh;
    if (addCorrections && comparatorPinnedForInterval)
    {
        // Both endpoints are below the zero-volt ramp, so their linear
        // trajectory is below it for the whole interval. Do not synthesize
        // crossings or accumulate a correction for Pulse Off's pinned level.
        dco.pulseState = 1.0f;
    }

    // A supply-held capacitor stays at +15 V when Unit Character changes the
    // scale used to express that voltage in the base-ramp coordinate. This is
    // a coordinate reprojection, not a capacitor step, so it creates no BLEP.
    const double initialTotalRampScale =
        static_cast<double>(dco.renderScale)
        * static_cast<double>(voice.rampCurrentScale);
    if (dco.positiveRailHeld)
    {
        dco.rampValue = dcoPositiveBaseRail(initialTotalRampScale);
        dco.rampSlopePerSecond = 0.0;
    }

    const auto eventSamplesAgo = [&](double elapsed) {
        const double age = intervalSeconds > 0.0
            ? std::clamp((intervalSeconds - elapsed) / intervalSeconds, 0.0, 1.0) : 0.0;
        return dcoResetCircuitEnabled_ ? age : static_cast<double>(static_cast<float>(age));
    };
    const auto clocksToNextPitInputFalling = [&](double atElapsed) {
        return rangeClockClocksToNextFallingEdge(atElapsed, range);
    };

    // A live card-current edit changes the physical interpretation of the
    // retained base coordinate before this interval begins. Reconcile that
    // left-boundary truth here; otherwise a stationary ramp can cross solely
    // from the scale change and be repaired incorrectly at samplesAgo=0.
    if (addCorrections && !comparatorPinnedForInterval)
    {
        const double rampVolts = 0.5 * rampAmplitudeVolts
            * initialTotalRampScale * (dco.rampValue + 1.0);
        const float stateAtStart = rampVolts >= thresholdStart
            ? 1.0f : -1.0f;
        if (stateAtStart != dco.pulseState)
        {
            addStep(dco.pulse, stateAtStart - dco.pulseState, 1.0f);
            dco.pulseState = stateAtStart;
#if defined(YOUKNOW_WORK_AUDIT)
            YOUKNOW_COUNT_DOMAIN_WORK(dcoComparatorTransitions, 1);
#endif
        }
    }

    double elapsed = 0.0;
    // At 8 kHz, 1x, the 4 MHz range clock and divider 8, the closed interval
    // can contain 126 OUT transitions plus 63 reset completions and 63 supply
    // hits. A fixed pitch transaction adds one pre-stage and two byte events;
    // 512 leaves a real margin over the 254-event supported worst case while
    // still bounding malformed state without allocating anything. IC35 reload
    // is chassis-global and does not add one event to each card's walk.
    constexpr int maximumEventsPerInterval = 512;
    for (int eventIndex = 0;
         eventIndex < maximumEventsPerInterval
             && elapsed < intervalSeconds - eventToleranceSeconds;
         ++eventIndex)
    {
        const double remaining = intervalSeconds - elapsed;
        const double pitSeconds =
            dco.pitState == Dco::PitState::stopped
                || dco.pitState == Dco::PitState::awaitingCount
            ? std::numeric_limits<double>::infinity()
            : std::max(0.0, dco.pitClocksToEvent) / pitClockHz;
        const double cpuWriteSeconds =
            dco.pitWriteState == Dco::PitWriteState::idle
                ? std::numeric_limits<double>::infinity()
                : std::max(0.0, dco.cpuStatesToWrite) / voiceCpuStateHz;
        const double resetSeconds = dco.resetSecondsRemaining > 0.0
            ? dco.resetSecondsRemaining
            : std::numeric_limits<double>::infinity();
        // The current-source ramp cannot charge beyond the ideal +15 V supply
        // bound. A newly staged long count can otherwise leave the old
        // high-note slope running through a hybrid half-cycle and produce
        // hundreds of rails of impossible capacitor voltage before the next
        // positive OUT edge.
        const double totalRampScale =
            static_cast<double>(dco.renderScale)
            * static_cast<double>(voice.rampCurrentScale);
        const double positiveBaseRail =
            dcoPositiveBaseRail(totalRampScale);
        const bool exponentialReset = dco.physicalResetActive && !dco.positiveRailHeld;
        const bool positiveRailNeedsValueClamp =
            dco.rampValue > positiveBaseRail;
        const double chargingRailSeconds = exponentialReset
            ? (dco.resetTargetValue > positiveBaseRail
                ? dco.resetTimeConstant * std::log(
                    (dco.resetTargetValue - dco.rampValue)
                    / (dco.resetTargetValue - positiveBaseRail))
                : std::numeric_limits<double>::infinity())
            : (dco.rampSlopePerSecond > 0.0
                ? std::max(0.0, (positiveBaseRail - dco.rampValue) / dco.rampSlopePerSecond)
                : std::numeric_limits<double>::infinity());
        const double positiveRailSeconds = positiveRailNeedsValueClamp
            ? 0.0
            : std::max(0.0, chargingRailSeconds);
        const double segment = std::min(
            { remaining, pitSeconds, cpuWriteSeconds, resetSeconds,
              positiveRailSeconds });

        if (addCorrections && !comparatorPinnedForInterval && segment > 0.0)
        {
            // Solve the physical comparator directly. renderScale can change
            // when a reset completes inside this interval; deriving ramp volts
            // per segment gives the suffix its new scale without delaying the
            // threshold remap to the next sample.
            const double threshold = thresholdStart
                + (thresholdEnd - thresholdStart)
                    * (elapsed / intervalSeconds);
            const double rampVolts = 0.5 * rampAmplitudeVolts
                * totalRampScale * (dco.rampValue + 1.0);
            const double rampSlopeVolts = 0.5 * rampAmplitudeVolts
                * totalRampScale * dco.rampSlopePerSecond;
            const double relativeSlope = rampSlopeVolts - thresholdSlope;
            if (exponentialReset)
            {
                const double targetVolts = 0.5 * rampAmplitudeVolts
                                        * totalRampScale * (dco.resetTargetValue + 1.0);
                const auto difference = [&](double time) {
                    return DcoResetCircuit::voltage(rampVolts, targetVolts,
                        dco.resetTimeConstant, time) - threshold - thresholdSlope * time;
                };
                // Exponential minus a moving linear threshold has at most
                // one stationary point, hence at most two real crossings.
                std::array<double, 3> boundaries { 0.0, segment, segment };
                int pieces = 1;
                const double ratio = rampSlopeVolts != 0.0
                    ? thresholdSlope / rampSlopeVolts : -1.0;
                if (ratio > 0.0 && ratio < 1.0)
                {
                    const double stationary = -dco.resetTimeConstant * std::log(ratio);
                    if (stationary > 0.0 && stationary < segment)
                    {
                        boundaries[1] = stationary;
                        pieces = 2;
                    }
                }
                for (int part = 0; part < pieces; ++part)
                {
                    double lo = boundaries[static_cast<std::size_t>(part)];
                    double hi = boundaries[static_cast<std::size_t>(part + 1)];
                    const double before = difference(lo);
                    const double after = difference(hi);
                    if (after == 0.0 || (before != 0.0 && (before > 0.0) == (after > 0.0)))
                        continue;
                    if (before == 0.0)
                        hi = lo;
                    else
                    {
                        for (int iteration = 0; iteration < 44; ++iteration)
                        {
                            const double middle = 0.5 * (lo + hi);
                            if ((difference(middle) > 0.0) == (before > 0.0))
                                lo = middle;
                            else
                                hi = middle;
                        }
                    }
                    const float state = after > 0.0 ? 1.0f : -1.0f;
                    if (state != dco.pulseState)
                    {
                        addStep(dco.pulse, state - dco.pulseState,
                                eventSamplesAgo(elapsed + 0.5 * (lo + hi)));
                        dco.pulseState = state;
#if defined(YOUKNOW_WORK_AUDIT)
                        YOUKNOW_COUNT_DOMAIN_WORK(dcoComparatorTransitions, 1);
#endif
                    }
                }
            }
            else if (std::abs(relativeSlope) > 1.0e-14)
            {
                const double crossing = (threshold - rampVolts)
                                      / relativeSlope;
                if (crossing >= -eventToleranceSeconds
                    && crossing <= segment + eventToleranceSeconds)
                {
                    const float state = relativeSlope > 0.0 ? 1.0f : -1.0f;
                    if (state != dco.pulseState)
                    {
                        addStep(dco.pulse, state - dco.pulseState,
                                eventSamplesAgo(elapsed + std::min(
                                    std::max(0.0, crossing), segment)));
                        dco.pulseState = state;
#if defined(YOUKNOW_WORK_AUDIT)
                        YOUKNOW_COUNT_DOMAIN_WORK(
                            dcoComparatorTransitions, 1);
#endif
                    }
                }
            }
        }

        if (exponentialReset)
        {
            if (addCorrections)
                addDcoResetCurvature(voice, dco.rampSlopePerSecond, elapsed, segment);
            dco.rampValue = DcoResetCircuit::voltage(dco.rampValue, dco.resetTargetValue,
                                                  dco.resetTimeConstant, segment);
            dco.rampSlopePerSecond = (dco.resetTargetValue - dco.rampValue)
                                  / dco.resetTimeConstant;
        }
        else
            dco.rampValue += dco.rampSlopePerSecond * segment;
        if (dco.resetSecondsRemaining > 0.0)
            dco.resetSecondsRemaining = std::max(
                0.0, dco.resetSecondsRemaining - segment);
        if (dco.pitState == Dco::PitState::awaitingCount)
        {
            // CE is stopped, not TP5. Follow the chassis-global falling/count
            // phase analytically while the two count bytes are in flight.
            // Exact equality is PIT-first policy, so the completed count waits
            // for the following falling edge.
            dco.pitClocksToEvent = clocksToNextPitInputFalling(
                elapsed + segment);
        }
        else if (dco.pitState != Dco::PitState::stopped)
            dco.pitClocksToEvent = std::max(
                0.0, dco.pitClocksToEvent - segment * pitClockHz);
        if (dco.pitWriteState != Dco::PitWriteState::idle)
            dco.cpuStatesToWrite = std::max(
                0.0, dco.cpuStatesToWrite - segment * voiceCpuStateHz);
        elapsed += segment;

        const bool resetComplete =
            resetSeconds <= segment + eventToleranceSeconds;
        const bool positiveRailHit =
            positiveRailSeconds <= segment + eventToleranceSeconds;
        const bool pitEvent =
            pitSeconds <= segment + eventToleranceSeconds;
        const bool cpuWriteEvent =
            cpuWriteSeconds <= segment + eventToleranceSeconds;
        if (!resetComplete && !positiveRailHit && !pitEvent
            && !cpuWriteEvent)
            break;

        // Complete an older C54 discharge before processing a coincident new
        // OUT edge. The ordering is deterministic; explicitly configured
        // gates can meet or overlap the next reset edge.
        if (resetComplete)
            beginDcoCharge(
                voice, eventSamplesAgo(elapsed), addCorrections);

        // If a scaled cycle reaches the supply bound exactly at the next
        // positive OUT edge, preserve the incoming charge slope so discharge
        // emits one direct BLAMP correction rather than two coincident table
        // walks through an artificial zero-slope state. A genuinely early rail
        // (or a coincident non-rising PIT event) still enters the hold above.
        const bool railMeetsRisingOut = positiveRailHit && pitEvent
            && dco.pitState == Dco::PitState::running && !dco.pitOutHigh;
        if (positiveRailHit)
        {
            const double valueBeforeClamp = dco.rampValue;
            const bool chargingIntoRail = dco.rampSlopePerSecond > 0.0;
            const double oldSlope = dcoCorrectionSlope(
                dco.rampSlopePerSecond
                * static_cast<double>(dco.renderScale) * voice.rampCurrentScale
                * intervalSeconds);
            dco.rampValue = positiveBaseRail;
            if (addCorrections && dco.saw.primed
                && positiveRailNeedsValueClamp)
            {
                const float valueStep = static_cast<float>(
                    (positiveBaseRail - valueBeforeClamp)
                    * static_cast<double>(dco.renderScale) * voice.rampCurrentScale);
                addStep(dco.saw, valueStep, eventSamplesAgo(elapsed));
            }

            // Charging reaches a real supply hold. A falling reset discovered
            // above a newly lowered coordinate rail is clamped at t=0 but
            // keeps falling; a live calibration change must not pause it.
            if (chargingIntoRail && !railMeetsRisingOut)
            {
                dco.rampSlopePerSecond = 0.0;
                dco.positiveRailHeld = true;
                if (addCorrections && dco.saw.primed)
                    addDcoSlope(voice, -oldSlope, eventSamplesAgo(elapsed));
            }
            else if (dco.rampSlopePerSecond < 0.0
                     && dco.resetSecondsRemaining > 0.0)
            {
                // A physical reset retains its R/C law after this voltage
                // clamp. The compatibility line instead keeps its original
                // deadline and ends at -1.
                if (dco.physicalResetActive)
                    refreshDcoResetTrajectory(voice);
                else
                    dco.rampSlopePerSecond =
                        (-1.0 - dco.rampValue) / dco.resetSecondsRemaining;
                const double newSlope = dcoCorrectionSlope(
                    dco.rampSlopePerSecond
                    * static_cast<double>(dco.renderScale) * voice.rampCurrentScale
                    * intervalSeconds);
                if (addCorrections && dco.saw.primed)
                    addDcoSlope(voice, newSlope - oldSlope,
                             eventSamplesAgo(elapsed));
            }
        }

        // This block deliberately precedes the byte-write block: a running
        // OUT transition tied with the MSB still belongs to the old count.
        if (pitEvent)
        {
            const auto event = dco.consumePitEvent();
            updateActiveDcoPeriod(dco, range);
            if (event == Dco::PitEvent::initialLoad)
            {
                // Cold start has no recovered pre-program capacitor state. Hold
                // the established low rail through one modelled reset interval,
                // then launch the first rise. This is initialization policy, not
                // a claim that loading CE generated an OUT edge.
                if (dco.coldInitialLoadPending)
                {
                    dco.coldInitialLoadPending = false;
                    dco.positiveRailHeld = false;
                    dco.rampValue = -1.0;
                    dco.rampSlopePerSecond = 0.0;
                    const double periodSeconds = std::max(
                        dco.periodSamples / oversampledRate_, 1.0e-12);
                    dco.resetSecondsRemaining = static_cast<double>(
                        resetFraction(periodSeconds)) * periodSeconds;
                }
            }
            else if (event == Dco::PitEvent::risingEdge)
            {
                beginDcoDischarge(
                    voice, eventSamplesAgo(elapsed), addCorrections);
            }
        }

        if (cpuWriteEvent)
        {
            if (dco.pitWriteState
                == Dco::PitWriteState::awaitingPitchPrestage)
            {
                // T is the existing converter boundary poll and the start of
                // ANI PA,$EF. The recovered no-interrupt paths place reset
                // control at T-389 states, running LSB at T-334, and both MSBs
                // at T-323. Compute pitch once at the common earliest point;
                // this is also where release-time transpose can request reset.
                // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L732-L741
                // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L783-L794
                prestageDcoPitchTransaction(
                    voice, clocksToNextPitInputFalling(elapsed),
                    eventSamplesAgo(elapsed), addCorrections);
            }
            else if (dco.pitWriteState == Dco::PitWriteState::awaitingLsb)
            {
                dco.pitWriteState = Dco::PitWriteState::awaitingMsb;
                dco.cpuStatesToWrite = pitLsbToMsbStates;
            }
            else if (dco.pitWriteState == Dco::PitWriteState::awaitingMsb)
            {
                dco.stageMode3Count(dco.pitWriteDivider);
                dco.pitWriteState = Dco::PitWriteState::idle;
                dco.cpuStatesToWrite = 0.0;
            }
        }
    }

    if (addCorrections)
    {
        if (!comparatorPinnedForInterval)
        {
            const double totalRampScale =
                static_cast<double>(dco.renderScale)
                * static_cast<double>(voice.rampCurrentScale);
            const double rampVolts = 0.5 * rampAmplitudeVolts
                * totalRampScale * (dco.rampValue + 1.0);
            const float comparatorAtEnd = pinnedHigh
                ? 1.0f
                : (rampVolts >= thresholdEnd ? 1.0f : -1.0f);
            if (comparatorAtEnd != dco.pulseState)
            {
                addStep(dco.pulse, comparatorAtEnd - dco.pulseState, 0.0f);
                dco.pulseState = comparatorAtEnd;
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(dcoComparatorTransitions, 1);
#endif
            }
        }
        voice.previousPulseThresholdVolts = static_cast<float>(thresholdEnd);
        voice.previousPulsePinnedHigh = pinnedHigh;
        voice.pulseThresholdPrimed = true;
    }
}

void YouKnowEngine::freewheelVoiceCard(Voice& voice) noexcept
{
    voice.freewheeling = true;
    advanceDcoPitAndRamp(
        voice, activeParameters_.range,
        voice.pulseThresholdVolts, voice.pulseThresholdVolts,
        voice.pulsePinnedHigh, voice.pulsePinnedHigh, false);

    // Keep the same four independent draws and reconstruction history as
    // the continuously rendered card behind its closed VCA.
    std::array<double, 4> stageNoise {};
    const int draws = activeParameters_.enableCardJohnsonFloor ? 4 : 1;
    const float temperatureScale =
        cards_[static_cast<std::size_t>(voice.cardIndex)].johnsonTemperatureScale;
    for (int stage = 0; stage < draws; ++stage)
    {
        voice.noiseState = xorshift32(voice.noiseState);
        if (activeParameters_.enableCardJohnsonFloor)
            stageNoise[static_cast<std::size_t>(stage)] =
                bipolarFromState(voice.noiseState) * filterNoiseVoltsDerived
                * noiseRateScale_ * temperatureScale;
    }
    voice.filter.setStageNoise(stageNoise);

    // C56/C50 is a 0.482 Hz physical state, not reconstruction work. Follow the
    // free-running ramp/comparator endpoint at low pitch without paying for its
    // inaudible BLEP or filter solve. Above 100 Hz use the exact duty mean: at
    // the low 8 kHz processing boundary this also avoids sampling a >Nyquist
    // comparator into false DC, while the omitted capacitor ripple is bounded
    // to about 45 mV for pulse at the crossover and falls with frequency. Regressions
    // compare both the lowest pitch and the >1-cycle/sample extreme to Exact.
    // The legacy A/B path (both node couplings off) deliberately retains its
    // former frozen state. The sub's half-wave mean sits on the same node
    // (see subWaveNodeMean); an idle card must keep tracking it, with or
    // without the pulse-off coupling, or the next note-on would replay the
    // SUB level as a C56 step.
    const bool trackPulseNode = activeParameters_.enablePulseOffWaveNodeCoupling;
    const bool trackSubNode = activeParameters_.enableSubHalfWaveNodeCoupling;
    const bool trackSawNode = activeParameters_.sawEnabled
                           && (trackPulseNode || trackSubNode);
    if (trackPulseNode || trackSubNode || trackSawNode)
    {
        auto& dco = voice.dco;
        const double totalScale = static_cast<double>(dco.renderScale)
                                * static_cast<double>(voice.rampCurrentScale);
        const double rampVolts = 0.5 * static_cast<double>(rampAmplitudeVolts)
                               * totalScale * (dco.rampValue + 1.0);
        constexpr double endpointTrackingMaximumHz = 100.0;
        const double carrierHz = oversampledRate_ * dcoMasterClockRatio_
            / std::max(dco.periodSamples, 1.0);
        float pulseNode = 0.0f;
        if (trackPulseNode)
        {
            dco.pulseState = voice.pulsePinnedHigh
                          || rampVolts >= voice.pulseThresholdVolts
                           ? 1.0f : -1.0f;
            pulseNode = carrierHz <= endpointTrackingMaximumHz
                ? dco.pulseState * pulseMixVolts
                : pulseWaveNodeMean(voice, activeParameters_);
        }
        // A common-clock offset changes ramp height at fixed current, hence
        // also the saw's DC mean. Retain its C56 charge behind the shut VCA,
        // including the smaller nominal CV/count ripple. Low notes use the
        // physical endpoint; high notes use the finite-reset/rail cycle mean.
        const float sawNode = !trackSawNode ? 0.0f
            : carrierHz <= endpointTrackingMaximumHz
                ? static_cast<float>(sawMixVolts
                    * (rampVolts / (0.5 * rampAmplitudeVolts) - 1.0))
                : steadyDcoSawMean(voice);
        static_cast<void>(voice.moduleCoupling.process(
            oscillatorLevelScale_
                * (pulseNode + subWaveNodeMean(voice, activeParameters_) + sawNode),
            moduleCouplingG_, 0.0f, 1.0f));
        if (trackPulseNode)
        {
            voice.previousPulseThresholdVolts = voice.pulseThresholdVolts;
            voice.previousPulsePinnedHigh = voice.pulsePinnedHigh;
            voice.pulseThresholdPrimed = true;
        }
    }
}

YouKnowEngine::VoiceFilterFrame YouKnowEngine::prepareVoiceFilter(
    Voice& voice, const EngineParameters& parameters,
    float noiseSample) noexcept
{
    // Extension slots have no continuously powered voice card behind them.
    // Their digital portamento state still advances on the converter pass,
    // but an unassigned slot has no DCO/filter/audio state that must run.
    if (!voice.active && voice.cardIndex >= hardwareVoices)
        return {};

    // A retired physical card's render is computed and then discarded: the
    // caller drops the sample, so the only thing this pass can change is the
    // free-running state a later reassignment starts from. Under the fast
    // tanh modes that state is advanced directly, at a fraction of the cost.
    // Exact keeps the established full render, so the reference kernel's
    // frozen fingerprints and work counters are untouched -- and switching
    // back to Exact is the way to switch this behaviour off.
    if (!voice.active && parameters.vcfTanhMode != VcfTanhMode::Exact
        && !coupledMixerEnabled_)
    {
        freewheelVoiceCard(voice);
        return {};
    }

#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(dcoFrames, 1);
#endif
    auto& dco = voice.dco;
    const auto& card = cards_[static_cast<std::size_t>(voice.cardIndex)];
    const auto boundedOmegaStep = [&](float baseOmegaStep) {
        return static_cast<float>(OtaCascade::clampOmegaStep(
            static_cast<double>(baseOmegaStep)
                * card.thermalFilterOmegaScale));
    };

    // Returning to full reconstruction (including a switch to Exact while
    // idle) must resume insertion into the physical reset residual ring.
    voice.freewheeling = false;

    // One shared event walk owns the M82C53 half-cycles, C54 ramp and
    // comparator. The timer divides the one configured ceramic-resonator
    // reference (nominal 8 MHz before the optional common temperature proxy).
    // Card temperature has no independent DCO pitch term.
    const float thresholdVolts = voice.pulseThresholdVolts;
    const float previousThresholdVolts = voice.pulseThresholdPrimed
        ? voice.previousPulseThresholdVolts : thresholdVolts;
    const bool previousPinnedHigh = voice.pulseThresholdPrimed
        ? voice.previousPulsePinnedHigh : voice.pulsePinnedHigh;
    advanceDcoPitAndRamp(
        voice, parameters.range, previousThresholdVolts, thresholdVolts,
        previousPinnedHigh, voice.pulsePinnedHigh, true);

    // Retained capacitor voltage supplies both the saw and PWM comparator;
    // the CV only changes its derivative at the converter event.
    const float sawNaive = static_cast<float>(
        (dco.rampValue + 1.0) * static_cast<double>(dco.renderScale)
        * voice.rampCurrentScale - 1.0);
    const float amplitude = sawMixVolts;
    const int sawCorrectionSlot = dco.saw.base;
    float sawReconstructed = dco.saw.advance(sawNaive);
    if (dcoResetCircuitEnabled_)
    {
        sawReconstructed += static_cast<float>(
            dco.resetSawCorrection[static_cast<std::size_t>(sawCorrectionSlot)]);
        dco.resetSawCorrection[static_cast<std::size_t>(sawCorrectionSlot)] = 0.0;
    }
    const float sawOut = sawReconstructed * amplitude;

    // Comparator and sub transitions were inserted at their PIT/ramp event
    // timestamps above; their logic levels are independent of ramp amplitude.
    const float pulseOut =
        dco.pulse.advance(dco.pulseState) * pulseMixVolts;
    const float subCurrent = parameters.enableSubDiodeControl
        ? SubLevelDiodeLaw::gain(subCv_) : static_cast<float>(subCv_);
    const float subGain = subMixVolts * subCurrent
        * (1.0f + card.subLevelError * 0.03f * parameters.calibration);
    // Half-wave: AC +/- subGain as before, plus a +subGain mean (see the
    // summing-node note below). The comparison switch keeps the zero-mean
    // square.
    const float subTrack = dco.sub.advance(dco.subState);
    const float subOut = parameters.enableSubHalfWaveNodeCoupling
        ? (subTrack + 1.0f) * subGain
        : subTrack * subGain;

    // --- Summing node --------------------------------------------------------
    // Module p. 13 (2026-08-07 designator read): saw and pulse leave the
    // waveshaper already summed on ONE per-voice WAVE output (IC12/IC8/IC4
    // pin 14 or 16), the sub joins that line through R101/R97 27k behind
    // D6/D5 from its own switch transistor -- 60k of conducting-path series
    // resistance back to the SUB LEVEL rail once R102/R99's 33k collector load
    // is counted (2026-08-20 junction read) -- the shared noise rail arrives
    // on its own leg, and C56/C50 couple the node into the voice module's
    // input.
    // No panel switch reaches this node: SAW is gated by a control rail at
    // the generator ("0: saw ON" at Tr24/R148), PULSE by the -0.8 V hold that
    // pins the comparator high on the fixed WAVE output, SUB by its collector
    // supply and NOISE by the level OTA. Exact pinned-node voltage and loading
    // remain OQ-15/OQ-11.
    // The sub's single series diode makes its current unipolar -- the rail
    // sources (9.92 V * subCv - V_D6) / 60k, roughly 155 uA at full scale
    // for an assumed ~0.6 V drop (p.12 identifies D6 as 1SS133), while Tr19 is
    // off and nothing on the other -- so its mean rides on this node for
    // C56/C50 to remove. SubLevelDiodeLaw supplies the measured aggregate
    // onset/soft knee at a settled WAVE bias; false enableSubDiodeControl
    // restores the old linear rail law. Saw-dependent modulation through
    // the node's swing remains calibration-dependent: the explicit coupled
    // candidate above can represent it, but has no installed-unit defaults.
    // The compatibility node's
    // DC-to-AC impedance ratio is voiced at 1, the floor of its <= 2 bracket,
    // inside the already-voiced subMixVolts coordinate. This is NOT the
    // removed "sub-driver amplitude asymmetry" (a fabricated 0.3 % inequality
    // of two divider levels, DC plus even harmonics, misattributed to 4013
    // edge timing): this DC is the topology-derived half-wave mean, adds no
    // even harmonics (50 % duty), leaves the edges unchanged, and is nulled
    // by C56 at steady state -- it is audible only as the C56/C59-shaped bump
    // a SUB level step produces.
    // Generators mute or clamp; legs never switch. The node's loading is one
    // configuration-independent constant, which the established
    // filterInputAttenuation coordinate already absorbs -- an earlier
    // revision modelled four switchable 100k legs against the module's 68k
    // and attenuated any patch with both waveforms on by a phantom 1.76 dB.
    // The absolute source-to-filter budget remains OQ-15.
    float mixed = 0.0f;
    if (parameters.sawEnabled)
        mixed += sawOut;
    if (pulseMixEnabled(parameters.pulseEnabled, voice.pulseDuty,
                        parameters.enablePulseOffWaveNodeCoupling))
        mixed += pulseOut;
    if (!coupledMixerEnabled_)
        mixed += subOut;
    // The priming and idle-tracking paths above scale the same three legs.
    mixed *= oscillatorLevelScale_;
    mixed += noiseSample
           * (1.0f + card.noiseLevelError * 0.03f * parameters.calibration)
           * agedNoiseGain_;

    static_assert(noiseReferenceRateHz == 192000.0,
                  "filterNoiseVoltsDerived hard-codes the 192 kHz reference "
                  "rate because it is computed at namespace scope");
    voice.noiseState = xorshift32(voice.noiseState);
    const float microscopicNoise = parameters.enableCardJohnsonFloor
        ? 0.0f : bipolarFromState(voice.noiseState) * filterNoiseVoltsVoiced;
    std::array<double, 4> stageNoise {};
    if (parameters.enableCardJohnsonFloor)
        for (std::size_t stage = 0; stage < stageNoise.size(); ++stage)
        {
            if (stage != 0)
                voice.noiseState = xorshift32(voice.noiseState);
            stageNoise[stage] = bipolarFromState(voice.noiseState)
                * filterNoiseVoltsDerived * noiseRateScale_
                * card.johnsonTemperatureScale;
        }
    voice.filter.setStageNoise(stageNoise);

    // --- Filter, amplifier -------------------------------------------------
    // C56/C50 stand between the summed WAVE node and pin 1 VCF IN, so the
    // module rejects the mixer's settled DC. Changes in that mean pass as
    // decaying transients into the nonlinear filter below. An enabled pulse
    // carries a substantial mean: the comparator's output is a
    // duty-asymmetric square, so its mean walks with PWM (at the 95 % duty the
    // hold's 0.6 V endpoint reaches, mean = 6 V * (2d - 1) = 5.4 V at the
    // node). The capacitor state follows the running source behind a shut
    // VCA, so a new note does not replay the entire settled mean as a step.
    // Actual PWM/SUB/source changes still charge C56 and can bias the filter
    // transiently. Their decay depends on the unresolved source/load network.
    //
    // The panel HPF is a different stage and stays where it is: the schematic
    // puts it on the jack board, downstream of the summing amplifier, so it is
    // one shared stage after all six voices rather than a leg inside each --
    // see the mix.
    float coupled;
    if (coupledMixerEnabled_)
    {
        const auto& c = coupledMixerCalibration_;
        // Required calibration maps the existing no-sub source sum to the
        // physical Thevenin source. The coupled solve replaces BOTH the
        // independent sub add and C56: its output is already volts at VCF IN.
        // Divide out the compatibility coordinate here so the existing
        // compensation/core path below receives those physical volts once.
        // Inactive cards run the same solve, preserving real capacitor charge;
        // the old freewheel mean is invalid for this nonlinear network.
        const auto node = voice.coupledMixer.process(c,
            c.sourceBiasVolts + c.sourceScale * mixed,
            CoupledSubMixer::railFullScaleVolts * subCv_,
            0.5 * (1.0 + subTrack), inverseOversampledRate_);
        coupled = static_cast<float>(node.filterVolts / filterInputAttenuation);
    }
    else
        coupled = voice.moduleCoupling.process(
            mixed, moduleCouplingG_, 0.0f, 1.0f);
    // Resistor noise enters the four OTA nodes after this coupling capacitor.
    // Only the retired voiced comparison seed still enters the signal input.
    // With the differential form the compensation rides inside the resonance
    // pair's tanh, so the drive reaching the cascade is the plain coupled
    // node; the split form keeps the feedforward multiply it always had.
    const float compensatedDrive =
        activeParameters_.enableDifferentialResonanceInput
            ? coupled * filterInputAttenuation
            : coupled * filterInputAttenuation * voice.inputCompensation;
    const float filterInput =
        compensatedDrive + microscopicNoise * noiseRateScale_;
    // V_t(T) = k * T / q, driven by the accelerated software temperature model.
    const float dynamicHeadroom =
        dynamicOtaHeadroomVolts(parameters, voice.cardIndex);
    // The same gradient enters through the control path's coefficient. Its
    // static pitch contribution is absorbed by the per-card service trim;
    // thermal headroom still evolves with the running warm-up clock.
    // The continuous cascade advances directly at this internal boundary.
    // Two fixed Merson half-steps are a numerical solver, not a higher
    // modelled sample rate: they add neither an inter-domain boundary nor
    // latency. Apply the product-grid cap after the thermal card spread so Unit
    // Character cannot push the numerical interval past the proven boundary.
    const float effectiveFilterOmegaStep = boundedOmegaStep(
        voice.filterOmegaStep);
    OtaCascade::ControlTrajectory eventControlTrajectory;
    const OtaCascade::ControlTrajectory* controlTrajectory = nullptr;
    if (exactVcfControlInterval_[
            static_cast<std::size_t>(voice.cardIndex)])
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(vcfExactControlMaps, 6);
#endif
        const auto& cutoffInterval = cutoffVcfHoldIntervals_[
            static_cast<std::size_t>(voice.cardIndex)];
        const auto& resonanceInterval = resonanceVcfHoldInterval_;

        struct VcfControl
        {
            double omega {};
            double feedback {};
        };
        const auto mapHeldControls = [&](double cutoffCounts,
                                         double resonanceCv) {
            const float mappedFeedback = resonanceFeedbackFor(
                static_cast<float>(resonanceCv), card, parameters.calibration,
                parameters.useCircuitDerivedResonanceShape);
            const float mappedAnalogCounts = cutoffAnalogCounts(
                static_cast<float>(cutoffCounts), card, parameters.calibration,
                powerSupplyDroop_ - railRippleVolts_);
            const float calibrationFeedback = parameters.useFixedVcfServiceFrequencyTrim
                ? resonanceFeedbackFor(1.0f, card, parameters.calibration,
                    parameters.useCircuitDerivedResonanceShape)
                : mappedFeedback;
            const float cutoffHz = vcfEffectiveCutoffHz(
                mappedAnalogCounts, calibrationFeedback,
                parameters.useServiced439522VcfCalibration ? voice.cardIndex : -1);
            const float limited = std::min(
                cutoffHz, static_cast<float>(oversampledRate_) * 0.45f);
            const float baseOmega = twoPi * limited
                                  * inverseOversampledRate_;
            return VcfControl {
                boundedOmegaStep(baseOmega),
                mappedFeedback
            };
        };

        const auto mappedStart = mapHeldControls(
            cutoffInterval.value.front(),
            resonanceInterval.value.front());
        const VcfControl mappedEnd {
            effectiveFilterOmegaStep, voice.feedback
        };
        const double previousOmega = voice.filter.parameterHistoryPrimed
            ? voice.filter.previousOmegaStep
            : static_cast<double>(effectiveFilterOmegaStep);
        const double previousFeedback = voice.filter.parameterHistoryPrimed
            ? voice.filter.previousFeedback
            : static_cast<double>(voice.feedback);
        const double previousHeadroom = voice.filter.parameterHistoryPrimed
            ? voice.filter.previousHeadroom
            : static_cast<double>(dynamicHeadroom);
        for (std::size_t point = 1;
             point + 1u < OtaCascade::controlNodePositions.size(); ++point)
        {
            const double position =
                OtaCascade::controlNodePositions[point];
            const auto mapped = mapHeldControls(
                cutoffInterval.value[point],
                resonanceInterval.value[point]);
            const double baselineOmega = previousOmega + position
                * (static_cast<double>(effectiveFilterOmegaStep)
                   - previousOmega);
            const double baselineFeedback = previousFeedback + position
                * (static_cast<double>(voice.feedback)
                   - previousFeedback);
            const double mappedLinearOmega = mappedStart.omega + position
                * (mappedEnd.omega - mappedStart.omega);
            const double mappedLinearFeedback = mappedStart.feedback + position
                * (mappedEnd.feedback - mappedStart.feedback);
            eventControlTrajectory.omegaStep[point] = baselineOmega
                + (mapped.omega - mappedLinearOmega);
            eventControlTrajectory.feedback[point] = baselineFeedback
                + (mapped.feedback - mappedLinearFeedback);
            eventControlTrajectory.headroom[point] = previousHeadroom
                + position * (static_cast<double>(dynamicHeadroom)
                              - previousHeadroom);
        }
        // Make the two ownership boundaries exact rather than relying on
        // cancellation of independently rounded mappings. Analogue drift,
        // rail loading and thermal warmup therefore retain the established
        // previous-to-current interpolation; only the hold's within-interval
        // curvature is added at the five interior Merson abscissae.
        eventControlTrajectory.omegaStep.front() = previousOmega;
        eventControlTrajectory.feedback.front() = previousFeedback;
        eventControlTrajectory.headroom.front() = previousHeadroom;
        eventControlTrajectory.omegaStep.back() = effectiveFilterOmegaStep;
        eventControlTrajectory.feedback.back() = voice.feedback;
        eventControlTrajectory.headroom.back() = dynamicHeadroom;
        controlTrajectory = &eventControlTrajectory;
    }
    VoiceFilterFrame frame;
    frame.input = filterInput;
    frame.omegaStep = effectiveFilterOmegaStep;
    frame.headroom = dynamicHeadroom;
    if (controlTrajectory != nullptr)
    {
        frame.trajectory = eventControlTrajectory;
        frame.hasTrajectory = true;
    }
    frame.needsFilter = true;
    return frame;
}

float YouKnowEngine::finishVoiceFilter(Voice& voice,
                                          float filtered) noexcept
{
    // C59 stands between pin 3 VCF OUT and pin 9 VCA IN, so the amplifier
    // never sees the filter's DC. The cascade makes DC of its own: the stage
    // offsets sit inside the loop, and an enabled pulse arrives duty
    // asymmetric, so the filter output's mean walks with PWM. Passed straight
    // through, that mean would be multiplied by the envelope and leave a
    // duty-dependent thump at every note-on and note-off -- which is what the
    // service procedure's VR30/R112 null exists to remove, and what the
    // module's own capacitor removes before it. The capacitor is a physical
    // node, so it is advanced for an inactive card too, ahead of the early
    // return below.
    const float vcaInput = voice.vcaInputCoupling.process(
        filtered, vcaInputCouplingG_, 0.0f, 1.0f);
    voice.vcaInputVolts = vcaInput;

    if (!voice.active)
        return 0.0f;

    // VR30 injects a signal-input null into the BA662 through R112; it is
    // separate from Tr20's control-current path and is adjusted per card to
    // minimise control-dependent output error. The former term reused
    // vcaControlOffset here, added an unexplained +0.8 mV bias and then
    // multiplied by control and VCA gain, producing an unsupported
    // control-squared pulse. Do not invent a residual until a calibrated
    // TP8--TP13 capture establishes its distribution.
    //
    // The pair itself saturates ahead of the control multiply (see
    // VoiceVcaSignalLaw); the switch only retains the linear multiply for
    // A/B renders. Keep the BA662's fixed-trim thermal gain in either path,
    // after C59: scaling the capacitor's input would create a different
    // transient and incorrectly change the stored coupling voltage.
    const float trimmed = vcaInput * voice.vcaInputTrim;
    const float drive = trimmed
        * voiceVcaThermalDriveScale(activeParameters_, voice.cardIndex);
    const float shaped = activeParameters_.enableVoiceVcaSignalSaturation
        ? VoiceVcaSignalLaw::shape(drive) : drive;
    // This fixed gain was formerly lost when the physical BA662 law was
    // normalized to unity. Apply it in volts before the 2.6-V model-unit
    // conversion, so every downstream circuit receives the service level.
    const float serviceGain = activeParameters_.enableVoiceVcaServiceGain
        ? VoiceVcaSignalLaw::serviceGain() : 1.0f;
    const float output = shaped * voice.vca * serviceGain * voltsToSample;

    voice.energy += voiceEnergyFollower_ * (std::abs(output) - voice.energy);
    return std::isfinite(output) ? output : 0.0f;
}

template <bool useCubicEarly>
float YouKnowEngine::renderVoice(Voice& voice,
                                    const EngineParameters& parameters,
                                    float noiseSample) noexcept
{
    auto frame = prepareVoiceFilter(voice, parameters, noiseSample);
    if (!frame.needsFilter)
        return 0.0f;
    const auto* trajectory = frame.hasTrajectory ? &frame.trajectory : nullptr;
    const float filtered = voice.filter.process<useCubicEarly>(
        frame.input, frame.omegaStep, voice.feedback, frame.headroom,
        parameters.enableVcfEarlyEffect, parameters.calibration, trajectory,
        parameters.vcfTanhMode, parameters.vcfSolverMode);
    return finishVoiceFilter(voice, filtered);
}

template float YouKnowEngine::renderVoice<false>(
    Voice&, const EngineParameters&, float) noexcept;
template float YouKnowEngine::renderVoice<true>(
    Voice&, const EngineParameters&, float) noexcept;

#if defined(YOUKNOW_HAS_VCF_PAIR_SIMD)
std::array<float, 2> YouKnowEngine::renderVoicePair(
    Voice& first, Voice& second, const EngineParameters& parameters,
    float noiseSample) noexcept
{
    auto firstFrame = prepareVoiceFilter(first, parameters, noiseSample);
    auto secondFrame = prepareVoiceFilter(second, parameters, noiseSample);
    std::array<float, 2> filtered {};
    bool processed = false;
#if !defined(YOUKNOW_WORK_AUDIT)
    if (firstFrame.needsFilter && secondFrame.needsFilter
        && !firstFrame.hasTrajectory && !secondFrame.hasTrajectory)
        processed =
            OtaCascade::tryProcessSettledRk4Pair(
                first.filter, firstFrame.input, firstFrame.omegaStep,
                first.feedback, firstFrame.headroom,
                second.filter, secondFrame.input, secondFrame.omegaStep,
                second.feedback, secondFrame.headroom,
                parameters.enableVcfEarlyEffect, parameters.calibration,
                filtered[0], filtered[1])
            || OtaCascade::tryProcessSettledMersonPair(
                first.filter, firstFrame.input, firstFrame.omegaStep,
                first.feedback, firstFrame.headroom,
                second.filter, secondFrame.input, secondFrame.omegaStep,
                second.feedback, secondFrame.headroom,
                parameters.enableVcfEarlyEffect, parameters.calibration,
                filtered[0], filtered[1]);
#endif
    if (!processed)
    {
        if (firstFrame.needsFilter)
            filtered[0] = first.filter.process<true>(
                firstFrame.input, firstFrame.omegaStep, first.feedback,
                firstFrame.headroom, parameters.enableVcfEarlyEffect,
                parameters.calibration,
                firstFrame.hasTrajectory ? &firstFrame.trajectory : nullptr,
                parameters.vcfTanhMode, parameters.vcfSolverMode);
        if (secondFrame.needsFilter)
            filtered[1] = second.filter.process<true>(
                secondFrame.input, secondFrame.omegaStep, second.feedback,
                secondFrame.headroom, parameters.enableVcfEarlyEffect,
                parameters.calibration,
                secondFrame.hasTrajectory ? &secondFrame.trajectory : nullptr,
                parameters.vcfTanhMode, parameters.vcfSolverMode);
    }

    return {
        firstFrame.needsFilter
            ? finishVoiceFilter(first, filtered[0]) : 0.0f,
        secondFrame.needsFilter
            ? finishVoiceFilter(second, filtered[1]) : 0.0f
    };
}

std::array<float, 4> YouKnowEngine::renderVoiceQuad(
    const std::array<Voice*, 4>& voices,
    const EngineParameters& parameters, float noiseSample) noexcept
{
    std::array<VoiceFilterFrame, 4> frames;
    for (std::size_t lane = 0; lane < voices.size(); ++lane)
        frames[lane] = prepareVoiceFilter(
            *voices[lane], parameters, noiseSample);

    std::array<float, 4> filtered {};
    bool processed = std::all_of(
        frames.begin(), frames.end(), [](const VoiceFilterFrame& frame) {
            return frame.needsFilter && !frame.hasTrajectory;
        });
    if (processed)
    {
        std::array<OtaCascade*, 4> cascades;
        std::array<float, 4> inputs;
        std::array<float, 4> omegaSteps;
        std::array<float, 4> feedbacks;
        std::array<float, 4> headrooms;
        for (std::size_t lane = 0; lane < voices.size(); ++lane)
        {
            cascades[lane] = &voices[lane]->filter;
            inputs[lane] = frames[lane].input;
            omegaSteps[lane] = frames[lane].omegaStep;
            feedbacks[lane] = voices[lane]->feedback;
            headrooms[lane] = frames[lane].headroom;
        }
        processed = OtaCascade::tryProcessSettledMersonQuad(
            cascades, inputs, omegaSteps, feedbacks, headrooms,
            parameters.enableVcfEarlyEffect, parameters.calibration,
            filtered);
    }

    if (!processed)
    {
        const auto processPair = [&](std::size_t firstLane) {
            const std::size_t secondLane = firstLane + 1u;
            bool pairProcessed = false;
            if (frames[firstLane].needsFilter
                && frames[secondLane].needsFilter
                && !frames[firstLane].hasTrajectory
                && !frames[secondLane].hasTrajectory)
                pairProcessed = OtaCascade::tryProcessSettledRk4Pair(
                    voices[firstLane]->filter, frames[firstLane].input,
                    frames[firstLane].omegaStep,
                    voices[firstLane]->feedback, frames[firstLane].headroom,
                    voices[secondLane]->filter, frames[secondLane].input,
                    frames[secondLane].omegaStep,
                    voices[secondLane]->feedback,
                    frames[secondLane].headroom,
                    parameters.enableVcfEarlyEffect, parameters.calibration,
                    filtered[firstLane], filtered[secondLane])
                    || OtaCascade::tryProcessSettledMersonPair(
                        voices[firstLane]->filter, frames[firstLane].input,
                        frames[firstLane].omegaStep,
                        voices[firstLane]->feedback,
                        frames[firstLane].headroom,
                        voices[secondLane]->filter,
                        frames[secondLane].input,
                        frames[secondLane].omegaStep,
                        voices[secondLane]->feedback,
                        frames[secondLane].headroom,
                        parameters.enableVcfEarlyEffect,
                        parameters.calibration,
                        filtered[firstLane], filtered[secondLane]);
            if (pairProcessed)
                return;
            for (const std::size_t lane : { firstLane, secondLane })
                if (frames[lane].needsFilter)
                    filtered[lane] = voices[lane]->filter.process<true>(
                        frames[lane].input, frames[lane].omegaStep,
                        voices[lane]->feedback, frames[lane].headroom,
                        parameters.enableVcfEarlyEffect,
                        parameters.calibration,
                        frames[lane].hasTrajectory
                            ? &frames[lane].trajectory : nullptr,
                        parameters.vcfTanhMode,
                        parameters.vcfSolverMode);
        };
        processPair(0u);
        processPair(2u);
    }

    std::array<float, 4> result {};
    for (std::size_t lane = 0; lane < voices.size(); ++lane)
        if (frames[lane].needsFilter)
            result[lane] = finishVoiceFilter(*voices[lane], filtered[lane]);
    return result;
}
#endif

// ---------------------------------------------------------------------------
// Decimation
// ---------------------------------------------------------------------------

void YouKnowEngine::downsamplePair(HalfbandDecimator& decimator,
                                      float firstLeft, float firstRight,
                                      float secondLeft, float secondRight,
                                      float& outputLeft, float& outputRight) noexcept
{
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(decimatorCalls, 1);
#endif
    decimator.left[static_cast<std::size_t>(decimator.writeIndex)] = firstLeft;
    decimator.right[static_cast<std::size_t>(decimator.writeIndex)] = firstRight;
    decimator.writeIndex = (decimator.writeIndex + 1) & (halfbandRingSize - 1);
    decimator.left[static_cast<std::size_t>(decimator.writeIndex)] = secondLeft;
    decimator.right[static_cast<std::size_t>(decimator.writeIndex)] = secondRight;
    decimator.writeIndex = (decimator.writeIndex + 1) & (halfbandRingSize - 1);

    float sumLeft = 0.0f;
    float sumRight = 0.0f;
    // The real half-band kernel is exactly symmetric after float
    // normalisation. Pairing its mirrored samples halves the multiplications
    // while preserving the same 95-tap response and group delay.
    const int newest = (decimator.writeIndex - 1) & (halfbandRingSize - 1);
    const int pairs = halfbandActiveTapCount_ / 2;
    for (int pair = 0; pair < pairs; ++pair)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(decimatorNonzeroTapVisits, 2);
        YOUKNOW_COUNT_DOMAIN_WORK(decimatorStereoMacs, 2);
#endif
        const auto& first = halfbandActiveTaps_[static_cast<std::size_t>(pair)];
        const auto& second = halfbandActiveTaps_[static_cast<std::size_t>(
            halfbandActiveTapCount_ - 1 - pair)];
        const int firstIndex = (newest - first.tap) & (halfbandRingSize - 1);
        const int secondIndex = (newest - second.tap) & (halfbandRingSize - 1);
        sumLeft += first.coefficient
            * (decimator.left[static_cast<std::size_t>(firstIndex)]
               + decimator.left[static_cast<std::size_t>(secondIndex)]);
        sumRight += first.coefficient
            * (decimator.right[static_cast<std::size_t>(firstIndex)]
               + decimator.right[static_cast<std::size_t>(secondIndex)]);
    }

    const auto& centre = halfbandActiveTaps_[static_cast<std::size_t>(pairs)];
    const int centreIndex = (newest - centre.tap) & (halfbandRingSize - 1);
    sumLeft += centre.coefficient
        * decimator.left[static_cast<std::size_t>(centreIndex)];
    sumRight += centre.coefficient
        * decimator.right[static_cast<std::size_t>(centreIndex)];
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(decimatorNonzeroTapVisits, 1);
    YOUKNOW_COUNT_DOMAIN_WORK(decimatorStereoMacs, 2);
#endif

    outputLeft = sumLeft;
    outputRight = sumRight;
}

// ---------------------------------------------------------------------------
// Block processing
// ---------------------------------------------------------------------------

void YouKnowEngine::process(float* left, float* right, int numSamples)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    if (!prepared_)
    {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }

    applyPendingOversamplingIfIdle();

    // Complete a fade and rebuild at the exact host-sample boundary even when
    // that point lies inside a large host block. Processing the two pieces
    // recursively is bounded to one split: the second piece begins at zero,
    // applies the pending rate, and fades in. Besides avoiding a long mute, it
    // lets every per-call rate-derived coefficient below be recomputed for the
    // correct side of the boundary.
    if (rateTransition_ == RateTransition::FadingOut
        && oversamplingRequested_ != oversamplingApplied_
        && rateTransitionGain_ > 0.0f)
    {
        // The decrement below snaps to zero once less than half a step is
        // left, so the sample that reaches zero is round(gain / step) from
        // here; ceil() used to disagree with the float crumb the subtraction
        // leaves at 8, 48, 192, 384 and 768 kHz and split a second time.
        const int samplesUntilZero = std::max(
            1, static_cast<int>(std::floor(
                   rateTransitionGain_ / rateTransitionStep_ + 0.5f)));
        if (samplesUntilZero < numSamples)
        {
            process(left, right, samplesUntilZero);
            process(left + samplesUntilZero, right + samplesUntilZero,
                    numSamples - samplesUntilZero);
            return;
        }
    }

    const auto& parameters = activeParameters_;

    // Resolve a reference calibration once per block, before the shared
    // Tr21/C42/BA662/C41 path. Nominal is exactly unity; the profile changes
    // source level only and leaves the measured control/spectral laws intact.
    const float mainNoiseSourceScale = parameters.mainNoiseLevelScale
        * mainNoiseCalibrationScale(parameters.mainNoiseCalibrationProfile);
    // Beyond the panel's clamp by design; see chorusNoiseCalibrationScale.
    const float chorusNoiseScale = parameters.chorusNoise
        * chorusNoiseCalibrationScale(parameters.chorusNoiseCalibrationProfile);

    if (!panelGlidePrimed_)
    {
        glidedVolume_ = parameters.volume;
        panelGlidePrimed_ = true;
    }

    // Each converter destination owns a separately named hold network. VCF,
    // voice VCA, common VCA, PWM and SUB have evidence-backed post-hold
    // networks. Resonance, DCO and NOISE have no post-hold network on p. 13
    // and use ideal acquisition at the write. At the datasheet's conditional
    // 15 V/25 C maximum Ron, 2.8 us is one RC time constant, not a maximum
    // acquisition time; a full-scale 12-bit step needs about 25.23 us to
    // settle within half an LSB in that ideal RC alone. Installed driver,
    // supply and load conditions remain separate qualifications; see the
    // source inventory beside converterHoldFarads. Exact full-interval decays
    // are precomputed when the processing rate changes; only the rare interval
    // that actually contains a fractional write needs an event-position
    // exponential.
    // Hold, scan and output coefficients are refreshed by updateProcessingRate;
    // this reference keeps the sample equations below unchanged while avoiding
    // per-host-block exponentials and divisions.
    const auto& coefficients = processingCoefficients_;
    // The physical VCA service correction belongs ahead of HPF, common VCA,
    // BBD and output saturation. Cancel only its constant gain at the FINAL
    // digital boundary, after every physical stage, so this circuit fix does
    // not also add 2.138 dB of plug-in loudness or new avoidable overloads.
    // It changes the volts-to-digital reference, not any internal voltage or
    // the established outputLevelPolicyDb. Large physical transients can
    // still exceed digital full scale, as they could on the previous model.
    // The false branch retains the exact former boundary and arithmetic.
    // The configured oscillator level is cancelled the same way, so a drive
    // change does not become a loudness change; it is exactly 1 unconfigured.
    const float outputBoundaryScale = (parameters.enableVoiceVcaServiceGain
        ? coefficients.outputBoundaryGain / VoiceVcaSignalLaw::serviceGain()
        : coefficients.outputBoundaryGain) / oscillatorLevelScale_;
    // Ordinary intervals use finite engine-owned state, sanitized targets and
    // precomputed finite decays. Keep exactOnePoleHoldEndpoint's full guards
    // for the rare physical event and direct hostile-input test paths.
    const auto advanceOrdinaryOnePoleHold = [](double state, float target,
                                                double decay) noexcept {
        const double resolvedTarget = static_cast<double>(target);
        return resolvedTarget + (state - resolvedTarget) * decay;
    };
    bool patchLevelCacheValid = false;
    std::uint64_t patchLevelCacheKey = 0u;
    float patchLevelCacheValue = 0.0f;
    bool outputCouplingCacheValid = false;
    std::uint32_t outputCouplingCacheKey = 0u;
    float outputCouplingCacheGain = 0.0f;
    float outputCouplingCacheNoiseScale = 0.0f;

    for (int sample = 0; sample < numSamples; ++sample)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(hostFrames, 1);
#endif
        float outputLeft = 0.0f;
        float outputRight = 0.0f;
        bool sounding = false;

        // Two decimation stages at 4x, one at 2x, none at 1x. The inner loop
        // renders one oversampled frame.
        std::array<float, maximumOversampleFactor> stageLeft {};
        std::array<float, maximumOversampleFactor> stageRight {};

        for (int step = 0; step < oversampling_; ++step)
        {
#if defined(YOUKNOW_WORK_AUDIT)
            YOUKNOW_COUNT_DOMAIN_WORK(internalFrames, 1);
            YOUKNOW_COUNT_DOMAIN_WORK(scanPolls, 1);
#endif
            // Hold one physical clock rate throughout this internal interval,
            // before converter/PIT phase queries, for every voice and IC35.
            // advanceThermalWarmup() later computes the next interval's T.
            refreshDcoMasterClock();
            struct PhysicalPassiveHoldEvent
            {
                bool active { false };
                ConverterWrite write { ConverterDestination::Resonance, -1 };
                double position { 0.0 };
                float previousTarget { 0.0f };
                float target { 0.0f };
            } physicalHoldEvent;
            const auto currentPassiveHoldTarget = [this](
                const ConverterWrite& write) noexcept
            {
                switch (write.destination)
                {
                    case ConverterDestination::Resonance:
                        return resonanceCvTarget_;
                    case ConverterDestination::CommonVca:
                        return sharedVcaTarget_;
                    case ConverterDestination::Sub:
                        return subCvTarget_;
                    case ConverterDestination::Pwm:
                        return pwmVoltsTarget_;
                    case ConverterDestination::Vcf:
                        if (write.voice >= 0 && write.voice < hardwareVoices)
                            return voices_[static_cast<std::size_t>(write.voice)]
                                .cutoffCountsTarget;
                        break;
                    case ConverterDestination::VoiceVca:
                        if (write.voice >= 0 && write.voice < hardwareVoices)
                            return voices_[static_cast<std::size_t>(write.voice)]
                                .vcaControlTarget;
                        break;
                    case ConverterDestination::Pitch:
                    case ConverterDestination::Noise:
                        break;
                }
                return 0.0f;
            };
            const float resonanceIntervalStart = resonanceCv_;
            // One converter serves the whole instrument. The service chart
            // and the hash-matched B-2 code establish the complete ordinal
            // write order and show sequential activity across the pass. The
            // normalized timing profile preserves that qualitative fact while
            // leaving exact physical offsets open.
            bool converterPassCompleted = false;
            advanceFirmwareControlEvents(controlScanPhase_);
            if (controlScanPhase_ >= converterPassEndPhase_)
            {
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(converterPassStarts, 1);
#endif
                controlScanPhase_ -= converterPassEndPhase_;
                if (activeConverterTimingProfile_ != ConverterTimingProfile::FirmwareControlNoInterrupt)
                    for (auto& voice : voices_)
                        voice.envelope.latchGate(sustainPedalDown_);
                nextConverterWrite_ = 0;
                converterPassEnvelopeUpdated_.fill(false);
                converterPassPortamentoUpdated_ = converterNextPassPortamentoUpdated_;
                converterNextPassPortamentoUpdated_ = false;
                // The common VCA's control constant is proportional to
                // absolute temperature (patchLevelGain), and the chassis
                // follows the accelerated thermal model. Resample it here, with the
                // pass that writes the VCA's own control byte: reading it
                // once per callback instead would make the level depend on
                // how the host partitions its blocks, and reading it every
                // internal sample would spend a power for a number that
                // moves by microdecibels across a pass.
                refreshJackBoardTemperature(parameters);
                if (assignmentRescanPending_)
                    assignmentRescanPassArmed_ = true;
                if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt)
                {
                    refreshFirmwareControlTrace();
                    advanceFirmwareControlEvents(controlScanPhase_);
                }
                else
                {
                const std::int16_t bendCommand = dcoBendCommand(pitchBendTarget_);
                dcoPitchBendWord_ = dcoBendWordForCommand(
                    bendCommand, controlAdcByte(parameters.benderDcoDepth));
                vcfBendCountsWord_ = vcfBendCountsWord(
                    bendCommand, controlAdcByte(parameters.benderVcfDepth));
                advanceLfo(parameters);
                dcoLfoPitchWord_ = dcoLfoPitchWordOffset(
                    lfoAccumulator_, lfoPolarity_ >= 0.0f, lfoDelayByte_,
                    storedControlByte(parameters.dcoLfoDepth),
                    storedControlByte(modWheelTarget_),
                    controlAdcByte(parameters.benderLfoDepth));
                vcfLfoCountsWord_ = vcfLfoCountsWord(
                    lfoAccumulator_, lfoPolarity_ >= 0.0f, lfoDelayByte_,
                    storedControlByte(parameters.vcfLfoDepth));
                converterPassPwmDacCode_ = parameters.pulseEnabled
                    ? pwmDacCode(parameters.pwmDepth, parameters.pwmSource,
                                 lfoAccumulator_, lfoPolarity_ >= 0.0f)
                    : 0u;
                refreshFirmwareDcoTiming();

                }

                // Slots above the six physical cards are an explicit product
                // extension. They reuse one complete logical update at the
                // pass boundary without pretending that the B-2 scans them.
                for (int slot = hardwareVoices; slot < maxVoices; ++slot)
                {
                    auto& voice = voices_[static_cast<std::size_t>(slot)];
#if defined(YOUKNOW_WORK_AUDIT)
                    YOUKNOW_COUNT_DOMAIN_WORK(extensionScanUpdates, 1);
#endif
                    const std::uint32_t count = updateVoiceScan(
                        voice, parameters);
                    programDcoCount(voice, count, voice.dcoResetPending);
                    voice.dcoResetPending = false;
                }
            }

            const auto& writes = converterWriteOrder();
            while (nextConverterWrite_ < writes.size()
                   && converterEventPhases_[nextConverterWrite_]
                          <= controlScanPhase_ + 1.0e-12)
            {
                const auto& write = writes[nextConverterWrite_];
                const bool relevant = isPassiveHoldWrite(write);
                const bool consumesLatch = passiveHoldEventLatch_.valid
                    && passiveHoldEventLatch_.ordinal == nextConverterWrite_
                    && passiveHoldEventLatch_.write.destination
                           == write.destination
                    && passiveHoldEventLatch_.write.voice == write.voice;
                float previousTarget = 0.0f;
                if (relevant && !consumesLatch)
                    previousTarget = currentPassiveHoldTarget(write);
                const float latchedTarget = consumesLatch
                    ? passiveHoldEventLatch_.target : 0.0f;
                updatePortamentoBeforeConverterWrite(write, parameters);
                updateEnvelopeBeforeConverterWrite(write, parameters);
                performConverterWrite(
                    write, parameters,
                    consumesLatch ? &latchedTarget : nullptr);
                if (relevant && !consumesLatch && !physicalHoldEvent.active)
                {
                    physicalHoldEvent.active = true;
                    physicalHoldEvent.write = write;
                    physicalHoldEvent.position = 0.0;
                    physicalHoldEvent.previousTarget = previousTarget;
                    physicalHoldEvent.target =
                        currentPassiveHoldTarget(write);
                }
                if (consumesLatch)
                {
#if defined(YOUKNOW_WORK_AUDIT)
                    YOUKNOW_COUNT_DOMAIN_WORK(
                        passiveHoldFractionalTargetCommits, 1);
                    if (write.destination == ConverterDestination::Resonance
                        || write.destination == ConverterDestination::Vcf)
                    YOUKNOW_COUNT_DOMAIN_WORK(
                        vcfFractionalTargetCommits, 1);
#endif
                    passiveHoldEventLatch_ = {};
                }
                ++nextConverterWrite_;
                if (nextConverterWrite_ == writes.size())
                    converterPassCompleted = true;
            }

            scheduleUpcomingDcoPitchPrestages(
                controlScanPhase_, coefficients.scanPhasePerInternalSample);

            advanceFirmwareControlEvents(controlScanPhase_ + coefficients.scanPhasePerInternalSample);

            if (!physicalHoldEvent.active
                && latchUpcomingPassiveHoldEvent(
                    controlScanPhase_, coefficients.scanPhasePerInternalSample,
                    parameters))
            {
                physicalHoldEvent.active = true;
                physicalHoldEvent.write = passiveHoldEventLatch_.write;
                physicalHoldEvent.position =
                    passiveHoldEventLatch_.eventPosition;
                physicalHoldEvent.target = passiveHoldEventLatch_.target;
                physicalHoldEvent.previousTarget =
                    currentPassiveHoldTarget(physicalHoldEvent.write);
            }

            const bool resonanceEvent = physicalHoldEvent.active
                && physicalHoldEvent.write.destination
                       == ConverterDestination::Resonance;
            if (envelopeHoldsConfigured_)
                advanceEnvelopeHoldControls(
                    controlScanPhase_, coefficients.scanPhasePerInternalSample,
                    physicalHoldEvent.active && physicalHoldEvent.position > 0.0
                        && physicalHoldEvent.write.destination == ConverterDestination::VoiceVca,
                    physicalHoldEvent.write.voice, physicalHoldEvent.target,
                    physicalHoldEvent.position, parameters);
            const bool cutoffHoldEvent = physicalHoldEvent.active
                && physicalHoldEvent.write.destination
                       == ConverterDestination::Vcf;
            // IC22c drives VR26/R107/Tr18 through bare wire, so the RESO CV
            // holds its written value and steps at the write. The interval is
            // rebuilt every sample either way: the cascade reads it beside the
            // cutoff trajectory, and a stale one would replay the pre-write
            // value at the early control nodes.
            if (resonanceEvent)
            {
                resonanceVcfHoldInterval_ = steppedHoldInterval(
                    resonanceIntervalStart, true, physicalHoldEvent.position,
                    physicalHoldEvent.target);
                resonanceCv_ = resonanceVcfHoldInterval_.endpoint;
            }
            else
                resonanceVcfHoldInterval_ = steppedHoldInterval(
                    resonanceCv_, false, 1.0, resonanceCv_);
            const bool commonVcaEvent = physicalHoldEvent.active
                && physicalHoldEvent.write.destination
                       == ConverterDestination::CommonVca;
            sharedVca_ = commonVcaEvent
                ? exactOnePoleHoldEndpoint(
                    sharedVca_, physicalHoldEvent.previousTarget, true,
                    physicalHoldEvent.position, physicalHoldEvent.target,
                    coefficients.internalIntervalSeconds,
                    coefficients.commonVcaTime,
                    coefficients.commonVcaDecay)
                : advanceOrdinaryOnePoleHold(
                    sharedVca_, sharedVcaTarget_,
                    coefficients.commonVcaDecay);

            const bool pwmEvent = physicalHoldEvent.active
                && physicalHoldEvent.write.destination
                       == ConverterDestination::Pwm;
            PwmHoldState pwmState { pwmVoltsFirstPole_, pwmVolts_ };
            pwmState = exactPwmHoldEndpoint(
                pwmState,
                pwmEvent ? physicalHoldEvent.previousTarget : pwmVoltsTarget_,
                pwmEvent, physicalHoldEvent.position,
                pwmEvent ? physicalHoldEvent.target : pwmVoltsTarget_,
                coefficients.internalIntervalSeconds,
                coefficients.pwmFullInterval);
            pwmVoltsFirstPole_ = pwmState.first;
            pwmVolts_ = pwmState.second;

            const bool subEvent = physicalHoldEvent.active
                && physicalHoldEvent.write.destination
                       == ConverterDestination::Sub;
            subCv_ = subEvent
                ? exactOnePoleHoldEndpoint(
                    subCv_, physicalHoldEvent.previousTarget, true,
                    physicalHoldEvent.position, physicalHoldEvent.target,
                    coefficients.internalIntervalSeconds,
                    static_cast<double>(subHoldSlewSeconds),
                    coefficients.subDecay)
                : advanceOrdinaryOnePoleHold(
                    subCv_, subCvTarget_, coefficients.subDecay);
            // IC26's C85 has no post-hold network. Direct assignment is
            // ideal acquisition; the datasheet RC coordinate is not an
            // installed settling bound (see converterHoldFarads).
            noiseCv_ = noiseCvTarget_;
            advanceThermalWarmup();
            advanceRailRipple(parameters);

            if (--driftControlCountdown_ <= 0)
            {
                // A fixed wall-clock rate. Counting internal samples instead
                // would make the modelled component wander four times faster
                // with oversampling on, so the same patch would drift
                // differently depending on a quality setting.
                driftControlCountdown_ = std::max(
                    1, static_cast<int>(oversampledRate_ / driftUpdateHz));
                // The gradient's cutoff factor rides the same clock as the
                // Johnson scales now that it develops with the warm-up.
                refreshVoiceCardThermalScales();
                for (auto& card : cards_)
                    updateVoiceCardDrift(card);
                // The holds' leakage ramps ride the same wall clock, one
                // step per tick, for the same reason.
                applyConverterHoldDroop(
                    parameters,
                    static_cast<float>(driftControlCountdown_)
                        * static_cast<float>(inverseOversampledRate_));
            }

            // One noise generator feeds every voice, so noise grows as more
            // keys are held instead of staying put. Its support circuit
            // band-shapes the rail: C42 into the level OTA's 4.7 kOhm input
            // bias high-passes at 33.9 Hz, the BA662 applies the scanned level,
            // and C41 against R79 low-passes that controlled output at
            // 4.82 kHz. One state still serves every voice; the passband below
            // that corner stays at unity, so in-band density keeps its
            // established rate normalisation there. Above it the bilinear
            // one-pole's zero at Nyquist thins the coarse grids against the
            // analogue pole -- about -1.2 dB at 10 kHz and -4 dB at 16 kHz
            // on a 48 kHz/1x grid, -0.5 dB over 0-20 kHz -- a numerical
            // limitation of the HQ-off rungs, not a modelled mechanism.
            if (noiseState_ == 0u)
                noiseState_ = 0x6d2b79f5u;
            const float rawNoise =
                gaussianFromNoiseState() * noiseRateScale_
                * mainNoiseSourceScale;
            // The hold voltage is what moves; Tr22 converts it to control
            // current instantaneously, so the onset law is applied after the
            // hold and ahead of both the C41-driven and legacy level paths.
            const float noiseDrive = parameters.useCircuitDerivedNoiseLevelShape
                ? CircuitDerivedNoiseLevelProfile::drive(noiseCv_)
                : noiseCv_;
            const float noiseSample = processMainNoiseSource(
                rawNoise, noiseDrive, parameters.enableNoiseLevelBeforeC41);

            // This audio-energy proxy produces a voiced shared cutoff shift;
            // it does not identify actual card current or regulator impedance.
            // The rectifier's ripple is carried by advanceRailRipple at the
            // sheet's one figure: Service Notes p. 16 gives a 3300 uF
            // reservoir per rail behind a 0.25 A secondary, so drawn at that
            // rating the unregulated sawtooth is 0.63 Vpp at 60 Hz mains
            // (0.76 Vpp at 50 Hz). The cards run on IC2, an M5230L dual
            // tracking regulator (p. 16), and its data sheet -- reproduced in
            // the 1987 Mitsubishi General Purpose ICs databook, p. 4-8 --
            // specifies ripple rejection RR = 68 dB at f = 120 Hz, measured
            // with its own circuit (b) at ei = 0 dBm, alongside output noise
            // VNO = 12 uVrms over 20 Hz-100 kHz, input regulation 0.02 %/V typ
            // and 0.1 %/V max, and load regulation 0.02 % typ and 0.1 % max.
            // Ripple rejection is typical with no published minimum. The
            // sawtooth's 120 Hz fundamental through that figure is 80 uV
            // peak at the rail, 0.003 cents peak through the voiced cutoff
            // transfer below; its harmonics have no published rejection and
            // are left out. Neither the rated-load assumption nor this
            // transfer establishes installed sidebands or a general
            // audibility limit.
            //
            // A second conditional estimate concerns the converter reference,
            // the output-high level of the 4050 buffers IC30-32
            // on the +5.0 V net (p. 13), and IC3 -- an M5231L, the single
            // regulator whose own sheet gives RR = 62 dB typ at 120 Hz --
            // derives that net from the already-regulated +15 V through R11
            // 100 ohm (p. 16). The two rejections cascade to about 0.05 ppm of
            // 5 V, two ten-thousandths of a 12-bit LSB, so the common-mode path
            // in that simplified cascaded-ripple calculation. This does not
            // bound local load transients, ground bounce, driver recovery or
            // the installed load-to-reference transfer.
            //
            // Keep the sum as an explicit proxy. Unit Character scales its
            // consequence once, where the cutoff shift is applied.
            float totalVoiceEnergy = 0.0f;
            for (const auto& v : voices_)
            {
                if (v.active)
                    totalVoiceEnergy += v.energy;
            }
            powerSupplyDroop_ = totalVoiceEnergy * 0.0015f;

            float mono = 0.0f;
            float loudestEnvelope = 0.0f;

            // Every slot, not just the first `limit` of them. The voice count
            // bounds what the key assigner may take; lowering it must stop new
            // notes rather than freeze notes that are already sounding.
            const auto updateVoiceForInterval = [&](int slot) {
                auto& voice = voices_[static_cast<std::size_t>(slot)];
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(holdVoiceUpdates, 1);
#endif
                // Each hold capacitor's own slew turns the scan's staircase
                // back into a continuous control voltage before it reaches
                // its converter. The amplifier's hold is the slow one.
                const bool cutoffEvent = cutoffHoldEvent
                    && physicalHoldEvent.write.destination
                           == ConverterDestination::Vcf
                    && physicalHoldEvent.write.voice == slot;
                const float cutoffIntervalStart = voice.cutoffCounts;
                if (cutoffEvent)
                {
                    cutoffVcfHoldIntervals_[static_cast<std::size_t>(slot)] =
                        exactVcfHoldInterval(
                            cutoffIntervalStart,
                            physicalHoldEvent.previousTarget, true,
                            physicalHoldEvent.position,
                            physicalHoldEvent.target,
                            coefficients.internalIntervalSeconds);
                    voice.cutoffCounts =
                        cutoffVcfHoldIntervals_[static_cast<std::size_t>(slot)]
                            .endpoint;
                }
                else if (resonanceEvent)
                {
                    cutoffVcfHoldIntervals_[static_cast<std::size_t>(slot)] =
                        exactVcfHoldInterval(
                            cutoffIntervalStart, voice.cutoffCountsTarget,
                            false, 1.0, voice.cutoffCountsTarget,
                            coefficients.internalIntervalSeconds);
                    voice.cutoffCounts =
                        cutoffVcfHoldIntervals_[static_cast<std::size_t>(slot)]
                            .endpoint;
                }
                else
                {
                    voice.cutoffCounts +=
                        (voice.cutoffCountsTarget - voice.cutoffCounts)
                            * coefficients.vcfSlew;
                }
                // Every slot reaches this store before renderVoice can read its
                // entry, so clearing the whole array at the interval boundary
                // would only write the same values twice.
                exactVcfControlInterval_[static_cast<std::size_t>(slot)] =
                    resonanceEvent || cutoffEvent;
                // IC24's per-voice hold has no identified acquisition law.
                // Ideal acquisition at the existing converter timestamp T
                // changes current now, preserving the charge from before T.
                updateDcoHeldCv(voice, voice.dcoCvTarget);
                const bool voiceVcaEvent = physicalHoldEvent.active
                    && physicalHoldEvent.write.destination
                           == ConverterDestination::VoiceVca
                    && physicalHoldEvent.write.voice == slot;
                if (envelopeHoldsConfigured_ && slot < hardwareVoices)
                {
                    // The independent upstream capacitor and C58 have already
                    // advanced together through this interval's mux edges.
                }
                else if (parameters.enableCoupledVoiceVcaControl)
                {
                    const auto& circuit = voiceVcaControlCircuit();
                    const double dt = coefficients.internalIntervalSeconds;
                    if (voiceVcaEvent)
                    {
                        voice.vcaControl = circuit.advance(
                            voice.vcaControl, physicalHoldEvent.previousTarget,
                            dt * physicalHoldEvent.position);
                        voice.vcaControl = circuit.advance(
                            voice.vcaControl, physicalHoldEvent.target,
                            dt * (1.0 - physicalHoldEvent.position));
                    }
                    else
                        voice.vcaControl = circuit.advance(
                            voice.vcaControl, voice.vcaControlTarget, dt);
                }
                else
                    voice.vcaControl = voiceVcaEvent
                    ? exactOnePoleHoldEndpoint(
                        voice.vcaControl,
                        physicalHoldEvent.previousTarget, true,
                        physicalHoldEvent.position,
                        physicalHoldEvent.target,
                        coefficients.internalIntervalSeconds,
                        static_cast<double>(voiceVcaHoldSlewSeconds),
                        coefficients.voiceVcaDecay)
                    : advanceOrdinaryOnePoleHold(
                        voice.vcaControl, voice.vcaControlTarget,
                        coefficients.voiceVcaDecay);
                // Neither value is needed for an inactive extension slot
                // (`!voice.active && cardIndex >= hardwareVoices`, and
                // `cardIndex` is always the slot). A freewheeling physical card
                // still needs the comparator duty when the Pulse-Off WAVE-node
                // model is live: its slow C56/C50 state follows the endpoint at
                // low carrier rates and that duty mean at high rates. The
                // expensive filter/audio coefficients remain skipped unless
                // the coupled mixer keeps the complete physical card live.
                const bool freewheels = !voice.active
                    && parameters.vcfTanhMode != VcfTanhMode::Exact
                    && !coupledMixerEnabled_;
                const bool hasAudioCell = voice.active || slot < hardwareVoices;
                if (hasAudioCell
                    && (!freewheels
                        || parameters.enablePulseOffWaveNodeCoupling))
                    updatePulseComparator(voice, parameters);
                if (hasAudioCell && !freewheels)
                {
                    updateVoiceAudio(voice, parameters);
                }
            };
            const auto accountRenderedVoice = [&](Voice& voice, float output) {
                mono += output;
                loudestEnvelope = std::max(
                    loudestEnvelope, voice.envelope.value);

                // Retire on the modelled amplifier actually being shut. A
                // card's optional control offset can hold the control voltage
                // just above the turn-on for ever, where the grounded-base
                // stage still passes an inaudible trickle; without an explicit
                // silence threshold a voice at -100 dB would block a deferred
                // quality change indefinitely.
                if (voice.envelope.stage == EnvelopeStage::Idle
                    && !voice.keyDown && !voice.sustained
                    && voice.vcaGain <= VoiceVcaControlLaw::silenceGain)
                    silenceVoice(voice);
                else
                    sounding = true;
            };
            const auto renderVoices = [&]<bool useCubicEarly> {
                for (int slot = 0; slot < maxVoices;)
                {
                    auto& voice = voices_[static_cast<std::size_t>(slot)];
                    updateVoiceForInterval(slot);

#if defined(YOUKNOW_HAS_VCF_PAIR_SIMD) \
    && !defined(YOUKNOW_WORK_AUDIT)
                    if constexpr (useCubicEarly)
                    {
                        if (slot + 3 < maxVoices && voice.active
                            && voices_[static_cast<std::size_t>(slot + 1)].active
                            && voices_[static_cast<std::size_t>(slot + 2)].active
                            && voices_[static_cast<std::size_t>(slot + 3)].active
                            && parameters.vcfTanhMode
                                   == VcfTanhMode::PolyZoned
                            && parameters.vcfSolverMode
                                   == VcfSolverMode::Rk4Single)
                        {
                            for (int offset = 1; offset < 4; ++offset)
                                updateVoiceForInterval(slot + offset);
                            const std::array<Voice*, 4> group {
                                &voice,
                                &voices_[static_cast<std::size_t>(slot + 1)],
                                &voices_[static_cast<std::size_t>(slot + 2)],
                                &voices_[static_cast<std::size_t>(slot + 3)]
                            };
                            const auto outputs = renderVoiceQuad(
                                group, parameters, noiseSample);
                            for (std::size_t lane = 0; lane < group.size();
                                 ++lane)
                                accountRenderedVoice(
                                    *group[lane], outputs[lane]);
                            slot += 4;
                            continue;
                        }
                        if (slot + 1 < maxVoices && voice.active
                            && voices_[static_cast<std::size_t>(slot + 1)].active
                            && parameters.vcfTanhMode
                                   == VcfTanhMode::PolyZoned
                            && parameters.vcfSolverMode
                                   == VcfSolverMode::Rk4Single)
                        {
                            auto& second = voices_[
                                static_cast<std::size_t>(slot + 1)];
                            updateVoiceForInterval(slot + 1);
                            const auto outputs = renderVoicePair(
                                voice, second, parameters, noiseSample);
                            accountRenderedVoice(voice, outputs[0]);
                            accountRenderedVoice(second, outputs[1]);
                            slot += 2;
                            continue;
                        }
                    }
#endif
                    if (!voice.active)
                    {
                        // The six physical DCO/filter cards remain powered behind
                        // their closed VCAs. Extension slots have no card state to
                        // advance, so avoid entering renderVoice only to take its
                        // identical early return.
                        if (voice.cardIndex < hardwareVoices)
                            renderVoice<useCubicEarly>(
                                voice, parameters, noiseSample);
                        ++slot;
                        continue;
                    }

                    accountRenderedVoice(
                        voice, renderVoice<useCubicEarly>(
                            voice, parameters, noiseSample));
                    ++slot;
                }
            };
            // The cubic Early multiplier is the shipped default since the
            // 2026-08-24 CPU pass, so neither arm is the unlikely one.
            if (useCubicEarly_)
                renderVoices.template operator()<true>();
            else
                renderVoices.template operator()<false>();

            // IC35/TP5 is shared by all six channels of the two M82C53s. Each
            // card-local event walk above read the same left-boundary phase;
            // advance that one physical divider once after every card consumes
            // this interval.
            advanceRangeClock(parameters.range);
            // Publish the next boundary's temperature only after all voices
            // and the shared prescaler consumed the preceding clock rate.
            refreshDcoMasterClock();
            displayEnvelope_ = loudestEnvelope;

            // The POLY/unison handler gated and cleared at the host event.
            // Reassign only after the complete ordered pass, so every physical
            // envelope has observed gate-off before any replacement Note On.
            if (activeConverterTimingProfile_ != ConverterTimingProfile::FirmwareControlNoInterrupt
                && converterPassCompleted && assignmentRescanPending_
                && assignmentRescanPassArmed_)
                completeVoiceAssignmentRescan();
            controlScanPhase_ += coefficients.scanPhasePerInternalSample;
            if (activeConverterTimingProfile_ == ConverterTimingProfile::FirmwareControlNoInterrupt
                && controlScanPhase_ >= converterPassEndPhase_)
            {
                advanceFirmwareControlEvents(controlScanPhase_);
                controlScanPhase_ -= converterPassEndPhase_;
                nextConverterWrite_ = 0;
                converterPassEnvelopeUpdated_.fill(false);
                converterPassPortamentoUpdated_ = converterNextPassPortamentoUpdated_ = false;
                refreshJackBoardTemperature(parameters);
                // Complete the logical assigner rescan only when the entire
                // 02EC→07B5 pass has returned, after every off-gate VCA hold.
                if (assignmentRescanPending_ && assignmentRescanPassArmed_)
                    completeVoiceAssignmentRescan();
                if (assignmentRescanPending_) assignmentRescanPassArmed_ = true;
                refreshFirmwareControlTrace();
                advanceFirmwareControlEvents(controlScanPhase_);
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(converterPassStarts, 1);
#endif
                for (int slot = hardwareVoices; slot < maxVoices; ++slot)
                {
                    auto& voice = voices_[static_cast<std::size_t>(slot)];
                    voice.envelope.latchGate(sustainPedalDown_);
                    const auto count = updateVoiceScan(voice, parameters);
                    programDcoCount(voice, count, voice.dcoResetPending);
                    voice.dcoResetPending = false;
                }
            }

            // One high-pass, on the summed voices. The schematic carries a
            // single set of parts for it -- on the jack board, downstream of
            // the summing amplifier -- not one set per voice, so this is where
            // it belongs. An earlier revision ran it inside each voice ahead of
            // that voice's own filter, which is a different circuit: a
            // high-pass feeding a resonant lowpass is not the same as one
            // following it, because what the high-pass removes is what the
            // resonance would otherwise have had to work on.
            const float busIn = voiceBusInput(mono);
            float effectiveCouplingG = voiceBusCouplingG_;
            if (parameters.enableElectrolyticC14Nonlinearity && parameters.calibration > 0.0f)
            {
                const float inputMagnitude = std::abs(busIn);
                const float capMod = 1.0f + 0.15f * (inputMagnitude / (1.0f + inputMagnitude))
                                   * parameters.calibration;
                effectiveCouplingG *= capMod;
            }
            const float coupled = voiceBusCoupling_.process(
                busIn, effectiveCouplingG, 0.0f, 1.0f);
            // IC3 selects which leg IC4a's summing node is driven from, but
            // it does not disconnect the leg it just left: that leg's 47 kOhm
            // is unswitched, so its capacitor keeps discharging through its
            // own 1 MOhm bleed into the same summing node. The shared filter
            // still runs every sample whichever leg is selected so the legacy
            // switch below stays bit-identical; with the physical legs on,
            // Boost is rendered by its own three-capacitor branch and One is
            // a bare wire, so the shared state is only read on the legacy path.
            const float sharedLeg = highPass_.process(coupled,
                                                      highPassG_,
                                                      highPassShelf_,
                                                      highPassHigh_);
            float shaped = sharedLeg;
            if (parameters.enableHighPassDepartingLegTail)
            {
                const bool twoActive =
                    parameters.highPass == HighPassMode::Two;
                const bool threeActive =
                    parameters.highPass == HighPassMode::Three;
                const bool boostActive =
                    parameters.highPass == HighPassMode::Boost;
                // Runs in both configurations: driven while selected, and
                // discharging its stored charge into the same summing node
                // while not. Its selected output replaces the shelf.
                const float boostLeg = processBoostBranch(coupled, boostActive);
                const float twoLeg = twoActive
                    ? highPassTwoLeg_.process(coupled, highPassG_, 0.0f, 1.0f)
                    : highPassTwoLeg_.process(0.0f, highPassTwoDepartG_,
                                              1.0f, 0.0f);
                const float threeLeg = threeActive
                    ? highPassThreeLeg_.process(coupled, highPassG_, 0.0f, 1.0f)
                    : highPassThreeLeg_.process(0.0f, highPassThreeDepartG_,
                                                1.0f, 0.0f);
                shaped = twoActive ? twoLeg
                       : threeActive ? threeLeg
                       : boostActive ? boostLeg
                       : sharedLeg;
                // output = +R29 * i and the departing current is
                // i = -Vc / (R_bleed + R_sum), hence the sign.
                if (!twoActive)
                    shaped -= highPassDepartRatio * twoLeg;
                if (!threeActive)
                    shaped -= highPassDepartRatio * threeLeg;
                // The boost branch's node voltages already carry their own
                // sign into the summing node, selected or not.
                if (!boostActive)
                    shaped += boostLeg;
            }

            if (highPassSwitchResistance_ > 0)
                shaped = static_cast<float>(highPassSwitch_.process(
                    busIn, static_cast<int>(parameters.highPass), [](double value) {
                        return static_cast<double>(outputSummerClip(static_cast<float>(value)));
                    }));

            // VCA LEVEL is the one common uPC1252H2 on the jack board, after
            // the voice sum and HPF. The six voice-module VCAs above are driven
            // only by ENV/GATE (plus the optional velocity extension).
            const float vcaInput = commonVcaInputCoupling_.process(
                shaped, commonVcaInputCouplingG_, 0.0f, 1.0f);
            const float patchLevelInput = static_cast<float>(sharedVca_);
            const auto patchLevelKey =
                (static_cast<std::uint64_t>(
                     std::bit_cast<std::uint32_t>(patchLevelInput)) << 32)
                | std::bit_cast<std::uint32_t>(jackBoardCelsius_);
            if (!patchLevelCacheValid || patchLevelCacheKey != patchLevelKey)
            {
                patchLevelCacheValue =
                    patchLevelGain(patchLevelInput, jackBoardCelsius_);
                patchLevelCacheKey = patchLevelKey;
                patchLevelCacheValid = true;
            }
            float levelled = vcaInput * patchLevelCacheValue;
            // The uPC1252H2's own output noise (see commonVcaOutputNoiseDbv)
            // appears at IC2b's output regardless of what the bus carries, so
            // it is added here, ahead of the dry/wet split, and the dry and
            // both wet legs carry it. Unit Character 0 keeps the exact-
            // silence calibrated nominal, exactly as the resistor floors do.
            if (parameters.enableCommonVcaNoise)
            {
                commonVcaNoiseState_ = xorshift32(commonVcaNoiseState_);
                levelled += bipolarFromState(commonVcaNoiseState_)
                          * coefficients.commonVcaNoiseScale
                          * parameters.calibration;
            }

            // The chorus input coupling capacitors sit in its two wet branches;
            // dry bypasses them. IC6 applies its component-derived dry/wet
            // gains when they recombine. Its +/-15 V clipping point has not
            // been measured under the output load, so no invented low-voltage
            // rail is inserted here; the main volume control follows it.
            float wetLeft = levelled;
            float wetRight = levelled;
            // A switched-off, fully settled chorus outputs bit-exactly the
            // dry routing whatever its muted lines hold, so the fast tanh
            // modes skip the BBD work behind it; the wet path rebuilds from
            // silence on engage. Exact keeps the established always-running
            // lines -- the same policy split as the voice-card freewheel.
            if (parameters.chorus != ChorusMode::Off
                || parameters.vcfTanhMode == VcfTanhMode::Exact
                || parameters.enableChorusClockMuteCircuit
                || !chorus_.processBypassedWhenSettled(levelled, wetLeft,
                                                       wetRight))
                chorus_.process(levelled, parameters.chorus,
                                chorusNoiseScale,
                                wetLeft, wetRight,
                                parameters.enableChorusClockBleed,
                                parameters.enableChorusHyperbolicSweep,
                                parameters.calibration,
                                parameters.useChorusRateNoiseHypothesis,
                                parameters.enableNarrowOneTwoChorus,
                                parameters.enableChorusMuteDrive,
                                parameters.enableChorusLineGainSpread,
                                parameters.chorusTimingProfile,
                                parameters.enableChorusClockMuteCircuit);

            // TA75558S IC6 has finite loaded output swing inside its +/-15 V
            // supplies. The modelled 13.5 V asymptote and knee are provisional
            // OQ-05 policy, not per-card tolerances, so Unit Character does not
            // scale them.
            const auto wetLeftKey = std::bit_cast<std::uint32_t>(wetLeft);
            const auto wetRightKey = std::bit_cast<std::uint32_t>(wetRight);
            wetLeft = outputSummerClip(wetLeft);
            // outputSummerClip's asymptote and exponent are fixed. Reuse only
            // for an identical float representation, preserving signed zero
            // and NaN payload distinctions as well as unequal stereo samples.
            wetRight = wetLeftKey == wetRightKey
                     ? wetLeft : outputSummerClip(wetRight);

            // TA75558S IC6 output slew limit. The datasheet's 1.0 V/us is a
            // typical value at unity gain and 2 kOhm, not a guaranteed limit
            // at the installed load. As a part policy -- like the shared swing
            // above -- it is not scaled by Unit Character: a previous revision
            // divided it by calibration, granting the pristine reference a
            // 10x faster op-amp and a full-character unit one slower than the
            // part's own datasheet.
            if (parameters.enableOpAmpSlewLimiting)
            {
                const float deltaL = wetLeft - outputSlewStateLeft_;
                outputSlewStateLeft_ += std::clamp(
                    deltaL, -coefficients.outputSlewMaxStep,
                    coefficients.outputSlewMaxStep);
                wetLeft = outputSlewStateLeft_;

                const float deltaR = wetRight - outputSlewStateRight_;
                outputSlewStateRight_ += std::clamp(
                    deltaR, -coefficients.outputSlewMaxStep,
                    coefficients.outputSlewMaxStep);
                wetRight = outputSlewStateRight_;
            }
            else
            {
                outputSlewStateLeft_ = wetLeft;
                outputSlewStateRight_ = wetRight;
            }

            // TA75558S open-loop gain is finite. With both signal legs
            // connected, the feedback/input resistor network sets the noise
            // gain and turns the part's 3 MHz typical GBW into the derived
            // closed-loop pole above. At ordinary rates the exponential is
            // effectively one (as the real pole is far above Nyquist); at
            // high-rate/offline operation this state supplies the missing
            // ultrasonic roll-off without adding another quality domain.
            outputBandwidthStateLeft_ +=
                coefficients.outputSummerBandwidthBlend
                * (wetLeft - outputBandwidthStateLeft_);
            outputBandwidthStateRight_ +=
                coefficients.outputSummerBandwidthBlend
                * (wetRight - outputBandwidthStateRight_);
            wetLeft = outputBandwidthStateLeft_;
            wetRight = outputBandwidthStateRight_;

            stageLeft[static_cast<std::size_t>(step)] = wetLeft;
            stageRight[static_cast<std::size_t>(step)] = wetRight;
        }

        if (oversampling_ == 4)
        {
            float firstLeft = 0.0f;
            float firstRight = 0.0f;
            float secondLeft = 0.0f;
            float secondRight = 0.0f;
            downsamplePair(firstDecimator_, stageLeft[0], stageRight[0],
                           stageLeft[1], stageRight[1], firstLeft, firstRight);
            downsamplePair(firstDecimator_, stageLeft[2], stageRight[2],
                           stageLeft[3], stageRight[3], secondLeft, secondRight);
            downsamplePair(secondDecimator_, firstLeft, firstRight,
                           secondLeft, secondRight, outputLeft, outputRight);
        }
        else if (oversampling_ == 2)
        {
            downsamplePair(firstDecimator_, stageLeft[0], stageRight[0],
                           stageLeft[1], stageRight[1], outputLeft, outputRight);
        }
        else
        {
            outputLeft = stageLeft[0];
            outputRight = stageRight[0];
        }

        applyLatencyPad(outputLeft, outputRight);

        glidedVolume_ +=
            (parameters.volume - glidedVolume_) * coefficients.outputGlide;

        // C17/C20, R54/R57 and VR1 are one loaded network, not a fixed pole
        // followed by an unrelated gain. The 41.3 kOhm selector ladder and
        // 101 kOhm headphone input load each wiper at every shaft position;
        // moving Volume changes both the settled gain and the resistance seen
        // by the still-continuous capacitor state. outputCouplingCornerHz(float)
        // and outputCouplingHighGain(float) each solve that identical wiper
        // network independently -- fine for the two callers that only want one
        // of the two values, but this call site always wants both, so it is
        // solved once here and both results are read off the one network.
        const auto outputCouplingKey =
            std::bit_cast<std::uint32_t>(glidedVolume_);
        float outputCouplingGain;
        float outputWiperNoiseScale;
        if (!outputCouplingCacheValid
            || outputCouplingCacheKey != outputCouplingKey)
        {
            const auto outputCouplingNetwork =
                outputCouplingWiperNetworkFor(glidedVolume_);
            const float outputCouplingCorner = 1.0f
                / (twoPi * outputCouplingCapacitanceF
                   * outputCouplingNetwork.resistance);
            outputCouplingG_ = std::tan(
                pi * outputCouplingCorner * inverseSampleRate_);
            // Preserve the above-Nyquist circuit's in-band magnitude at
            // ordinary host rates; derivation in YouKnowOutputJack.h.
            outputJackCoefficients_ = OutputJackLowPass::coefficients(
                outputJackCornerHzFor(outputCouplingNetwork), sampleRate_);
            outputCouplingGain = outputCouplingNetwork.loadedLower > 0.0f
                ? outputCouplingNetwork.loadedLower
                    / outputCouplingNetwork.resistance
                : 0.0f;
            outputCouplingCacheKey = outputCouplingKey;
            outputCouplingCacheGain = outputCouplingGain;
            const float wiperNoiseDensity = std::sqrt(
                4.0f * boltzmannConstant * outputNoiseTemperatureKelvin
                * outputWiperNoiseResistance(glidedVolume_));
            outputWiperNoiseScale = wiperNoiseDensity
                * std::sqrt(1.5f * static_cast<float>(sampleRate_))
                * voltsToSample;
            outputCouplingCacheNoiseScale = outputWiperNoiseScale;
            outputCouplingCacheValid = true;
        }
        else
        {
            outputCouplingGain = outputCouplingCacheGain;
            outputWiperNoiseScale = outputCouplingCacheNoiseScale;
        }
        outputNoiseStateLeft_ = xorshift32(outputNoiseStateLeft_);
        outputNoiseStateRight_ = xorshift32(outputNoiseStateRight_);
        // Unit Character zero is the project's deterministic calibrated-
        // nominal reference, including an exact digital silence floor; one is
        // the declared physical reference. Keep that established endpoint
        // contract while introducing the derived real-resistor floor.
        const float outputNoiseLeft = bipolarFromState(outputNoiseStateLeft_)
                                    * parameters.calibration;
        const float outputNoiseRight = bipolarFromState(outputNoiseStateRight_)
                                     * parameters.calibration;
        const float summerNoiseScale =
            coefficients.outputSummerNoiseScale * jackBoardJohnsonScale_;
        outputLeft += outputNoiseLeft * summerNoiseScale;
        outputRight += outputNoiseRight * summerNoiseScale;
        outputLeft = outputCouplingLeft_.process(
            outputLeft, outputCouplingG_, 0.0f, outputCouplingGain);
        outputRight = outputCouplingRight_.process(
            outputRight, outputCouplingG_, 0.0f, outputCouplingGain);
        outputWiperNoiseStateLeft_ = xorshift32(outputWiperNoiseStateLeft_);
        outputWiperNoiseStateRight_ = xorshift32(outputWiperNoiseStateRight_);
        const float wiperNoiseScale =
            outputWiperNoiseScale * jackBoardJohnsonScale_;
        outputLeft += bipolarFromState(outputWiperNoiseStateLeft_)
                    * parameters.calibration * wiperNoiseScale;
        outputRight += bipolarFromState(outputWiperNoiseStateRight_)
                     * parameters.calibration * wiperNoiseScale;
        // C22/C21 with R64/R65: the jack node the plug sees. The coupling and
        // the wiper's noise both sit behind the 2.2 kOhm, so the pole follows
        // them. It is the nominal circuit's, so Unit Character does not scale
        // it; see OutputJackLowPass for the numerical matching policy.
        outputLeft = outputJackLeft_.process(outputLeft, outputJackCoefficients_);
        outputRight = outputJackRight_.process(outputRight, outputJackCoefficients_);

        // How long the voices have been gone, which is what a pending quality
        // change waits on: the output path needs that long to run dry.
        if (sounding)
            oversamplingIdleSamples_ = 0;
        else if (oversamplingIdleSamples_ < oversamplingQuietSamples_)
            ++oversamplingIdleSamples_;

        const float transitionGain = rateTransitionGain_;
        left[sample] = std::isfinite(outputLeft)
                     ? outputLeft * outputBoundaryScale
                           * transitionGain
                     : 0.0f;
        right[sample] = std::isfinite(outputRight)
                      ? outputRight * outputBoundaryScale
                            * transitionGain
                      : 0.0f;

        if (rateTransition_ == RateTransition::FadingOut)
        {
            // Land exactly on zero at the round(gain / step) sample the split
            // in process() scheduled. Subtracting the step in float leaves a
            // crumb of about 1e-6 at the rates where 0.005 * fs is an integer
            // (8, 48, 192, 384, 768 kHz), which used to cost one more split
            // and put the rebuild one host sample after the boundary.
            rateTransitionGain_ -= rateTransitionStep_;
            if (rateTransitionGain_ < 0.5f * rateTransitionStep_)
                rateTransitionGain_ = 0.0f;
        }
        else if (rateTransition_ == RateTransition::FadingIn)
        {
            rateTransitionGain_ = std::min(
                1.0f, rateTransitionGain_ + rateTransitionStep_);
            if (rateTransitionGain_ >= 1.0f)
                rateTransition_ = RateTransition::Idle;
        }
    }

    updateActiveVoiceCount();
}

} // namespace youknow

#if defined(YOUKNOW_WORK_AUDIT)
#undef YOUKNOW_COUNT_DOMAIN_WORK
#endif
