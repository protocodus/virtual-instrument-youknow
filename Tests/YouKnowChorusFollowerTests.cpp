// Independent component-stamped MNA for the finite chorus support model.
// Roland JUNO-106 Service Notes p.15 supplies the topology/parts; Toshiba's
// 2SA1015 sheet pp.1-2 supplies named typical/sensitivity coordinates.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// https://media.digikey.com/PDF/Data%20Sheets/Toshiba%20PDFs/2SA1015.pdf
// Panasonic MN3009 pp.37-39 supplies the conditional bias/source coordinate.
// https://www.ka-electronics.com/images/pdf/Panasonic_BBD.pdf
// No measured installed transistor rank, Early voltage or fitted EQ is implied.
#include "DSP/YouKnowChorus.h"
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace
{
std::atomic<bool> countAllocations { false };
std::atomic<unsigned> allocations { 0 };
}
void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }

namespace youknow
{
struct YouKnowTestAccess
{
    static float input(Chorus& chorus, float sample)
    { return chorus.advanceInputSupport(sample); }
    static float output(Chorus& chorus, float sample, bool connected)
    {
        // This synthetic fixture supplies samples of a continuous source,
        // whose reference uses the original cubic input reconstruction.
        // It does not supply BBD hold events: those are independently covered
        // by YouKnowChorusOutputRecoveryTests and its event-split node oracle.
        auto transition = connected ? chorus.support_.exactOutputConnected
                                    : chorus.support_.exactOutputMuted;
        transition.heldOutputMap.available = false;
        chorus.lineA_.held = sample;
        return chorus.lineA_.process(0.0f, 0.0f, chorus.sampleRate_,
            transition, 0.0f, false);
    }
    static auto inputState(const Chorus& chorus)
    { return chorus.inputSupport_.exactState; }
    static auto outputState(const Chorus& chorus)
    { return chorus.lineA_.exactOutputState; }
    static auto buckets(const Chorus& chorus) { return chorus.lineA_.cells; }
    static auto phase(const Chorus& chorus) { return chorus.lineA_.clockPhase; }
    static auto rng(const Chorus& chorus) { return chorus.lineA_.noiseState; }
    static auto builds(const Chorus& chorus) { return chorus.supportBuildCount_; }
    static auto branchInputs(const Chorus& chorus)
    { return std::array {chorus.lineA_.previousInput, chorus.lineB_.previousInput}; }
    static auto engineProfile(const YouKnowEngine& engine)
    { return engine.chorus_.getSupportProfile(); }
    static void engineQuality(YouKnowEngine& engine, int factor)
    {
        engine.oversamplingRequested_ = engine.oversamplingApplied_ = factor;
        engine.updateProcessingRate(true);
    }
};
}

namespace
{
using youknow::Chorus;
using youknow::ChorusSupportProfile;
using Probe = youknow::YouKnowTestAccess;
using Complex = std::complex<double>;
constexpr double pi = std::numbers::pi_v<double>;
constexpr double thermalVolts = 1.380649e-23 * 298.15 / 1.602176634e-19;
constexpr double biasOhms = (8.3 / 15.0 * 37000.0) * (1.0 - 8.3 / 15.0);

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
    {
        std::cerr << message << ": " << actual << " vs " << expected << '\n';
        throw std::runtime_error(message);
    }
}

template <std::size_t N, typename T = Complex>
using Matrix = std::array<std::array<T, N>, N>;

template <std::size_t N, typename T>
std::array<T, N> solve(Matrix<N, T> matrix, std::array<T, N> rhs)
{
    for (std::size_t col = 0; col < N; ++col)
    {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < N; ++row)
            if (std::abs(matrix[row][col]) > std::abs(matrix[pivot][col])) pivot = row;
        require(std::abs(matrix[pivot][col]) > 1e-24, "singular independent MNA");
        std::swap(matrix[pivot], matrix[col]);
        std::swap(rhs[pivot], rhs[col]);
        const T diagonal = matrix[col][col];
        for (std::size_t entry = col; entry < N; ++entry) matrix[col][entry] /= diagonal;
        rhs[col] /= diagonal;
        for (std::size_t row = 0; row < N; ++row)
        {
            if (row == col) continue;
            const T multiplier = matrix[row][col];
            for (std::size_t entry = col; entry < N; ++entry)
                matrix[row][entry] -= multiplier * matrix[col][entry];
            rhs[row] -= multiplier * rhs[col];
        }
    }
    return rhs;
}

template <std::size_t N, typename T>
void stamp(Matrix<N, T>& matrix, int a, int b, T admittance)
{
    if (a >= 0) matrix[a][a] += admittance;
    if (b >= 0) matrix[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
        matrix[a][b] -= admittance;
        matrix[b][a] -= admittance;
    }
}

// Positive PNP emitter currents, solved from actual DC resistor currents and
// V_E-V_B=0.61. No production operating-point constants are used here.
std::array<double, 2> operatingCurrents(bool pre, double beta = 200.0)
{
    Matrix<7, double> matrix {};
    std::array<double, 7> rhs {};
    // tap/source, base1, emitter1, base2, emitter2, IE1, IE2.
    if (pre)
    {
        matrix[0][0] = 1.0; // IC2b nominal zero-voltage DC source.
        matrix[1][1] += 1.0 / 44000.0;
        matrix[1][0] -= 1.0 / 44000.0;
    }
    else
    {
        stamp(matrix, 0, -1, 1.0 / 3500.0 + 1.0 / 47000.0);
        rhs[0] = -10.37 / 3500.0;
        stamp(matrix, 0, 1, 1.0 / 44000.0);
    }
    stamp(matrix, 2, 3, 1.0 / 44000.0);
    for (int index = 0; index < 2; ++index)
    {
        const int base = 1 + 2 * index, emitter = base + 1, current = 5 + index;
        const double resistor = pre && index == 0 ? 22000.0 : 10000.0;
        stamp(matrix, emitter, -1, 1.0 / resistor);
        if (pre) rhs[emitter] = 15.0 / resistor;
        matrix[emitter][current] += 1.0;
        matrix[base][current] -= 1.0 / (beta + 1.0);
        matrix[current][emitter] = 1.0;
        matrix[current][base] = -1.0;
        rhs[current] = 0.61;
    }
    const auto nodes = solve(matrix, rhs);
    return { nodes[5] * beta / (beta + 1.0), nodes[6] * beta / (beta + 1.0) };
}

enum class Parasitics { None, CmuOnly, FullTypical, FullMinimumFt };

double typicalFt(double amperes)
{
    // Rounded manual reads from Toshiba's typical graph at VCE=-10V,25C;
    // interpolation is a sensitivity coordinate, never a tolerance bound.
    constexpr std::array<double, 5> current { .1, .3, 1, 3, 10 };
    constexpr std::array<double, 5> mhz { 45, 75, 130, 210, 310 };
    const double ma = amperes * 1000.0;
    for (std::size_t index = 1; index < current.size(); ++index)
        if (ma <= current[index])
        {
            const double fraction = std::log(ma / current[index - 1])
                / std::log(current[index] / current[index - 1]);
            return 1e6 * std::exp(std::log(mhz[index - 1])
                + fraction * std::log(mhz[index] / mhz[index - 1]));
        }
    return 310e6;
}

Complex analog(bool pre, double frequency, bool connected = true,
               bool finite = true, Parasitics parasitics = Parasitics::CmuOnly,
               double rbb = 0.0, int branches = 2, double biasResistance = biasOhms,
               double numericalRate = 0.0)
{
    // Explicit transistor terminal nodes and two distinct input branches.
    // This is frequency-domain KCL, independent of production state reduction.
    Matrix<12> matrix {};
    std::array<Complex, 12> rhs {};
    const Complex s(0.0, 2.0 * pi * frequency);
    const auto resistor = [&](int a, int b, double ohms)
    { stamp(matrix, a, b, Complex(1.0 / ohms)); };
    const auto capacitor = [&](int a, int b, double farads)
    {
        if (pre && numericalRate > 0.0)
        {
            // Independently assign numerical anchors by physical terminal
            // pairs. Both distinct 100 nF/2.2 nF branches stay explicit;
            // Cmu and its parallel base capacitor receive the same scale.
            double anchor = 0.0;
            if ((a == 0 && b == 2) || (a == 1 && b == -1))
                anchor = 1.0 / (2 * pi * 22000 * std::sqrt(820e-12 * 684e-12));
            else if ((a == 3 && b == 5) || (a == 4 && b == -1))
                anchor = 1.0 / (2 * pi * 22000 * std::sqrt(1.8e-9 * 274e-12));
            else if (a == 5 && (b == 6 || b == 8))
                anchor = 1.0 / (2 * pi * (100000 + biasOhms) * 100e-9);
            else if ((a == 7 || a == 9) && b == -1)
                anchor = 1.0 / (2 * pi * 10000 * 2.2e-9);
            if (anchor > 0.0)
            {
                const double angle = pi * std::min(anchor, .45 * numericalRate) / numericalRate;
                farads *= angle / std::tan(angle);
            }
        }
        stamp(matrix, a, b, s * farads);
    };
    std::array<std::array<int, 3>, 2> transistors {};
    int output = 0;
    if (pre)
    {
        resistor(0, -1, 22000); rhs[0] = 1.0 / 22000;
        resistor(0, 1, 22000); resistor(2, 3, 22000); resistor(3, 4, 22000);
        resistor(2, -1, 22000); resistor(5, -1, 10000);
        capacitor(0, 2, 820e-12); capacitor(1, -1, 680e-12);
        capacitor(3, 5, 1.8e-9); capacitor(4, -1, 270e-12);
        for (int branch = 0; branch < branches; ++branch)
        {
            const int coupling = 6 + 2 * branch, input = coupling + 1;
            capacitor(5, coupling, 100e-9);
            resistor(coupling, -1, 100000.0 + biasResistance);
            resistor(coupling, input, 10000);
            capacitor(input, -1, 2.2e-9);
        }
        if (branches == 1) matrix[8][8] = matrix[9][9] = 1.0;
        transistors = {{ {1, 2, 10}, {4, 5, 11} }};
        output = 7;
    }
    else
    {
        resistor(0, -1, 3500); resistor(0, -1, 47000);
        // Preserve the existing BBD loaded-source coordinate. This scale
        // belongs to that prior calibration, not to the finite followers.
        rhs[0] = 1.0 / 3500.0 + 1.0 / 47000.0;
        resistor(0, 1, 22000); resistor(1, 2, 22000);
        resistor(3, 4, 22000); resistor(4, 5, 22000);
        resistor(3, -1, 10000); resistor(6, -1, 10000);
        resistor(7, -1, 22000);
        if (connected) resistor(7, -1, 39000);
        capacitor(0, -1, 2.2e-9); capacitor(1, 3, 820e-12);
        capacitor(2, -1, 680e-12); capacitor(4, 6, 1.8e-9);
        capacitor(5, -1, 270e-12); capacitor(6, 7, 1e-6);
        transistors = {{ {2, 3, 8}, {5, 6, 9} }};
        matrix[10][10] = matrix[11][11] = 1.0;
        output = 7;
    }
    const auto currents = operatingCurrents(pre);
    for (int index = 0; index < 2; ++index)
    {
        const auto [base, emitter, internal] = transistors[index];
        if (!finite)
        {
            // Ideal voltage follower as a controlled voltage source with an
            // explicit current unknown, not a copied cascade formula.
            matrix[emitter][internal] = 1.0;
            matrix[internal][emitter] = 1.0;
            matrix[internal][base] = -1.0;
            continue;
        }
        const int effectiveBase = rbb > 0.0 ? internal : base;
        if (rbb > 0.0) resistor(base, internal, rbb);
        else matrix[internal][internal] = 1.0;
        const double gm = currents[index] / thermalVolts;
        stamp(matrix, effectiveBase, emitter, Complex(gm / 200.0));
        matrix[emitter][emitter] += gm;
        matrix[emitter][effectiveBase] -= gm;
        if (parasitics != Parasitics::None)
            capacitor(effectiveBase, -1, 4e-12);
        if (parasitics == Parasitics::FullTypical || parasitics == Parasitics::FullMinimumFt)
        {
            const double ft = parasitics == Parasitics::FullTypical
                ? typicalFt(currents[index]) : 80e6;
            capacitor(effectiveBase, emitter, std::max(0.0, gm / (2.0 * pi * ft) - 4e-12));
        }
    }
    return solve(matrix, rhs)[output];
}

Complex digital(const Chorus::SupportChain::ExactTransition& transition,
                double frequency, double sampleRate, bool pre = false)
{
    Matrix<6> matrix {};
    std::array<Complex, 6> rhs {};
    const Complex z = std::polar(1.0, -2.0 * pi * frequency / sampleRate);
    for (std::size_t row = 0; row < 6; ++row)
    {
        for (std::size_t column = 0; column < 6; ++column)
            matrix[row][column] = (row == column ? 1.0 : 0.0)
                - z * transition.stateByColumn[column][row];
        Complex delay(1.0);
        for (std::size_t sample = 0; sample < 4; ++sample, delay *= z)
            rhs[row] += delay * transition.driveBySample[sample][row];
    }
    const auto state = solve(matrix, rhs);
    if (pre) return state[5];
    Complex result = transition.outputDirect;
    for (std::size_t index = 0; index < 6; ++index)
        result += state[index] * transition.outputByState[index];
    return result;
}

void oracleChecks()
{
    const auto pre = operatingCurrents(true), post = operatingCurrents(false);
    near(pre[0], .6509655627e-3, 1e-12, "Tr13 DC current/sign");
    near(pre[1], 1.3278274174e-3, 1e-12, "Tr14 DC current/sign");
    near(post[0], .8828569079e-3, 1e-12, "Tr15 DC current/sign");
    near(post[1], .8006503802e-3, 1e-12, "Tr16 DC current/sign");
    // In the ideal limit, the two physical branches cannot influence their
    // zero-impedance driver. Independently factor the two Sallen-Key sections
    // and the last loaded coupling/passive pair to validate the MNA topology.
    for (double frequency : {20., 100., 1000., 8000., 16000., 20000.})
    {
        const Complex s(0, 2 * pi * frequency);
        const double r = 22000;
        Complex factored = 1.0 / (1.0 + 2 * r * 680e-12 * s + r*r*820e-12*680e-12*s*s);
        factored /= 1.0 + 2 * r * 270e-12 * s + r*r*1.8e-9*270e-12*s*s;
        const Complex load = 1.0 / (1.0 / (100000 + biasOhms)
            + 1.0 / (10000.0 + 1.0 / (s * 2.2e-9)));
        factored *= load / (load + 1.0 / (s * 100e-9)) / (1.0 + s * 10000.0 * 2.2e-9);
        near(std::abs(analog(true, frequency, true, false) / factored - 1.0),
             0, 1e-11, "ideal MNA disagrees with independent factored circuit");
    }
    const double branchLoss = 20 * std::log10(std::abs(analog(true, 8000)
        / analog(true, 8000, true, true, Parasitics::CmuOnly, 0, 1)));
    require(branchLoss < -.20 && branchLoss > -.35,
            "second wet branch failed to load finite Tr14");
}

void approximationScreen()
{
    double maximumMagnitude = 0, maximumTypical = 0, maximumPhase = 0, maximumRbb = 0;
    for (int point = 0; point <= 300; ++point)
    {
        const double frequency = 20 * std::pow(1000.0, point / 300.0);
        const Complex reduced = analog(true, frequency) * analog(false, frequency);
        for (auto parasitics : {Parasitics::FullTypical, Parasitics::FullMinimumFt})
        {
            const Complex full = analog(true, frequency, true, true, parasitics, 30)
                * analog(false, frequency, true, true, parasitics, 30);
            const Complex ratio = reduced / full;
            maximumMagnitude = std::max(maximumMagnitude, std::abs(20 * std::log10(std::abs(ratio))));
            if (parasitics == Parasitics::FullTypical)
                maximumTypical = std::max(maximumTypical, std::abs(20 * std::log10(std::abs(ratio))));
            maximumPhase = std::max(maximumPhase, std::abs(std::arg(ratio) * 180 / pi));
        }
        const Complex withRbb = analog(true, frequency, true, true, Parasitics::CmuOnly, 30)
            * analog(false, frequency, true, true, Parasitics::CmuOnly, 30);
        maximumRbb = std::max(maximumRbb, std::abs(20 * std::log10(std::abs(reduced / withRbb))));
    }
    std::cout << "Cpi/rbb approximation screen: typical " << maximumTypical
              << " dB, combined maximum " << maximumMagnitude << " dB, "
              << maximumPhase << " deg; rbb alone " << maximumRbb << " dB\n";
    require(maximumMagnitude < .080, "reduced Cmu model exceeds full-capacitance screen");
    require(maximumTypical < .050, "reduced Cmu model exceeds typical-curve screen");
    require(maximumPhase < .31, "reduced Cmu phase exceeds full-capacitance screen");
    require(maximumRbb < .004, "rbb omission exceeds its screened contribution");
}

// Recover the analog generator from a high-grid exact transition using a
// convergent matrix logarithm. Constant-drive equilibrium gives B=-A*xDC.
// This independently checks the production's continuous circuit without
// mistaking cubic reconstruction error for a topology/coefficient error.
Complex recoveredAnalog(const Chorus::SupportChain::ExactTransition& transition,
                        double frequency, bool pre)
{
    constexpr double rate = 768000.0;
    Matrix<6, double> delta {}, power {}, generator {};
    Matrix<6, double> equilibriumMatrix {};
    std::array<double, 6> constantDrive {};
    for (int row = 0; row < 6; ++row)
    {
        for (int column = 0; column < 6; ++column)
        {
            delta[row][column] = transition.stateByColumn[column][row]
                - (row == column ? 1.0 : 0.0);
            equilibriumMatrix[row][column] = -delta[row][column];
        }
        for (int sample = 0; sample < 4; ++sample)
            constantDrive[row] += transition.driveBySample[sample][row];
    }
    power = delta;
    for (int term = 1; term <= 80; ++term)
    {
        for (int row = 0; row < 6; ++row)
            for (int column = 0; column < 6; ++column)
                generator[row][column] += power[row][column]
                    * ((term & 1) ? rate / term : -rate / term);
        Matrix<6, double> next {};
        for (int row = 0; row < 6; ++row)
            for (int column = 0; column < 6; ++column)
                for (int inner = 0; inner < 6; ++inner)
                    next[row][column] += power[row][inner] * delta[inner][column];
        power = next;
    }
    const auto equilibrium = solve(equilibriumMatrix, constantDrive);
    Matrix<6> resolvent {};
    std::array<Complex, 6> drive {};
    for (int row = 0; row < 6; ++row)
        for (int column = 0; column < 6; ++column)
        {
            resolvent[row][column] = -generator[row][column];
            if (row == column) resolvent[row][column] += Complex(0, 2 * pi * frequency);
            drive[row] -= generator[row][column] * equilibrium[column];
        }
    const auto response = solve(resolvent, drive);
    if (pre) return response[5];
    Complex result = transition.outputDirect;
    for (int index = 0; index < 6; ++index)
        result += transition.outputByState[index] * response[index];
    return result;
}

void topologyAndNumericalTransfer()
{
    const auto highGrid = Chorus::supportChainFor(768000, ChorusSupportProfile::Nominal2SA1015);
    double topologyError = 0;
    for (bool pre : {true, false})
        for (bool connected : {false, true})
            for (double frequency : {.1, 1., 20., 100., 1000., 8000., 16000., 20000.})
            {
                const auto& transition = pre ? highGrid.exactInput
                    : connected ? highGrid.exactOutputConnected : highGrid.exactOutputMuted;
                const double error = std::abs(recoveredAnalog(transition, frequency, pre)
                    / analog(pre, frequency, connected) - 1.0);
                topologyError = std::max(topologyError, error);
            }
    std::cout << "Recovered analog vs component MNA relative error " << topologyError << '\n';
    require(topologyError < 2e-8, "prepared finite support is not the physical nodal network");

    for (double rate : {8000., 16000., 32000., 44100., 48000., 88200., 96000., 176400., 192000., 384000., 768000.})
    {
        const auto support = Chorus::supportChainFor(static_cast<float>(rate),
            ChorusSupportProfile::Nominal2SA1015);
        double preMagnitude = 0, postMagnitude = 0, prePhase = 0, postPhase = 0;
        for (int point = 0; point <= 80; ++point)
        {
            const double frequency = 20 * std::pow(std::min(20000., .4 * rate) / 20., point / 80.);
            const bool bilinear = rate < Chorus::minimumExactInputSupportRate;
            const auto& input = bilinear ? support.bilinearInput : support.exactInput;
            const Complex actualPre = digital(input, frequency, rate, true);
            if (bilinear)
            {
                const double warped = rate / pi * std::tan(pi * frequency / rate);
                const Complex warpedCircuit = analog(true, warped, true, true,
                    Parasitics::CmuOnly, 0, 2, biasOhms, rate);
                near(std::abs(actualPre / warpedCircuit - 1.0), 0, 2e-7,
                     "low-grid coupled bilinear chain misses its declared warped circuit");
            }
            const Complex preRatio = actualPre / analog(true, frequency);
            preMagnitude = std::max(preMagnitude, std::abs(20 * std::log10(std::abs(preRatio))));
            prePhase = std::max(prePhase, std::abs(std::arg(preRatio) * 180 / pi));
            for (bool connected : {false, true})
            {
                const auto& output = connected ? support.exactOutputConnected : support.exactOutputMuted;
                const Complex ratio = digital(output, frequency, rate) / analog(false, frequency, connected);
                postMagnitude = std::max(postMagnitude, std::abs(20 * std::log10(std::abs(ratio))));
                postPhase = std::max(postPhase, std::abs(std::arg(ratio) * 180 / pi));
            }
        }
        std::cout << "Analog error " << rate << " Hz: pre " << preMagnitude << " dB/"
                  << prePhase << " deg; post " << postMagnitude << " dB/" << postPhase << " deg\n";
        // Low grids have a declared bilinear input approximation and limited
        // sampled-drive bandwidth. Exact digital agreement below must not be
        // described as an analog error guarantee there. HQ qualifies 20 kHz.
        if (rate >= Chorus::minimumExactInputSupportRate)
            require(std::max(preMagnitude, postMagnitude) < .05
                    && std::max(prePhase, postPhase) < .22,
                    "HQ finite support exceeds the stated audio-band accuracy");
    }
}

std::unique_ptr<Chorus> device(double rate, ChorusSupportProfile profile)
{
    auto result = std::make_unique<Chorus>();
    require(result->configureSupportProfile(profile), "support profile rejected before prepare");
    result->prepare(rate);
    return result;
}

Complex measured(double rate, double frequency, bool pre, bool connected,
                 ChorusSupportProfile profile = ChorusSupportProfile::Nominal2SA1015)
{
    auto chorus = device(rate, profile);
    const int settle = static_cast<int>(.5 * rate);
    const int count = std::max(2048, static_cast<int>(.03 * rate));
    double cc = 0, ss = 0, cs = 0, yc = 0, ys = 0;
    for (int sample = 0; sample < settle + count; ++sample)
    {
        const double angle = 2 * pi * frequency * sample / rate;
        const double cosine = std::cos(angle), sine = std::sin(angle);
        const float input = static_cast<float>(.125 * cosine);
        const double output = pre ? Probe::input(*chorus, input)
                                  : Probe::output(*chorus, input, connected);
        if (sample < settle) continue;
        cc += cosine * cosine; ss += sine * sine; cs += cosine * sine;
        yc += output * cosine; ys += output * sine;
    }
    const double determinant = cc * ss - cs * cs;
    return Complex((yc * ss - ys * cs), -(ys * cc - yc * cs)) / (determinant * .125);
}

void renderedTransfer()
{
    double maximum = 0;
    for (double rate : {8000., 16000., 32000., 44100., 48000., 88200., 96000., 176400., 192000., 384000., 768000.})
    {
        const auto support = Chorus::supportChainFor(static_cast<float>(rate), ChorusSupportProfile::Nominal2SA1015);
        for (double frequency : {53., 997., std::min(8000., .21 * rate)})
            for (int path = 0; path < 3; ++path)
            {
                const bool pre = path == 0, connected = path == 2;
                const auto& transition = pre
                    ? (rate < Chorus::minimumExactInputSupportRate ? support.bilinearInput : support.exactInput)
                    : (connected ? support.exactOutputConnected : support.exactOutputMuted);
                const double error = std::abs(measured(rate, frequency, pre, connected)
                    / digital(transition, frequency, rate, pre) - 1.0);
                maximum = std::max(maximum, error);
            }
    }
    std::cout << "Smooth-source input/Line render vs digital transfer relative error " << maximum << '\n';
    require(maximum < 1e-6, "real render path does not use the prepared finite support");
}

void lowGridComparison()
{
    for (double rate : {44100., 48000., 96000.})
        for (double frequency : {100., 1000., 4000., 8000., 12000., 16000.})
        {
            const Complex finite = measured(rate, frequency, true, true);
            const Complex ideal = measured(rate, frequency, true, true, ChorusSupportProfile::IdealFollowers);
            const Complex physicalFinite = analog(true, frequency);
            const Complex physicalIdeal = analog(true, frequency, true, false,
                Parasitics::None, 0, 2, 0); // Raw profile's historical ideal bias source.
            const auto db = [](Complex value) { return 20 * std::log10(std::abs(value)); };
            std::cout << "Low-grid " << rate << " Hz, " << frequency << " Hz: raw error "
                      << db(ideal / physicalIdeal) << " dB, finite error "
                      << db(finite / physicalFinite) << " dB; actual finite/raw "
                      << db(finite / ideal) << " dB, analog finite/ideal "
                      << db(physicalFinite / physicalIdeal) << " dB\n";
        }
}

void stateAndCompatibility()
{
    auto raw = std::make_unique<Chorus>();
    auto explicitRaw = device(48000, ChorusSupportProfile::IdealFollowers);
    raw->prepare(48000);
    auto finite = device(48000, ChorusSupportProfile::Nominal2SA1015);
    double difference = 0;
    for (int sample = 0; sample < 12000; ++sample)
    {
        const float input = static_cast<float>(.1 * std::sin(sample * .31));
        float left = 0, right = 0, referenceLeft = 0, referenceRight = 0, finiteLeft = 0, finiteRight = 0;
        raw->process(input, youknow::ChorusMode::One, .03f, left, right);
        explicitRaw->process(input, youknow::ChorusMode::One, .03f, referenceLeft, referenceRight);
        finite->process(input, youknow::ChorusMode::One, .03f, finiteLeft, finiteRight);
        require(left == referenceLeft && right == referenceRight, "raw default changed relative to explicit ideal profile");
        difference += std::abs(finiteLeft - left) + std::abs(finiteRight - right);
        require(Probe::phase(*finite) == Probe::phase(*raw) && Probe::rng(*finite) == Probe::rng(*raw),
                "support profile changed BBD clock/RNG evolution");
        const auto inputs = Probe::branchInputs(*finite);
        require(inputs[0] == inputs[1], "both BBD branches did not receive shared finite support");
    }
    require(difference > .1, "full Chorus::process bypassed the finite support profile");
    require(!finite->configureSupportProfile(ChorusSupportProfile::IdealFollowers), "live support-profile change was accepted");

    finite->prepareSupportRates(48000);
    const auto builds = Probe::builds(*finite);
    const auto buckets = Probe::buckets(*finite);
    const auto phase = Probe::phase(*finite);
    const auto rng = Probe::rng(*finite);
    countAllocations = true;
    for (double rate : {96000., 192000., 48000.}) finite->prepare(rate, true);
    countAllocations = false;
    require(allocations == 0, "cached HQ support change allocated");
    require(Probe::builds(*finite) == builds, "cached HQ change rebuilt transitions");
    require(Probe::buckets(*finite) == buckets && Probe::phase(*finite) == phase && Probe::rng(*finite) == rng,
            "HQ change discarded physical BBD state");
    require(Probe::inputState(*finite) == std::array<double, 6>{}
            && Probe::outputState(*finite) == std::array<double, 6>{},
            "HQ change broke established zero-gain support-reset policy");
    require(finite->getSupportProfile() == ChorusSupportProfile::Nominal2SA1015, "HQ change lost profile");
    auto fresh = device(48000, ChorusSupportProfile::Nominal2SA1015);
    finite->reset();
    for (int sample = 0; sample < 1024; ++sample)
    {
        float left, right, refLeft, refRight;
        const float input = static_cast<float>(.2 * std::cos(.07 * sample));
        finite->process(input, youknow::ChorusMode::Two, .1f, left, right);
        fresh->process(input, youknow::ChorusMode::Two, .1f, refLeft, refRight);
        require(left == refLeft && right == refRight, "reset differs from fresh finite support state");
    }
}

void dcAndRuntimeBounds()
{
    for (double rate : {8000., 16000., 32000., 44100., 192000., 768000.})
        for (int path = 0; path < 3; ++path)
        {
            auto chorus = device(rate, ChorusSupportProfile::Nominal2SA1015);
            double maximumTail = 0;
            countAllocations = true;
            for (int sample = 0; sample < static_cast<int>(rate); ++sample)
            {
                const float output = path == 0 ? Probe::input(*chorus, .25f)
                    : Probe::output(*chorus, .25f, path == 2);
                if (sample > static_cast<int>(rate * .9))
                    maximumTail = std::max(maximumTail, std::abs(static_cast<double>(output)));
            }
            countAllocations = false;
            near(maximumTail, 0, 1e-8, "finite support leaked held DC after settling");
            require(allocations == 0, "finite support audio path allocated");
            for (float sample : {std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity()})
            {
                const float output = path == 0 ? Probe::input(*chorus, sample)
                    : Probe::output(*chorus, sample, path == 2);
                require(std::isfinite(output), "nonfinite sample poisoned finite support");
            }
        }
}

void lowGridInterruptedTransients()
{
    // Steps interrupted before the slow coupling settles, plus a Nyquist
    // burst, exercise all numerical states at the low-grid clamp endpoints.
    // This is a finite-state headroom/stability screen, not a hardware peak
    // specification or an analog-accuracy claim near Nyquist.
    for (double rate : {8000., 16000., 32000.})
    {
        auto chorus = device(rate, ChorusSupportProfile::Nominal2SA1015);
        double maximum = 0.0, tail = 0.0;
        countAllocations = true;
        for (int sample = 0; sample < static_cast<int>(rate); ++sample)
        {
            float input = sample < rate * .2 ? .5f
                : sample < rate * .21 ? -.75f : sample < rate * .4 ? .125f : 0.0f;
            if (sample > rate * .55 && sample < rate * .6)
                input = (sample & 1) ? .5f : -.5f;
            const double output = Probe::input(*chorus, input);
            require(std::isfinite(output), "low-grid transient became nonfinite");
            maximum = std::max(maximum, std::abs(output));
            if (sample > rate * .9) tail = std::max(tail, std::abs(output));
        }
        countAllocations = false;
        require(allocations == 0, "low-grid transient allocated");
        require(maximum < 2.0, "low-grid bounded input produced excessive state excursion");
        require(tail < 1e-6, "low-grid interrupted transient failed to decay");
        std::cout << "Low-grid " << rate << " Hz interrupted-step maximum " << maximum << '\n';
    }
}

void engineProfileContract()
{
    auto raw = std::make_unique<youknow::YouKnowEngine>();
    require(Probe::engineProfile(*raw) == ChorusSupportProfile::IdealFollowers,
            "raw Engine does not preserve ideal support reference");
    auto product = std::make_unique<youknow::YouKnowEngine>();
    youknow::ProductFidelityProfile::configureBeforePrepare(*product);
    require(Probe::engineProfile(*product) == ChorusSupportProfile::Nominal2SA1015Nonlinear,
            "product profile did not select nonlinear finite chorus support");
    product->prepare(48000, 128, 4);
    require(!product->configureChorusSupport(ChorusSupportProfile::IdealFollowers),
            "Engine allowed support reconfiguration after prepare");
    product->reset();
    require(Probe::engineProfile(*product) == ChorusSupportProfile::Nominal2SA1015Nonlinear,
            "Engine reset dropped finite chorus support");
    for (int factor : {1, 2, 4})
    {
        Probe::engineQuality(*product, factor);
        require(Probe::engineProfile(*product) == ChorusSupportProfile::Nominal2SA1015Nonlinear,
                "Engine HQ rebuild dropped finite chorus support");
    }
    product->prepare(96000, 128, 4);
    require(Probe::engineProfile(*product) == ChorusSupportProfile::Nominal2SA1015Nonlinear,
            "Engine host reprepare dropped finite chorus support");

    // A cache built before profile configuration must not silently supply
    // ideal coefficients when prepare() later selects the finite profile.
    auto cached = std::make_unique<Chorus>();
    cached->prepareSupportRates(48000);
    require(cached->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015),
            "pre-prepare cache incorrectly locked profile configuration");
    cached->prepare(48000);
    auto fresh = device(48000, ChorusSupportProfile::Nominal2SA1015);
    for (int sample = 0; sample < 2048; ++sample)
    {
        const float input = static_cast<float>(.1 * std::sin(sample * .7));
        require(Probe::input(*cached, input) == Probe::input(*fresh, input),
                "profile selection reused stale ideal coefficient cache");
    }
}
}

int main()
{
    try
    {
        oracleChecks();
        approximationScreen();
        topologyAndNumericalTransfer();
        renderedTransfer();
        lowGridComparison();
        stateAndCompatibility();
        dcAndRuntimeBounds();
        lowGridInterruptedTransients();
        engineProfileContract();
        std::cout << "Chorus follower checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
