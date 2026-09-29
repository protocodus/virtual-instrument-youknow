// Independent physical-node qualification of the nominal forward-active
// chorus followers. This oracle stamps Roland's actual resistor/capacitor
// nodes and Ic=Ic0*exp(delta(Ve-Vb)/VT), Ib=Ic/beta in volts/amperes.
// It never imports runtime state matrices, numerical current-port weights,
// or solver results as reference values. Harmonic perturbation and a refined
// continuous-time trapezoidal DAE cross-check each other before runtime tests.
// Topology: Roland JUNO-106 Service Notes p.15:
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// Nominal 2SA1015 beta/DC coordinate: Toshiba pp.1-2:
// https://media.digikey.com/PDF/Data%20Sheets/Toshiba%20PDFs/2SA1015.pdf
// Forward-active exponential law and current-dependent follower VBE:
// https://wiki.analog.com/university/courses/electronics/text/chapter-8
// https://www.analog.com/en/resources/analog-dialogue/studentzone/studentzone-april-2021.html
// Constant beta, Cmu4pF, ro=infinity and25C are explicit approximations;
// no installed-device, reverse-transport, rail-clipping or noise claim.
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
std::atomic<unsigned> allocationCount { 0 };
}
void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed)) ++allocationCount;
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
        // The harmonic/DAE fixtures inject a continuous synthetic source and
        // qualify its cubic reconstruction. They do not supply BBD hold
        // events; YouKnowChorusOutputRecoveryTests qualifies that production
        // path separately against an event-split physical node reference.
        auto transition = connected ? chorus.support_.exactOutputConnected
                                    : chorus.support_.exactOutputMuted;
        transition.heldOutputMap.available = false;
        chorus.lineA_.held = sample;
        return chorus.lineA_.process(0, 0, chorus.sampleRate_,
            transition, 0, false);
    }
    static auto inputState(const Chorus& chorus) { return chorus.inputSupport_.exactState; }
    static auto outputState(const Chorus& chorus) { return chorus.lineA_.exactOutputState; }
    static auto phase(const Chorus& chorus) { return chorus.lineA_.clockPhase; }
    static auto rng(const Chorus& chorus) { return chorus.lineA_.noiseState; }
    static auto buckets(const Chorus& chorus) { return chorus.lineA_.cells; }
    static auto nonlinearState(const Chorus& chorus, bool pre)
    { return pre ? chorus.inputSupport_.nonlinear : chorus.lineA_.nonlinearOutput; }
    static auto builds(const Chorus& chorus) { return chorus.supportBuildCount_; }
    static auto engineProfile(const YouKnowEngine& engine) { return engine.chorus_.getSupportProfile(); }
    static void quality(YouKnowEngine& engine, int factor)
    {
        engine.oversamplingRequested_ = engine.oversamplingApplied_ = factor;
        engine.updateProcessingRate(true);
    }
};
}

namespace
{
constexpr double pi = std::numbers::pi_v<double>;
constexpr double vt = 1.380649e-23 * 298.15 / 1.602176634e-19;
constexpr double voltsPerUnit = 2.6;
constexpr double beta = 200.0;
using youknow::Chorus;
using youknow::ChorusSupportProfile;
using Probe = youknow::YouKnowTestAccess;
constexpr auto nonlinearProfile = ChorusSupportProfile::Nominal2SA1015Nonlinear;
constexpr std::size_t nodes = 10;
using Complex = std::complex<double>;
template <std::size_t N, class T = double>
using Vector = std::array<T, N>;
template <std::size_t N, class T = double>
using Matrix = std::array<Vector<N, T>, N>;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <std::size_t N, class T>
Vector<N, T> solve(Matrix<N, T> a, Vector<N, T> b)
{
    for (std::size_t col = 0; col < N; ++col)
    {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < N; ++row)
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
        require(std::abs(a[pivot][col]) > 1e-25, "singular independent nodal system");
        std::swap(a[pivot], a[col]); std::swap(b[pivot], b[col]);
        const T divisor = a[col][col];
        for (std::size_t j = col; j < N; ++j) a[col][j] /= divisor;
        b[col] /= divisor;
        for (std::size_t row = 0; row < N; ++row)
        {
            if (row == col) continue;
            const T multiple = a[row][col];
            for (std::size_t j = col; j < N; ++j) a[row][j] -= multiple * a[col][j];
            b[row] -= multiple * b[col];
        }
    }
    return b;
}

template <std::size_t N, class T>
void stamp(Matrix<N, T>& a, int p, int q, T value)
{
    if (p >= 0) a[p][p] += value;
    if (q >= 0) a[q][q] += value;
    if (p >= 0 && q >= 0) { a[p][q] -= value; a[q][p] -= value; }
}

std::array<double, 2> dcCurrents(bool pre)
{
    // Source/tap, b1,e1,b2,e2,Ie1,Ie2. Solve fixed nominal VBE plus
    // actual resistor KCL; do not copy implementation bias constants.
    Matrix<7> g {}; Vector<7> b {};
    if (pre)
    {
        g[0][0] = 1;
        g[1][1] = 1.0 / 44000; g[1][0] = -1.0 / 44000;
    }
    else
    {
        stamp(g, 0, -1, 1.0 / 3500 + 1.0 / 47000);
        stamp(g, 0, 1, 1.0 / 44000); b[0] = -10.37 / 3500;
    }
    stamp(g, 2, 3, 1.0 / 44000);
    for (int i = 0; i < 2; ++i)
    {
        const int base = 1 + 2 * i, emitter = base + 1, current = 5 + i;
        const double resistance = pre && i == 0 ? 22000.0 : 10000.0;
        stamp(g, emitter, -1, 1.0 / resistance);
        if (pre) b[emitter] = 15.0 / resistance;
        g[emitter][current] = 1; g[base][current] = -1.0 / (beta + 1);
        g[current][emitter] = 1; g[current][base] = -1; b[current] = .61;
    }
    const auto x = solve(g, b);
    return {x[5] * beta / (beta + 1), x[6] * beta / (beta + 1)};
}

struct PhysicalCircuit
{
    Matrix<nodes> g {}, c {};
    Vector<nodes> drive {};
    std::array<std::array<int, 2>, 2> transistors {};
    std::array<double, 2> ic {};
    bool pre {};
    std::vector<std::array<int, 2>> capacitorTerminals;

    explicit PhysicalCircuit(bool beforeBbd, bool connected = true) : pre(beforeBbd)
    {
        const auto r = [&](int p, int q, double ohms) { stamp(g, p, q, 1.0 / ohms); };
        const auto cap = [&](int p, int q, double farads)
        { stamp(c, p, q, farads); capacitorTerminals.push_back({p, q}); };
        if (pre)
        {
            r(0, -1, 22000); drive[0] = 1.0 / 22000;
            r(0, 1, 22000); r(2, 3, 22000); r(3, 4, 22000);
            r(2, -1, 22000); r(5, -1, 10000);
            cap(0, 2, 820e-12); cap(1, -1, 684e-12);
            cap(3, 5, 1.8e-9); cap(4, -1, 274e-12);
            const double bias = (8.3 / 15 * 37000) * (1 - 8.3 / 15);
            for (int branch = 0; branch < 2; ++branch)
            {
                const int coupling = 6 + 2 * branch, input = coupling + 1;
                cap(5, coupling, 100e-9); r(coupling, -1, 100000 + bias);
                r(coupling, input, 10000); cap(input, -1, 2.2e-9);
            }
            transistors = {{{1, 2}, {4, 5}}};
        }
        else
        {
            r(0, -1, 3500); r(0, -1, 47000);
            // Same established loaded-source input coordinate as the product;
            // finite-follower attenuation is never normalized away.
            drive[0] = 1.0 / 3500 + 1.0 / 47000;
            r(0, 1, 22000); r(1, 2, 22000); r(3, 4, 22000); r(4, 5, 22000);
            r(3, -1, 10000); r(6, -1, 10000); r(7, -1, 22000);
            if (connected) r(7, -1, 39000);
            cap(0, -1, 2.2e-9); cap(1, 3, 820e-12); cap(2, -1, 684e-12);
            cap(4, 6, 1.8e-9); cap(5, -1, 274e-12); cap(6, 7, 1e-6);
            transistors = {{{2, 3}, {5, 6}}};
            g[8][8] = g[9][9] = 1;
        }
        ic = dcCurrents(pre);
        for (int i = 0; i < 2; ++i)
        {
            const auto [base, emitter] = transistors[i];
            const double gm = ic[i] / vt;
            stamp(g, base, emitter, gm / beta);
            g[emitter][emitter] += gm; g[emitter][base] -= gm;
        }
    }

    Vector<nodes, Complex> frequencySolve(double frequency, Vector<nodes, Complex> rhs) const
    {
        Matrix<nodes, Complex> y {};
        for (std::size_t row = 0; row < nodes; ++row)
            for (std::size_t col = 0; col < nodes; ++col)
                y[row][col] = Complex(g[row][col], 2 * pi * frequency * c[row][col]);
        return solve(y, rhs);
    }

    template <class T>
    Vector<nodes, T> currentDrive(std::array<T, 2> current) const
    {
        Vector<nodes, T> result {};
        for (int i = 0; i < 2; ++i)
        {
            const auto [base, emitter] = transistors[i];
            result[base] += current[i] / beta;
            result[emitter] -= current[i] * (1 + 1 / beta);
        }
        return result;
    }

    std::array<Vector<nodes, Complex>, 3> perturbation(double frequency, double rms) const
    {
        Vector<nodes, Complex> input {};
        for (std::size_t i = 0; i < nodes; ++i) input[i] = drive[i] * rms * std::sqrt(2.0);
        const auto v1 = frequencySolve(frequency, input);
        std::array<Complex, 2> d1 {}, i0 {}, i2 {}, i3 {};
        for (int i = 0; i < 2; ++i)
        {
            const auto [base, emitter] = transistors[i];
            d1[i] = v1[emitter] - v1[base];
            i0[i] = ic[i] * std::norm(d1[i]) / (4 * vt * vt);
            i2[i] = ic[i] * d1[i] * d1[i] / (4 * vt * vt);
        }
        const auto v0 = frequencySolve(0, currentDrive(i0));
        const auto v2 = frequencySolve(2 * frequency, currentDrive(i2));
        for (int i = 0; i < 2; ++i)
        {
            const auto [base, emitter] = transistors[i];
            const Complex d2 = v2[emitter] - v2[base];
            i3[i] = ic[i] * (d1[i] * d2 / (2 * vt * vt)
                + d1[i] * d1[i] * d1[i] / (24 * vt * vt * vt));
        }
        return {v1, v2, frequencySolve(3 * frequency, currentDrive(i3))};
    }
};

// Refined physical-node trapezoidal DAE. Full-node Newton and an independently
// chosen substep are intentionally different from the runtime port reduction
// and exact-linear/current-interpolation integrator.
struct ContinuousOracle
{
    PhysicalCircuit circuit;
    Vector<nodes> voltage {};
    double previousInput = 0;
    double sampleRate;
    Matrix<nodes> stepMatrix {};
    unsigned maximumIterations = 0;

    ContinuousOracle(bool pre, bool connected, double rate)
        : circuit(pre, connected), sampleRate(rate)
    {
        for (std::size_t row = 0; row < nodes; ++row)
            for (std::size_t col = 0; col < nodes; ++col)
                stepMatrix[row][col] = 2 * rate * circuit.c[row][col] + circuit.g[row][col];
    }

    void setOutputLoad(bool connected)
    {
        require(!circuit.pre, "output load requested on input oracle");
        const auto previous = voltage;
        circuit = PhysicalCircuit(false, connected);
        // Project algebraic nodes onto the new load while imposing every
        // old physical capacitor voltage as a constraint. This independent
        //16-unknown MNA does not carry an old-topology transistor current.
        Matrix<16> constraint {};
        Vector<16> target {}, state {};
        for (std::size_t row = 0; row < nodes; ++row)
        {
            target[row] = circuit.drive[row] * previousInput;
            state[row] = voltage[row];
            for (std::size_t col = 0; col < nodes; ++col)
                constraint[row][col] = circuit.g[row][col];
        }
        for (std::size_t i = 0; i < 6; ++i)
        {
            const auto [p, q] = circuit.capacitorTerminals[i];
            constraint[p][nodes + i] = constraint[nodes + i][p] = 1;
            target[nodes + i] = previous[p];
            if (q >= 0)
            {
                constraint[q][nodes + i] = constraint[nodes + i][q] = -1;
                target[nodes + i] -= previous[q];
            }
        }
        bool converged = false;
        for (int iteration = 0; iteration < 20; ++iteration)
        {
            Vector<nodes> node {};
            std::copy_n(state.begin(), nodes, node.begin());
            const auto nonlinear = residualCurrent(node);
            auto residual = target;
            auto jacobian = constraint;
            for (std::size_t row = 0; row < 16; ++row)
            {
                if (row < nodes) residual[row] += nonlinear[row];
                for (std::size_t col = 0; col < 16; ++col)
                    residual[row] -= constraint[row][col] * state[col];
            }
            for (int i = 0; i < 2; ++i)
            {
                const auto [base, emitter] = circuit.transistors[i];
                const double slope = circuit.ic[i] / vt
                    * std::expm1((node[emitter] - node[base]) / vt);
                stamp(jacobian, base, emitter, slope / beta);
                jacobian[emitter][emitter] += slope;
                jacobian[emitter][base] -= slope;
            }
            const auto correction = solve(jacobian, residual);
            double largest = 0;
            for (std::size_t i = 0; i < 16; ++i)
            {
                state[i] += correction[i];
                if (i < nodes) largest = std::max(largest, std::abs(correction[i]));
            }
            if (largest < 2e-12) { converged = true; break; }
        }
        require(converged, "physical load-switch projection did not converge");
        std::copy_n(state.begin(), nodes, voltage.begin());
        for (const auto& terminals : circuit.capacitorTerminals)
        {
            const auto [p, q] = terminals;
            const double before = previous[p] - (q >= 0 ? previous[q] : 0.0);
            const double after = voltage[p] - (q >= 0 ? voltage[q] : 0.0);
            require(std::abs(before - after) < 2e-12, "physical oracle load switch discarded capacitor charge");
        }
        for (std::size_t row = 0; row < nodes; ++row)
            for (std::size_t col = 0; col < nodes; ++col)
                stepMatrix[row][col] = 2 * sampleRate * circuit.c[row][col] + circuit.g[row][col];
    }

    Vector<nodes> residualCurrent(const Vector<nodes>& state) const
    {
        std::array<double, 2> residual {};
        for (int i = 0; i < 2; ++i)
        {
            const auto [base, emitter] = circuit.transistors[i];
            const double d = (state[emitter] - state[base]) / vt;
            residual[i] = circuit.ic[i] * (std::expm1(d) - d);
        }
        return circuit.currentDrive(residual);
    }

    double step(double input)
    {
        auto rhs = residualCurrent(voltage);
        for (std::size_t row = 0; row < nodes; ++row)
        {
            rhs[row] += circuit.drive[row] * (input + previousInput);
            for (std::size_t col = 0; col < nodes; ++col)
                rhs[row] += (2 * sampleRate * circuit.c[row][col] - circuit.g[row][col]) * voltage[col];
        }
        auto next = voltage;
        bool converged = false;
        for (unsigned iteration = 0; iteration < 20; ++iteration)
        {
            auto residual = residualCurrent(next);
            auto jacobian = stepMatrix;
            for (std::size_t row = 0; row < nodes; ++row)
            {
                residual[row] += rhs[row];
                for (std::size_t col = 0; col < nodes; ++col)
                    residual[row] -= stepMatrix[row][col] * next[col];
            }
            for (int i = 0; i < 2; ++i)
            {
                const auto [base, emitter] = circuit.transistors[i];
                const double slope = circuit.ic[i] / vt
                    * std::expm1((next[emitter] - next[base]) / vt);
                stamp(jacobian, base, emitter, slope / beta);
                jacobian[emitter][emitter] += slope;
                jacobian[emitter][base] -= slope;
            }
            const auto correction = solve(jacobian, residual);
            double largest = 0;
            for (std::size_t i = 0; i < nodes; ++i)
            {
                next[i] += correction[i];
                largest = std::max(largest, std::abs(correction[i]));
            }
            maximumIterations = std::max(maximumIterations, iteration + 1);
            if (largest < 2e-12) { converged = true; break; }
        }
        require(converged, "independent continuous oracle failed Newton convergence");
        voltage = next; previousInput = input;
        return next[7];
    }
};

std::array<Complex, 3> oracleTone(bool pre, bool connected, double rate,
                                double frequency, double rms)
{
    ContinuousOracle oracle(pre, connected, rate);
    // An integer number of periods gives orthogonal complex coefficients.
    const int count = static_cast<int>(std::round(rate / frequency)) * 12;
    const int settle = static_cast<int>(rate * .4);
    std::array<Complex, 3> result {};
    for (int n = 0; n < settle + count; ++n)
    {
        const double phase = 2 * pi * frequency * n / rate;
        const double sample = oracle.step(std::sqrt(2.0) * rms * std::cos(phase));
        if (n < settle) continue;
        for (int h = 1; h <= 3; ++h)
            result[h - 1] += sample * std::polar(2.0 / count, -h * phase);
    }
    return result;
}

std::unique_ptr<Chorus> device(double rate, ChorusSupportProfile profile = nonlinearProfile)
{
    auto chorus = std::make_unique<Chorus>();
    require(chorus->configureSupportProfile(profile), "profile configuration refused before prepare");
    chorus->prepare(rate);
    return chorus;
}

double runtimeStep(Chorus& chorus, bool pre, double physicalInput, bool connected = true)
{
    const float modelInput = static_cast<float>(physicalInput / voltsPerUnit);
    return voltsPerUnit * (pre ? Probe::input(chorus, modelInput)
        : Probe::output(chorus, modelInput, connected));
}

std::array<Complex, 3> runtimeTone(bool pre, bool connected, double rate,
                                 double frequency, double rms,
                                 ChorusSupportProfile profile = nonlinearProfile)
{
    auto chorus = device(rate, profile);
    // All chosen frequency/rate pairs have integer cycles in .1 second.
    const int count = static_cast<int>(std::round(rate * .1));
    const int settle = static_cast<int>(std::round(rate * .4));
    std::array<Complex, 3> result {};
    for (int n = 0; n < settle + count; ++n)
    {
        const double phase = 2 * pi * frequency * n / rate;
        const double sample = runtimeStep(*chorus, pre, std::sqrt(2.0) * rms * std::cos(phase), connected);
        require(std::isfinite(sample), "runtime tone became nonfinite");
        if (n < settle) continue;
        for (int h = 1; h <= 3; ++h)
            result[h - 1] += sample * std::polar(2.0 / count, -h * phase);
    }
    require(Probe::nonlinearState(*chorus, pre).fallbackCount == 0,
            "ordinary tone needed nonlinear numerical fallback");
    return result;
}

void infinitesimalAndDc()
{
    for (double rate : {8000., 16000., 32000., 44100., 48000., 96000., 192000., 768000.})
        for (bool pre : {true, false})
            for (bool connected : {true, false})
            {
                auto linear = device(rate, ChorusSupportProfile::Nominal2SA1015);
                auto nonlinear = device(rate);
                double largest = 0, scale = 0;
                for (int n = 0; n < 4096; ++n)
                {
                    const double input = 1e-6 * std::sin(n * .37);
                    const double a = runtimeStep(*linear, pre, input, connected);
                    const double b = runtimeStep(*nonlinear, pre, input, connected);
                    largest = std::max(largest, std::abs(a - b));
                    scale = std::max(scale, std::abs(a));
                }
                require(largest / scale < 3e-6, "nonlinear tangent changed the declared linear numerical mapping");
                nonlinear->reset();
                for (int n = 0; n < 4096; ++n)
                    require(runtimeStep(*nonlinear, pre, 0, connected) == 0,
                            "zero signal generated nonlinear DC or a startup offset");
                double tail = 0;
                allocationCount = 0; countAllocations = true;
                for (int n = 0; n < static_cast<int>(rate); ++n)
                {
                    const double result = runtimeStep(*nonlinear, pre, .5, connected);
                    require(std::isfinite(result), "held input produced nonfinite state");
                    if (n > .9 * rate) tail = std::max(tail, std::abs(result));
                }
                countAllocations = false;
                require(allocationCount == 0, "nonlinear support audio path allocated");
                require(tail < 1e-8, "nonlinear support leaked held DC after settling");
                require(Probe::nonlinearState(*nonlinear, pre).fallbackCount == 0,
                        "nominal DC/tangent drive needed nonlinear numerical fallback");
            }
}

void harmonicQualification()
{
    for (bool pre : {true, false})
        for (double frequency : {1000., 8000.})
            for (double level : {.05, .78, 1.5})
            {
                const auto reference = oracleTone(pre, true, frequency >= 8000 ? 3072000 : 1536000, frequency, level);
                for (double rate : {48000., 88200., 96000., 176400., 192000., 768000.})
                {
                    const auto actual = runtimeTone(pre, true, rate, frequency, level);
                    std::cout << "Tone " << (pre ? "pre" : "post") << ' ' << frequency
                              << " Hz " << level << " Vrms at " << rate << " Hz";
                    for (int h = 0; h < 3; ++h)
                    {
                        if ((h + 1) * frequency >= .5 * rate)
                        {
                            std::cout << " H" << h + 1 << " at/outside Nyquist: complex ratio unidentifiable";
                            continue;
                        }
                        const Complex ratio = actual[h] / reference[h];
                        const double db = 20 * std::log10(std::abs(ratio));
                        const double degrees = std::arg(ratio) * 180 / pi;
                        std::cout << " H" << h + 1 << ' ' << db << " dB/" << degrees << " deg";
                        if (rate >= 176400 && h <= 1)
                            require(std::abs(db) < .2 && std::abs(degrees) < 2,
                                    "HQ nonlinear fundamental/H2 diverged from continuous physical oracle");
                        if (rate >= 176400 && h == 2)
                        {
                            // Float input/output rounding limits a tiny H3.
                            // Bound its coherent coefficient error by four
                            // epsilon times the fundamental amplitude, plus
                            //6% of the physical H3 for integration accuracy.
                            const double floatFloor = 4 * std::numeric_limits<float>::epsilon()
                                * std::abs(actual[0]);
                            require(std::abs(actual[h] - reference[h])
                                        < .06 * std::abs(reference[h]) + floatFloor,
                                    "HQ H3 error exceeds physical integration and float floor bound");
                        }
                    }
                    const double thd = 100 * std::sqrt(std::norm(actual[1]) + std::norm(actual[2])) / std::abs(actual[0]);
                    std::cout << " THD(H2,H3)=" << thd << "%\n";
                }
            }
}

std::array<Complex, 4> twoTone(bool pre, double rate, bool reference)
{
    constexpr std::array<double, 4> products {700, 2700, 300, 2400};
    const int settle = static_cast<int>(rate * .4), count = static_cast<int>(rate * .1);
    auto runtime = reference ? nullptr : device(rate);
    ContinuousOracle oracle(pre, true, rate);
    std::array<Complex, 4> result {};
    for (int n = 0; n < settle + count; ++n)
    {
        const double time = n / rate;
        const double input = .4 * std::sqrt(2.0)
            * (std::cos(2 * pi * 1000 * time) + std::cos(2 * pi * 1700 * time));
        const double output = reference ? oracle.step(input) : runtimeStep(*runtime, pre, input);
        if (n < settle) continue;
        for (std::size_t i = 0; i < products.size(); ++i)
            result[i] += output * std::polar(2.0 / count, -2 * pi * products[i] * time);
    }
    return result;
}

void intermodulationQualification()
{
    for (bool pre : {true, false})
    {
        const auto reference = twoTone(pre, 1536000, true);
        const auto refined = twoTone(pre, 768000, true);
        for (std::size_t i = 0; i < reference.size(); ++i)
            require(std::abs(refined[i] / reference[i] - 1.0) < .002,
                    "continuous two-tone oracle failed step refinement");
        for (double rate : {48000., 88200., 176400., 192000., 768000.})
        {
            const auto actual = twoTone(pre, rate, false);
            for (std::size_t i = 0; i < actual.size(); ++i)
            {
                const Complex ratio = actual[i] / reference[i];
                std::cout << "IM " << pre << ' ' << rate << " Hz product " << i
                          << " error " << 20 * std::log10(std::abs(ratio)) << " dB/"
                          << std::arg(ratio) * 180 / pi << " deg\n";
                if (rate >= 176400)
                    require(std::abs(ratio - 1.0) < .05,
                            "HQ intermodulation diverged from independent physical oracle");
            }
        }
    }
}

void nonlinearAliasScreen()
{
    constexpr double tone = 15000, inputRms = 1.5;
    for (bool pre : {true, false})
        for (double rate : {44100., 48000., 88200., 176400., 192000., 768000.})
        {
            auto nonlinear = device(rate);
            auto linear = device(rate, ChorusSupportProfile::Nominal2SA1015);
            // Identify the first two harmonic orders that fold into the
            // 20kHz band away from the original15k tone. The physical
            // continuous source has only integer multiples of15k, so these
            // low bins are numerical alias products, not real audio-band
            // harmonics or BBD clock folding. This is a measured screen;
            // it does not reuse the stationary *linear* SGA gate.
            std::array<double, 2> frequencies {};
            std::array<int, 2> orders {};
            int found = 0;
            for (int h = 2; h < 100 && found < 2; ++h)
            {
                const double folded = std::abs(std::remainder(h * tone, rate));
                if (folded > 20 && folded < 20000 && std::abs(folded - tone) > 1)
                {
                    frequencies[found] = folded;
                    orders[found++] = h;
                }
            }
            require(found == 2, "alias screen failed to select isolated bins");
            const int settle = static_cast<int>(rate * .4), count = static_cast<int>(rate * .1);
            Complex fundamental {};
            std::array<Complex, 2> residual {};
            for (int n = 0; n < settle + count; ++n)
            {
                const double time = n / rate;
                const double input = inputRms * std::sqrt(2.0) * std::cos(2 * pi * tone * time);
                const double a = runtimeStep(*nonlinear, pre, input);
                const double b = runtimeStep(*linear, pre, input);
                if (n < settle) continue;
                fundamental += a * std::polar(2.0 / count, -2 * pi * tone * time);
                for (int i = 0; i < 2; ++i)
                    residual[i] += (a - b) * std::polar(2.0 / count, -2 * pi * frequencies[i] * time);
            }
            for (int i = 0; i < 2; ++i)
                std::cout << "Nonlinear alias " << pre << ' ' << rate << " Hz H" << orders[i]
                          << " -> " << frequencies[i] << " Hz "
                          << 20 * std::log10(std::max(std::abs(residual[i] / fundamental), 1e-30))
                          << " dBc; input " << inputRms << " Vrms; spur "
                          << std::abs(residual[i]) << " Vpeak/"
                          << std::abs(residual[i]) / std::sqrt(2.0) << " Vrms; "
                          << 20 * std::log10(std::max(std::abs(residual[i]) / voltsPerUnit, 1e-30))
                          << " dB relative to2.6Vpeak node (not plugin-outputdBFS); fundamental "
                          << std::abs(fundamental) << " Vpeak; noBBD\n";
        }
}

void stateAndLifecycle()
{
    auto nonlinear = device(48000);
    auto linear = device(48000, ChorusSupportProfile::Nominal2SA1015);
    double difference = 0;
    for (int n = 0; n < 12000; ++n)
    {
        float left, right, a, b;
        const float input = static_cast<float>(.35 * std::sin(n * .17));
        nonlinear->process(input, youknow::ChorusMode::One, .05f, left, right);
        linear->process(input, youknow::ChorusMode::One, .05f, a, b);
        difference += std::abs(left - a) + std::abs(right - b);
        require(Probe::phase(*nonlinear) == Probe::phase(*linear)
                && Probe::rng(*nonlinear) == Probe::rng(*linear),
                "nonlinear followers changed clock or noise-source timing");
    }
    require(difference > .001, "full Chorus render bypassed nonlinear followers");
    require(!nonlinear->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015),
            "live support profile mutation accepted");
    nonlinear->prepareSupportRates(48000);
    const auto cells = Probe::buckets(*nonlinear);
    const auto phase = Probe::phase(*nonlinear);
    const auto rng = Probe::rng(*nonlinear);
    const auto builds = Probe::builds(*nonlinear);
    allocationCount = 0; countAllocations = true;
    for (double rate : {96000., 192000., 48000.}) nonlinear->prepare(rate, true);
    countAllocations = false;
    require(allocationCount == 0 && Probe::builds(*nonlinear) == builds,
            "cached nonlinear HQ selection allocated or rebuilt matrices");
    require(Probe::buckets(*nonlinear) == cells && Probe::phase(*nonlinear) == phase
            && Probe::rng(*nonlinear) == rng, "nonlinear HQ change discarded BBD physical state");
    require(Probe::inputState(*nonlinear) == std::array<double, 6>{}
            && Probe::outputState(*nonlinear) == std::array<double, 6>{},
            "nonlinear HQ change broke existing support reset under zero-gain policy");
    require(nonlinear->getSupportProfile() == nonlinearProfile, "HQ change lost nonlinear profile");
    nonlinear->reset();
    auto fresh = device(48000);
    for (int n = 0; n < 4096; ++n)
    {
        const double input = std::sin(n * .07);
        require(runtimeStep(*nonlinear, true, input) == runtimeStep(*fresh, true, input),
                "nonlinear reset retained stale residual-current history");
    }
    auto engine = std::make_unique<youknow::YouKnowEngine>();
    require(engine->configureChorusSupport(nonlinearProfile), "Engine refused nonlinear profile before prepare");
    engine->prepare(48000, 128, 4);
    require(!engine->configureChorusSupport(ChorusSupportProfile::Nominal2SA1015),
            "Engine accepted nonlinear profile mutation after prepare");
    engine->reset();
    for (int factor : {1, 2, 4})
    {
        Probe::quality(*engine, factor);
        require(Probe::engineProfile(*engine) == nonlinearProfile, "Engine lifecycle lost nonlinear profile");
    }
    engine->prepare(96000, 128, 4);
    require(Probe::engineProfile(*engine) == nonlinearProfile, "host reprepare lost nonlinear profile");
}

void switchedLoadPhysicalReference()
{
    for (double rate : {176400., 192000.})
    {
        auto chorus = device(rate);
        constexpr int substeps = 8;
        ContinuousOracle oracle(false, true, rate * substeps);
        bool connected = true;
        double errorEnergy = 0, signalEnergy = 0, peakError = 0;
        const auto source = [](double time)
        {
            const double envelope = std::pow(std::sin(pi * std::min(time / .01, 1.0) / 2), 2);
            return envelope * (1.2 * std::sin(2 * pi * 997 * time)
                + .3 * std::sin(2 * pi * 2341 * time));
        };
        for (int n = 1; n < static_cast<int>(rate * .08); ++n)
        {
            const bool load = (n / static_cast<int>(rate * .007)) % 2 == 0;
            if (load != connected)
            {
                oracle.setOutputLoad(load);
                connected = load;
            }
            double physical = 0;
            for (int sub = 1; sub <= substeps; ++sub)
                physical = oracle.step(source((n - 1 + sub / double(substeps)) / rate));
            const double actual = runtimeStep(*chorus, false, source(n / rate), connected);
            errorEnergy += (actual - physical) * (actual - physical);
            signalEnergy += physical * physical;
            peakError = std::max(peakError, std::abs(actual - physical));
        }
        const double relative = std::sqrt(errorEnergy / signalEnergy);
        std::cout << "Physical switched-load " << rate << " Hz relative RMS " << relative
                  << " peak error " << peakError << " V\n";
        require(relative < .002, "muted/connected nonlinear state diverged from physical charge-continuous reference");
        require(Probe::nonlinearState(*chorus, false).fallbackCount == 0,
                "physical load-switch scene required numerical fallback");
    }
}

void malformedAndTransientSafety()
{
    for (double rate : {8000., 16000., 32000., 44100., 48000., 192000., 768000.})
        for (bool pre : {true, false})
        {
            auto chorus = device(rate);
            double peak = 0;
            allocationCount = 0; countAllocations = true;
            for (int n = 0; n < static_cast<int>(rate * .25); ++n)
            {
                // Ordinary physical drive, interrupted DC steps, and mode
                // loading changes; no invented rail clipping assertion.
                double input = n < .02 * rate ? 1.5 : n < .04 * rate ? -1.2
                    : n < .06 * rate ? .3 : n < .08 * rate ? std::sin(n * .73) : 0;
                const bool connected = (n / 37) % 2 != 0;
                const double value = runtimeStep(*chorus, pre, input, connected);
                require(std::isfinite(value), "interrupted nonlinear transient became nonfinite");
                peak = std::max(peak, std::abs(value));
            }
            countAllocations = false;
            require(allocationCount == 0, "nonlinear transient path allocated");
            require(peak < 8, "bounded physical drive caused an excessive numerical excursion");
            require(Probe::nonlinearState(*chorus, pre).fallbackCount == 0,
                    "ordinary interrupted/load-switched transient needed numerical fallback");
            for (double malformed : {std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), 1e30})
                require(std::isfinite(runtimeStep(*chorus, pre, malformed)), "malformed input poisoned nonlinear support");
            for (int n = 0; n < 10000; ++n)
                require(std::isfinite(runtimeStep(*chorus, pre, .1 * std::sin(n * .07))),
                        "nonlinear support failed to recover after malformed input");
            std::cout << "Transient " << pre << ' ' << rate << " Hz peak " << peak << " V\n";
        }
}

void oracleChecks()
{
    for (bool pre : {true, false})
    {
        const PhysicalCircuit circuit(pre);
        const auto small = circuit.perturbation(1000, .01);
        std::cout << "Physical perturbation " << (pre ? "pre" : "post")
                  << " H2 " << 20 * std::log10(std::abs(small[1][7] / small[0][7]))
                  << " dBc\n";
        const auto weak = oracleTone(pre, true, 1536000, 1000, .01);
        require(std::abs(weak[1] / small[1][7] - 1.0) < .0001,
                "continuous oracle H2 disagrees with independent analytic perturbation");
        require(std::abs(weak[2] / small[2][7] - 1.0) < .002,
                "continuous oracle H3 disagrees with independent analytic perturbation");
        for (double frequency : {1000., 8000.})
        {
            const double lowRate = frequency == 1000 ? 768000 : 3072000;
            const auto a = oracleTone(pre, true, lowRate, frequency, .78);
            const auto b = oracleTone(pre, true, 2 * lowRate, frequency, .78);
            for (int h = 0; h < 3; ++h)
            {
                const double relative = std::abs(a[h] / b[h] - 1.0);
                std::cout << "Oracle refinement " << pre << ' ' << frequency << " Hz H" << h + 1
                          << " relative " << relative << '\n';
                require(relative < .001, "physical oracle not converged in complex harmonics");
            }
            const double thd = 100 * std::sqrt(std::norm(b[1]) + std::norm(b[2])) / std::abs(b[0]);
            std::cout << "Continuous physical " << (pre ? "pre" : "post") << ' ' << frequency
                      << " Hz .78Vrms THD " << thd << "%\n";
        }
    }
}
}

int main()
{
    try
    {
        oracleChecks();
        infinitesimalAndDc();
        harmonicQualification();
        intermodulationQualification();
        nonlinearAliasScreen();
        stateAndLifecycle();
        switchedLoadPhysicalReference();
        malformedAndTransientSafety();
        std::cout << "Chorus nonlinear oracle checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
