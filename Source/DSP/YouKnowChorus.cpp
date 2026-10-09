#include "YouKnowChorus.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <type_traits>

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
// Shared double-precision pi. Four support-network builders below and the
// deterministic tone/heterodyne-bleed oscillators each wrote this literal out
// independently; one file-scope constant keeps them from drifting apart.
constexpr double pi = 3.14159265358979323846;

// Aggregate charge-transfer inefficiency of the whole line, condensed to one
// pole advanced at the BBD clock rate. The MN3009's 12 kHz bandwidth row at a
// 40 kHz clock describes the complete part, including the rectangular output
// hold, and is referenced to 1 kHz. This model's hold contributes
// sinc(12/40) = -1.326 dB versus DC, so the residual pole contributes -1.674 dB
// rather than another -3 dB. Solving at 0.3 cycles/sample gives -3.000 dB versus
// DC and -2.972 dB versus 1 kHz, within 0.03 dB of that datasheet row. The state
// advances once per modeled BBD shift (one fCP period), so a fixed coefficient
// already makes the pole's absolute frequency follow the clock. Scaling it
// again from clockHz would change the normalized response and double-count it.
//
// The 2026-08-07 two-phase output-stage solve confirmed this hold-plus-
// residual-pole structure outright -- OUT1/OUT2 present the same sample on
// complementary half-cycles, so their sum is a full-period hold -- but left
// the typical part's coefficient an unresolved span, because the datasheet's
// Gi-fi and Gi-fcp panels contradict each other about what the curves
// measure. The guaranteed-minimum anchor below sits inside every candidate
// reading's band and stands; the suites fence the cross-reading guard band.
constexpr float transferSmear = 0.8654743f;

// Static transfer of the delay line, referred to the model's signal scale
// (1.0 = 2.6 V at the node). A plain tanh with a 2.9 V asymptote already bends
// too far below the rails: it gives about 1.2% THD at the part's 0.78 Vrms
// test point instead of the specified typical 0.3%. The datasheet's 1.5 Vrms
// input-swing row is a guaranteed minimum measured at the 2.5% THD criterion,
// not a second typical-distortion anchor. This constrained algebraic clip keeps
// the approximately 2.9 V asymptote while fitting 0.3% at 0.78 Vrms and the
// typical curve's approximately 2% at 2.0 Vrms. Its quadratic term supplies
// the gentle low-level bend independently of the steep near-rail closure.
constexpr float bbdSaturationLevel = 1.1246614f; // 2.924 V at the node
constexpr float bbdSaturationCurvature = 1.2044546f;
constexpr float bbdSaturationExponent = 12.9395323f;

// Panasonic's printed p.38 THD-Vbias curve distinguishes the large-swing
// optimum near -8.3 V (about 0.46% at 1 kHz/0 dBm/40 kHz) from the minimum
// small-signal THD near -9.1 V. Roland p.19 balances clipping at the former
// criterion rather than specifying minimum low-level THD. The new candidate
// conditionally subtracts noise power using a declared 1.25 mVrms prior,
// midpoint of the THD-Vi-derived 1.1-1.4 mVrms bracket documented in the
// header. sqrt(.0046^2-(.00125/.78)^2) gives about .43% harmonic distortion;
// the noise bracket gives .4235-.4378%. Dense Fourier quadrature solves only
// this curvature in the established 0.78 Vrms coordinate, retaining the
// exponent, unity tangent and 2.924 V asymptote. At 2 Vrms it predicts about
// 2.21%, within the graph's approximate 2-2.5% high-level reading. The bias
// and level plots' unspecified measurement bandwidths need not agree: this
// deembedding is a conservative named hypothesis, not identification of the
// installed bias, harmonic distribution or noise split. The existing noise
// source is retained rather than counting all .46% again as harmonics.
// Do not add a clock multiplier: the THD-fcp graph includes
// filter-dependent aliases/noise already carried by clock sampling.
// https://www.ka-electronics.com/images/pdf/Panasonic_BBD.pdf#page=40
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=19
constexpr float bbdServicedBiasCurvature = 1.8116256f;

double bbdTransferBase(double normalised) noexcept
{
    return 1.0
         + static_cast<double>(bbdSaturationCurvature)
               * normalised * normalised
         + std::pow(normalised,
                    static_cast<double>(bbdSaturationExponent));
}

// The fitted exponent is non-integral, so there is no multiply/root identity
// analogous to the output summer's fixed eighth power. Sample the reference
// normalized curve through four rails instead; beyond that already-saturated
// range the defensive double-power path remains available for arbitrary finite
// inputs. Analytic slopes make a compact 512-interval Hermite table agree with
// the original float result to one ULP while remaining monotone.
constexpr std::size_t bbdTransferHermiteIntervals = 512u;
constexpr double bbdTransferHermiteLimit = 4.0;

struct BbdTransferHermiteNode
{
    double value {};
    double slope {};
};

const std::array<BbdTransferHermiteNode,
                 bbdTransferHermiteIntervals + 1u> bbdTransferHermiteTable = [] {
    std::array<BbdTransferHermiteNode,
               bbdTransferHermiteIntervals + 1u> result {};
    const double exponent = static_cast<double>(bbdSaturationExponent);
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const double normalised = bbdTransferHermiteLimit
            * static_cast<double>(index)
            / static_cast<double>(bbdTransferHermiteIntervals);
        const double squared = normalised * normalised;
        const double base = bbdTransferBase(normalised);
        const double denominator = std::pow(base, 1.0 / exponent);
        result[index].value = normalised / denominator;
        result[index].slope =
            (1.0 + static_cast<double>(bbdSaturationCurvature)
                         * (1.0 - 2.0 / exponent) * squared)
            / (base * denominator);
    }
    return result;
}();

// Fixed component candidate, built before audio processing. Both profiles
// use the same monotone Hermite support and defensive extreme-input fallback.
const std::array<BbdTransferHermiteNode,
                 bbdTransferHermiteIntervals + 1u> bbdServicedBiasHermiteTable = [] {
    std::array<BbdTransferHermiteNode,
               bbdTransferHermiteIntervals + 1u> result {};
    const double exponent = static_cast<double>(bbdSaturationExponent);
    const double curvature = static_cast<double>(bbdServicedBiasCurvature);
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const double normalised = bbdTransferHermiteLimit
            * static_cast<double>(index)
            / static_cast<double>(bbdTransferHermiteIntervals);
        const double squared = normalised * normalised;
        const double base = 1.0 + curvature * squared
            + std::pow(normalised, exponent);
        const double denominator = std::pow(base, 1.0 / exponent);
        result[index].value = normalised / denominator;
        result[index].slope = (1.0 + curvature * (1.0 - 2.0 / exponent) * squared)
            / (base * denominator);
    }
    return result;
}();

double interpolatedBbdTransfer(double normalised,
    const std::array<BbdTransferHermiteNode,
                     bbdTransferHermiteIntervals + 1u>& table = bbdTransferHermiteTable) noexcept
{
    const double position = normalised
        * static_cast<double>(bbdTransferHermiteIntervals)
        / bbdTransferHermiteLimit;
    const auto index = static_cast<std::size_t>(position);
    const double fraction = position - static_cast<double>(index);
    const double squared = fraction * fraction;
    const double cubic = squared * fraction;
    constexpr double width = bbdTransferHermiteLimit
                           / static_cast<double>(bbdTransferHermiteIntervals);
    const auto& first = table[index];
    const auto& second = table[index + 1u];
    return (2.0 * cubic - 3.0 * squared + 1.0) * first.value
         + (cubic - 2.0 * squared + fraction) * width * first.slope
         + (-2.0 * cubic + 3.0 * squared) * second.value
         + (cubic - squared) * width * second.slope;
}

// There is no divider ahead of the line. A previous revision put one here --
// 33 kOhm against 12 kOhm -- on a reading of the schematic that the schematic
// does not support: the 100 kOhm at each line's input injects adjustable DC
// bias from VR1/VR2 and is not the lower leg of anything, and the 10 kOhm
// against 2.2 nF beside it is a low-pass pole, not an attenuator. VR1/VR2 trim
// each line's DC bias so the positive and negative clipping points are
// symmetrical during the service procedure's 10 Vpp, 1 kHz TP2 test; they do
// not set a signal gain.
//
// The voice summer ahead of the whole effect does attenuate, but it attenuates
// dry and wet alike, so it is not a chorus divider either and cannot change the
// balance the summing resistors set.

// The independent random noise each wet line writes at its own clock edges is
// normalised by the explicit HISS-100 wet-line product policy. Its derivation
// is on `Chorus::independentLineRandomAmplitude` in the header, beside the
// separate Panasonic part-output maximum and the modelled board transfer.

// Support filters, from the schematic rather than from an estimate of it. Each
// side of the line carries two emitter-follower Sallen-Key sections built on
// equal 22 kOhm pairs, so each section's corner comes from its capacitors and
// its Q from their ratio alone:
//
//   Tr13 / Tr15   820 pF feedback, 680 pF shunt  ->  9.69 kHz, Q 0.549
//   Tr14 / Tr16   1.8 nF feedback, 270 pF shunt  -> 10.38 kHz, Q 1.291
//
// and the input adds one passive pole, R122 10 kOhm against C52 2.2 nF, at
// an isolated 7.23 kHz. C44/C47 and R120/R114 100 kOhm give an isolated
// 15.9 Hz high-pass. These last two capacitors share an unbuffered node;
// inputSupportMatrix() includes their mutual loading.
//
// This replaces a single pole at 9.9 kHz in and 9.5 kHz out, which was a guess
// at a fifth-order response rather than the response itself, and was therefore
// far too bright: four poles near 10 kHz reach -12 dB at 15 kHz where one
// reaches -4. It also settles the standing disagreement in the right
// direction. A sibling's wet path had been fitted with a *single* pole at
// 14 kHz, which cannot describe this circuit; the schematic shows why a
// single-pole fit was never going to.
constexpr float antiAliasFirstHz = 9688.0f;
constexpr float antiAliasFirstFeedbackF = 820.0e-12f;
constexpr float antiAliasFirstShuntF = 680.0e-12f;
constexpr float antiAliasSecondHz = 10377.0f;
constexpr float antiAliasSecondFeedbackF = 1.8e-9f;
constexpr float antiAliasSecondShuntF = 270.0e-12f;
constexpr double inputCouplingFarads = 0.1e-6; // C44 / C47
constexpr double inputBiasReturnOhms = 100000.0; // R120 / R114
constexpr double inputPassiveSeriesOhms = 10000.0; // R122 / R115
constexpr double inputPassiveFarads = 2.2e-9; // C52 / C56
constexpr float antiAliasPassiveHz = static_cast<float>(1.0
    / (2.0 * pi * inputPassiveSeriesOhms * inputPassiveFarads));
constexpr float inputCouplingHz = static_cast<float>(1.0
    / (2.0 * pi * inputBiasReturnOhms * inputCouplingFarads));
constexpr float wetOutputCouplingCapacitanceF = 1.0e-6f; // C28 / C25
constexpr float wetOutputBleedOhms = 22000.0f;           // R103 / R81
constexpr float wetMixerInputOhms = 39000.0f;            // R72 / R74

// The output's fifth low-pass reactive element is at the BBD tap-summing node:
// both complementary held outputs reach C45/C48 (2.2 nF) through R118/R119
// or R111/R112 (3.3 kOhm), while R117/R110 (47 kOhm) returns the node to
// ground. The followers provide complementary half-wave drive, not two
// guaranteed continuously conducting sources. Their combined waveform is a
// full-period hold; this does NOT make their output resistances parallel.
// Panasonic's circuit and Reticon's Buss/Weckler1976 p.57/Fig.6 establish
// that distinction; exact inactive conductance/crossover remain unidentified.
// https://www.experimentalistsanonymous.com/diy/Datasheets/MN3009.pdf#page=2
// https://www.imagesensors.org/Past%20Workshops/Dick%20Bredthauer%20Collection/1976%20CCD%20Conference%20Scotland/1976%2007%20Buss.pdf#page=3
//
// Keep the existing 3.5 kOhm EFFECTIVE boundary provisional (OQ-04). Its old
// explanation as two active (3.3+3.7) kOhm legs in parallel is withdrawn.
// Gi-RL measures loaded gain, not instantaneous source impedance. The same
// manufacturer's MN3005 test (book p.19) uses two labelled RL returns and
// a balance pot P. With one active source resistance r and an unloaded
// centered wiper, H=L*(L+P/2)/(r*(2L+P)+L*(L+P)). The pot alone adds .195 dB
// to the 50->100k labelled-load gain change for P=5k (.352 dB for P=10k),
// comparable to the old resistance estimate's .305 dB. The MN3009 curve's
// exact fixture and load-dependent operating point are not identified, so
// neither r nor an open-circuit DC level follows uniquely. Do not substitute
// 7k by merely deleting the old parallel factor. Numerical circuit audits
// qualify this declared boundary, not its physical identification. R98/R107
// loads the tap, so its capacitor and both sections remain one coupled solve.
//
// Panasonic BBD book, MN3009 pp. 37-39 (circuit, Gi-RL, application):
// https://www.ka-electronics.com/images/pdf/Panasonic_BBD.pdf
//
// The reconstruction corners below equal the anti-alias corners because the
// 106's own p. 15 scan reads them so at designator level (2026-08-07):
// pre-BBD C33 820p/C31 680p then C34 1.8n/C32 270p, and per output line
// C37/C35 and C38/C36 (line 1), C42/C40 and C43/C41 (line 2), every section
// the same 22k/22k pair, alongside per-BBD 10k/2.2n input poles (R122/R115,
// C52/C56), both 3.3k tap pairs into 47k/2.2n, and 100n/100k branch
// coupling -- agreeing with the sister board's clone netlist that first
// corroborated the family. OQ-04 keeps only the loaded transfer.
constexpr float outputTapReturnOhms = 47000.0f;
constexpr float outputTapShuntFarads = 2.2e-9f;
constexpr float outputTapEffectiveDriveOhms = 3500.0f;
constexpr float reconstructionSeriesOhms = 22000.0f;

// The wet-mute glide is expressed relative to dry. The final IC6 summer's
// absolute 100/47 dry gain is applied after the BBDs in process(); putting it
// before them would drive their fitted nonlinearity too hard.
constexpr float lineGain = Chorus::wetToDryGain;   // 47/39, +1.62 dB

template <std::size_t Size>
using FixedMatrix = std::array<std::array<double, Size>, Size>;

template <std::size_t Size>
FixedMatrix<Size> matrixIdentity() noexcept
{
    FixedMatrix<Size> result {};
    for (std::size_t index = 0; index < Size; ++index)
        result[index][index] = 1.0;
    return result;
}

template <std::size_t Size>
FixedMatrix<Size> matrixMultiply(const FixedMatrix<Size>& left,
                                 const FixedMatrix<Size>& right) noexcept
{
    FixedMatrix<Size> result {};
    for (std::size_t row = 0; row < Size; ++row)
        for (std::size_t inner = 0; inner < Size; ++inner)
        {
            const double value = left[row][inner];
            for (std::size_t column = 0; column < Size; ++column)
                result[row][column] += value * right[inner][column];
        }
    return result;
}

template <std::size_t Size>
FixedMatrix<Size> matrixLinearCombination(
    const FixedMatrix<Size>& left, double leftScale,
    const FixedMatrix<Size>& right, double rightScale) noexcept
{
    FixedMatrix<Size> result {};
    for (std::size_t row = 0; row < Size; ++row)
        for (std::size_t column = 0; column < Size; ++column)
            result[row][column] = leftScale * left[row][column]
                                + rightScale * right[row][column];
    return result;
}

template <std::size_t Size>
bool matrixSolve(FixedMatrix<Size> left, FixedMatrix<Size> right,
                 FixedMatrix<Size>& result) noexcept
{
    for (std::size_t pivot = 0; pivot < Size; ++pivot)
    {
        std::size_t best = pivot;
        double bestMagnitude = std::abs(left[pivot][pivot]);
        for (std::size_t row = pivot + 1; row < Size; ++row)
        {
            const double magnitude = std::abs(left[row][pivot]);
            if (magnitude > bestMagnitude)
            {
                best = row;
                bestMagnitude = magnitude;
            }
        }
        if (!(bestMagnitude > 1.0e-30) || !std::isfinite(bestMagnitude))
            return false;
        if (best != pivot)
        {
            std::swap(left[best], left[pivot]);
            std::swap(right[best], right[pivot]);
        }

        const double inversePivot = 1.0 / left[pivot][pivot];
        for (std::size_t column = pivot; column < Size; ++column)
            left[pivot][column] *= inversePivot;
        for (std::size_t column = 0; column < Size; ++column)
            right[pivot][column] *= inversePivot;

        for (std::size_t row = 0; row < Size; ++row)
        {
            if (row == pivot)
                continue;
            const double factor = left[row][pivot];
            left[row][pivot] = 0.0;
            for (std::size_t column = pivot + 1; column < Size; ++column)
                left[row][column] -= factor * left[pivot][column];
            for (std::size_t column = 0; column < Size; ++column)
                right[row][column] -= factor * right[pivot][column];
        }
    }
    result = right;
    return true;
}

// Higham's scaling-and-squaring [13/13] Pade construction.  The augmented
// matrix is dimensionless (one numerical interval), keeping its norm bounded
// even though the analog state coordinates are expressed in volts.
template <std::size_t Size>
FixedMatrix<Size> matrixExponential(FixedMatrix<Size> matrix) noexcept
{
    constexpr std::array<double, 14> b {
        64764752532480000.0, 32382376266240000.0,
        7771770303897600.0, 1187353796428800.0,
        129060195264000.0, 10559470521600.0,
        670442572800.0, 33522128640.0, 1323241920.0,
        40840800.0, 960960.0, 16380.0, 182.0, 1.0
    };
    constexpr double theta13 = 5.371920351148152;

    double norm = 0.0;
    for (std::size_t column = 0; column < Size; ++column)
    {
        double sum = 0.0;
        for (std::size_t row = 0; row < Size; ++row)
            sum += std::abs(matrix[row][column]);
        norm = std::max(norm, sum);
    }
    int squarings = 0;
    if (norm > theta13)
        squarings = std::max(0, static_cast<int>(
            std::ceil(std::log2(norm / theta13))));
    const double scale = std::ldexp(1.0, -squarings);
    for (auto& row : matrix)
        for (double& value : row)
            value *= scale;

    const auto identity = matrixIdentity<Size>();
    const auto a2 = matrixMultiply(matrix, matrix);
    const auto a4 = matrixMultiply(a2, a2);
    const auto a6 = matrixMultiply(a4, a2);

    auto innerU = matrixLinearCombination(a6, b[13], a4, b[11]);
    innerU = matrixLinearCombination(innerU, 1.0, a2, b[9]);
    innerU = matrixMultiply(a6, innerU);
    innerU = matrixLinearCombination(innerU, 1.0, a6, b[7]);
    innerU = matrixLinearCombination(innerU, 1.0, a4, b[5]);
    innerU = matrixLinearCombination(innerU, 1.0, a2, b[3]);
    innerU = matrixLinearCombination(innerU, 1.0, identity, b[1]);
    const auto u = matrixMultiply(matrix, innerU);

    auto innerV = matrixLinearCombination(a6, b[12], a4, b[10]);
    innerV = matrixLinearCombination(innerV, 1.0, a2, b[8]);
    auto v = matrixMultiply(a6, innerV);
    v = matrixLinearCombination(v, 1.0, a6, b[6]);
    v = matrixLinearCombination(v, 1.0, a4, b[4]);
    v = matrixLinearCombination(v, 1.0, a2, b[2]);
    v = matrixLinearCombination(v, 1.0, identity, b[0]);

    const auto denominator = matrixLinearCombination(v, 1.0, u, -1.0);
    const auto numerator = matrixLinearCombination(v, 1.0, u, 1.0);
    FixedMatrix<Size> result {};
    if (!matrixSolve(denominator, numerator, result))
        return identity;
    for (int step = 0; step < squarings; ++step)
        result = matrixMultiply(result, result);
    return result;
}

using AnalogMatrix = FixedMatrix<6>;
using AnalogDrive = std::array<double, 6>;

template <std::size_t Size>
void stampConductance(FixedMatrix<Size>& matrix, int p, int q,
                      double conductance) noexcept
{
    if (p >= 0) matrix[p][p] += conductance;
    if (q >= 0) matrix[q][q] += conductance;
    if (p >= 0 && q >= 0)
    {
        matrix[p][q] -= conductance;
        matrix[q][p] -= conductance;
    }
}

// Roland p15: Tr13/14 are PNP, driven by IC2b (M5218L) pin2;
// R90=22k/R91=10k return to +15V. Tr15/16 and Tr17/18 return through
// 10k to ground. All collectors go to -15V.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
// Toshiba 2SA1015 p.2 typical curves at 25 C support beta ~200 and
// |VBE| ~0.61 V here; Y/GR limits at -6 V/-2 mA are not installed
// operating-point bounds or a measured GR population mean. Cob = 4 pF
// is typical at VCB = -10 V, not a measured voltage-dependent curve.
// https://media.digikey.com/PDF/Data%20Sheets/Toshiba%20PDFs/2SA1015.pdf
constexpr double followerBeta = 200.0;
constexpr double followerVbe = 0.61;
constexpr double followerThermalVolts = 1.380649e-23 * 298.15 / 1.602176634e-19;
constexpr double followerCmuFarads = 4.0e-12;
// Panasonic p.38 marks optimum input bias ~-8.3 V (large-signal criterion),
// which its Vo-Vi curve maps to ~-10 V loaded output. The retained -10.37 V
// coordinate came from -10*(1+3700/100000). As explained at the effective
// output boundary above, the curve/fixture does not establish that source
// resistance or DC extrapolation. This is a provisional bias convention,
// not an identified open-circuit voltage or installed follower current.
// https://www.ka-electronics.com/images/pdf/Panasonic_BBD.pdf (printed37-39)
constexpr double followerBbdTheveninDc = -10.0 * (1.0 + 3700.0 / 100000.0);
// R116 (15k), VR1 (10k) and R121 (12k) span ground to -15 V; R120
// (100k) reaches the wiper. Its full nominal trim range gives 8.108..9.25k
// source resistance. The -8.3 V nominal wiper gives 9144.756 Ohm,
// in series with the 100k.
constexpr double followerBiasSourceOhms = (8.3 / 15.0 * 37000.0)
                                        * (1.0 - 8.3 / 15.0);

std::array<double, 2> followerCollectorCurrents(bool input) noexcept
{
    // tap, base1, emitter1, base2, emitter2, emitter-current1/2.
    FixedMatrix<7> a {}, rhs {}, solution {};
    stampConductance(a, 0, 1, 1.0 / 44000.0);
    stampConductance(a, 2, 3, 1.0 / 44000.0);
    if (input)
    {
        // IC2b's nominal DC output is zero, with low source impedance.
        a[0].fill(0.0);
        a[0][0] = 1.0;
    }
    else
    {
        a[0][0] += 1.0 / 3500.0 + 1.0 / 47000.0;
        rhs[0][0] = followerBbdTheveninDc / 3500.0;
    }
    for (int index = 0; index < 2; ++index)
    {
        const int b = 1 + 2 * index;
        const int e = b + 1;
        const int current = 5 + index;
        const double emitterR = input && index == 0 ? 22000.0 : 10000.0;
        a[e][e] += 1.0 / emitterR;
        rhs[e][0] = input ? 15.0 / emitterR : 0.0;
        a[e][current] += 1.0;
        a[b][current] -= 1.0 / (followerBeta + 1.0);
        a[current][e] = 1.0;
        a[current][b] = -1.0;
        rhs[current][0] = followerVbe;
    }
    if (!matrixSolve(a, rhs, solution))
        return {};
    return {{ solution[5][0] * followerBeta / (followerBeta + 1.0),
              solution[6][0] * followerBeta / (followerBeta + 1.0) }};
}

struct FiniteSupportCircuit
{
    AnalogMatrix generator {};
    AnalogDrive drive {};
    AnalogDrive equilibrium {};
    AnalogDrive outputByState {};
    double outputDirect { 0.0 };
    Chorus::SupportChain::NonlinearFollowerPorts nonlinear {};
    std::array<AnalogDrive, 2> currentDrive {};
};

FiniteSupportCircuit finiteSupportCircuit(
    bool input, double wetConnected, double inputWarpRate = 0.0) noexcept
{
    // Physical capacitor voltages, not integrator carries:
    // pre:  j1-e1, b1, j2-e2, b2, e2-coupled, BBDinput
    // post: tap, j1-e1, b1, j2-e2, b2, e2-output.
    // Cmu is parallel with each grounded base capacitor. Omitting Cpi and
    // rbb avoids four >100MHz parasitic modes, while retaining the finite
    // gm/rpi loading that matters at audio frequencies. The independent
    // typical-curve audit bounds the combined reduction to ~.049dB through
    // 20k (rbb30Ohm alone ~.004dB) for both chains. Early effect is omitted:
    // conditional VA25/50/100V screens add up to .274/.162/.090dB; no
    // original-part low-current slope justifies selecting an installed VA.
    FixedMatrix<14> system {}, rhs {}, solution {};
    auto resistor = [&](int p, int q, double ohms)
    { stampConductance(system, p, q, 1.0 / ohms); };
    std::array<double, 6> capacitance {};
    auto capacitor = [&](int index, int p, int q, double farads)
    {
        const int current = 8 + index;
        system[p][current] += 1.0;
        system[current][p] += 1.0;
        if (q >= 0)
        {
            system[q][current] -= 1.0;
            system[current][q] -= 1.0;
        }
        rhs[current][index] = 1.0;
        if (input && inputWarpRate > 0.0)
        {
            // Numerical prewarp of the physical capacitor coordinates. Each
            // SK pair uses its isolated component pole; the coupling and
            // shunt capacitors use their own branch RC anchors. Scaling the
            // capacitors before the coupled solve preserves finite reciprocal
            // loading and DC bias. Cmu remains part of the summed base cap;
            // this does not assert a different physical transistor capacitance.
            const double corner = index < 2
                ? 1.0 / (2.0 * pi * 22000.0
                         * std::sqrt(820e-12 * (680e-12 + followerCmuFarads)))
                : index < 4
                    ? 1.0 / (2.0 * pi * 22000.0
                             * std::sqrt(1.8e-9 * (270e-12 + followerCmuFarads)))
                    : index == 4
                        ? 1.0 / (2.0 * pi * (100000.0 + followerBiasSourceOhms)
                                 * 100e-9)
                        : 1.0 / (2.0 * pi * 10000.0 * 2.2e-9);
            // Match the legacy below-Nyquist anchor clamp, including 8 kHz.
            const double angle = pi * std::min(corner, 0.45 * inputWarpRate)
                               / inputWarpRate;
            farads *= angle / std::tan(angle);
        }
        capacitance[index] = farads;
    };
    const auto ic = followerCollectorCurrents(input);
    auto transistor = [&](int index, int base, int emitter)
    {
        const double gm = ic[index] / followerThermalVolts;
        stampConductance(system, base, emitter, gm / followerBeta);
        system[emitter][emitter] += gm;
        system[emitter][base] -= gm;
        // A positive residual collector current leaves the emitter and
        // enters the base through Ic/beta. The already stamped gm/rpi is
        // its tangent; these two independent columns carry only the excess
        // Ic0*(exp(deltaVeb/VT)-1-deltaVeb/VT), never the bias or tangent twice.
        rhs[emitter][7 + index] = -(1.0 + 1.0 / followerBeta);
        rhs[base][7 + index] = 1.0 / followerBeta;
    };
    if (input)
    {
        // j1,b1,e1,j2,b2,e2,coupled,BBDinput. Both identical BBD input
        // branches load Tr14: double capacitances and halve resistances.
        resistor(0, -1, 22000.0);
        rhs[0][6] = 1.0 / 22000.0;
        resistor(0, 1, 22000.0);
        resistor(2, 3, 22000.0);
        resistor(3, 4, 22000.0);
        resistor(2, -1, 22000.0);
        resistor(5, -1, 10000.0);
        resistor(6, -1, (100000.0 + followerBiasSourceOhms) * 0.5);
        resistor(6, 7, 5000.0);
        capacitor(0, 0, 2, 820e-12);
        capacitor(1, 1, -1, 680e-12 + followerCmuFarads);
        capacitor(2, 3, 5, 1.8e-9);
        capacitor(3, 4, -1, 270e-12 + followerCmuFarads);
        capacitor(4, 5, 6, 200e-9);
        capacitor(5, 7, -1, 4.4e-9);
        transistor(0, 1, 2);
        transistor(1, 4, 5);
    }
    else
    {
        // tap,j1,b1,e1,j2,b2,e2,output. The established input coordinate
        // already includes the nominal100k BBD load; preserve that existing
        // source factor, but do not compensate finite-buffer attenuation.
        resistor(0, -1, 3500.0);
        resistor(0, -1, 47000.0);
        rhs[0][6] = 1.0 / 3500.0 + 1.0 / 47000.0;
        resistor(0, 1, 22000.0);
        resistor(1, 2, 22000.0);
        resistor(3, 4, 22000.0);
        resistor(4, 5, 22000.0);
        resistor(3, -1, 10000.0);
        resistor(6, -1, 10000.0);
        resistor(7, -1, 22000.0);
        if (wetConnected > 0.0) resistor(7, -1, 39000.0 / wetConnected);
        capacitor(0, 0, -1, 2.2e-9);
        capacitor(1, 1, 3, 820e-12);
        capacitor(2, 2, -1, 680e-12 + followerCmuFarads);
        capacitor(3, 4, 6, 1.8e-9);
        capacitor(4, 5, -1, 270e-12 + followerCmuFarads);
        capacitor(5, 6, 7, 1e-6);
        transistor(0, 2, 3);
        transistor(1, 5, 6);
    }
    // [G B; B' 0] [nodeVoltages; capacitorCurrents] = [drive*u; capVoltages].
    // Solve every basis at prepare-time, then x'=C^-1*i. This eliminates the
    // algebraic emitter nodes without dropping reciprocal branch loading.
    FiniteSupportCircuit circuit;
    if (!matrixSolve(system, rhs, solution))
        return circuit;
    for (std::size_t row = 0; row < 6; ++row)
    {
        for (std::size_t column = 0; column < 6; ++column)
            circuit.generator[row][column] = solution[8 + row][column]
                                          / capacitance[row];
        circuit.drive[row] = solution[8 + row][6] / capacitance[row];
        circuit.outputByState[row] = solution[7][row];
    }
    circuit.outputDirect = solution[7][6];
    circuit.nonlinear.collectorCurrentAmps = ic;
    circuit.nonlinear.topology = input ? 1 : (wetConnected ? 3 : 2);
    for (std::size_t port = 0; port < 2; ++port)
    {
        const std::size_t base = (input ? 1u : 2u) + 3u * port;
        const std::size_t emitter = base + 1u;
        for (std::size_t state = 0; state < 6; ++state)
        {
            circuit.nonlinear.junctionByState[port][state] =
                solution[emitter][state] - solution[base][state];
            circuit.currentDrive[port][state] =
                solution[8 + state][7 + port] / capacitance[state];
        }
        circuit.nonlinear.junctionByInput[port] =
            solution[emitter][6] - solution[base][6];
        circuit.nonlinear.outputByCurrent[port] = solution[7][7 + port];
        for (std::size_t current = 0; current < 2; ++current)
            circuit.nonlinear.junctionByCurrent[port][current] =
                solution[emitter][7 + current] - solution[base][7 + current];
    }
    AnalogMatrix dcDrive {}, dcSolution {};
    for (std::size_t row = 0; row < 6; ++row)
        dcDrive[row][0] = -circuit.drive[row];
    if (matrixSolve(circuit.generator, dcDrive, dcSolution))
        for (std::size_t row = 0; row < 6; ++row)
            circuit.equilibrium[row] = dcSolution[row][0];
    return circuit;
}

Chorus::SupportChain::DenseInputMap denseInputMap(
    const AnalogMatrix& analog, const AnalogDrive& drive,
    const std::array<AnalogDrive, 2>& currentDrive, double sampleRate) noexcept
{
    Chorus::SupportChain::DenseInputMap result;
    if (sampleRate < Chorus::minimumExactInputSupportRate)
        return result;
    FixedMatrix<18> augmented {};
    for (std::size_t row = 0; row < 6; ++row)
    {
        for (std::size_t column = 0; column < 6; ++column)
            augmented[row][column] = analog[row][column] / sampleRate;
        augmented[row][6] = drive[row] / sampleRate;
        for (std::size_t port = 0; port < 2; ++port)
            augmented[row][10 + 4 * port] = currentDrive[port][row] / sampleRate;
    }
    for (std::size_t channel = 0; channel < 3; ++channel)
        for (std::size_t derivative = 0; derivative < 3; ++derivative)
            augmented[6 + 4 * channel + derivative][7 + 4 * channel + derivative] = 1.0;
    constexpr std::array<std::array<double, 4>, 4> polynomial {{
        {{ 0.0, 1.0, 0.0, 0.0 }},
        {{ 1.0 / 3.0, 0.5, -1.0, 1.0 / 6.0 }},
        {{ 1.0, -2.0, 1.0, 0.0 }},
        {{ 1.0, -3.0, 3.0, -1.0 }}
    }};
    // e5' exp(theta*M), with the full-interval cubic forcing coordinates.
    // CURRENT finite A has |lambda*h|<=.370 and ||A*h||inf<=1.029 at
    // 176.4k; the omitted Cpi/rbb modes are not silently Taylor-expanded.
    // Taylor10 plus theta^11 endpoint correction has error
    // sum(k>=12) a_k*(theta^k-theta^11). Each factor's maximum on [0,1]
    // is (11/k)^(11/(k-11))*(1-11/k). Taking absolute rows BEFORE the
    // history transform gives tail <2.312e-13, with remainder after k=40
    // <7.6e-44 (states/u bounded by 1; q by existing Ic0/2.6, scaling
    // linearly). Raw state coefficients scale as h^k and forcing derivative
    // j as h^(k-j), so the absolute bound at 176.4k covers ALL higher HQ
    // rates, not just six validation grids. It covers ideal/finite support
    // and cubic/linear priming; double rounding is separate. This bounds
    // dense interpolation, not physical nonlinear integration or hardware.
    std::array<double, 18> row {};
    row[5] = 1.0;
    for (std::size_t power = 0; power < result.outputByPower.size(); ++power)
    {
        auto& stored = result.outputByPower[power];
        for (std::size_t state = 0; state < 6; ++state)
            stored[state] = row[state];
        for (std::size_t channel = 0; channel < 3; ++channel)
            for (std::size_t sample = 0; sample < 4; ++sample)
                for (std::size_t derivative = 0; derivative < 4; ++derivative)
                    stored[6 + 4 * channel + sample] +=
                        row[6 + 4 * channel + derivative] * polynomial[derivative][sample];
        std::array<double, 18> next {};
        for (std::size_t column = 0; column < 18; ++column)
            for (std::size_t index = 0; index < 18; ++index)
                next[column] += row[index] * augmented[index][column];
        for (auto& value : next)
            value /= static_cast<double>(power + 1);
        row = next;
    }
    result.available = true;
    return result;
}

Chorus::SupportChain::HeldOutputMap heldOutputMap(
    const AnalogMatrix& analog, const AnalogDrive& drive, double rate,
    const Chorus::SupportChain::ExactTransition& transition,
    const AnalogDrive& equilibrium) noexcept
{
    Chorus::SupportChain::HeldOutputMap result;
    if (rate < Chorus::minimumExactInputSupportRate)
        return result;

    // Integrate each real held-output jump before sampling the physical
    // network: Holters/Parker2018, section3.2, equations12-25.
    // https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf
    // G(h)=integral_0^h exp(A*s)*b ds. The degree18 Taylor remainder on
    // 0<=h<=T is bounded by ||Tb||*exp(||TA||)*||TA||^18/19!:
    // <5e-12 for either declared network at HQ, before roundoff. Lower
    // grids retain BLEP; exact physical integration alone does not suppress
    // above-Nyquist output energy there.
    for (std::size_t row = 0; row < 6; ++row)
        result.byPower[0][row] = drive[row] / rate;
    for (std::size_t power = 1; power < result.byPower.size(); ++power)
        for (std::size_t row = 0; row < 6; ++row)
            for (std::size_t column = 0; column < 6; ++column)
                result.byPower[power][row] += analog[row][column] / rate
                    * result.byPower[power - 1][column]
                    / static_cast<double>(power + 1);
    for (const auto& term : result.byPower)
        for (std::size_t row = 0; row < 6; ++row)
            result.fullIntervalDrive[row] += term[row];
    for (std::size_t row = 0; row < 6; ++row)
    {
        double target = equilibrium[row];
        for (std::size_t column = 0; column < 6; ++column)
            target -= transition.stateByColumn[column][row] * equilibrium[column];
        // Preserve the stored DC identity around the slow coupling pole.
        // A tiny theta^19 correction leaves the first18 Taylor coefficients
        // unchanged and anchors the full-interval forcing to that identity.
        result.endpointCorrection[row] = target - result.fullIntervalDrive[row];
        result.fullIntervalDrive[row] = target;
    }
    result.available = true;
    return result;
}

Chorus::SupportChain::ExactTransition exactTransition(
    const AnalogMatrix& analog, const AnalogDrive& analogDrive,
    const std::array<double, 6>& constantInputEquilibrium,
    double sampleRate) noexcept
{
    FixedMatrix<10> augmented {};
    const double interval = 1.0 / sampleRate;
    for (std::size_t row = 0; row < 6; ++row)
    {
        for (std::size_t column = 0; column < 6; ++column)
            augmented[row][column] = interval * analog[row][column];
        augmented[row][6] = interval * analogDrive[row];
    }
    augmented[6][7] = 1.0;
    augmented[7][8] = 1.0;
    augmented[8][9] = 1.0;
    const auto exponential = matrixExponential(augmented);

    // Initial polynomial coordinates at the previous sample for the unique
    // cubic through u[n], u[n-1], u[n-2], u[n-3].
    constexpr std::array<std::array<double, 4>, 4> polynomial {
        std::array<double, 4> { 0.0, 1.0, 0.0, 0.0 },
        std::array<double, 4> { 1.0 / 3.0, 0.5, -1.0, 1.0 / 6.0 },
        std::array<double, 4> { 1.0, -2.0, 1.0, 0.0 },
        std::array<double, 4> { 1.0, -3.0, 3.0, -1.0 }
    };

    Chorus::SupportChain::ExactTransition result;
    for (std::size_t row = 0; row < 6; ++row)
    {
        for (std::size_t column = 0; column < 6; ++column)
            result.stateByColumn[column][row] = exponential[row][column];
        for (std::size_t sample = 0; sample < 4; ++sample)
            for (std::size_t derivative = 0; derivative < 4; ++derivative)
                result.driveBySample[sample][row] +=
                    exponential[row][6 + derivative]
                    * polynomial[derivative][sample];

        // Force the constant-input equilibrium identity exactly in stored
        // double arithmetic. This removes an otherwise tiny DC leak from
        // cancellation around the very slow coupling pole at 768 kHz.
        double target = constantInputEquilibrium[row];
        for (std::size_t column = 0; column < 6; ++column)
            target -= result.stateByColumn[column][row]
                    * constantInputEquilibrium[column];
        double actual = 0.0;
        for (std::size_t sample = 0; sample < 4; ++sample)
            actual += result.driveBySample[sample][row];
        result.driveBySample[0][row] += target - actual;
    }
    return result;
}

void prepareEndpointCurrentMap(
    Chorus::SupportChain::NonlinearFollowerPorts& ports) noexcept
{
    ports.endpointJunctionByCurrent = ports.junctionByCurrent;
    for (std::size_t junction = 0; junction < 2; ++junction)
        for (std::size_t current = 0; current < 2; ++current)
            for (std::size_t state = 0; state < 6; ++state)
                ports.endpointJunctionByCurrent[junction][current] +=
                    ports.junctionByState[junction][state]
                    * ports.stateByCurrent[current][state];
}

Chorus::SupportChain::ExactTransition bilinearTransition(
    const FiniteSupportCircuit& circuit, double sampleRate,
    bool nonlinear = false) noexcept
{
    const double halfInterval = 0.5 / sampleRate;
    const auto identity = matrixIdentity<6>();
    const auto denominator = matrixLinearCombination(
        identity, 1.0, circuit.generator, -halfInterval);
    const auto numerator = matrixLinearCombination(
        identity, 1.0, circuit.generator, halfInterval);
    AnalogMatrix transition {};
    Chorus::SupportChain::ExactTransition result;
    if (!matrixSolve(denominator, numerator, transition))
        return result;
    for (std::size_t row = 0; row < 6; ++row)
    {
        double constantDrive = circuit.equilibrium[row];
        for (std::size_t column = 0; column < 6; ++column)
        {
            result.stateByColumn[column][row] = transition[row][column];
            constantDrive -= transition[row][column] * circuit.equilibrium[column];
        }
        // Equal current/previous input weights enforce the physical DC
        // equilibrium and are algebraically (I-hA/2)^-1*h*b/2.
        result.driveBySample[0][row] = 0.5 * constantDrive;
        result.driveBySample[1][row] = 0.5 * constantDrive;
    }
    if (nonlinear)
    {
        result.nonlinear = circuit.nonlinear;
        result.nonlinear.enabled = true;
        AnalogMatrix currentRhs {}, currentSolution {};
        for (std::size_t row = 0; row < 6; ++row)
            for (std::size_t port = 0; port < 2; ++port)
                currentRhs[row][port] = halfInterval * circuit.currentDrive[port][row];
        if (matrixSolve(denominator, currentRhs, currentSolution))
            for (std::size_t row = 0; row < 6; ++row)
                for (std::size_t port = 0; port < 2; ++port)
                {
                    result.nonlinear.stateByPreviousCurrent[port][row] =
                        currentSolution[row][port];
                    result.nonlinear.stateByCurrent[port][row] =
                        currentSolution[row][port];
                }
        prepareEndpointCurrentMap(result.nonlinear);
    }
    return result;
}

Chorus::SupportChain::ExactTransition finiteExactTransition(
    const FiniteSupportCircuit& circuit, double sampleRate,
    bool nonlinear = false) noexcept
{
    auto result = exactTransition(circuit.generator, circuit.drive,
                                  circuit.equilibrium, sampleRate);
    result.outputByState = circuit.outputByState;
    if (circuit.nonlinear.topology != 1)
        result.heldOutputMap = heldOutputMap(circuit.generator, circuit.drive, sampleRate,
                                            result, circuit.equilibrium);
    result.outputDirect = circuit.outputDirect;
    result.finiteReadout = true;
    if (nonlinear)
    {
        result.nonlinear = circuit.nonlinear;
        result.nonlinear.enabled = true;
        // Integrate each nonlinear current as a line between its two solved
        // endpoints. This is a second-order residual approximation, not an
        // exact nonlinear solve in time. The physical linear transition and
        // cubic input history above remain bit-for-bit the same coefficients.
        FixedMatrix<10> augmented {};
        for (std::size_t row = 0; row < 6; ++row)
        {
            for (std::size_t column = 0; column < 6; ++column)
                augmented[row][column] = circuit.generator[row][column] / sampleRate;
            for (std::size_t port = 0; port < 2; ++port)
                augmented[row][6 + 2 * port] = circuit.currentDrive[port][row] / sampleRate;
        }
        augmented[6][7] = augmented[8][9] = 1.0;
        const auto exponential = matrixExponential(augmented);
        for (std::size_t row = 0; row < 6; ++row)
            for (std::size_t port = 0; port < 2; ++port)
            {
                result.nonlinear.stateByCurrent[port][row] =
                    exponential[row][7 + 2 * port];
                result.nonlinear.stateByPreviousCurrent[port][row] =
                    exponential[row][6 + 2 * port] - exponential[row][7 + 2 * port];
            }
        prepareEndpointCurrentMap(result.nonlinear);
        if (sampleRate >= Chorus::minimumExactInputSupportRate)
        {
            // At HQ the endpoint-linear residual's H2 attenuation is
            // measurable near 8 kHz. Integrate the unique cubic through the
            // current and three previous solved currents instead. This is a
            // higher-order numerical interpolation, not a changed current law.
            // Its current endpoint remains implicit in the same two-port solve.
            FixedMatrix<14> cubicAugmented {};
            for (std::size_t row = 0; row < 6; ++row)
            {
                for (std::size_t column = 0; column < 6; ++column)
                    cubicAugmented[row][column] = circuit.generator[row][column] / sampleRate;
                for (std::size_t port = 0; port < 2; ++port)
                    cubicAugmented[row][6 + 4 * port] = circuit.currentDrive[port][row] / sampleRate;
            }
            for (std::size_t port = 0; port < 2; ++port)
                for (std::size_t derivative = 0; derivative < 3; ++derivative)
                    cubicAugmented[6 + 4 * port + derivative][7 + 4 * port + derivative] = 1.0;
            const auto cubicExponential = matrixExponential(cubicAugmented);
            constexpr std::array<std::array<double, 4>, 4> polynomial {
                std::array<double, 4> { 0.0, 1.0, 0.0, 0.0 },
                std::array<double, 4> { 1.0 / 3.0, 0.5, -1.0, 1.0 / 6.0 },
                std::array<double, 4> { 1.0, -2.0, 1.0, 0.0 },
                std::array<double, 4> { 1.0, -3.0, 3.0, -1.0 }
            };
            auto& ports = result.nonlinear;
            for (std::size_t row = 0; row < 6; ++row)
                for (std::size_t port = 0; port < 2; ++port)
                    for (std::size_t sample = 0; sample < 4; ++sample)
                        for (std::size_t derivative = 0; derivative < 4; ++derivative)
                            ports.cubicStateByCurrentSample[sample][port][row] +=
                                cubicExponential[row][6 + 4 * port + derivative]
                                * polynomial[derivative][sample];
            ports.cubicEndpointJunctionByCurrent = ports.junctionByCurrent;
            for (std::size_t junction = 0; junction < 2; ++junction)
                for (std::size_t current = 0; current < 2; ++current)
                    for (std::size_t state = 0; state < 6; ++state)
                        ports.cubicEndpointJunctionByCurrent[junction][current] +=
                            ports.junctionByState[junction][state]
                            * ports.cubicStateByCurrentSample[0][current][state];
            ports.cubicResidual = true;
        }
    }
    return result;
}

AnalogMatrix inputSupportMatrix() noexcept
{
    const double w1 = 2.0 * pi * antiAliasFirstHz;
    const double w2 = 2.0 * pi * antiAliasSecondHz;
    const double wc = 1.0 / (inputBiasReturnOhms * inputCouplingFarads);
    const double wp = 1.0 / (inputPassiveSeriesOhms * inputPassiveFarads);
    const double loading = 1.0 / (inputPassiveSeriesOhms * inputCouplingFarads);
    const double k1 = 1.0 / Chorus::sallenKeyQ(
        antiAliasFirstFeedbackF, antiAliasFirstShuntF);
    const double k2 = 1.0 / Chorus::sallenKeyQ(
        antiAliasSecondFeedbackF, antiAliasSecondShuntF);
    AnalogMatrix matrix {};
    // Voltage-like analog integrator coordinates: BP1, LP1, BP2, LP2,
    // voltage across the coupling capacitor and passive-pole output voltage.
    // Roland JUNO-106 Service Notes, July 31, 1984, jack board p. 15:
    // https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
    // C44/C47 drives R120/R114 and the unbuffered R122/C52 (R115/C56)
    // branch together. With u=LP2, x=voltage across C44 and y=C52 voltage,
    // KCL gives x'=(wc+loading)*(u-x)-loading*y, y'=wp*(u-x-y).
    // The separable HP*LP approximation omitted loading, losing Rbias*C52
    // from the transfer denominator and making the wet input about 0.19 dB
    // too hot at midband. Both paths retain the existing ideal bias-source
    // boundary: the unknown installed VR1/VR2 setting/source impedance and
    // emitter-follower output impedance are not newly calibrated here.
    matrix[0][0] = -k1 * w1;
    matrix[0][1] = -w1;
    matrix[1][0] = w1;
    matrix[2][1] = w2;
    matrix[2][2] = -k2 * w2;
    matrix[2][3] = -w2;
    matrix[3][2] = w2;
    matrix[4][3] = wc + loading;
    matrix[4][4] = -wc - loading;
    matrix[4][5] = -loading;
    matrix[5][3] = wp;
    matrix[5][4] = -wp;
    matrix[5][5] = -wp;
    return matrix;
}

AnalogDrive inputSupportDrive() noexcept
{
    AnalogDrive drive {};
    drive[0] = 2.0 * pi * antiAliasFirstHz;
    return drive;
}

AnalogMatrix outputSupportMatrix(double wetConnected) noexcept
{
    const double source = outputTapEffectiveDriveOhms;
    const double tapReturn = outputTapReturnOhms;
    const double tapCap = outputTapShuntFarads;
    const double series = reconstructionSeriesOhms;
    const double firstFeedback = antiAliasFirstFeedbackF;
    const double firstShunt = antiAliasFirstShuntF;
    const double secondFeedback = antiAliasSecondFeedbackF;
    const double secondShunt = antiAliasSecondShuntF;
    const double wc = wetConnected == 0.0 || wetConnected == 1.0
        ? 2.0 * pi * Chorus::wetOutputCouplingCornerHz(wetConnected != 0.0)
        : (1.0 / wetOutputBleedOhms + wetConnected / wetMixerInputOhms)
            / wetOutputCouplingCapacitanceF;
    AnalogMatrix matrix {};
    // Physical node coordinates: tap, first R-R junction/follower, second
    // R-R junction/follower, then the C28/C25 coupling-capacitor lowpass
    // voltage. C45/C48 is loaded by R98/R107, so the tap and first Sallen-Key
    // section are one coupled network rather than two independent filters.
    // This raw comparison profile keeps Tr15/Tr16 (Tr17/Tr18 on the other
    // line) ideal. Product finite/nonlinear followers are stamped separately
    // in finiteSupportCircuit, with their documented nominal assumptions.
    matrix[0][0] = -(1.0 / source + 1.0 / tapReturn + 1.0 / series)
                 / tapCap;
    matrix[0][1] = 1.0 / (series * tapCap);

    matrix[1][0] = 1.0 / (series * firstFeedback);
    matrix[1][1] = 1.0 / (series * firstShunt)
                 - 2.0 / (series * firstFeedback);
    matrix[1][2] = -1.0 / (series * firstShunt)
                 + 1.0 / (series * firstFeedback);
    matrix[2][1] = 1.0 / (series * firstShunt);
    matrix[2][2] = -1.0 / (series * firstShunt);

    matrix[3][2] = 1.0 / (series * secondFeedback);
    matrix[3][3] = 1.0 / (series * secondShunt)
                 - 2.0 / (series * secondFeedback);
    matrix[3][4] = -1.0 / (series * secondShunt)
                 + 1.0 / (series * secondFeedback);
    matrix[4][3] = 1.0 / (series * secondShunt);
    matrix[4][4] = -1.0 / (series * secondShunt);

    matrix[5][4] = wc;
    matrix[5][5] = -wc;
    return matrix;
}

AnalogDrive outputSupportDrive() noexcept
{
    AnalogDrive drive {};
    // Preserve the established loaded wet-level convention. The MN3009
    // Gi-RL fixture does not identify active source impedance or installed
    // insertion gain; this normalization is not a new hardware calibration.
    // The effective source boundary remains OQ-04. The optional same-chip
    // insertion-gain prior scales the held signal upstream; do not compensate
    // this loaded-source convention again or scale output-referred noise.
    const double source = outputTapEffectiveDriveOhms;
    const double tapReturn = outputTapReturnOhms;
    const double tapCap = outputTapShuntFarads;
    drive[0] = (1.0 / source + 1.0 / tapReturn) / tapCap;
    return drive;
}

bool advanceExactSupport(
    std::array<double, 6>& state,
    const Chorus::SupportChain::ExactTransition& transition,
    double current, double previous, double previous2,
    double previous3, const std::array<double, 6>* heldForcing = nullptr) noexcept
{
    constexpr double maximumState =
        static_cast<double>(std::numeric_limits<float>::max()) / 16.0;
    // Both production callers sanitize the current sample before storing it
    // in these histories. Keep the final state guard below as the recovery
    // boundary instead of rechecking all four owned values on every advance.
    const std::array<double, 4> samples {
        current, previous, previous2, previous3
    };
    std::array<double, 6> next {};
    for (std::size_t column = 0; column < 6; ++column)
        for (std::size_t row = 0; row < 6; ++row)
            next[row] += transition.stateByColumn[column][row]
                       * state[column];
    if (heldForcing != nullptr)
    {
        for (std::size_t row = 0; row < 6; ++row)
            next[row] += (*heldForcing)[row];
    }
    else
        for (std::size_t sample = 0; sample < 4; ++sample)
            for (std::size_t row = 0; row < 6; ++row)
                next[row] += transition.driveBySample[sample][row]
                           * samples[sample];
    for (double value : next)
    {
        if (!(std::abs(value) <= maximumState))
        {
            state.fill(0.0);
            return false;
        }
    }
    state = next;
    return true;
}

using FollowerPair = std::array<double, 2>;
using FollowerPairMatrix = std::array<FollowerPair, 2>;

// Forward-active law Ic=Ic0*exp(delta(Ve-Vb)/VT), with Ib=Ic/beta:
// https://wiki.analog.com/university/courses/electronics/text/chapter-8
// https://www.analog.com/en/resources/analog-dialogue/studentzone/studentzone-april-2021.html
// Ic0 is the same solved nominal bias as the finite-linear profile; no new
// scale current, Early voltage, rail-clipping or gain parameter is introduced.
// Constant beta/Cmu, omitted Cpi/rbb/ro, and the conditional BBD DC fixture
// retain that profile's limits; this forward-active law does not add saturation.
// Subtract the tangent without cancellation near the bias point. q is a
// collector-current correction in amperes / nodeVoltsPerUnit; the existing
// gm/rpi network already carries the constant and first-order currents.
bool followerResidualCurrent(
    const FollowerPair& thermalJunction, const FollowerPair& collectorCurrent,
    FollowerPair& current, FollowerPair& derivative) noexcept
{
    for (std::size_t port = 0; port < 2; ++port)
    {
        const double y = thermalJunction[port];
        // Evaluation guard, not a physical junction clamp. Rejected Newton
        // trials are shortened, and failure falls back to the linear interval.
        if (!std::isfinite(y) || y > 80.0)
            return false;
        double exponentialMinusOne;
        double remainder;
        if (std::abs(y) <= 0.25)
        {
            // Both Newton terms share exp(y)-1-y. Evaluate that residual
            // directly near the bias point, avoiding cancellation and a
            // libm call for each port. Taylor degree 12 has absolute
            // truncation error <3.1e-18 over this interval, below double
            // rounding at its endpoints and far below the solve tolerance.
            // Pair even/odd coefficients to shorten the dependency chain.
            const double squared = y * y;
            remainder = squared * ((0.5 + y * (1.0 / 6.0))
                + squared * ((1.0 / 24.0 + y * (1.0 / 120.0))
                + squared * ((1.0 / 720.0 + y * (1.0 / 5040.0))
                + squared * ((1.0 / 40320.0 + y * (1.0 / 362880.0))
                + squared * ((1.0 / 3628800.0 + y * (1.0 / 39916800.0))
                + squared * (1.0 / 479001600.0))))));
            exponentialMinusOne = y + remainder;
        }
        else
        {
            exponentialMinusOne = std::expm1(y);
            remainder = exponentialMinusOne - y;
        }
        const double scale = collectorCurrent[port] / Chorus::nodeVoltsPerUnit;
        current[port] = scale * remainder;
        derivative[port] = scale * exponentialMinusOne;
        if (!std::isfinite(current[port]) || !std::isfinite(derivative[port]))
            return false;
    }
    return true;
}

// Solve y = (2.6/VT)*(P*x + Q*u + D*q(y)) with bounded damped Newton.
// D is either the algebraic map or the endpoint map including Gamma1.
// Residual-based acceptance matters near cutoff: dq/dy can be negative, so
// an unqualified fixed-point iteration is not generally contractive.
bool solveFollowerJunctions(
    const Chorus::SupportChain::NonlinearFollowerPorts& ports,
    const std::array<double, 6>& state, double input,
    const FollowerPairMatrix& currentMap, FollowerPair& junction,
    FollowerPair& current, std::uint32_t& maximumIterations) noexcept
{
    constexpr double thermalScale = Chorus::nodeVoltsPerUnit / followerThermalVolts;
    constexpr unsigned iterationLimit = 20;
    constexpr unsigned backtrackLimit = 12;
    constexpr double tolerance = 2.0e-12;
    FollowerPair base {};
    FollowerPairMatrix coupling {};
    for (std::size_t port = 0; port < 2; ++port)
    {
        base[port] = ports.junctionByInput[port] * input;
        for (std::size_t index = 0; index < 6; ++index)
            base[port] += ports.junctionByState[port][index] * state[index];
        base[port] *= thermalScale;
        for (std::size_t other = 0; other < 2; ++other)
            coupling[port][other] = thermalScale * currentMap[port][other];
    }
    auto residual = [&](const FollowerPair& y, FollowerPair& q,
                        FollowerPair& derivative, FollowerPair& error)
    {
        if (!followerResidualCurrent(y, ports.collectorCurrentAmps, q, derivative))
            return std::numeric_limits<double>::infinity();
        for (std::size_t port = 0; port < 2; ++port)
            error[port] = y[port] - base[port]
                - coupling[port][0] * q[0] - coupling[port][1] * q[1];
        return std::max(std::abs(error[0]), std::abs(error[1]));
    };
    FollowerPair derivative {}, error {};
    double norm = residual(junction, current, derivative, error);
    for (unsigned iteration = 0; iteration < iterationLimit; ++iteration)
    {
        maximumIterations = std::max(maximumIterations, iteration + 1);
        if (norm <= tolerance * (1.0 + std::max(std::abs(junction[0]),
                                                std::abs(junction[1]))))
            return true;
        const double a = 1.0 - coupling[0][0] * derivative[0];
        const double b = -coupling[0][1] * derivative[1];
        const double c = -coupling[1][0] * derivative[0];
        const double d = 1.0 - coupling[1][1] * derivative[1];
        const double determinant = a * d - b * c;
        if (!std::isfinite(norm) || !std::isfinite(determinant)
            || std::abs(determinant) < 1.0e-18)
            return false;
        FollowerPair step {{ (d * error[0] - b * error[1]) / determinant,
                             (a * error[1] - c * error[0]) / determinant }};
        const double largestStep = std::max(std::abs(step[0]), std::abs(step[1]));
        double damping = largestStep > 8.0 ? 8.0 / largestStep : 1.0;
        bool accepted = false;
        for (unsigned backtrack = 0; backtrack < backtrackLimit; ++backtrack)
        {
            const FollowerPair trial {{ junction[0] - damping * step[0],
                                       junction[1] - damping * step[1] }};
            FollowerPair trialCurrent {}, trialDerivative {}, trialError {};
            const double trialNorm = residual(trial, trialCurrent, trialDerivative,
                                              trialError);
            if (trialNorm < norm)
            {
                junction = trial;
                current = trialCurrent;
                derivative = trialDerivative;
                error = trialError;
                norm = trialNorm;
                accepted = true;
                break;
            }
            damping *= 0.5;
        }
        if (!accepted)
            return false;
    }
    // The final permitted Newton update can itself reach tolerance.
    return norm <= tolerance * (1.0 + std::max(std::abs(junction[0]),
                                               std::abs(junction[1])));
}

bool advanceNonlinearSupport(
    std::array<double, 6>& state,
    Chorus::SupportChain::NonlinearState& history,
    const Chorus::SupportChain::ExactTransition& transition,
    double current, double previous, double previous2, double previous3,
    const std::array<double, 6>* heldForcing = nullptr) noexcept
{
    const auto& ports = transition.nonlinear;
    const auto previousState = state;
    const bool finiteIntervalAccepted = advanceExactSupport(
        state, transition, current, previous, previous2, previous3, heldForcing);
    // Keep this safe finite-linear endpoint until every nonlinear solve/state
    // is accepted. Falling back is a numerical recovery policy, never a model
    // of transistor saturation or clipping; qualification checks its count.
    auto fail = [&]()
    {
        history.valid = false;
        history.previousCurrent = {};
        history.previous2Current = {};
        history.previous3Current = {};
        history.currentHistoryDepth = 0;
        history.junctionThermalVolts = {};
        if (history.fallbackCount != std::numeric_limits<std::uint32_t>::max())
            ++history.fallbackCount;
    };
    FollowerPair oldCurrent = history.previousCurrent;
    FollowerPair junction = history.junctionThermalVolts;
    const bool historyMatches = history.valid && history.topology == ports.topology;
    if (!historyMatches)
    {
        // Changing the output load preserves capacitor charge, but its old
        // algebraic current must be recomputed in the newly selected topology.
        if (!solveFollowerJunctions(ports, previousState, previous,
                ports.junctionByCurrent, junction, oldCurrent,
                history.maximumIterations))
        {
            fail();
            return finiteIntervalAccepted;
        }
    }
    const bool cubic = ports.cubicResidual && historyMatches
        && history.currentHistoryDepth >= 3;
    auto base = state;
    if (cubic)
    {
        const std::array<FollowerPair, 3> currents {{ oldCurrent,
            history.previous2Current, history.previous3Current }};
        for (std::size_t sample = 0; sample < 3; ++sample)
            for (std::size_t port = 0; port < 2; ++port)
                for (std::size_t index = 0; index < 6; ++index)
                    base[index] += ports.cubicStateByCurrentSample[sample + 1][port][index]
                                 * currents[sample][port];
    }
    else
        for (std::size_t port = 0; port < 2; ++port)
            for (std::size_t index = 0; index < 6; ++index)
                base[index] += ports.stateByPreviousCurrent[port][index] * oldCurrent[port];
    FollowerPair newCurrent {};
    if (!solveFollowerJunctions(ports, base, current,
            cubic ? ports.cubicEndpointJunctionByCurrent : ports.endpointJunctionByCurrent,
            junction, newCurrent, history.maximumIterations))
    {
        fail();
        return finiteIntervalAccepted;
    }
    auto next = base;
    for (std::size_t port = 0; port < 2; ++port)
        for (std::size_t index = 0; index < 6; ++index)
            next[index] += (cubic ? ports.cubicStateByCurrentSample[0][port][index]
                                  : ports.stateByCurrent[port][index]) * newCurrent[port];
    constexpr double maximumState =
        static_cast<double>(std::numeric_limits<float>::max()) / 16.0;
    for (double value : next)
        if (!(std::abs(value) <= maximumState))
        {
            fail();
            return finiteIntervalAccepted;
        }
    state = next;
    history.previous3Current = historyMatches ? history.previous2Current : oldCurrent;
    history.previous2Current = oldCurrent;
    history.currentHistoryDepth = static_cast<std::uint8_t>(std::min(
        3, (historyMatches ? static_cast<int>(history.currentHistoryDepth) : 1) + 1));
    history.previousCurrent = newCurrent;
    history.junctionThermalVolts = junction;
    history.topology = ports.topology;
    history.valid = true;
    return finiteIntervalAccepted;
}

std::uint32_t nextNoiseState(std::uint32_t state) noexcept
{
    if (state == 0u)
        state = 0xd1b54a35u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float noiseFromState(std::uint32_t state) noexcept
{
    return static_cast<float>(state & 0xffffffu) * (2.0f / 16777215.0f) - 1.0f;
}

// Symmetric triangle over a 0..1 phase, in -1..+1.
//
// This is the shape the circuit makes, not a convenient stand-in for it. IC1b
// integrates a constant current for the whole of each half cycle -- see the
// derivation on Chorus::lfoTimingOhms -- so both flanks are straight. An RC
// relaxation oscillator would bend them, and this one is not that.
double triangle(double phase) noexcept
{
    const double folded = phase < 0.5 ? phase : 1.0 - phase;
    return folded * 4.0 - 1.0;
}

} // namespace

Chorus::StereoNoiseSample Chorus::correlatedRandomStep(
    std::uint32_t& commonState, std::uint32_t& orthogonalState,
    float correlation) noexcept
{
    commonState = nextNoiseState(commonState);
    orthogonalState = nextNoiseState(orthogonalState);

    const float common = noiseFromState(commonState);
    const float orthogonal = noiseFromState(orthogonalState);
    const float rho = std::isfinite(correlation)
        ? std::clamp(correlation, -1.0f, 1.0f) : 0.0f;
    const float orthogonalGain = std::sqrt(std::max(0.0f, 1.0f - rho * rho));
    return { common, rho * common + orthogonalGain * orthogonal };
}

float Chorus::deterministicToneStep(double& phase, float frequencyHz,
                                    float sampleRate) noexcept
{
    if (!std::isfinite(frequencyHz) || !std::isfinite(sampleRate)
        || sampleRate <= 0.0f)
        return 0.0f;

    phase += static_cast<double>(std::max(frequencyHz, 0.0f))
           / static_cast<double>(sampleRate);
    phase -= std::floor(phase);
    return static_cast<float>(std::sin(2.0 * pi * phase));
}

Chorus::ModeSettings Chorus::settingsFor(
    ChorusMode mode, ChorusTimingProfile timingProfile) noexcept
{
    // The rates are this instrument's own, straight from its circuit:
    // derivedRateHz() evaluates f = 1/(4 * beta * R_eff * C3) with the
    // schematic's timing network (6.4352941 MOhm for I, 3.9638889 MOhm for
    // II), the summing-node comparator ratio 33/47 and the 0.1 uF integrator
    // capacitor, landing 0.55329 Hz and 0.89826 Hz. Scope readings of a
    // 106-chorus clone corroborate both within 3% (0.537/0.879 Hz), and both
    // truncate to the published about-0.5 and about-0.8. The JUNO-60 pair that
    // used to stand in for the scale is superseded, as its 1.682 ratio already
    // was; the suites keep both only as comparison values.
    //
    // The sweep endpoints are the 106's own third-party-measured figures:
    // delay 1.4 ms to 6.4 ms, scoped on a designator-faithful build of the
    // p. 15 chorus board carrying genuine 256-stage MN3009s and compared
    // directly against a real JUNO-106 by its owner, who published the scope
    // plots and called the two sweeps identical. The same excursion --
    // +/-2.5 ms about a 3.9 ms centre -- also matches the one independent
    // depth report on record. This supersedes the JUNO-60's calibrated
    // 1.66-5.35 ms capture, which stays in the suite as the
    // sibling-instrument comparison value: the two boards share their Tr22
    // clock driver, but not every timing part around it. Two narrower clone
    // clock readings (28-38 kHz expected by that kit's build guide, 28-60 kHz
    // observed on a suspected-faulty build) remain on record as
    // contradictions under OQ-01, which still requests a calibrated capture
    // of an original unit.
    //
    // In the nominal circuit, modes I and II differ in speed alone: the mode
    // line changes a timing resistance, while the triangle's amplitude is set
    // by the comparator's unchanged threshold ratio. This derives equal
    // nominal excursion; it does not measure Mode II's installed endpoints
    // or transfer the effective Mode-I fit below to that mode. No JUNO-106,
    // HS-60 or MKS-7 Mode II measurement is public (2026-09-22 search);
    // KR-106's 0.842 Hz / +/-1.71 ms Mode II is labelled Juno-6. Anwander's
    // survey gives I and II "100% amount" on the JUNO-6 and JUNO-60 and calls
    // the 106's LFO "1LFO with two speed settings", and a clone of this board
    // (Alpes Machines One-O-Six) marks presets I and II on its rate control
    // but one "Juno 106 chorus depth level": the rate-only law again.
    // OwnerBlend below therefore also moves Mode II onto the blend at the
    // derived II/I rate ratio, as the owner chose by ear (2026-09-22).
    // https://www.florian-anwander.de/roland_string_choruses/
    // https://www.alpesmachines.net/index.php/analogue-chorus/one-o-six-chorus/one-o-six-how-to-use
    constexpr float centre = 0.5f * (0.0014f + 0.0064f);
    constexpr float sweep = 0.5f * (0.0064f - 0.0014f);
    constexpr float rateOne = static_cast<float>(derivedRateHz(true));
    constexpr float rateTwo = static_cast<float>(derivedRateHz(false));
    // The identified unit's spectral fit, and KR-106's click series at that
    // unit's rate (ChorusTimingProfile records the click data's provenance).
    constexpr float spectralRate = 0.5159334275f;
    constexpr float spectralCentre = 0.00338027575f;
    constexpr float spectralSweep = 0.00176176683f;
    constexpr float clickRate = 0.514f;
    constexpr float clickCentre = 0.00330f;
    constexpr float clickSweep = 0.00213f;
    switch (mode)
    {
        case ChorusMode::One:
            // Comparison-only profile identified by AnalyzeChorusCapture.py
            // from the hash-pinned bank_A1x recording below. Complete C1/C3/C5
            // notes fit the model-based effective timing; C2/C4 are withheld.
            // The shipped-support estimator recovers a known model within
            // 20 us centre / 12 us depth / 0.000006 Hz rate. Hardware held-out
            // residual is 0.113 versus 0.095 training, appreciably above the
            // known-model 0.015 residual. Thus these are effective coordinates
            // under that support/triangle hypothesis, not direct chip-delay
            // measurements, a population nominal, or a Mode-II calibration.
            // AIFF SHA256: b235ba2236c1a509627ce1e84fa35004b0d7c3e99eb36899c4c5de63cc668662
            // https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16#issuecomment-4184997000
            // Three OQ-01 comparison candidates with distinct evidence; see
            // ChorusTimingProfile. The spectral coordinates above are fitted,
            // while the circuit estimate below depends on unmeasured values.
            switch (timingProfile)
            {
                case ChorusTimingProfile::A11Spectral:
                case ChorusTimingProfile::HardwareEvidence:
                    return { spectralRate, spectralCentre, spectralSweep, lineGain };
                case ChorusTimingProfile::A11ClickTiming:
                    return { clickRate, clickCentre, clickSweep, lineGain };
                case ChorusTimingProfile::OwnerBlend:
                    // The owner's by-ear decision of 2026-09-17
                    // (Docs/decisions.md): the mean of the shipping clone
                    // endpoints, the identified unit's spectral fit counted
                    // twice and its click-timing coordinates -- a compromise
                    // weighted towards the fit, not a measurement of any
                    // unit: 3.49 ms centre, +/-2.04 ms, 0.5248 Hz.
                    return { 0.25f * (rateOne + 2.0f * spectralRate + clickRate),
                             0.25f * (centre + 2.0f * spectralCentre + clickCentre),
                             0.25f * (sweep + 2.0f * spectralSweep + clickSweep),
                             lineGain };
                case ChorusTimingProfile::DerivedNominal:
                    // Conditional estimate, not a measured hardware bound:
                    // Roland p. 15 puts R123=1.8k and R125=10k in the base
                    // divider; R124=8.2k is Tr19's emitter resistor. Ignoring
                    // base current, I0 ~= (15*R123/(R123+R125)-Vbe)/R124,
                    // or 199.77 uA at assumed Vbe=0.65 V. Even independent
                    // +/-1% resistors give 193.14-206.60 uA at those fixed
                    // supply/junction voltages, not a 195-203 uA guarantee.
                    // d(delay)/dV = 256*C53/I0; TP4 is +/-beta*Vsat, so its
                    // ~9.48 V PEAK at Vsat=13.5 V gives ~1.83 ms half-depth
                    // with C53=150 pF and I0=199 uA. The 3.02 ms centre also
                    // assumes junction drops and an unmeasured reset dead time.
                    // Roland p. 15 and Panasonic MN3101 (fCP=fosc/2):
                    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
                    // https://www.experimentalistsanonymous.com/diy/Datasheets/MN3101.pdf#page=3
                    return { rateOne, 0.00302f, 0.00183f, lineGain };
                case ChorusTimingProfile::Shipping:
                default:
                    break;
            }
            return { rateOne, centre, sweep, lineGain };
        case ChorusMode::Two:
            if (timingProfile == ChorusTimingProfile::HardwareEvidence)
            {
                // Mode I is the verified recording's effective fit above.
                // The board's binary rate switch changes timing resistance,
                // leaving triangle thresholds/excursion unchanged. Applying
                // that rate ratio is a named schematic estimate for Mode II,
                // not an independent Mode-II capture or a chip-delay claim.
                return { spectralRate * (rateTwo / rateOne),
                         spectralCentre, spectralSweep, lineGain };
            }
            // Among historical profiles, only the blend reaches Mode II
            // (Docs/decisions.md, 2026-09-22): the other candidates were Mode I
            // readings, and the rate-only
            // mode line keeps the blend's excursion at 3.49 ms +/-2.04 ms,
            // 0.852 Hz. The ratio is the derived one; no Mode II was measured.
            if (timingProfile == ChorusTimingProfile::OwnerBlend)
            {
                const auto blend = settingsFor(ChorusMode::One, timingProfile);
                return { blend.rateHz * (rateTwo / rateOne),
                         blend.centreDelaySeconds, blend.sweepSeconds, lineGain };
            }
            return { rateTwo, centre, sweep, lineGain };
        // Product extension. Roland's original owner manual permits Off/I/II
        // and excludes simultaneous I+II; the board has an enable line plus
        // one binary rate line, not a third timing resistance. Retain the
        // established summed-rate sound as a product compatibility choice.
        // No JUNO-6/60 fast-mode calibration is inferred for this extension.
        // https://cdn.roland.com/assets/media/pdf/JUNO-106_OM.pdf#page=3
        case ChorusMode::OneTwo:
            return { rateOne + rateTwo, centre, sweep, lineGain };
        case ChorusMode::Off:
        default:
            // Bypass mutes the wet return and the modulator free-runs. The
            // button line also reaches both MN3101 oscillators: the C16 node
            // feeds D3/R41 with R47 across them into C15, whose junction
            // drives Tr23/Tr28 on the oscillator nodes (p. 15), clamping the
            // clocks about 0.2-0.26 s after the return has muted and
            // releasing them about 35 ms after the button comes on, some
            // 80 ms before the return opens -- inaudible either way, so the
            // lines keep clocking here and an effect prepared while off
            // still needs a real clock programme and sweep depth.
            return { rateOne, centre, sweep, 0.0f };
    }
}

float Chorus::bbdTransfer(float input) noexcept
{
    if (!std::isfinite(input))
        return 0.0f;

    // Interpolate in double precision through the range reached by ordinary
    // audio. The double-power fallback beyond four rails lets even an extreme
    // finite float approach the rail instead of overflowing an intermediate
    // power and folding back to zero.
    const double normalised = std::abs(static_cast<double>(input))
                            / static_cast<double>(bbdSaturationLevel);
    if (normalised < bbdTransferHermiteLimit)
        return std::copysign(static_cast<float>(
            static_cast<double>(bbdSaturationLevel)
                * interpolatedBbdTransfer(normalised)), input);
    const double inverse = 1.0 / normalised;
    const double exponent = static_cast<double>(bbdSaturationExponent);
    const double correction = std::pow(
        1.0
            + static_cast<double>(bbdSaturationCurvature)
                  * std::pow(inverse, exponent - 2.0)
            + std::pow(inverse, exponent),
        1.0 / exponent);
    return std::copysign(static_cast<float>(
        static_cast<double>(bbdSaturationLevel) / correction), input);
}

float Chorus::bbdServicedBiasTransfer(float input) noexcept
{
    if (!std::isfinite(input))
        return 0.0f;
    const double normalised = std::abs(static_cast<double>(input))
                            / static_cast<double>(bbdSaturationLevel);
    if (normalised < bbdTransferHermiteLimit)
        return std::copysign(static_cast<float>(
            static_cast<double>(bbdSaturationLevel)
                * interpolatedBbdTransfer(normalised, bbdServicedBiasHermiteTable)), input);
    const double inverse = 1.0 / normalised;
    const double exponent = static_cast<double>(bbdSaturationExponent);
    const double correction = std::pow(
        1.0 + static_cast<double>(bbdServicedBiasCurvature)
                  * std::pow(inverse, exponent - 2.0)
            + std::pow(inverse, exponent), 1.0 / exponent);
    return std::copysign(static_cast<float>(
        static_cast<double>(bbdSaturationLevel) / correction), input);
}

float Chorus::transferLossStep(float& state, float input) noexcept
{
    // One fixed per-transfer coefficient, advanced once per clock edge. The
    // recursion runs on the clock grid, so the absolute corner already moves
    // with the clock exactly as the physical per-stage inefficiency does; a
    // previous revision additionally scaled the coefficient with the clock
    // (unity at an uncited 26 kHz, slope 1.5e-6 per Hz), which un-anchored
    // the derivation -- at the datasheet's own 40 kHz condition it rendered
    // -2.76 dB where the part is specified -3.0 dB -- and brightened with
    // clock where per-transfer loss physically worsens. The fixture drives
    // this exact function, so the anchor holds at every clock again.
    state += transferSmear * (input - state);
    return state;
}

float Chorus::interpolateBbdInput(float current, float previous,
                                  float previous2, float previous3,
                                  double ageInSamples) noexcept
{
    if (!std::isfinite(current) || !std::isfinite(previous)
        || !std::isfinite(previous2) || !std::isfinite(previous3)
        || !std::isfinite(ageInSamples))
        return 0.0f;

    const double age = std::clamp(ageInSamples, 0.0, 1.0);
    const double t = -age;
    const double l0 = (t + 1.0) * (t + 2.0) * (t + 3.0) / 6.0;
    const double l1 = -t * (t + 2.0) * (t + 3.0) / 2.0;
    const double l2 = t * (t + 1.0) * (t + 3.0) / 2.0;
    const double l3 = -t * (t + 1.0) * (t + 2.0) / 6.0;
    const double interpolated = l0 * static_cast<double>(current)
                              + l1 * static_cast<double>(previous)
                              + l2 * static_cast<double>(previous2)
                              + l3 * static_cast<double>(previous3);
    if (!std::isfinite(interpolated))
        return 0.0f;

    constexpr double maximum =
        static_cast<double>(std::numeric_limits<float>::max());
    return static_cast<float>(std::clamp(interpolated, -maximum, maximum));
}

double Chorus::bbdPolyBlepResidual(double distanceInSamples) noexcept
{
    // Integrated third-order Lagrange residual used by Gabrielli, D'Angelo
    // and Squartini's BBD reference implementation. Its support covers the
    // two numerical samples on either side of a discontinuity. Distances in
    // this model are always non-negative; handling NaN and negative test input
    // as zero keeps this pure helper defensive without extending the curve.
    if (!(distanceInSamples >= 0.0) || distanceInSamples >= 2.0)
        return 0.0;

    const double d = distanceInSamples;
    if (d < 1.0)
    {
        return ((((0.125 * d - 1.0 / 3.0) * d - 0.25) * d + 1.0)
                 * d - 0.5);
    }

    return ((((-1.0 / 24.0 * d + 1.0 / 3.0) * d - 11.0 / 12.0)
             * d + 1.0) * d - 1.0 / 3.0);
}

float Chorus::onePoleG(float cutoffHz, float sampleRate) noexcept
{
    // The wet coupling pole is at 15.9 Hz, so the old 20 Hz defensive floor
    // silently moved a real component value. A small positive floor still
    // protects tan() from invalid callers without voicing the circuit.
    //
    // The upper ceiling is defensive: tan() turns negative past fs/2 and
    // would invert the recursion. On the audited 44.1-96 kHz fallback grids,
    // shipping calls use only the 15.9 Hz coupling and 7.234 kHz passive
    // corners, comfortably below it. Lower supported stress-test grids are
    // necessarily Nyquist-limited; the exact output transition owns the
    // higher tap/reconstruction poles at every rate.
    const float limited = std::clamp(cutoffHz, 0.1f, sampleRate * 0.45f);
    const float g = std::tan(3.14159265358979324f * limited / sampleRate);
    return g / (1.0f + g);
}

// One topology-preserving-transform lowpass step. The coefficient above only
// places the corner where it was asked for if the state is advanced by twice
// the difference term: the plain `state += g * (input - state)` recursion wants
// a different coefficient entirely and would put the 9.9 kHz corner near
// 4.6 kHz at the engine's 192 kHz internal rate.
float Chorus::supportFilterStep(float& state, float input, float g) noexcept
{
    const float difference = (input - state) * g;
    const float output = difference + state;
    state = output + difference;
    return output;
}

float Chorus::wetOutputCouplingCornerHz(bool wetConnected) noexcept
{
    const float resistance = wetConnected
        ? wetOutputBleedOhms * wetMixerInputOhms
            / (wetOutputBleedOhms + wetMixerInputOhms)
        : wetOutputBleedOhms;
    return 1.0f / (2.0f * static_cast<float>(pi)
                   * wetOutputCouplingCapacitanceF * resistance);
}

// An equal-resistor Sallen-Key's damping comes entirely from its capacitor
// ratio. Both sections here are built that way -- 22 kOhm twice -- so this is
// the whole of their Q.
float Chorus::sallenKeyQ(float feedbackFarads, float shuntFarads) noexcept
{
    if (shuntFarads <= 0.0f)
        return 0.5f;
    return 0.5f * std::sqrt(feedbackFarads / shuntFarads);
}

Chorus::BiquadCoefficients Chorus::sallenKeyCoefficients(float cutoffHz, float q,
                                                         float sampleRate) noexcept
{
    const float limited = std::clamp(cutoffHz, 20.0f, sampleRate * 0.45f);
    BiquadCoefficients coefficients;
    coefficients.g = std::tan(3.14159265358979324f * limited / sampleRate);
    coefficients.k = 1.0f / std::max(q, 0.05f);
    return coefficients;
}

// Zavalishin's topology-preserving state-variable form, lowpass output. The
// prewarped `g` puts the corner where it was asked for at any host rate, which
// a direct-form biquad with bilinear coefficients would not do as cleanly at
// the engine's 192 kHz internal rate.
float Chorus::biquadStep(BiquadState& state, float input,
                         const BiquadCoefficients& coefficients) noexcept
{
    const float g = coefficients.g;
    const float k = coefficients.k;
    // `k` is the form's own damping term, which is already 1/Q -- doubling it
    // here would halve every section's Q and darken the whole chain.
    const float highPass = (input - (k + g) * state.s1 - state.s2)
                         / (1.0f + g * (g + k));
    const float bandPass = g * highPass + state.s1;
    state.s1 = g * highPass + bandPass;
    const float lowPass = g * bandPass + state.s2;
    state.s2 = g * bandPass + lowPass;
    return lowPass;
}

Chorus::SupportChain Chorus::supportChainFor(
    float sampleRate, ChorusSupportProfile profile) noexcept
{
    SupportChain chain;
    chain.inputCouplingG = onePoleG(inputCouplingHz, sampleRate);
    chain.passiveG = onePoleG(antiAliasPassiveHz, sampleRate);
    // Keep the reviewed low-grid prewarping of each reactive rate, but solve
    // both capacitor currents simultaneously. Both currents through C44 use
    // its same warped capacitance; their ratio stays R120/R122 = 10.
    const double gc = chain.inputCouplingG / (1.0 - chain.inputCouplingG);
    const double gp = chain.passiveG / (1.0 - chain.passiveG);
    const double gl = gc * inputBiasReturnOhms / inputPassiveSeriesOhms;
    const double determinant = (1.0 + gc + gl) * (1.0 + gp) - gl * gp;
    chain.inputCouplingInverse = {{
        {{ (1.0 + gp) / determinant, -gl / determinant }},
        {{ -gp / determinant, (1.0 + gc + gl) / determinant }}
    }};
    chain.inputCouplingDrive = {{
        (gc * (1.0 + gp) + gl) / determinant, gp / determinant
    }};
    chain.antiAliasFirst = sallenKeyCoefficients(
        antiAliasFirstHz,
        sallenKeyQ(antiAliasFirstFeedbackF, antiAliasFirstShuntF), sampleRate);
    chain.antiAliasSecond = sallenKeyCoefficients(
        antiAliasSecondHz,
        sallenKeyQ(antiAliasSecondFeedbackF, antiAliasSecondShuntF), sampleRate);
    constexpr std::array<double, 6> inputEquilibrium {
        0.0, 1.0, 0.0, 1.0, 1.0, 0.0
    };
    constexpr std::array<double, 6> outputEquilibrium {
        1.0, 1.0, 1.0, 1.0, 1.0, 1.0
    };
    if (profile == ChorusSupportProfile::Nominal2SA1015
        || profile == ChorusSupportProfile::Nominal2SA1015Nonlinear)
    {
        const bool nonlinear = profile == ChorusSupportProfile::Nominal2SA1015Nonlinear;
        const auto input = finiteSupportCircuit(true, false);
        chain.exactInput = finiteExactTransition(input, sampleRate, nonlinear);
        chain.denseInput = denseInputMap(input.generator, input.drive,
            nonlinear ? input.currentDrive : std::array<AnalogDrive, 2> {}, sampleRate);
        // Low grids retain the reviewed alias/response tradeoff of TPT,
        // using component-anchored capacitor prewarps in one coupled solve.
        // Unwarped bilinear excessively attenuates 8-12 kHz; exact cubic at
        // 44.1/48 kHz improves analog response but increases stationary SGA.
        // This mapping retains real finite-buffer attenuation, without EQ or
        // gain compensation. HQ uses the unmodified physical exact circuit.
        chain.bilinearInput = bilinearTransition(
            finiteSupportCircuit(true, false, sampleRate), sampleRate, nonlinear);
        chain.exactOutputMuted = finiteExactTransition(
            finiteSupportCircuit(false, false), sampleRate, nonlinear);
        chain.exactOutputConnected = finiteExactTransition(
            finiteSupportCircuit(false, true), sampleRate, nonlinear);
    }
    else
    {
        chain.exactInput = exactTransition(
            inputSupportMatrix(), inputSupportDrive(), inputEquilibrium, sampleRate);
        chain.denseInput = denseInputMap(
            inputSupportMatrix(), inputSupportDrive(), {}, sampleRate);
        chain.exactOutputMuted = exactTransition(
            outputSupportMatrix(false), outputSupportDrive(), outputEquilibrium,
            sampleRate);
        chain.exactOutputConnected = exactTransition(
            outputSupportMatrix(true), outputSupportDrive(), outputEquilibrium,
            sampleRate);
        chain.exactOutputMuted.heldOutputMap = heldOutputMap(
            outputSupportMatrix(false), outputSupportDrive(), sampleRate,
            chain.exactOutputMuted, outputEquilibrium);
        chain.exactOutputConnected.heldOutputMap = heldOutputMap(
            outputSupportMatrix(true), outputSupportDrive(), sampleRate,
            chain.exactOutputConnected, outputEquilibrium);
    }
    chain.wetConductance.front() = chain.exactOutputMuted;
    chain.wetConductance.back() = chain.exactOutputConnected;
    for (std::size_t i = 1; i < SupportChain::wetConductanceIntervals; ++i)
    {
        const double ratio = static_cast<double>(i) / SupportChain::wetConductanceIntervals;
        if (profile == ChorusSupportProfile::Nominal2SA1015
            || profile == ChorusSupportProfile::Nominal2SA1015Nonlinear)
            chain.wetConductance[i] = finiteExactTransition(
                finiteSupportCircuit(false, ratio), sampleRate,
                profile == ChorusSupportProfile::Nominal2SA1015Nonlinear);
        else
        {
            const auto matrix = outputSupportMatrix(ratio);
            chain.wetConductance[i] = exactTransition(
                matrix, outputSupportDrive(), outputEquilibrium, sampleRate);
            chain.wetConductance[i].heldOutputMap = heldOutputMap(
                matrix, outputSupportDrive(), sampleRate,
                chain.wetConductance[i], outputEquilibrium);
        }
    }
    // Tr5 open: C16 and C13 are two physical coordinates joined by R48,
    // not cascaded independent RCs. Prepare exp(A / fs) once; the audio path
    // only advances the two voltages relative to their loaded DC rest.
    const double dt = 1.0 / static_cast<double>(sampleRate);
    const double pullUp = 1.0 / muteDrivePullUpOhms;
    const double series = 1.0 / muteDriveSeriesOhms;
    const double lower = 1.0 / (muteDriveBaseOhms + muteDriveEmitterOhms);
    FixedMatrix<2> muteDriveMatrix {{
        {{ -dt * (pullUp + series) / muteDriveNodeFarads,
            dt * series / muteDriveNodeFarads }},
        {{  dt * series / muteDriveHoldFarads,
           -dt * (series + lower) / muteDriveHoldFarads }}
    }};
    chain.muteDriveOpenTransition = matrixExponential(muteDriveMatrix);
    // Roland Service Notes p.15: R46 330 Ohm remains between Tr5's
    // collector and C16 when the transistor conducts. The same two-node
    // system gains (Vnode + 15) / R46 sink current; R50 still pulls upward.
    // C16*dVnode/dt = (15-Vnode)/R50 - (Vnode-Vhold)/R48
    //                 - (Vnode+15)/R46.
    // C13*dVhold/dt = (Vnode-Vhold)/R48 - (Vhold+15)/(R49+R42).
    // This restores the known resistor while keeping the established ideal
    // Tr5 saturation coordinate and 0.6 V Tr4 threshold prior.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
    muteDriveMatrix[0][0] -= dt / (muteDriveSinkOhms * muteDriveNodeFarads);
    chain.muteDriveConductingTransition = matrixExponential(muteDriveMatrix);

    for (std::size_t index = 0; index < chain.clockMuteTransitions.size(); ++index)
    {
        const bool tr5Conducting = index >= 16u || (index & 8u) != 0;
        const bool clockBaseClamped = (index & 4u) != 0;
        const bool wetBaseClamped = (index & 2u) != 0;
        const bool diodeConducting = (index & 1u) != 0;
        auto& circuit = chain.clockMuteTransitions[index];
        auto& a = circuit.generator;
        const bool tr5CurrentLimited = index >= 16u;
        // In Tr5's forward-active region the available base current bounds
        // Ic. R46 still carries that current; the collector is Vnode-R46*Ic,
        // rather than a stiff -15 V. At Vnode=-15+R46*Ic it reaches the
        // retained ideal saturation coordinate and becomes the legacy R46
        // conductance. Sink current and capacitor slopes agree at that
        // boundary. No extra switching delay or slew constant is introduced.
        const double sink = tr5Conducting && !tr5CurrentLimited
                          ? 1.0 / muteDriveSinkOhms : 0.0;
        const double sinkCurrent = tr5CurrentLimited ? tr5CollectorCurrentLimit : 0.0;
        const double diode = diodeConducting ? 1.0 / clockMuteDiodeSeriesOhms : 0.0;
        const double branch = 1.0 / clockMuteBypassOhms + diode;
        const double wetDown = wetBaseClamped ? 1.0 / muteDriveBaseOhms : lower;
        const double wetRail = -muteDriveRailVolts
            + (wetBaseClamped ? muteDriveJunctionVolts : 0.0);
        const double clockDown = 2.0 / (clockMuteBaseOhms
            + (clockBaseClamped ? 0.0 : clockMuteEmitterOhms));
        const double clockRail = -muteDriveRailVolts
            + (clockBaseClamped ? muteDriveJunctionVolts : 0.0);
        const double junctionCurrent = diode * muteDriveJunctionVolts;
        // Current C15 -> C16 is (Vc-Vn)/R47 + max(Vc-Vn-Vj,0)/R41.
        // Its sign reverses in the two capacitor equations. Both base-divider
        // paths are present even though only one clock clamp would stop audio.
        // When a base's unloaded divider reaches +0.6 V above its emitter,
        // the junction carries the excess current. The capacitor then sees
        // R49 (or each R130/R146) into -14.4 V, not the series resistor pair.
        // Both expressions agree at the switching boundary; capacitor charge
        // and the ODE's first derivative stay continuous there.
        a[0] = {{ -(pullUp + series + sink + branch) / muteDriveNodeFarads,
                    series / muteDriveNodeFarads, branch / muteDriveNodeFarads,
                    (muteDriveRailVolts * (pullUp - sink) - sinkCurrent - junctionCurrent)
                        / muteDriveNodeFarads }};
        a[1] = {{ series / muteDriveHoldFarads, -(series + wetDown) / muteDriveHoldFarads,
                    0.0, wetRail * wetDown / muteDriveHoldFarads }};
        a[2] = {{ branch / clockMuteFarads, 0.0, -(branch + clockDown) / clockMuteFarads,
                    (junctionCurrent + clockRail * clockDown) / clockMuteFarads }};
        FixedMatrix<3> dcMatrix {}, dcDrive {}, dcSolution {};
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 3; ++column)
                dcMatrix[row][column] = a[row][column];
            dcDrive[row][0] = -a[row][3];
        }
        if (matrixSolve(dcMatrix, dcDrive, dcSolution))
            for (std::size_t row = 0; row < 3; ++row)
                circuit.equilibrium[row] = dcSolution[row][0];
        for (auto& row : a)
            for (double& value : row)
                value *= dt;
        circuit.transition = matrixExponential(a);
    }
    return chain;
}

std::array<double, 6> Chorus::SupportChain::HeldOutputMap::valueAt(
    double age) const noexcept
{
    std::array<double, 6> result {};
    if (!(age > 0.0))
        return result;
    if (age >= 1.0)
        return fullIntervalDrive;
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(bbdEventOutputHornerMacs, 18 * 6);
    YOUKNOW_COUNT_DOMAIN_WORK(bbdEventOutputEndpointScales, 6);
#endif
    for (std::size_t row = 0; row < 6; ++row)
        result[row] = endpointCorrection[row] * age;
    for (std::size_t power = byPower.size(); power > 0; --power)
        for (std::size_t row = 0; row < 6; ++row)
            result[row] = (result[row] + byPower[power - 1][row]) * age;
    return result;
}

double Chorus::InputCaptureInterval::valueAt(double fraction) const noexcept
{
    if (!enabled || !std::isfinite(fraction))
        return 0.0;
    if (fraction <= 0.0)
        return initialOutput;
    if (fraction >= 1.0)
        return finalOutput;
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(bbdDenseInputHornerMacs, degree);
#endif
    double result = outputByPower.back();
    for (int power = degree - 1; power >= 0; --power)
        result = result * fraction + outputByPower[static_cast<std::size_t>(power)];
    return std::isfinite(result) ? result : 0.0;
}

void Chorus::InputSupport::reset() noexcept
{
    couplingState = 0.0f;
    passiveState = 0.0f;
    antiAliasFirst.reset();
    antiAliasSecond.reset();
    exactState.fill(0.0);
    nonlinear = {};
    exactPrevious = 0.0;
    exactPrevious2 = 0.0;
    exactPrevious3 = 0.0;
    captureInterval = {};
}

float Chorus::advanceInputSupport(float input, bool prepareCapture) noexcept
{
    // Band-limit ahead of the lines. Everything above half the clock would
    // fold, exactly as it does in the part. The two Sallen-Key sections
    // precede the wet-only C44/C47 coupling high-pass; the passive
    // 10 kOhm / 2.2 nF pole is last, immediately beside the MN3009 input.
    // A corrupt host buffer must not poison the persistent support state.
    // Sixty-four model units is over 160 V at this node and therefore far
    // outside every circuit or engine fixture; treat anything beyond it as a
    // corrupt sample instead of turning it into a long full-scale BBD burst.
    constexpr float maximumSupportInput = 64.0f;
    const float supportInput = std::isfinite(input)
            && std::abs(input) <= maximumSupportInput
        ? input : 0.0f;
    auto& capture = inputSupport_.captureInterval;
    capture.enabled = false;
    const bool finiteFollowers = supportProfile_ == ChorusSupportProfile::Nominal2SA1015
        || supportProfile_ == ChorusSupportProfile::Nominal2SA1015Nonlinear;
    if (finiteFollowers || sampleRate_ >= Chorus::minimumExactInputSupportRate)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(bbdExactInputSupportAdvances, 1);
        YOUKNOW_COUNT_DOMAIN_WORK(bbdExactSupportCoordinateUpdates, 6);
        YOUKNOW_COUNT_DOMAIN_WORK(bbdExactSupportMacs, 60);
#endif
        const double exactInput = static_cast<double>(supportInput);
        const auto& transition =
            finiteFollowers && sampleRate_ < Chorus::minimumExactInputSupportRate
                ? support_.bilinearInput : support_.exactInput;
        std::array<double, 18> coordinates;
        const bool buildCapture = prepareCapture && support_.denseInput.available;
        const auto& oldHistory = inputSupport_.nonlinear;
        const bool cubicCurrents = transition.nonlinear.cubicResidual
            && oldHistory.valid && oldHistory.topology == transition.nonlinear.topology
            && oldHistory.currentHistoryDepth >= 3;
        if (buildCapture)
        {
            std::copy(inputSupport_.exactState.begin(), inputSupport_.exactState.end(),
                      coordinates.begin());
            coordinates[6] = exactInput;
            coordinates[7] = inputSupport_.exactPrevious;
            coordinates[8] = inputSupport_.exactPrevious2;
            coordinates[9] = inputSupport_.exactPrevious3;
            for (std::size_t port = 0; port < 2; ++port)
            {
                coordinates[10 + 4 * port] = 0.0;
                coordinates[11 + 4 * port] = oldHistory.previousCurrent[port];
                coordinates[12 + 4 * port] = oldHistory.previous2Current[port];
                coordinates[13 + 4 * port] = oldHistory.previous3Current[port];
            }
        }
        bool finiteIntervalAccepted;
        if (transition.nonlinear.enabled)
            finiteIntervalAccepted = advanceNonlinearSupport(inputSupport_.exactState, inputSupport_.nonlinear,
                transition, exactInput, inputSupport_.exactPrevious,
                inputSupport_.exactPrevious2, inputSupport_.exactPrevious3);
        else
            finiteIntervalAccepted = advanceExactSupport(inputSupport_.exactState, transition,
                exactInput, inputSupport_.exactPrevious,
                inputSupport_.exactPrevious2, inputSupport_.exactPrevious3);
        if (buildCapture && finiteIntervalAccepted)
        {
            const auto& history = inputSupport_.nonlinear;
            for (std::size_t port = 0; port < 2; ++port)
            {
                const std::size_t offset = 10 + 4 * port;
                if (!transition.nonlinear.enabled || !history.valid)
                {
                    // Numerical nonlinear fallback accepts the finite-linear
                    // endpoint; its consistent dense interval has zero q.
                    std::fill_n(coordinates.begin() + offset, 4, 0.0);
                    continue;
                }
                const double now = history.previousCurrent[port];
                // On re-prime this is qold solved from unchanged cap charge.
                const double old = history.previous2Current[port];
                coordinates[offset] = now;
                coordinates[offset + 1] = old;
                if (!cubicCurrents)
                {
                    // Collinear virtual histories reuse the cubic map for
                    // the accepted startup line qold+theta*(qnew-qold).
                    coordinates[offset + 2] = 2.0 * old - now;
                    coordinates[offset + 3] = 3.0 * old - 2.0 * now;
                }
            }
            constexpr double maximum =
                static_cast<double>(std::numeric_limits<float>::max()) / 16.0;
            bool finite = true;
            for (double value : coordinates)
                finite = finite && std::abs(value) <= maximum;
            capture.initialOutput = coordinates[5];
            capture.finalOutput = inputSupport_.exactState[5];
            double endpoint = 0.0;
#if defined(YOUKNOW_WORK_AUDIT)
            YOUKNOW_COUNT_DOMAIN_WORK(bbdDenseInputIntervals, 1);
            YOUKNOW_COUNT_DOMAIN_WORK(bbdDenseInputMacs, 11 * 18);
#endif
            for (std::size_t power = 0; power < support_.denseInput.outputByPower.size(); ++power)
            {
                double value = 0.0;
                for (std::size_t coordinate = 0; coordinate < coordinates.size(); ++coordinate)
                    value += support_.denseInput.outputByPower[power][coordinate]
                           * coordinates[coordinate];
                capture.outputByPower[power] = value;
                endpoint += value;
                finite = finite && std::abs(value) <= maximum;
            }
            // Pure numerical correction: preserves start derivatives through
            // 10, cancels the leading omitted term, and reaches the accepted
            // endpoint exactly (including its constant-input DC identity).
            capture.outputByPower.back() = capture.finalOutput - endpoint;
            capture.enabled = finite
                && std::abs(capture.outputByPower.back()) <= maximum
                && std::abs(capture.finalOutput) <= maximum;
        }
        inputSupport_.exactPrevious3 = inputSupport_.exactPrevious2;
        inputSupport_.exactPrevious2 = inputSupport_.exactPrevious;
        inputSupport_.exactPrevious = exactInput;
        return static_cast<float>(inputSupport_.exactState[5]);
    }

#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(bbdLegacyInputSupportFrames, 1);
#endif
    float limited = Chorus::biquadStep(
        inputSupport_.antiAliasFirst, supportInput, support_.antiAliasFirst);
    limited = Chorus::biquadStep(
        inputSupport_.antiAliasSecond, limited, support_.antiAliasSecond);
    const auto& inverse = support_.inputCouplingInverse;
    const auto& drive = support_.inputCouplingDrive;
    const double coupling = inverse[0][0] * inputSupport_.couplingState
                          + inverse[0][1] * inputSupport_.passiveState
                          + drive[0] * limited;
    const double passive = inverse[1][0] * inputSupport_.couplingState
                         + inverse[1][1] * inputSupport_.passiveState
                         + drive[1] * limited;
    inputSupport_.couplingState = 2.0 * coupling - inputSupport_.couplingState;
    inputSupport_.passiveState = 2.0 * passive - inputSupport_.passiveState;
    return static_cast<float>(passive);
}

void Chorus::Line::reset(std::uint32_t seed) noexcept
{
    outputEventCount = 0;
    outputEvents.fill({});
    cells.fill(0.0f);
    writeIndex = 0;
    clockPhase = 0.0;
    held = 0.0f;
    previousInput = 0.0f;
    previousInput2 = 0.0f;
    previousInput3 = 0.0f;
    exactOutputState.fill(0.0);
    nonlinearOutput = {};
    exactOutputPrevious = 0.0f;
    exactOutputPrevious2 = 0.0f;
    exactOutputPrevious3 = 0.0f;
    transferState = 0.0f;
    noiseState = seed | 1u;
    transferNoiseState = (seed ^ 0xd1b54a35u) | 1u;
    previousTransferNoise = 0.0f;
    bucketNoise = {};
    pastBlepEvents.fill({});
    pastBlepEventCount = 0;
}

void Chorus::Line::resetAudioRateSupport() noexcept
{
    outputEventCount = 0;
    outputEvents.fill({});
    // The interpolation histories are indexed on the numerical grid, and the
    // legacy low-rate TPT carries below embed its old interval. The engine
    // calls this only at zero output gain after waiting for musical tails.
    // BBD buckets, write/clock position, transfer state, held output and RNG
    // remain free-running.
    previousInput = 0.0f;
    previousInput2 = 0.0f;
    previousInput3 = 0.0f;
    // The exact coordinates are physical and mode-compatible, but this rate
    // boundary still deliberately reinitializes the entire numerical support
    // under the engine's established zero-gain transition. Preserving them
    // while clearing/reseeding the cubic drive is a separate qualification.
    // The shared input side is cleared beside this, by the same callers.
    exactOutputState.fill(0.0);
    nonlinearOutput = {};
    exactOutputPrevious = 0.0f;
    exactOutputPrevious2 = 0.0f;
    exactOutputPrevious3 = 0.0f;
    // Event ages are measured in samples of the numerical output grid. They
    // cannot be reinterpreted at the new rate, unlike the literal BBD state
    // deliberately preserved above. Their complete support is only two old
    // samples and the engine performs this reset under a zero-gain fade.
    pastBlepEvents.fill({});
    pastBlepEventCount = 0;
}

void Chorus::Line::ageBlepEvents() noexcept
{
    int retained = 0;
    for (int index = 0; index < pastBlepEventCount; ++index)
    {
        auto event = pastBlepEvents[static_cast<std::size_t>(index)];
        event.ageInSamples += 1.0;
        if (event.ageInSamples < 2.0)
            pastBlepEvents[static_cast<std::size_t>(retained++)] = event;
    }
    pastBlepEventCount = retained;
}

void Chorus::Line::rememberBlepEvent(float jump,
                                     double ageInSamples) noexcept
{
    // The compile-time bound above proves this cannot fill at any supported
    // clock/sample-rate ratio. Keep the guard in release builds nonetheless:
    // a future caller violating those declared limits must remain finite and
    // real-time safe rather than writing outside the fixed array.
    if (pastBlepEventCount >= maximumBlepEvents)
        return;

    pastBlepEvents[static_cast<std::size_t>(pastBlepEventCount++)] = {
        jump, ageInSamples
    };
}

double Chorus::Line::deterministicBlepCorrection(
    double clockIncrement, float noiseScale) const noexcept
{
    double correction = 0.0;

    // If the most recent edge changed s[-1] to s[0] by delta, the reference
    // output is s[0] + delta * beta(age). Older past edges have the same sign.
    for (int index = 0; index < pastBlepEventCount; ++index)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(blepPastCorrectionVisits, 1);
#endif
        const auto& event = pastBlepEvents[static_cast<std::size_t>(index)];
        correction += static_cast<double>(event.jump)
                    * Chorus::bbdPolyBlepResidual(event.ageInSamples);
    }

    if (!(clockIncrement > 0.0) || !std::isfinite(clockIncrement))
        return correction;

    // Buckets that will emerge during the residual's two-sample lookahead are
    // already in the ring: even at 200 kHz / 8 kHz there are at most 50, short
    // of one 128-cell revolution. Advance a local copy of the aggregate
    // transfer-loss state through those known values. The random edge source
    // is equally reproducible: advance a LOCAL RNG copy, never the live one.
    // Including its steps reconstructs the declared held-noise process before
    // sampling it on this numerical grid. Omitting those steps instead folded
    // extra broadband power into the audible band at low processing rates.
    // No physical state (bucket/index/phase/held/transfer/RNG) moves here.
    const double inverseIncrement = 1.0 / clockIncrement;
    // The output changes on the complementary half phase. If that edge has
    // already happened in this period, one input write intervenes before the
    // next output read; start the prediction cursor one slot farther ahead.
    const bool outputPendingThisPeriod = clockPhase < 0.5;
    double distance = ((outputPendingThisPeriod ? 0.5 : 1.5) - clockPhase)
                    * inverseIncrement;
    float predictedTransferState = transferState;
    float predictedHeld = held;
    std::uint32_t predictedNoiseState = noiseState;
    std::uint32_t predictedTransferNoiseState = transferNoiseState;
    float predictedPreviousTransferNoise = previousTransferNoise;
    int futureIndex = writeIndex;
    if (!outputPendingThisPeriod)
        futureIndex = futureIndex + 1 < cellPairs ? futureIndex + 1 : 0;

    for (int event = 0;
         event < maximumBlepEvents && distance < 2.0;
         ++event, distance += inverseIncrement)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(blepFuturePredictionVisits, 1);
#endif
        futureIndex = futureIndex + 1 < cellPairs ? futureIndex + 1 : 0;
        const float before = predictedHeld;
        Chorus::transferLossStep(
            predictedTransferState,
            cells[static_cast<std::size_t>(futureIndex)]);
        predictedNoiseState = nextNoiseState(predictedNoiseState);
        predictedTransferNoiseState = nextNoiseState(predictedTransferNoiseState);
        const float transferDraw = noiseFromState(predictedTransferNoiseState);
        const float draw = ChorusBucketNoise::step(noiseFromState(predictedNoiseState),
            transferDraw,predictedPreviousTransferNoise,bucketNoise);
        predictedPreviousTransferNoise = transferDraw;
        predictedHeld = predictedTransferState * signalInsertionGain + draw
            * Chorus::independentLineRandomAmplitude * noiseScale;
        const float jump = predictedHeld - before;

        // A future change s[0] -> s[1] enters the authors' correction with
        // the opposite sign: s[0] - (s[1] - s[0]) * beta(timeUntilEdge).
        correction -= static_cast<double>(jump)
                    * Chorus::bbdPolyBlepResidual(distance);
    }

    return correction;
}

float Chorus::Line::processClockedCore(float limitedInput, double clockHz,
                                       float sampleRate,
                                       float noiseScale,
                                       const InputCaptureInterval* capture,
                                       bool recoverHeldOutput) noexcept
{
    outputEventCount = 0;
    ageBlepEvents();

    const double increment =
        static_cast<double>(clockHz) / static_cast<double>(sampleRate);
    const double endPhase = clockPhase + increment;
    // Panasonic MN3009 p.43: C0 is captured when CP2 closes, then odd stages
    // move on CP1; the complementary edge moves even stages. The
    // alternating output followers read C256/C257: a captured sample holds over
    // [127.5,128.5) full periods, with center delay 128/fcp.
    // https://www.experimentalistsanonymous.com/diy/Datasheets/MN3009.pdf#page=2
    // Holters/Parker Eq.1 likewise places input/output on opposite edges:
    // https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf#page=2
    // A same-edge read/write ring adds an unintended half-period to that
    // center. Keep the 128-slot transport, but separate its input and output
    // events rather than compensating with a fitted time or clock offset.
    // At reset the zero ring includes the implicit zero capture at phase 0;
    // the first live output event is .5 and the first live input is 1.
    int halfEvent = clockPhase < 0.5 ? 1 : 2;
    for (int events = 0;
         events < maximumHalfCycleEventsPerSample && 0.5 * halfEvent <= endPhase;
         ++events, ++halfEvent)
    {
        const double eventPhase = 0.5 * halfEvent;
        const double ageInSamples = increment > 0.0
            ? std::clamp((endPhase - eventPhase) / increment, 0.0, 1.0)
            : 0.0;
        if ((halfEvent & 1) == 0)
        {
#if defined(YOUKNOW_WORK_AUDIT)
            YOUKNOW_COUNT_DOMAIN_WORK(bbdShifts, 1);
#endif
            float atEdge;
            if (capture != nullptr && capture->enabled)
            {
#if defined(YOUKNOW_WORK_AUDIT)
                YOUKNOW_COUNT_DOMAIN_WORK(bbdDenseInputCaptures, 1);
#endif
                atEdge = static_cast<float>(capture->valueAt(1.0 - ageInSamples));
            }
            else
                atEdge = Chorus::interpolateBbdInput(
                    limitedInput, previousInput, previousInput2, previousInput3,
                    ageInSamples);
            // Charge acquisition/overload happens only at the input edge.
            writeIndex = writeIndex + 1 < cellPairs ? writeIndex + 1 : 0;
            cells[static_cast<std::size_t>(writeIndex)] =
                transferProfile == ChorusBbdTransferProfile::ServicedBiasEstimate
                    ? Chorus::bbdServicedBiasTransfer(atEdge)
                    : Chorus::bbdTransfer(atEdge);
            continue;
        }

        // This slot will be overwritten by the next integer input edge.
        // Reading it here makes input n emerge at output phase n+127.5.
        const int readIndex = writeIndex + 1 < cellPairs ? writeIndex + 1 : 0;
        const float emerging = cells[static_cast<std::size_t>(readIndex)];
        const float heldBefore = held;
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(bbdPhysicalOutputEvents, 1);
#endif
        Chorus::transferLossStep(transferState, emerging);

        // Keep the literal per-output-event source and its rounded physical held
        // value unchanged. Its discontinuity needs the same host-grid
        // reconstruction as the signal; no new physical color or amplitude is
        // inferred. The continuous iid staircase's averaged PSD is
        // variance/fcp*sinc(f/fcp)^2 before the external reconstruction filter.
        // AuditChorusNoise checks an independently integrated staircase and
        // the high-rate limit, not a fitted noise spectrum from this part's
        // single A-weighted maximum row.
        // This iid source does not represent every BBD noise mechanism:
        // Weckler/Buss, Reticon 1977, printed p.5/Fig.6 describe transfer
        // noise with a sin^2(pi*f/fcp) shape and distributed transfer loss.
        // https://www.imagesensors.org/Past%20Workshops/Marvin%20White%20Collection/1977%20Short%20Course/1977%203%20Weckler.pdf
        // A unit-variance first difference reproduces the ideal shape, but
        // alone overstates the captured high/low density contrast. The optional
        // effective covariance family splits the existing source budget and
        // normalizes A-weighted output, with explicit fraction/correlation.
        // These do not identify microscopic storage/transfer strengths(OQ-03).
        // Both extra state and the legacy draw advance ONLY at physical output
        // events; the BLEP predictor uses copies of these SAME histories.
        noiseState = nextNoiseState(noiseState);
        transferNoiseState = nextNoiseState(transferNoiseState);
        const float transferDraw = noiseFromState(transferNoiseState);
        const float draw = ChorusBucketNoise::step(noiseFromState(noiseState),
            transferDraw,previousTransferNoise,bucketNoise);
        previousTransferNoise = transferDraw;
        // Absolute insertion gain belongs to the emerging signal source,
        // before postfilter/follower drive. The independent random term is
        // already output-referred; it is not multiplied by signal gain.
        held = transferState * signalInsertionGain + draw
               * Chorus::independentLineRandomAmplitude * noiseScale;
        rememberBlepEvent(held - heldBefore, ageInSamples);
        if (recoverHeldOutput && outputEventCount < maximumHalfCycleEventsPerSample)
            outputEvents[static_cast<std::size_t>(outputEventCount++)] = {
                static_cast<double>(held) - static_cast<double>(heldBefore),
                ageInSamples
            };
    }
    // Every declared-rate event fits the bound. If a corrupt ratio exceeds
    // it, discard excess whole periods instead of retaining a clock backlog.
    // Stopped clocks retain the fractional position and consume no events.
    clockPhase = endPhase;
    if (clockPhase >= 1.0)
        clockPhase -= std::floor(clockPhase);
    previousInput3 = previousInput2;
    previousInput2 = previousInput;
    previousInput = limitedInput;

    if (recoverHeldOutput)
        return held;
    return held + static_cast<float>(deterministicBlepCorrection(increment, noiseScale));
}

float Chorus::Line::process(
    float limited, double clockHz, float sampleRate,
    const SupportChain::ExactTransition& outputTransition,
    float noiseScale, bool useBlep,
    const InputCaptureInterval* capture) noexcept
{
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(bbdLineFrames, 1);
#endif
    const auto& eventMap = outputTransition.heldOutputMap;
    const bool eventOutput = eventMap.available;
    const double heldStart = held;
    const float correctedHold = processClockedCore(
        limited, clockHz, sampleRate, noiseScale, capture, eventOutput);
    const float reconstructedHold = eventOutput ? held : (useBlep ? correctedHold : held);
    std::array<double, 6> forcing {};
    if (eventOutput)
    {
#if defined(YOUKNOW_WORK_AUDIT)
        YOUKNOW_COUNT_DOMAIN_WORK(bbdEventOutputFrames, 1);
        YOUKNOW_COUNT_DOMAIN_WORK(bbdEventOutputForcingMacs, 6);
#endif
        for (std::size_t row = 0; row < 6; ++row)
            forcing[row] = eventMap.fullIntervalDrive[row] * heldStart;
        for (int event = 0; event < outputEventCount; ++event)
        {
#if defined(YOUKNOW_WORK_AUDIT)
            YOUKNOW_COUNT_DOMAIN_WORK(bbdEventOutputEvents, 1);
            YOUKNOW_COUNT_DOMAIN_WORK(bbdEventOutputForcingMacs, 6);
#endif
            const auto& change = outputEvents[static_cast<std::size_t>(event)];
            const auto weight = eventMap.valueAt(change.ageInSamples);
            for (std::size_t row = 0; row < 6; ++row)
                forcing[row] += weight[row] * change.jump;
        }
    }
    const auto* forcingPointer = eventOutput ? &forcing : nullptr;

    // HQ integrates the literal held transfer/noise steps through all output
    // poles before sampling; lower grids retain the qualified BLEP/cubic
    // reconstruction. The nonlinear current polynomial still spans this
    // complete uniform interval: exact linear step forcing does not make
    // that residual-current interpolation an exact nonlinear solution.
    // Held jumps leave current continuous but can kink its derivative, so
    // the cubic residual need not retain its smooth-input convergence order.
    const double exactOutput = std::isfinite(reconstructedHold)
        ? static_cast<double>(reconstructedHold) : 0.0;
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(bbdExactOutputSupportAdvances, 1);
    YOUKNOW_COUNT_DOMAIN_WORK(bbdExactSupportCoordinateUpdates, 6);
    YOUKNOW_COUNT_DOMAIN_WORK(bbdExactSupportMacs, eventOutput ? 36 : 60);
#endif
    if (outputTransition.nonlinear.enabled)
        advanceNonlinearSupport(exactOutputState, nonlinearOutput, outputTransition,
            exactOutput, eventOutput ? heldStart : exactOutputPrevious,
            exactOutputPrevious2, exactOutputPrevious3, forcingPointer);
    else
        advanceExactSupport(exactOutputState, outputTransition,
            exactOutput, exactOutputPrevious, exactOutputPrevious2, exactOutputPrevious3,
            forcingPointer);
    exactOutputPrevious3 = exactOutputPrevious2;
    exactOutputPrevious2 = exactOutputPrevious;
    exactOutputPrevious = exactOutput;
    if (outputTransition.finiteReadout)
    {
        double output = outputTransition.outputDirect * exactOutput;
        for (std::size_t index = 0; index < exactOutputState.size(); ++index)
            output += outputTransition.outputByState[index] * exactOutputState[index];
        if (outputTransition.nonlinear.enabled && nonlinearOutput.valid)
            for (std::size_t port = 0; port < 2; ++port)
                output += outputTransition.nonlinear.outputByCurrent[port]
                        * nonlinearOutput.previousCurrent[port];
        return static_cast<float>(output);
    }
    return static_cast<float>(exactOutputState[4] - exactOutputState[5]);
}

bool Chorus::configureSupportProfile(ChorusSupportProfile profile) noexcept
{
    if (supportProfilePrepared_)
        return false;
    supportProfile_ = profile;
    supportRatesPrepared_ = false;
    return true;
}

bool Chorus::configureBbdTransferProfile(ChorusBbdTransferProfile profile) noexcept
{
    if (supportProfilePrepared_
        || (profile != ChorusBbdTransferProfile::Legacy
            && profile != ChorusBbdTransferProfile::ServicedBiasEstimate))
        return false;
    lineA_.transferProfile = lineB_.transferProfile = profile;
    return true;
}

bool Chorus::configureBbdInsertionGainProfile(ChorusBbdInsertionGainProfile profile) noexcept
{
    if (supportProfilePrepared_
        || (profile != ChorusBbdInsertionGainProfile::UnityReference
            && profile != ChorusBbdInsertionGainProfile::HoltersParkerJuno60Estimate))
        return false;
    // +2.3 dB from the same MN3009 in Holters/Parker's Juno-60 circuit.
    // Compute outside processing; neither this approximate sibling prior nor
    // the floating-point factor identifies an installed Juno-106 chip gain.
    const float gain = profile == ChorusBbdInsertionGainProfile::UnityReference
        ? 1.0f : static_cast<float>(std::pow(10.0, 2.3 / 20.0));
    lineA_.insertionGainProfile = lineB_.insertionGainProfile = profile;
    lineA_.signalInsertionGain = lineB_.signalInsertionGain = gain;
    return true;
}

void Chorus::prepareSupportRates(double hostSampleRate) noexcept
{
    constexpr std::array<int, 3> factors { 1, 2, 4 };
    const double base = std::clamp(hostSampleRate, 8000.0, 768000.0);
    for (std::size_t index = 0; index < factors.size(); ++index)
    {
        const auto rate = static_cast<float> (
            std::clamp(base * static_cast<double> (factors[index]),
                       8000.0, 768000.0));
        preparedSupportRates_[index] = rate;
        preparedSupport_[index] = supportChainFor(rate, supportProfile_);
        ++supportBuildCount_;
    }
    supportRatesPrepared_ = true;
}

namespace
{
using NoiseMomentTable=std::array<double,Chorus::noiseMomentIntervals+1>;
NoiseMomentTable makeNoiseMoments(ChorusSupportProfile profile) noexcept
{
    // Analogue small-signal transfer of the SAME connected post circuit,
    // including the selected finite followers and nominal JFET Ron. Noise
    // voltage is small relative to Vt; the nonlinear profile has this same
    // idle tangent. This normalization is independent of numerical rate.
    // The budget is defined at the nominal SETTLED conducting product gate.
    // Keep that source reference during a mute transition: recalibrating to
    // the departing load would counteract physical attenuation. Diagnostics
    // with finite drive disabled retain this nominal reference rather than
    // claiming an exact budget for their ideal-switch loading.
    const double ratio=ChorusMuteDrive::conductanceRatio(0.0);
    AnalogMatrix a; AnalogDrive b,readout{{0,0,0,0,1,-1}}; double direct=0;
    if(profile==ChorusSupportProfile::IdealFollowers)
    {a=outputSupportMatrix(ratio);b=outputSupportDrive();}
    else
    {const auto circuit=finiteSupportCircuit(false,ratio);a=circuit.generator;b=circuit.drive;readout=circuit.outputByState;direct=circuit.outputDirect;}
    constexpr int quadrature=2048;
    std::array<double,quadrature> weightedPower{},frequency{};
    for(int k=0;k<quadrature;++k)
    {
        const double f=20.0+(k+.5)*(20000.0-20.0)/quadrature;
        frequency[k]=f;
        std::array<std::array<std::complex<double>,7>,6> matrix{};
        for(int i=0;i<6;++i){for(int j=0;j<6;++j)matrix[i][j]=-a[i][j];matrix[i][i]+=std::complex<double>(0,2*pi*f);matrix[i][6]=b[i];}
        for(int j=0;j<6;++j)
        {
            int pivot=j;for(int i=j+1;i<6;++i)if(std::norm(matrix[i][j])>std::norm(matrix[pivot][j]))pivot=i;
            std::swap(matrix[j],matrix[pivot]);const auto divisor=matrix[j][j];
            for(int k2=j;k2<7;++k2)matrix[j][k2]/=divisor;
            for(int i=0;i<6;++i)if(i!=j){const auto scale=matrix[i][j];for(int k2=j;k2<7;++k2)matrix[i][k2]-=scale*matrix[j][k2];}
        }
        std::complex<double> output=direct;for(int i=0;i<6;++i)output+=readout[i]*matrix[i][6];
        const double f2=f*f;
        const double ra=12194.0*12194.0*f2*f2/((f2+20.6*20.6)*std::sqrt((f2+107.7*107.7)*(f2+737.9*737.9))*(f2+12194.0*12194.0));
        weightedPower[k]=std::norm(output)*std::pow(ra/.79434639,2);
    }
    NoiseMomentTable result{};
    for(std::size_t i=0;i<result.size();++i)
    {
        const double clock=10000.0+190000.0*i/Chorus::noiseMomentIntervals;
        double power=0,moment=0;
        for(int k=0;k<quadrature;++k)
        {const double x=pi*frequency[k]/clock;const double w=weightedPower[k]*std::pow(std::sin(x)/x,2);power+=w;moment+=w*std::cos(2*x);}
        result[i]=moment/power;
    }
    return result;
}
const NoiseMomentTable& noiseMomentsFor(ChorusSupportProfile profile) noexcept
{
    // Warmed by prepare(), never lazily initialized on an audio callback.
    if(profile==ChorusSupportProfile::IdealFollowers)
    {static const auto ideal=makeNoiseMoments(profile);return ideal;}
    static const auto finite=makeNoiseMoments(ChorusSupportProfile::Nominal2SA1015);
    return finite;
}
}
double Chorus::bucketNoiseCosineMoment(double clockHz) const noexcept
{
    if(noiseCosineMoments_==nullptr||!std::isfinite(clockHz))return 0.0;
    const double index=(std::clamp(clockHz,10000.0,200000.0)-10000.0)
                       *noiseMomentIntervals/190000.0;
    const auto i=std::min(static_cast<std::size_t>(index),noiseMomentIntervals-1);
    const double t=index-i;
    return (*noiseCosineMoments_)[i]+t*((*noiseCosineMoments_)[i+1]-(*noiseCosineMoments_)[i]);
}

void Chorus::prepare(double sampleRate, bool preserveState) noexcept
{
    supportProfilePrepared_ = true;
    noiseCosineMoments_ = &noiseMomentsFor(supportProfile_);
    sampleRate_ = static_cast<float>(std::clamp(sampleRate, 8000.0, 768000.0));
    inverseSampleRate_ = 1.0f / sampleRate_;
    wetMuteGlide_ = 1.0f - std::exp(-inverseSampleRate_ / wetMuteTimeConstantSeconds);
    const auto cached = supportRatesPrepared_
        ? std::find(preparedSupportRates_.begin(), preparedSupportRates_.end(),
                    sampleRate_)
        : preparedSupportRates_.end();
    if (cached != preparedSupportRates_.end())
    {
        support_ = preparedSupport_[static_cast<std::size_t> (
            std::distance(preparedSupportRates_.begin(), cached))];
    }
    else
    {
        support_ = supportChainFor(sampleRate_, supportProfile_);
        ++supportBuildCount_;
    }
    wetTransitionRatio_ = -1.0;
    if (preserveState)
    {
        lineA_.resetAudioRateSupport();
        lineB_.resetAudioRateSupport();
        inputSupport_.reset();
    }
    else
        reset(false);
}

void Chorus::reset(bool preserveLfoPhase) noexcept
{
    const double continuingPhase = lfoPhase_;
    lineA_.reset(0x9e3779b9u);
    lineB_.reset(0x85ebca6bu);
    inputSupport_.reset();
    lfoPhase_ = preserveLfoPhase ? continuingPhase : 0.0;
    const auto runningWhileMuted = settingsFor(ChorusMode::Off);
    wetGain_ = runningWhileMuted.wetGain;
    rateHz_ = runningWhileMuted.rateHz;
    sweep_ = runningWhileMuted.sweepSeconds;
    centreDelay_ = runningWhileMuted.centreDelaySeconds;
    commonNoiseState_ = 0xd1b54a35u;
    orthogonalNoiseState_ = 0x94d049bbu;
    humPhase_ = 0.0;
    clockSpurPhaseA_ = 0.0;
    clockSpurPhaseB_ = 0.0;
    optionalSpurPhaseA_ = 0.0;
    optionalSpurPhaseB_ = 0.0;
    runningMode_ = ChorusMode::One;
    hardwareModeSelection_ = ChorusMode::Off;
    // A patch loaded with the effect switched on is not a player reaching for
    // the button: there is nothing to glide from. The first sample after a
    // reset takes the mode as it stands, and only changes made afterwards
    // glide. The mute drive is primed to the same rest on that first sample.
    primed_ = false;
    muteDriveNodeVolts_ = muteDriveMutedNodeRestVolts();
    muteDriveHoldVolts_ = muteDriveHoldRestVolts(muteDriveNodeVolts_);
    muteDriveMuted_ = true;
    muteDriveEnabled_ = false;
    finiteMuteDriveEnabled_ = false;
    finiteTr5DriveEnabled_ = false;
    muteGateVolts_ = -14.4;
    wetInputConductanceRatio_ = 0.0;
    wetTransitionRatio_ = -1.0;
    clockMuteVolts_ = -muteDriveRailVolts;
    clockMuteEnabled_ = false;
    clocksStopped_ = false;
}

float Chorus::lineInsertionGainDraw() noexcept
{
    // One fixed-seed bipolar draw, from line A's own noise seed, so the same
    // instrument renders every launch. Plain integer hash; no claim of a
    // distribution shape.
    std::uint32_t x = 0x9e3779b9u;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return static_cast<float>(x & 0xffffffu) * (2.0f / 16777215.0f) - 1.0f;
}

float Chorus::rateProportionalNoiseGain(float rateHz) noexcept
{
    const float reference = static_cast<float>(derivedRateHz(true));
    if (!(rateHz > 0.0f) || !(reference > 0.0f))
        return 1.0f;
    return rateHz / reference;
}

bool Chorus::processBypassedWhenSettled(float input, float& left,
                                        float& right) noexcept
{
    // Until the wet-mute glide has decayed to exactly zero -- or before the
    // first process() call has primed the settings -- the full path still
    // contributes audible wet, so the caller must keep running it.
    // This comparison circuit preserves stopped bucket charge and all analog
    // filter coordinates. The old skip deliberately discards that history;
    // keep ordinary support evolution here (clock=0 already skips BBD shifts).
    if (!primed_ || wetGain_ != 0.0f || clockMuteEnabled_)
        return false;

    // Muting the return does not disconnect C16/C13 from their drive circuit
    // (Service Notes p. 15). Keep charging them after the audible glide ends,
    // or a later engage starts from stale charge and opens earlier than Exact.
    if (muteDriveEnabled_)
        advanceMuteDrive(true);

    // The modulation LFO free-runs behind the switch, exactly as it does
    // through the full path with mode Off.
    lfoPhase_ += static_cast<double>(rateHz_) / static_cast<double>(sampleRate_);
    if (lfoPhase_ >= 1.0f)
        lfoPhase_ -= std::floor(lfoPhase_);

    wetPathFlushPending_ = true;
    left = dryMixGain * input;
    right = dryMixGain * input;
    return true;
}

void Chorus::advanceMuteDrive(bool commandMute) noexcept
{
    if (clockMuteEnabled_)
    {
        advanceClockMuteDrive(commandMute);
        return;
    }
    // Both command states retain the two physical capacitor coordinates.
    // Only the prepared conductance matrix and its DC equilibrium switch;
    // there is no charge reset when Tr5 begins conducting through R46.
    const double nodeRest = commandMute ? muteDriveMutedNodeRestVolts()
                                       : muteDriveConductingNodeRestVolts();
    const double holdRest = muteDriveHoldRestVolts(nodeRest);
    const auto& transition = commandMute ? support_.muteDriveOpenTransition
                                        : support_.muteDriveConductingTransition;
    const double node = muteDriveNodeVolts_ - nodeRest;
    const double hold = muteDriveHoldVolts_ - holdRest;
    muteDriveNodeVolts_ = nodeRest + transition[0][0] * node + transition[0][1] * hold;
    muteDriveHoldVolts_ = holdRest + transition[1][0] * node + transition[1][1] * hold;
    updateWetGate();
}

void Chorus::updateWetGate() noexcept
{
    if (finiteMuteDriveEnabled_)
    {
        muteGateVolts_ = ChorusMuteDrive::gateVolts(muteDriveHoldVolts_);
        wetInputConductanceRatio_ = ChorusMuteDrive::conductanceRatio(muteGateVolts_);
        muteDriveMuted_ = wetInputConductanceRatio_ == 0.0;
    }
    else
        muteDriveMuted_ = muteDriveHoldVolts_ >= muteDriveThresholdVolts;
}

namespace
{
template<class T> void blendWetArray(T& result, const T& a, const T& b, double t)
{
    if constexpr (std::is_floating_point_v<T>) result = a + t * (b - a);
    else for (std::size_t i=0; i<result.size(); ++i) blendWetArray(result[i], a[i], b[i], t);
}
}
const Chorus::SupportChain::ExactTransition& Chorus::finiteWetTransition() noexcept
{
    if (wetInputConductanceRatio_ == 0.0) return support_.wetConductance.front();
    if (wetInputConductanceRatio_ == 1.0) return support_.wetConductance.back();
    if (wetTransitionRatio_ == wetInputConductanceRatio_) return wetTransition_;
    const double index = wetInputConductanceRatio_ * SupportChain::wetConductanceIntervals;
    const auto lower = static_cast<std::size_t>(index);
    const double blend = index - lower;
    const auto& a = support_.wetConductance[lower];
    const auto& b = support_.wetConductance[lower+1];
    wetTransition_ = a;
    auto& r=wetTransition_;
    blendWetArray(r.stateByColumn,a.stateByColumn,b.stateByColumn,blend);
    blendWetArray(r.driveBySample,a.driveBySample,b.driveBySample,blend);
    blendWetArray(r.outputByState,a.outputByState,b.outputByState,blend);
    r.outputDirect=a.outputDirect+blend*(b.outputDirect-a.outputDirect);
    blendWetArray(r.heldOutputMap.byPower,a.heldOutputMap.byPower,b.heldOutputMap.byPower,blend);
    blendWetArray(r.heldOutputMap.fullIntervalDrive,a.heldOutputMap.fullIntervalDrive,b.heldOutputMap.fullIntervalDrive,blend);
    blendWetArray(r.heldOutputMap.endpointCorrection,a.heldOutputMap.endpointCorrection,b.heldOutputMap.endpointCorrection,blend);
    auto& n=r.nonlinear;
    const auto& na=a.nonlinear; const auto& nb=b.nonlinear;
    blendWetArray(n.junctionByState,na.junctionByState,nb.junctionByState,blend);
    blendWetArray(n.junctionByInput,na.junctionByInput,nb.junctionByInput,blend);
    blendWetArray(n.junctionByCurrent,na.junctionByCurrent,nb.junctionByCurrent,blend);
    blendWetArray(n.endpointJunctionByCurrent,na.endpointJunctionByCurrent,nb.endpointJunctionByCurrent,blend);
    blendWetArray(n.stateByPreviousCurrent,na.stateByPreviousCurrent,nb.stateByPreviousCurrent,blend);
    blendWetArray(n.stateByCurrent,na.stateByCurrent,nb.stateByCurrent,blend);
    blendWetArray(n.outputByCurrent,na.outputByCurrent,nb.outputByCurrent,blend);
    blendWetArray(n.cubicStateByCurrentSample,na.cubicStateByCurrentSample,nb.cubicStateByCurrentSample,blend);
    blendWetArray(n.cubicEndpointJunctionByCurrent,na.cubicEndpointJunctionByCurrent,nb.cubicEndpointJunctionByCurrent,blend);
    // Positive channel conductance is always the same connected topology;
    // crossing an interpolation grid must not reset capacitor/current history.
    n.topology=3;
    wetTransitionRatio_=wetInputConductanceRatio_;
    return r;
}

void Chorus::advanceClockMuteDrive(bool commandMute) noexcept
{
    using State = std::array<double, 4>;
    State state {{ muteDriveNodeVolts_, muteDriveHoldVolts_, clockMuteVolts_, 1.0 }};
    const auto apply = [](const FixedMatrix<4>& matrix, const State& input) {
        State result {};
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 4; ++column)
                result[row] += matrix[row][column] * input[column];
        return result;
    };
    // Fractional intervals are needed at the three junction crossings and
    // optionally where Tr5 leaves its base-current-limited active region.
    // A 12-term exponential action is converged on the supported >=8 kHz
    // grid (fastest RC ~0.7 ms), without constructing matrices on the callback.
    const auto fractional = [&](const FixedMatrix<4>& generator,
                                const State& input, double fraction) {
        State result = input;
        State term = input;
        for (int order = 1; order <= 12; ++order)
        {
            term = apply(generator, term);
            for (std::size_t row = 0; row < 4; ++row)
            {
                term[row] *= fraction / order;
                result[row] += term[row];
            }
        }
        return result;
    };
    const auto junctionVoltages = [](const State& value) {
        return std::array<double, 4> {{
            value[2] - value[0] - muteDriveJunctionVolts,
            value[1] - muteDriveThresholdVolts,
            value[2] - clockMuteThresholdVolts,
            value[0] - tr5CurrentLimitNodeVolts
        }};
    };
    const auto initial = junctionVoltages(state);
    std::size_t region = (initial[0] > 0.0 ? 1u : 0u)
                       | (initial[1] > 0.0 ? 2u : 0u)
                       | (initial[2] > 0.0 ? 4u : 0u)
                       | (finiteTr5DriveEnabled_ && !commandMute && initial[3] > 0.0
                              ? 16u : 0u);
    const std::size_t command = commandMute ? 0u : 8u;
    const auto circuitIndex = [&](std::size_t junctionRegion) {
        return (junctionRegion & 16u) != 0
             ? 16u + (junctionRegion & 7u) : command | junctionRegion;
    };
    double remaining = 1.0;
    // A sample can cross more than one junction, especially immediately after
    // an interrupted command. Process the earliest crossing and then all the
    // remaining charge evolution. Eight segments bound callback work even for
    // a degenerate state exactly on several boundaries; three independent
    // junctions plus the optional Tr5 current-limit boundary are ample margin
    // on the supported >=8 kHz passive RC grid.
    for (int segment = 0; segment < 8 && remaining > 0.0; ++segment)
    {
        const auto& circuit = support_.clockMuteTransitions[circuitIndex(region)];
        const State candidate = remaining == 1.0
            ? apply(circuit.transition, state)
            : fractional(circuit.generator, state, remaining);
        const auto candidateVoltages = junctionVoltages(candidate);
        double firstCrossing = remaining;
        std::size_t firstBit = 0;
        const std::size_t boundaries = finiteTr5DriveEnabled_ && !commandMute ? 4u : 3u;
        for (std::size_t junction = 0; junction < boundaries; ++junction)
        {
            const std::size_t bit = junction == 3 ? 16u : std::size_t { 1 } << junction;
            const bool conducting = (region & bit) != 0;
            // Suppress roundoff-sized excursions at simultaneous crossings;
            // the omitted current is <1e-16 A, not a physical hysteresis model.
            const double signedEnd = conducting ? -candidateVoltages[junction]
                                                : candidateVoltages[junction];
            if (signedEnd <= 1.0e-12)
                continue;
            double lower = 0.0, upper = remaining;
            for (int iteration = 0; iteration < 32; ++iteration)
            {
                const double middle = 0.5 * (lower + upper);
                const auto value = fractional(circuit.generator, state, middle);
                if ((junctionVoltages(value)[junction] > 0.0) == conducting)
                    lower = middle;
                else
                    upper = middle;
            }
            const double crossing = 0.5 * (lower + upper);
            if (crossing < firstCrossing)
            {
                firstCrossing = crossing;
                firstBit = bit;
            }
        }
        if (firstBit == 0)
        {
            state = candidate;
            remaining = 0.0;
        }
        else
        {
            state = fractional(circuit.generator, state, firstCrossing);
            remaining -= firstCrossing;
            region ^= firstBit;
        }
    }
    // Numerical fail-safe only: finish a degenerate sub-sample with the last
    // region rather than losing elapsed time or resetting capacitor charge.
    // The component-node reference includes simultaneous/multiple crossings.
    if (remaining > 0.0)
        state = fractional(support_.clockMuteTransitions[circuitIndex(region)].generator,
                           state, remaining);
    muteDriveNodeVolts_ = state[0];
    muteDriveHoldVolts_ = state[1];
    clockMuteVolts_ = state[2];
    updateWetGate();
    clocksStopped_ = clockMuteVolts_ >= clockMuteThresholdVolts;
}

void Chorus::process(float input, ChorusMode mode, float noiseScale,
                     float& left, float& right,
                     bool enableClockBleed,
                     bool enableHyperbolicSweep,
                     float calibration,
                     bool useRateProportionalNoiseHypothesis,
                     bool enableNarrowOneTwo,
                     bool enableMuteDrive,
                     bool enableLineGainSpread,
                     ChorusTimingProfile timingProfile,
                     bool enableClockMuteCircuit,
                     bool enableFiniteMuteDrive,
                     bool enableCorrelatedNoise,
                     float noiseTransferFraction,
                     float noiseTransferCorrelation,
                     bool enableFiniteTr5Drive) noexcept
{
#if defined(YOUKNOW_WORK_AUDIT)
    YOUKNOW_COUNT_DOMAIN_WORK(chorusFrames, 1);
#endif
    // The settled bypass skipped the muted lines, so their content is stale
    // history from before the skip began. Rebuild the wet path from silence:
    // the established from-zero wet glide then brings the effect in exactly
    // as a patch loaded with chorus engaged comes in.
    if (wetPathFlushPending_) [[unlikely]]
    {
        wetPathFlushPending_ = false;
        lineA_.reset(0x9e3779b9u);
        lineB_.reset(0x85ebca6bu);
        inputSupport_.reset();
        clockSpurPhaseA_ = 0.0;
        clockSpurPhaseB_ = 0.0;
    }

    const auto target = settingsFor(mode, timingProfile);
    const bool hardwareSelection = hardwareModeSelection_ != ChorusMode::Off;
    const auto clockTarget = hardwareSelection
                           ? settingsFor(hardwareModeSelection_, timingProfile) : target;

    const bool commandMute = mode == ChorusMode::Off;
    // Finite collector current requires the clamped-base KCL of the complete
    // three-capacitor circuit; the passive raw two-node comparison omits it.
    const bool nextClockMuteEnabled =
        (enableClockMuteCircuit || enableFiniteMuteDrive || enableFiniteTr5Drive) && enableMuteDrive;
    finiteMuteDriveEnabled_ = enableFiniteMuteDrive && enableMuteDrive;
    finiteTr5DriveEnabled_ = enableFiniteTr5Drive && enableMuteDrive;
    if (!primed_)
    {
        rateHz_ = clockTarget.rateHz;
        sweep_ = clockTarget.sweepSeconds;
        centreDelay_ = clockTarget.centreDelaySeconds;
        wetGain_ = target.wetGain;
        // The drive rests where the command has held it: Tr5 open and both
        // capacitors at their positive rests when muted, or the finite-R46
        // loaded rests when conducting.
        muteDriveNodeVolts_ = commandMute ? muteDriveMutedNodeRestVolts()
                                          : muteDriveConductingNodeRestVolts();
        muteDriveHoldVolts_ = muteDriveHoldRestVolts(muteDriveNodeVolts_);
        muteDriveMuted_ = commandMute;
        if (nextClockMuteEnabled)
        {
            // D3 is reverse biased at both settled command states.
            const auto& rest = support_.clockMuteTransitions[commandMute ? 6u : 8u].equilibrium;
            muteDriveNodeVolts_ = rest[0];
            muteDriveHoldVolts_ = rest[1];
            clockMuteVolts_ = rest[2];
        }
        primed_ = true;
    }

    clockMuteEnabled_ = nextClockMuteEnabled;
    clocksStopped_ = clockMuteEnabled_ && clockMuteVolts_ >= clockMuteThresholdVolts;

    if (mode != ChorusMode::Off || hardwareSelection)
    {
        rateHz_ = clockTarget.rateHz;
        sweep_ = clockTarget.sweepSeconds;
        centreDelay_ = clockTarget.centreDelaySeconds;
        runningMode_ = hardwareSelection ? hardwareModeSelection_ : mode;
    }
    float wetTarget = target.wetGain;
    muteDriveEnabled_ = enableMuteDrive;
    if (enableMuteDrive)
    {
        advanceMuteDrive(commandMute);
        wetTarget = settingsFor(runningMode_).wetGain * (finiteMuteDriveEnabled_
            ? static_cast<float>(wetInputConductanceRatio_) : (muteDriveMuted_ ? 0.0f : 1.0f));
    }
    else
    {
        muteDriveMuted_ = commandMute;
    }
    if (finiteMuteDriveEnabled_)
        wetGain_ = wetTarget; // physical channel trajectory already continuous
    else
        wetGain_ += (wetTarget - wetGain_) * wetMuteGlide_;
    // The glide is geometric and never reaches zero by itself: below about
    // 1.4e-42 the product underflows and wetGain_ parks on a denormal, so the
    // exact-zero test in processBypassedWhenSettled only ever passed under
    // the plug-in's ScopedNoDenormals. Flush at FLT_MIN, the flush-to-zero
    // threshold that mode applies, so the JUCE-free tools and tests settle
    // the same way the plug-in does. Nothing audible moves: a gain below
    // FLT_MIN is already zero in the mix.
    if (std::abs(wetGain_) < std::numeric_limits<float>::min())
        wetGain_ = 0.0f;
    // The finite drive uses the named same-part incremental JFET channel
    // law, rather than a binary threshold followed by the legacy 5 ms glide.
    // All downstream signal, C25/C28 loading and IC6 Norton noise use that
    // one continuous conductance. Signal-dependent channel distortion, gate
    // capacitance and charge injection remain unmeasured; no transient is
    // invented. Raw/reference processing retains its declared declick glide.

    const double intervalStartPhase = lfoPhase_;
    const double phaseIncrement = static_cast<double>(rateHz_)
                                / static_cast<double>(sampleRate_);
    const double intervalEndPhase = intervalStartPhase + phaseIncrement;
    lfoPhase_ = intervalEndPhase;
    if (lfoPhase_ >= 1.0)
        lfoPhase_ -= std::floor(lfoPhase_);

    // Delay sweep trajectory. The linear-in-delay law below is the circuit's
    // own: on p. 15 each MN3101's oscillator is Tr19 (R123 1.8k / R124 8.2k /
    // R125 10k), a fixed current source charging C53 150 pF through R132
    // 6.8k, with the TP4 triangle setting the upper threshold through
    // Tr21/R129/D9 and Tr22 resetting C53 from OX3. A constant charge current
    // between a fixed lower and an LFO-set upper threshold makes the clock
    // period affine in the triangle voltage, so the delay (128 periods) is
    // linear in the LFO and the clock hyperbolic in it. KR-106's ~50-point
    // click-timing series across the modulation cycle (16 us RMS residual
    // against a straight line, recorded in OQ-01) corroborates the
    // derivation. It also renders the instrument's fixed-detune character: a
    // linear delay flank is a constant pitch offset, where a bent flank
    // slides through it.
    //
    // The path behind `enableHyperbolicSweep` is a comparison hypothesis that
    // does not describe this board: a current-modulated oscillator whose
    // clock is linear in the control voltage, hence a bending delay. It is
    // kept for A/B renders only. When it engages it bends about the clock's
    // own endpoints, not the delay's centre: an earlier centre-relative
    // revision rendered a 38%-too-wide 2.30-7.40 ms range at Unit Character
    // 1.0 instead of the then-shipped 1.66-5.35 ms, which OQ-01 records.
    // Bending about the endpoint clocks keeps both endpoints exact at every
    // blend amount, so the two laws differ only in the trajectory between
    // them.
    const auto clockAtPhase = [&](double phase) noexcept
    {
        phase -= std::floor(phase);
        const double modulation = triangle(phase);
        double nominalDelayA = centreDelay_ + sweep_ * modulation;
        double nominalDelayB = centreDelay_ - sweep_ * modulation;

        if (enableHyperbolicSweep && calibration > 0.0f && centreDelay_ > 1.0e-5f)
        {
            const double maxDelay = static_cast<double>(centreDelay_) + sweep_;
            const double minDelay = std::max(
                static_cast<double>(centreDelay_) - sweep_, 1.0e-5);
            const double clockAtMinDelay = cellPairs / minDelay;
            const double clockAtMaxDelay = cellPairs / maxDelay;
            const double clockMid = 0.5 * (clockAtMinDelay + clockAtMaxDelay);
            const double clockSpread = 0.5 * (clockAtMinDelay - clockAtMaxDelay);
            const double hypDelayA = cellPairs / (clockMid - clockSpread * modulation);
            const double hypDelayB = cellPairs / (clockMid + clockSpread * modulation);
            // Retain the comparison law and its bounded blend. Only the
            // numerical clock-integration point changes for this hypothesis.
            const double blend = std::clamp(static_cast<double>(calibration), 0.0, 1.0);
            nominalDelayA += (hypDelayA - nominalDelayA) * blend;
            nominalDelayB += (hypDelayB - nominalDelayB) * blend;
        }
        return std::array<double, 2> {{
            std::clamp(cellPairs / std::max(nominalDelayA, 1.0e-4),
                       static_cast<double>(minimumClockHz), static_cast<double>(maximumClockHz)),
            std::clamp(cellPairs / std::max(nominalDelayB, 1.0e-4),
                       static_cast<double>(minimumClockHz), static_cast<double>(maximumClockHz))
        }};
    };

    // Advance the BBD clock through the elapsed interval, not at its right
    // endpoint. With delay affine along a triangle flank, f=N/delay has the
    // exact integral (N/slope)*log(delayEnd/delayStart). Midpoint quadrature
    // has local O(dt^3) error there; an endpoint rectangle accumulates an
    // O(dt) phase bias that moves genuine clock-folded images in phase.
    // Split at triangle cusps before applying midpoint, so a derivative
    // discontinuity cannot degrade that bound. Declared LFO rates and the
    // 8 kHz minimum grid permit at most one cusp in an interval.
    // Tools/AuditBbdDynamicQuality.cpp independently integrates this clock
    // law with its exact logarithm/inverse. Continuous BBD event chronology:
    // https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf
    // No physical delay/clock-cycle convention or fitted time shift changes.
    auto clock = clockAtPhase(0.5 * (intervalStartPhase + intervalEndPhase));
    const double cusp = intervalStartPhase < 0.5 ? 0.5 : 1.0;
    if (intervalEndPhase > cusp)
    {
        const double firstFraction = (cusp - intervalStartPhase) / phaseIncrement;
        const auto first = clockAtPhase(0.5 * (intervalStartPhase + cusp));
        const auto second = clockAtPhase(0.5 * (cusp + intervalEndPhase));
        for (std::size_t line = 0; line < clock.size(); ++line)
            clock[line] = firstFraction * first[line]
                        + (1.0 - firstFraction) * second[line];
    }
    // This interval-average approximation feeds the existing constant-clock
    // edge-age calculation and two-sample BLEP predictor. Their within-sample
    // scheduling error remains a separate O(dt^2) approximation; fixed-clock
    // Line callers keep their exact former behavior, including RNG advances.
    const double clockA = clocksStopped_ ? 0.0 : clock[0];
    const double clockB = clocksStopped_ ? 0.0 : clock[1];
    if(enableCorrelatedNoise)
    {
        auto mix=ChorusBucketNoise::coefficients(noiseTransferFraction,noiseTransferCorrelation);
        lineA_.bucketNoise=mix;lineB_.bucketNoise=mix;
        if(mix.eta!=0.0)
        {
            ChorusBucketNoise::normalize(lineA_.bucketNoise,bucketNoiseCosineMoment(clock[0]));
            ChorusBucketNoise::normalize(lineB_.bucketNoise,bucketNoiseCosineMoment(clock[1]));
        }
    }
    else {lineA_.bucketNoise={};lineB_.bucketNoise={};}

    // C28/C25 see the 39 kOhm mixer legs through Tr11/Tr12, so their
    // loading follows the RC-delayed gate, including continuous finite
    // channel conductance when selected. Physical capacitor charge is never
    // reset at a command, a conductance-grid boundary or an open channel.
    const auto& wetOutputTransition = finiteMuteDriveEnabled_
        ? finiteWetTransition()
        : (muteDriveMuted_ ? support_.exactOutputMuted : support_.exactOutputConnected);
    // The relative real-instrument calibration and its alternative causal
    // hypothesis act on the lines' random floor only. Neither is a claim that
    // a standalone mode-II MN3009 exceeds its datasheet row: the observation
    // was made at the completed instrument's output, and the exact physical
    // insertion point remains OQ-03. The optional common/hum/spur layers below
    // stay on the plain Chorus Noise master.
    const float modeNoiseGain = useRateProportionalNoiseHypothesis
        ? rateProportionalNoiseGain(rateHz_)
        : measuredModeNoiseGain(runningMode_);
    const float lineNoiseScale = noiseScale * modeNoiseGain;
    // One input support network for both wet branches; only the clock differs
    // between them. See `InputSupport` for why that is the model rather than
    // an optimisation of it.
    // Evaluate the shared physical input filter at each BBD capture instant,
    // rather than interpolating four already filtered endpoint samples.
    // Fractional physical-filter evaluation: Holters/Parker2018, Section3.1.
    // https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf#page=2
    // This dense extension retains OUR existing cubic input/current forcing;
    // it does not replace it with that paper's impulse-invariant input model.
    const bool capturePending = support_.denseInput.available
        && ((clockA > 0.0 && lineA_.clockPhase + clockA / sampleRate_ >= 1.0)
            || (clockB > 0.0 && lineB_.clockPhase + clockB / sampleRate_ >= 1.0));
    const float limitedInput = advanceInputSupport(input, capturePending);
    const auto* capture = inputSupport_.captureInterval.enabled
        ? &inputSupport_.captureInterval : nullptr;
    float wetA = lineA_.process(limitedInput, clockA, sampleRate_,
                                wetOutputTransition, lineNoiseScale, true, capture);
    float wetB = lineB_.process(limitedInput, clockB, sampleRate_,
                                wetOutputTransition, lineNoiseScale, true, capture);

    if (enableClockBleed && !clocksStopped_)
    {
        clockSpurPhaseA_ += static_cast<double>(clockA) * inverseSampleRate_;
        clockSpurPhaseB_ += static_cast<double>(clockB) * inverseSampleRate_;
        clockSpurPhaseA_ -= std::floor(clockSpurPhaseA_);
        clockSpurPhaseB_ -= std::floor(clockSpurPhaseB_);
        // Scaled by the one Chorus Noise master with no floor. A revision
        // clamped this to `max(noiseScale, 0.1f)`, which left a tenth of the
        // bleed tone alive at noiseScale 0 and broke this class's own
        // contract -- process() documents 0.0 as removing every declared
        // chorus-noise component, and the bleed is one of them.
        const float bleedScale = 0.005f * noiseScale;
        const float heterodyneBleedA = bleedScale * static_cast<float>(std::sin(2.0 * pi * clockSpurPhaseA_));
        const float heterodyneBleedB = bleedScale * static_cast<float>(std::sin(2.0 * pi * clockSpurPhaseB_));
        wetA += heterodyneBleedA;
        wetB += heterodyneBleedB;
    }

    // These mechanisms are deliberately separate from the compatibility hiss
    // above.  Their insertion point, spectra, levels and stereo correlation
    // are all voiced/unknown pending the calibrated OQ-03 capture.  Zero is
    // therefore the production default, and this branch leaves the old render
    // bit-identical when no optional component has been configured.
    const bool hasOptionalNoise = optionalNoise_.commonRandomAmplitude != 0.0f
        || optionalNoise_.humAmplitude != 0.0f
        || optionalNoise_.clockSpurAmplitude != 0.0f;
    if (hasOptionalNoise)
    {
        float optionalA = 0.0f;
        float optionalB = 0.0f;

        if (optionalNoise_.commonRandomAmplitude != 0.0f)
        {
            const auto common = correlatedRandomStep(
                commonNoiseState_, orthogonalNoiseState_,
                optionalNoise_.commonRandomCorrelation);
            optionalA += optionalNoise_.commonRandomAmplitude * common.lineA;
            optionalB += optionalNoise_.commonRandomAmplitude * common.lineB;
        }

        if (optionalNoise_.humAmplitude != 0.0f)
        {
            // A common deterministic term is the smallest useful hypothesis;
            // polarity and channel imbalance remain unknown.
            const float hum = optionalNoise_.humAmplitude
                * deterministicToneStep(humPhase_, optionalNoise_.humFrequencyHz,
                                        sampleRate_);
            optionalA += hum;
            optionalB += hum;
        }

        if (optionalNoise_.clockSpurAmplitude != 0.0f && !clocksStopped_)
        {
            // Each candidate spur follows its own modulated BBD clock, on its
            // own accumulator.  The harmonic and post-line insertion level are
            // disabled hypotheses, not claims about a measured unit.
            // `deterministicToneStep` advances the phase it is handed, and the
            // heterodyne bleed above already advances clockSpurPhaseA_/B_ by
            // the same clock: sharing them made each tone run at twice its
            // intended frequency whenever both were enabled together.
            optionalA += optionalNoise_.clockSpurAmplitude
                * deterministicToneStep(
                    optionalSpurPhaseA_, clockA * optionalNoise_.clockSpurHarmonic,
                    sampleRate_);
            optionalB += optionalNoise_.clockSpurAmplitude
                * deterministicToneStep(
                    optionalSpurPhaseB_, clockB * optionalNoise_.clockSpurHarmonic,
                    sampleRate_);
        }

        wetA += optionalA * noiseScale;
        wetB += optionalB * noiseScale;
    }

    // Both ordinary modes carry dry plus one wet line per channel. The I+II
    // product extension uses the owner's chosen narrow colour: equal mid
    // folding preserves the exact mono sum while removing the wet side.
    // This is a product routing choice, not a stock JUNO-106 both-button
    // circuit. The comparison switch retains the former wide routing.
    // Each MN3009 carries its own insertion gain inside Panasonic's +/-4 dB
    // row and nothing on the board trims it; solved only when Unit Character
    // moves. The narrow I+II fold below then averages the two returns exactly
    // as the summed mono output would.
    if (enableLineGainSpread)
    {
        if (calibration != lineGainCalibration_)
        {
            // One draw sets the two parts' relative offset, split evenly so
            // the normalised absolute wet level (OQ-04 policy) stays put.
            const float halfOffsetDb = 0.5f * lineInsertionGainSpreadDb
                                     * lineInsertionGainDraw()
                                     * std::clamp(calibration, 0.0f, 2.0f);
            lineGainA_ = std::pow(10.0f, halfOffsetDb / 20.0f);
            lineGainB_ = std::pow(10.0f, -halfOffsetDb / 20.0f);
            lineGainCalibration_ = calibration;
        }
        wetA *= lineGainA_;
        wetB *= lineGainB_;
    }
    if (mode == ChorusMode::OneTwo && enableNarrowOneTwo)
    {
        const float wetMid = 0.5f * (wetA + wetB);
        wetA = wetMid;
        wetB = wetMid;
    }
    left = dryMixGain * (input + wetA * wetGain_);
    right = dryMixGain * (input + wetB * wetGain_);
}

} // namespace youknow

#if defined(YOUKNOW_WORK_AUDIT)
#undef YOUKNOW_COUNT_DOMAIN_WORK
#endif
