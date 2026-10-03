#include "DSP/YouKnowOutputNetwork.h"
#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace
{
using Network = youknow::OutputNetwork;
using Selector = Network::Selector;
using Configuration = Network::Configuration;
using Complex = std::complex<long double>;
constexpr long double pi = std::numbers::pi_v<long double>;
constexpr long double k = 1.380649e-23L;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Reference
{
    Complex leftInput {}, rightInput {};
    long double noisePsd {};
};

// Independent unreduced component-node reference. Unlike production, retain
// R54, both pot segments, R77/78 and R81/84 separately, each of the three
// ladder resistors and R64/65. Zero-ohm pot endpoints are exact node merges.
// Solve both physical channels; mono joins their two jack nodes, leaving two
// 1nF capacitors and a SINGLE external resistance/capacitance. Every internal resistor injects
// its own uncorrelated Norton current with PSD 4kT/R. No two-port reduction,
// spectral-factor formula or doubled-load shortcut is used by this oracle.
Reference reference(Configuration configuration, double volume,
                    double frequency, double temperature)
{
    constexpr int nodesPerChannel = 7, physicalNodes = 14, ground = 14;
    // A=C17 right, T=pot top, W=wiper, M/L=selector taps, H=IC7 input, J=jack.
    enum { A, T, W, M, L, H, J };
    std::array<int, physicalNodes + 1> parent {};
    for (int i = 0; i <= physicalNodes; ++i) parent[i] = i;
    const auto root = [&](int node) {
        while (parent[node] != node) node = parent[node];
        return node;
    };
    const auto join = [&](int a, int b) { parent[root(a)] = root(b); };
    for (int side = 0; side != 2; ++side)
    {
        const int offset = side * nodesPerChannel;
        if (volume == 0.0) join(offset + W, ground);
        if (volume == 1.0) join(offset + T, offset + W);
    }
    if (configuration.mono) join(J, nodesPerChannel + J);
    std::array<int, physicalNodes + 1> node {}, assigned {};
    assigned.fill(-2);
    assigned[root(ground)] = -1;
    int count = 0;
    for (int i = 0; i <= physicalNodes; ++i)
    {
        const int r = root(i);
        if (assigned[r] == -2) assigned[r] = count++;
        node[i] = assigned[r];
    }

    std::array<std::array<Complex, physicalNodes * 2>, physicalNodes> matrix {};
    struct Resistor { int from, to; long double ohms; };
    std::vector<Resistor> noiseResistors;
    const auto admittance = [&](int from, int to, Complex y) {
        const int p = node[from], q = node[to];
        if (p >= 0) matrix[p][p] += y;
        if (q >= 0) matrix[q][q] += y;
        if (p >= 0 && q >= 0) { matrix[p][q] -= y; matrix[q][p] -= y; }
    };
    const auto resistor = [&](int from, int to, long double ohms, bool internal = true) {
        if (ohms == 0) return; // already merged above
        admittance(from, to, 1.0L / ohms);
        if (internal) noiseResistors.push_back({node[from], node[to], ohms});
    };
    const Complex s { 0.0L, 2.0L * pi * frequency };
    for (int side = 0; side != 2; ++side)
    {
        const int o = side * nodesPerChannel;
        resistor(o + A, o + T, 1500.0L);
        resistor(o + T, o + W, (1.0L - volume) * 10000.0L);
        resistor(o + W, ground, volume * 10000.0L);
        resistor(o + W, o + H, 1000.0L);
        resistor(o + H, ground, 100000.0L);
        resistor(o + W, o + M, 33000.0L);
        resistor(o + M, o + L, 6800.0L);
        resistor(o + L, ground, 1500.0L);
        const int tap = configuration.selector == Selector::High ? W
                      : configuration.selector == Selector::Medium ? M : L;
        resistor(o + tap, o + J, 2200.0L);
        admittance(o + A, ground, s * 10e-6L);
        admittance(o + J, ground, s * 1e-9L);
        if (!configuration.mono || side == 0)
            admittance(o + J, ground, s * static_cast<long double>(
                configuration.externalCapacitanceFarads));
        if (configuration.loadOhms > 0.0 && (!configuration.mono || side == 0))
            resistor(o + J, ground, configuration.loadOhms, false);
    }

    // Long-double complex nodal inverse; use it for both driven sources and
    // each individual resistor-noise source, without repeated elimination.
    for (int row = 0; row != count; ++row) matrix[row][count + row] = 1.0L;
    for (int col = 0; col != count; ++col)
    {
        int pivot = col;
        for (int row = col + 1; row != count; ++row)
            if (std::abs(matrix[row][col]) > std::abs(matrix[pivot][col])) pivot = row;
        require(std::abs(matrix[pivot][col]) > 0.0L, "singular independent output MNA");
        std::swap(matrix[col], matrix[pivot]);
        const Complex diagonal = matrix[col][col];
        for (int j = col; j < 2 * count; ++j) matrix[col][j] /= diagonal;
        for (int row = 0; row != count; ++row)
            if (row != col)
            {
                const Complex factor = matrix[row][col];
                for (int j = col; j < 2 * count; ++j)
                    matrix[row][j] -= factor * matrix[col][j];
            }
    }
    const int output = node[J];
    Reference result;
    result.leftInput = matrix[output][count + node[A]] * s * 10e-6L;
    result.rightInput = matrix[output][count + node[nodesPerChannel + A]] * s * 10e-6L;
    for (const auto& r : noiseResistors)
    {
        const Complex transfer = (r.from >= 0 ? matrix[output][count + r.from] : Complex {})
                               - (r.to >= 0 ? matrix[output][count + r.to] : Complex {});
        result.noisePsd += 4.0L * k * temperature / r.ohms * std::norm(transfer);
    }
    return result;
}

void testExactReduction()
{
    double worstSignal = 0, worstNoise = 0;
    for (const auto selector : {Selector::High, Selector::Medium, Selector::Low})
        for (const double volume : {0.0, 0.01, 0.5, 1.0})
            for (const double load : {0.0, 600.0, 10000.0, 47000.0, 100000.0, 1e6})
                for (const bool mono : {false, true})
                for (const double capacitance : {0.0, 480e-12, 960e-12, 2e-9, 5e-9, 100e-9})
                {
                    const Configuration c {selector, load, mono, capacitance};
                    Network network;
                    require(network.prepare(48000, c) && network.setVolume(volume),
                            "valid output-network configuration rejected");
                    const auto coefficients = network.coefficients();
                    require(coefficients.lowPoleHz > 0 && coefficients.highPoleHz > coefficients.lowPoleHz
                                && coefficients.passbandGain >= 0 && coefficients.passbandGain < 1
                                && coefficients.noiseLowGain > 0
                                && coefficients.noiseDensityPerRootKelvin > 0,
                            "nonphysical passive output coefficients");
                    for (const double frequency : {0.0, .01, .2, 1.0, 2.0, 10.0, 100.0,
                                                   1000.0, 10000.0, 20000.0, 100000.0})
                    {
                        const auto mna = reference(c, volume, frequency, 298.15);
                        const Complex wanted = mna.leftInput + mna.rightInput;
                        const auto got = network.analogResponse(frequency);
                        const Complex actual {got.real(), got.imag()};
                        const double signalError = static_cast<double>(std::abs(actual - wanted)
                            / std::max(1e-16L, std::abs(wanted)));
                        const double noiseError = std::abs(network.analogNoisePsd(frequency, 298.15)
                            / static_cast<double>(mna.noisePsd) - 1.0);
                        worstSignal = std::max(worstSignal, signalError);
                        worstNoise = std::max(worstNoise, noiseError);
                        require(signalError < 2e-9, "analogue signal reduction differs from full component MNA");
                        require(noiseError < 2e-9, "noise spectral factor differs from individual-resistor MNA");
                        if (mono)
                            require(std::abs(mna.leftInput - mna.rightInput) < 1e-15L,
                                    "mono reference did not share equal source branches");
                        else
                            require(std::abs(mna.rightInput) == 0.0L,
                                    "stereo reference leaked the other channel");
                    }
                }
    std::cout << "Full MNA relative error: signal " << worstSignal << ", noise " << worstNoise << '\n';
}

// The realized cascade has two real poles. At sample 2048 the upper-pole
// transient is negligible even at 768kHz; sum the remaining sub-audio tail
// analytically so its frequency response does not require minutes of audio.
// The low-pole ratio follows the documented TPT discretization. Compare the
// complete measured impulse plus tail with independent ANALOGUE MNA below.
std::complex<double> transform(const std::vector<double>& impulse,
                               double frequency, double rate, double lowPole)
{
    const auto z = std::polar(1.0, -2.0 * std::numbers::pi * frequency / rate);
    std::complex<double> sum {}, power {1.0, 0.0};
    for (const double value : impulse) { sum += value * power; power *= z; }
    const double g = std::tan(std::numbers::pi * lowPole / rate);
    const double pole = (1.0 - g) / (1.0 + g);
    return sum + impulse.back() * pole * power / (1.0 - pole * z);
}

void testRealizedMagnitude()
{
    double worstSignalDb = 0, worstNoiseDb = 0, worstPhaseDegrees = 0;
    for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 768000.0})
        for (const auto selector : {Selector::High, Selector::Medium, Selector::Low})
            for (const double volume : {0.0, 0.5, 1.0})
                for (const double load : {0.0, 10000.0})
                    for (const bool mono : {false, true})
                    for (const double capacitance : {0.0, 480e-12, 960e-12, 2e-9, 5e-9})
                    {
                        const Configuration config {selector, load, mono, capacitance};
                        Network network;
                        require(network.prepare(rate, config) && network.setVolume(volume),
                                "valid realized circuit rejected");
                        std::vector<double> signal(2048), noise(2048);
                        for (std::size_t i = 0; i < signal.size(); ++i)
                            signal[i] = network.process(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0)[0];
                        network.reset();
                        require(network.setNoise(298.15, 1.0), "valid noise temperature rejected");
                        for (std::size_t i = 0; i < noise.size(); ++i)
                            noise[i] = network.process(0.0, 0.0, i == 0 ? 1.0 : 0.0, 0.0)[0];
                        const double limit = std::min(20000.0, rate * 0.45);
                        for (const double f : {.01, .2, 1.0, 2.0, 10.0, 100.0, 1000.0,
                                               limit * .25, limit * .5, limit * .8, limit})
                        {
                            const auto mna = reference(config, volume, f, 298.15);
                            // Added capacitance can bring the upper pole into
                            // the audio band. The existing one-pole magnitude
                            // match is approximate between its anchors; retain
                            // its original0.24dB bound at0pF and qualify the
                            // wider0.55dB bound on the expanded capacitance grid.
                            const double toleranceDb = capacitance == 0.0 ? .24 : .55;
                            const auto actual = transform(signal, f, rate, network.coefficients().lowPoleHz);
                            const auto actualNoise = transform(noise, f, rate, network.coefficients().lowPoleHz);
                            const double wanted = static_cast<double>(std::abs(mna.leftInput + mna.rightInput));
                            if (wanted > 1e-14)
                            {
                                const double error = std::abs(20.0 * std::log10(std::abs(actual) / wanted));
                                worstSignalDb = std::max(worstSignalDb, error);
                                require(error < toleranceDb, "realized source magnitude exceeds qualified circuit error");
                                const std::complex<double> expected {
                                    static_cast<double>((mna.leftInput + mna.rightInput).real()),
                                    static_cast<double>((mna.leftInput + mna.rightInput).imag())};
                                worstPhaseDegrees = std::max(worstPhaseDegrees,
                                    std::abs(std::arg(actual / expected)) * 180.0 / std::numbers::pi);
                            }
                            else
                                require(std::abs(actual) < 1e-14, "zero volume passed a source signal");
                            const double measuredPsd = 2.0 / rate * std::norm(actualNoise)
                                                     * (mono ? 2.0 : 1.0);
                            const double noiseError = std::abs(10.0 * std::log10(
                                measuredPsd / static_cast<double>(mna.noisePsd)));
                            worstNoiseDb = std::max(worstNoiseDb, noiseError);
                            if (noiseError >= toleranceDb)
                                std::cerr << "noise magnitude fixture rate=" << rate
                                          << " selector=" << static_cast<int>(selector)
                                          << " volume=" << volume << " load=" << load
                                          << " mono=" << mono << " Cext=" << capacitance
                                          << " f=" << f << " error=" << noiseError << "dB\n";
                            require(noiseError < toleranceDb, "realized noise PSD exceeds qualified circuit error");
                        }
                    }
    std::cout << "Realized worst magnitude error: signal " << worstSignalDb << "dB, noise "
              << worstNoiseDb << "dB; phase remains approximate (worst " << worstPhaseDegrees << "deg)\n";
}

std::array<double, 2> excitation(int frame)
{
    return {std::sin(frame * .13) + .1 * std::cos(frame * .011),
            .7 * std::cos(frame * .07) - .3 * std::sin(frame * .021)};
}

void testHistoriesAndGuards()
{
    Network a, b;
    const Configuration medium {Selector::Medium, 10000, false};
    require(a.prepare(48000, medium) && b.prepare(48000, medium), "prepare failed");
    for (int i = 0; i != 1000; ++i)
    {
        const auto input = excitation(i);
        require(a.process(input[0], input[1]) == b.process(input[0], input[1]),
                "deterministic output mismatch");
    }
    require(a.prepare(48000, medium), "same-rate prepare rejected");
    require(a.setVolume(1) && a.configure(medium) && a.setNoise(0, 0), "unchanged setup rejected");
    require(!a.prepare(0, medium)
                && !a.configure({static_cast<Selector>(99), 0, false})
                && !a.configure({Selector::High, -1, false})
                && !a.configure({Selector::High, .1, false})
                && !a.configure({Selector::High, 0, false, -1e-12})
                && !a.configure({Selector::High, 0, false, 101e-9})
                && !a.configure({Selector::High, 0, false,
                                  std::numeric_limits<double>::quiet_NaN()})
                && !a.configure({Selector::High, 0, false,
                                  std::numeric_limits<double>::infinity()})
                && !a.setVolume(std::numeric_limits<double>::quiet_NaN())
                && !a.setNoise(-1, 1), "malformed configuration accepted");
    for (int i = 1000; i != 1200; ++i)
    {
        const auto input = excitation(i);
        require(a.process(input[0], input[1]) == b.process(input[0], input[1]),
                "same-rate prepare or rejected setup cleared histories");
    }
    a.reset(); b.reset();
    for (int i = 0; i != 100; ++i)
    {
        const auto input = excitation(i);
        require(a.process(input[0], input[1]) == b.process(input[0], input[1]),
                "reset did not reproduce the prepared network");
    }
    require(a.prepare(96000, {Selector::Low, 47000, true, 960e-12}) && a.setVolume(.5)
                && a.setNoise(313.15, 1), "live reconfiguration failed");
    const auto retained = a.process(0, 0);
    require(std::isfinite(retained[0]) && retained[0] == retained[1]
                && std::abs(retained[0]) > 1e-12,
            "rate/selector/mono change lost all histories or mono identity");
    for (int i = 0; i != 2000; ++i)
    {
        const auto input = excitation(i);
        if (i % 11 == 0)
        {
            require(a.configure({static_cast<Selector>((i / 11) % 3),
                                  i % 2 == 0 ? 0.0 : 10000.0, i % 3 == 0,
                                  (i % 6) * 1e-9})
                        && a.setVolume((i % 101) / 100.0), "valid switching setup failed");
        }
        const auto output = a.process(input[0], input[1], .25, -.5);
        require(std::isfinite(output[0]) && std::isfinite(output[1]),
                "switching coefficients produced nonfinite output");
    }
    require(a.configure({Selector::High, 0, false}) && a.setVolume(0)
                && a.setNoise(0, 0), "zero-volume setup failed");
    a.reset();
    for (int i = 0; i != 100; ++i)
        require(a.process(100, -100, 1, -1) == std::array<double, 2> {},
                "zero-volume/no-noise endpoint is not silent");
    for (const auto selector : {Selector::High, Selector::Medium, Selector::Low})
    {
        require(a.configure({selector, 1, false})
                    && a.setVolume(std::numeric_limits<double>::denorm_min()),
                "finite subnormal volume rejected");
        require(std::isfinite(a.coefficients().lowPoleHz)
                    && std::isfinite(a.coefficients().highPoleHz)
                    && std::isfinite(a.coefficients().noiseLowGain),
                "subnormal volume overflowed the wiper conductance");
    }
}

void testMonoIdentityAndThermalScaling()
{
    for (const auto selector : {Selector::High, Selector::Medium, Selector::Low})
    for (const double capacitance : {0.0, 480e-12, 960e-12, 2e-9, 5e-9})
    {
        Network stereo, mono;
        require(stereo.prepare(48000, {selector, 20000, false, capacitance / 2.0})
                    && mono.prepare(48000, {selector, 10000, true, capacitance})
                    && stereo.setVolume(.5) && mono.setVolume(.5)
                    && stereo.setNoise(298.15, 1) && mono.setNoise(298.15, 1),
                "mono identity setup failed");
        for (int i = 0; i != 4096; ++i)
        {
            const auto input = excitation(i);
            const double nl = std::sin(i * 1.234), nr = std::cos(i * .893);
            const auto separate = stereo.process(input[0], input[1], nl, nr);
            const auto shared = mono.process(input[0], input[1], nl, nr);
            require(shared[0] == shared[1] && shared[0] == .5 * (separate[0] + separate[1]),
                    "mono does not equal the common mode of its effective branches");
        }
    }
    Network cold, hot;
    require(cold.prepare(48000, {Selector::Low, 47000, false})
                && hot.prepare(48000, {Selector::Low, 47000, false})
                && cold.setNoise(298.15, .5) && hot.setNoise(313.15, 1),
            "temperature setup failed");
    double error = 0, energy = 0;
    const double ratio = 2.0 * std::sqrt(313.15 / 298.15);
    for (int i = 0; i != 4096; ++i)
    {
        const double noise = std::sin(i * 1.213);
        const double a = cold.process(0, 0, noise)[0], b = hot.process(0, 0, noise)[0];
        error += (b - ratio * a) * (b - ratio * a); energy += b * b;
    }
    require(error / energy < 1e-24, "noise does not scale with amplitude and sqrt(Kelvin)");
}

struct EngineTake { std::vector<float> left, right; };

EngineTake engineNoise(double rate, int quality, bool mono, bool settled,
                       float character, int block, int frames, float capacitancePf = 0)
{
    youknow::YouKnowEngine engine;
    require(engine.configureThermalStart(settled), "engine thermal setup rejected");
    engine.prepare(rate, 256, quality);
    youknow::EngineParameters p;
    p.volume = 0;
    p.calibration = character;
    p.chorus = youknow::ChorusMode::Off;
    p.enableVoiceVcaServiceGain = false;
    p.outputSelector = Selector::Medium;
    p.outputLoadOhms = 10000;
    p.outputCapacitancePf = capacitancePf;
    p.outputMono = mono;
    p.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    p.vcfSolverMode = youknow::VcfSolverMode::Rk4Single;
    engine.setParameters(p);
    EngineTake result {std::vector<float>(frames), std::vector<float>(frames)};
    for (int offset = 0; offset != frames;)
    {
        const int n = std::min(block, frames - offset);
        engine.process(result.left.data() + offset, result.right.data() + offset, n);
        offset += n;
    }
    return result;
}

void testEngineRoutingAndNoise()
{
    // With VR1 grounded, the selected Medium tap sees 33k||8.3k to ground,
    // followed by R64. This isolates the complete output module from card,
    // common-VCA and IC6 noise and signal. Mono physically parallels the two
    // resistive sources and the two jack capacitors, then adds one 10k load.
    // Reconstruct that simple circuit directly instead of using module helpers.
    constexpr double sourceResistance = 2200.0 + 33000.0 * 8300.0 / 41300.0;
    constexpr double load = 10000.0, temperature = 313.15;
    constexpr int settle = 256, frames = 131072;
    const double boundary = youknow::YouKnowEngine::outputBoundaryGain()
                          / youknow::YouKnowEngine::internalVoltsPerUnit;
    for (const bool mono : {false, true})
        for (const double rate : {44100.0, 48000.0, 96000.0})
        for (const float externalPf : {0.0f, 960.0f, 5000.0f})
        {
            const auto take = engineNoise(rate, 1, mono, true, 1, 256, settle + frames, externalPf);
            const double rs = sourceResistance / (mono ? 2.0 : 1.0);
            const double capacitance = 1e-9 * (mono ? 2.0 : 1.0) + externalPf * 1e-12;
            const double cutoff = 1.0 / (2.0 * static_cast<double>(pi)
                * (rs * load / (rs + load)) * capacitance);
            const double gain = load / (rs + load);
            const double expected = boundary * std::sqrt(4.0 * static_cast<double>(k)
                * temperature * rs * gain * gain * cutoff * std::atan(rate / (2.0 * cutoff)));
            double left = 0, right = 0, cross = 0;
            for (int i = settle; i != settle + frames; ++i)
            {
                const double l = take.left[i], r = take.right[i];
                require(std::isfinite(l) && std::isfinite(r), "nonfinite rendered output-network floor");
                left += l * l; right += r * r; cross += l * r;
                if (mono) require(l == r, "engine did not return the shared mono jack on both channels");
            }
            // The magnitude-matched discretization's whole-Nyquist-band bias
            // is below0.12dB here; 3% RMS includes that and sampling variation.
            require(std::abs(std::sqrt(left / frames) / expected - 1.0) < .03
                        && std::abs(std::sqrt(right / frames) / expected - 1.0) < .03,
                    "engine output-network noise misses independent loaded-circuit PSD");
            if (!mono)
                require(std::abs(cross / std::sqrt(left * right)) < .02,
                        "engine stereo output-network noises are correlated");
        }
    for (const bool mono : {false, true})
    for (const float externalPf : {0.0f, 960.0f})
    {
        const auto singles = engineNoise(48000, 1, mono, false, 1, 1, 4096, externalPf);
        const auto blocks = engineNoise(48000, 1, mono, false, 1, 128, 4096, externalPf);
        require(singles.left == blocks.left && singles.right == blocks.right,
                "warming output-network noise depends on host block boundaries");
        const auto q1 = engineNoise(48000, 1, mono, true, 1, 128, 4096, externalPf);
        const auto q4 = engineNoise(48000, 4, mono, true, 1, 128, 4096, externalPf);
        require(q1.left == q4.left && q1.right == q4.right,
                "output-network floor changes with the internal quality rate");
        const auto zero = engineNoise(48000, 1, mono, false, 0, 128, 4096, externalPf);
        for (std::size_t i = 0; i != zero.left.size(); ++i)
            require(zero.left[i] == 0.0f && zero.right[i] == 0.0f,
                    "output-network floor broke Character-zero silence");
    }
    std::cout << "Engine: loaded Medium mono/stereo PSD, PCM units, warming block invariance, quality and zero-noise passed\n";
}

void testCableSizingAndEngineGuards()
{
    Network reference, shortCable, longCable, mono;
    require(reference.prepare(48000, {Selector::High, 47000, false})
                && shortCable.prepare(48000, {Selector::High, 47000, false, 480e-12})
                && longCable.prepare(48000, {Selector::High, 47000, false, 960e-12})
                && mono.prepare(48000, {Selector::High, 47000, true, 960e-12}),
            "nominal cable configuration rejected");
    // Compare the entire coupled network, not R64*C alone. Each stereo jack
    // receives its own cable; mono retains both internal 1nF and one cable.
    const auto magnitude = [](const Network& n) { return std::abs(n.analogResponse(20000)); };
    require(magnitude(reference) > magnitude(shortCable)
                && magnitude(shortCable) > magnitude(longCable)
                && magnitude(mono) > magnitude(longCable),
            "declared cable length or mono sharing did not alter the physical treble response");
    std::cout << "GS-6 nominal 3m/6m added 20kHz loss at High/47k/Volume100%: "
              << -20.0 * std::log10(magnitude(shortCable) / magnitude(reference)) << "/"
              << -20.0 * std::log10(magnitude(longCable) / magnitude(reference)) << "dB\n";

    const auto base = engineNoise(48000, 1, false, true, 1, 128, 4096);
    for (const float malformed : {-1.0f, std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity()})
    {
        const auto take = engineNoise(48000, 1, false, true, 1, 128, 4096, malformed);
        require(take.left == base.left && take.right == base.right,
                "malformed engine capacitance did not preserve the zero-load fallback");
    }
    const auto maximum = engineNoise(48000, 1, false, true, 1, 128, 4096, 100000);
    const auto clamped = engineNoise(48000, 1, false, true, 1, 128, 4096,
                                     std::numeric_limits<float>::max());
    require(maximum.left == clamped.left && maximum.right == clamped.right,
            "engine external capacitance exceeded the numerical circuit domain");
}
} // namespace

int main()
{
    try
    {
        testExactReduction();
        testRealizedMagnitude();
        testHistoriesAndGuards();
        testMonoIdentityAndThermalScaling();
        testEngineRoutingAndNoise();
        testCableSizingAndEngineGuards();
        std::cout << "PASS: complete output source/noise/mono circuit, realized magnitude and retained histories\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
