// Fractional pre-BBD captures need a continuous physical-node reference;
// checking only the accepted filter endpoints misses interpolation error.
// This input-only MNA stamps the two followers and BOTH BBD input branches
// from Roland JUNO-106 Service Notes p.15, independently of runtime matrices:
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// The nominal forward-active model retains the qualified beta=200, 25 C,
// Cmu=4 pF and ro=infinity assumptions. It does not identify installed parts.
// A refined full-node trapezoidal/Newton DAE is deliberately different from
// production's exact linear flow plus interpolated residual-current ports.
// Only HQ uses that physical mapping; lower grids retain their existing
// prewarped numerical policy and are tested for compatibility separately.
#include "DSP/YouKnowChorus.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace { std::atomic<bool> counting {false}; std::atomic<unsigned> allocations {0}; }
void* operator new(std::size_t n)
{
    if (counting) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

namespace youknow
{
struct YouKnowTestAccess
{
    static float input(Chorus& c, float value, bool capture = true)
    { return c.advanceInputSupport(value, capture); }
    static const auto& capture(const Chorus& c) { return c.inputSupport_.captureInterval; }
    static auto inputState(const Chorus& c) { return c.inputSupport_.exactState; }
    static auto nonlinear(const Chorus& c) { return c.inputSupport_.nonlinear; }
    static auto inputHistory(const Chorus& c)
    { return std::array {c.inputSupport_.exactPrevious, c.inputSupport_.exactPrevious2,
                         c.inputSupport_.exactPrevious3}; }
    static float line(Chorus& c, bool right, float input, double clock,
                      const Chorus::InputCaptureInterval* capture)
    { return (right ? c.lineB_ : c.lineA_).processClockedCore(input, clock, c.sampleRate_, 0, capture); }
    static auto cells(const Chorus& c, bool right = false)
    { return (right ? c.lineB_ : c.lineA_).cells; }
    static auto phase(const Chorus& c, bool right = false)
    { return (right ? c.lineB_ : c.lineA_).clockPhase; }
    static int index(const Chorus& c, bool right = false)
    { return (right ? c.lineB_ : c.lineA_).writeIndex; }
};
}

namespace
{
using youknow::Chorus;
using youknow::ChorusSupportProfile;
using Probe = youknow::YouKnowTestAccess;
constexpr double pi = std::numbers::pi_v<double>;
constexpr double beta = 200.0, voltsPerUnit = 2.6;
constexpr double vt = 1.380649e-23 * 298.15 / 1.602176634e-19;
constexpr auto nonlinearProfile = ChorusSupportProfile::Nominal2SA1015Nonlinear;
template <std::size_t N> using Vector = std::array<double, N>;
template <std::size_t N> using Matrix = std::array<Vector<N>, N>;

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

template <std::size_t N> Vector<N> solve(Matrix<N> a, Vector<N> b)
{
    for (std::size_t col = 0; col < N; ++col)
    {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < N; ++row)
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
        require(std::abs(a[pivot][col]) > 1e-20, "singular physical input MNA");
        std::swap(a[col], a[pivot]); std::swap(b[col], b[pivot]);
        const double inverse = 1 / a[col][col];
        for (std::size_t j = col; j < N; ++j) a[col][j] *= inverse;
        b[col] *= inverse;
        for (std::size_t row = 0; row < N; ++row)
            if (row != col)
            {
                const double multiple = a[row][col];
                for (std::size_t j = col; j < N; ++j) a[row][j] -= multiple * a[col][j];
                b[row] -= multiple * b[col];
            }
    }
    return b;
}
template <std::size_t N> void stamp(Matrix<N>& a, int p, int q, double value)
{
    if (p >= 0) a[p][p] += value;
    if (q >= 0) a[q][q] += value;
    if (p >= 0 && q >= 0) { a[p][q] -= value; a[q][p] -= value; }
}

std::array<double, 2> biasCurrents()
{
    // Unknowns source,b1,e1,b2,e2,Ie1,Ie2. DC KCL and Ve-Vb=.61 V;
    // each pair of 22k resistors combines only for this DC calculation.
    Matrix<7> a {}; Vector<7> b {};
    a[0][0] = 1; a[1][1] = 1.0 / 44000; a[1][0] = -1.0 / 44000;
    stamp(a, 2, 3, 1.0 / 44000);
    for (int i = 0; i < 2; ++i)
    {
        const int base = 1 + 2 * i, emitter = base + 1, current = 5 + i;
        const double load = i ? 10000 : 22000;
        stamp(a, emitter, -1, 1 / load); b[emitter] = 15 / load;
        a[emitter][current] = 1; a[base][current] = -1 / (beta + 1);
        a[current][emitter] = 1; a[current][base] = -1; b[current] = .61;
    }
    const auto x = solve(a, b);
    return {x[5] * beta / (beta + 1), x[6] * beta / (beta + 1)};
}

// Samples at normalized positions 1,0,-1,-2. Independent Lagrange basis,
// not the runtime dense extension or its prepared polynomial coefficients.
double sourcePolynomial(double fraction, const std::array<double, 4>& samples)
{
    double value = 0;
    for (int i = 0; i < 4; ++i)
    {
        double basis = 1;
        for (int j = 0; j < 4; ++j)
            if (i != j) basis *= (fraction - (1 - j)) / double(j - i);
        value += basis * samples[i];
    }
    return value;
}

struct PhysicalInput
{
    // Nodes j1,b1,e1,j2,b2,e2,couplingA,inputA,couplingB,inputB.
    Matrix<10> g {}, c {};
    Vector<10> v {};
    const std::array<double, 2> ic = biasCurrents();
    const bool nonlinear;
    double previousInput = 0;
    unsigned maximumIterations = 0;

    explicit PhysicalInput(bool useNonlinearity) : nonlinear(useNonlinearity)
    {
        const auto resistor = [&](int p, int q, double r) { stamp(g, p, q, 1 / r); };
        resistor(0, -1, 22000); resistor(0, 1, 22000);
        resistor(2, 3, 22000); resistor(3, 4, 22000);
        resistor(2, -1, 22000); resistor(5, -1, 10000);
        stamp(c, 0, 2, 820e-12); stamp(c, 1, -1, 684e-12);
        stamp(c, 3, 5, 1.8e-9); stamp(c, 4, -1, 274e-12);
        const double bias = (8.3 / 15 * 37000) * (1 - 8.3 / 15);
        for (int branch = 0; branch < 2; ++branch)
        {
            const int coupling = 6 + 2 * branch, input = coupling + 1;
            stamp(c, 5, coupling, 100e-9); resistor(coupling, -1, 100000 + bias);
            resistor(coupling, input, 10000); stamp(c, input, -1, 2.2e-9);
        }
        for (int i = 0; i < 2; ++i)
        {
            const int base = 1 + 3 * i, emitter = base + 1;
            const double gm = ic[i] / vt;
            stamp(g, base, emitter, gm / beta);
            g[emitter][emitter] += gm; g[emitter][base] -= gm;
        }
    }
    Vector<10> current(const Vector<10>& state) const
    {
        Vector<10> result {};
        if (nonlinear)
            for (int i = 0; i < 2; ++i)
            {
                const int base = 1 + 3 * i, emitter = base + 1;
                const double y = (state[emitter] - state[base]) / vt;
                const double q = ic[i] * (std::expm1(y) - y);
                result[base] += q / beta; result[emitter] -= q * (1 + 1 / beta);
            }
        return result;
    }
    void step(double input, double dt)
    {
        Matrix<10> matrix {};
        auto rhs = current(v);
        rhs[0] += (input + previousInput) / 22000;
        for (int row = 0; row < 10; ++row)
            for (int col = 0; col < 10; ++col)
            {
                matrix[row][col] = 2 / dt * c[row][col] + g[row][col];
                rhs[row] += (2 / dt * c[row][col] - g[row][col]) * v[col];
            }
        auto next = v;
        bool converged = false;
        for (unsigned iteration = 0; iteration < 20; ++iteration)
        {
            auto residual = current(next);
            auto jacobian = matrix;
            for (int row = 0; row < 10; ++row)
            {
                residual[row] += rhs[row];
                for (int col = 0; col < 10; ++col) residual[row] -= matrix[row][col] * next[col];
            }
            if (nonlinear)
                for (int i = 0; i < 2; ++i)
                {
                    const int base = 1 + 3 * i, emitter = base + 1;
                    const double slope = ic[i] / vt * std::expm1((next[emitter] - next[base]) / vt);
                    stamp(jacobian, base, emitter, slope / beta);
                    jacobian[emitter][emitter] += slope; jacobian[emitter][base] -= slope;
                }
            const auto correction = solve(jacobian, residual);
            double largest = 0;
            for (int i = 0; i < 10; ++i)
            { next[i] += correction[i]; largest = std::max(largest, std::abs(correction[i])); }
            maximumIterations = std::max(maximumIterations, iteration + 1);
            if (largest < 2e-12) { converged = true; break; }
        }
        require(converged, "physical fractional oracle did not converge");
        v = next; previousInput = input;
    }
    void advance(double from, double to, double rate, const std::array<double, 4>& inputs, int refinement)
    {
        if (to <= from) return;
        const int steps = std::max(1, int(std::ceil((to - from) * refinement)));
        const double dt = (to - from) / (rate * steps);
        for (int i = 1; i <= steps; ++i)
            step(sourcePolynomial(from + (to - from) * i / steps, inputs), dt);
    }
};

std::unique_ptr<Chorus> device(double rate, ChorusSupportProfile profile = nonlinearProfile)
{
    auto c = std::make_unique<Chorus>();
    require(c->configureSupportProfile(profile), "pre-prepare support configuration failed");
    c->prepare(rate);
    return c;
}
double signal(int scene, double time)
{
    if (scene == 0) return std::sqrt(2.0) * .01 * std::sin(2 * pi * 1000 * time);
    if (scene == 1) return std::sqrt(2.0) * .78 * std::sin(2 * pi * 15000 * time);
    const double tones = .25 * (std::sin(2 * pi * 3100 * time)
        + std::sin(2 * pi * 7900 * time + .31) + std::sin(2 * pi * 15100 * time + .73));
    if (scene == 2) return tones;
    if (scene == 3) return 4 * tones;
    return (time < .0004 ? 0 : time < .0007 ? 1.5 : -.75) + tones;
}
struct Error
{
    double squared {}, energy {}, peak {};
    void add(double actual, double reference)
    { squared += (actual - reference) * (actual - reference); energy += reference * reference; peak = std::max(peak, std::abs(actual - reference)); }
    double nrms() const { return std::sqrt(squared / std::max(energy, 1e-30)); }
};

void physicalFractions(double rate, int scene, bool nonlinear)
{
    auto runtime = device(rate, nonlinear ? nonlinearProfile : ChorusSupportProfile::Nominal2SA1015);
    PhysicalInput coarse(nonlinear), fine(nonlinear);
    std::array<double, 4> inputs {}, endpoints {};
    std::array<double, 2> phase {.173, .719};
    constexpr std::array<double, 2> clocks {37001, 199999};
    Error dense, legacy, endpoint, refinement, captures, priming, primingEndpoint, primingRefinement;
    double branchMismatch = 0, currentTrajectoryError = 0;
    // 256/512 subdivisions resolve the hardest 15k input to <2 ppm;
    // every other row also proves convergence instead of trusting a step size.
    for (int frame = 1; frame <= 256; ++frame)
    {
        for (int i = 3; i > 0; --i) { inputs[i] = inputs[i - 1]; endpoints[i] = endpoints[i - 1]; }
        const float input = float(signal(scene, frame / rate) / voltsPerUnit);
        inputs[0] = double(input) * voltsPerUnit;
        const auto oldCurrent = Probe::nonlinear(*runtime);
        endpoints[0] = double(Probe::input(*runtime, input)) * voltsPerUnit;
        const auto newCurrent = Probe::nonlinear(*runtime);
        const auto& interval = Probe::capture(*runtime);
        require(interval.enabled, "HQ input did not prepare dense capture interval");
        std::vector<double> fractions {0, .03125, .173, .499, .501, .827, .96875, 1};
        std::vector<double> events;
        for (int side = 0; side < 2; ++side)
        {
            const double increment = clocks[side] / rate, end = phase[side] + increment;
            for (double edge = 1; edge <= end; edge += 1) events.push_back((edge - phase[side]) / increment);
            phase[side] = end - std::floor(end);
        }
        fractions.insert(fractions.end(), events.begin(), events.end());
        std::sort(fractions.begin(), fractions.end());
        fractions.erase(std::unique(fractions.begin(), fractions.end()), fractions.end());
        double previous = 0;
        for (double fraction : fractions)
        {
            coarse.advance(previous, fraction, rate, inputs, 256);
            fine.advance(previous, fraction, rate, inputs, 512);
            const double reference = fine.v[7], value = interval.valueAt(fraction) * voltsPerUnit;
            refinement.add(coarse.v[7], reference);
            branchMismatch = std::max(branchMismatch, std::abs(fine.v[7] - fine.v[9]));
            if (fraction > 0 && fraction < 1)
            { dense.add(value, reference); legacy.add(sourcePolynomial(fraction, endpoints), reference); }
            else endpoint.add(value, reference);
            if (frame <= 3)
            {
                priming.add(value, reference); primingRefinement.add(coarse.v[7], reference);
                if (fraction == 0 || fraction == 1) primingEndpoint.add(value, reference);
            }
            if (std::find(events.begin(), events.end(), fraction) != events.end()) captures.add(value, reference);
            if (nonlinear)
                for (int port = 0; port < 2; ++port)
                {
                    const std::array<double, 4> history {newCurrent.previousCurrent[port],
                        oldCurrent.previousCurrent[port], oldCurrent.previous2Current[port],
                        oldCurrent.previous3Current[port]};
                    const double interpolated = oldCurrent.valid && oldCurrent.currentHistoryDepth >= 3
                        ? sourcePolynomial(fraction, history)
                        : history[1] + fraction * (history[0] - history[1]);
                    const int base = 1 + 3 * port, emitter = base + 1;
                    const double y = (fine.v[emitter] - fine.v[base]) / vt;
                    const double physical = fine.ic[port] * (std::expm1(y) - y);
                    // Diagnostic against the physical reference trajectory,
                    // not literal KCL closure of the scalar-only dense query.
                    // Its existing interpolated q is approximate. Normalize
                    // by DC current, never by q near a zero.
                    currentTrajectoryError = std::max(currentTrajectoryError,
                        std::abs(interpolated * voltsPerUnit - physical) / fine.ic[port]);
                }
            previous = fraction;
        }
    }
    std::cout << "Physical fractions rate=" << rate << " scene=" << scene << " nonlinear=" << nonlinear
              << " dense=" << dense.nrms() << " captured=" << captures.nrms()
              << " legacy=" << legacy.nrms() << " endpoint=" << endpoint.nrms()
              << " oracle refinement=" << refinement.nrms() << " priming=" << priming.nrms()
              << " priming endpoint=" << primingEndpoint.nrms()
              << " priming refinement=" << primingRefinement.nrms()
              << " current-trajectory mismatch/Ic0=" << currentTrajectoryError << " peak V=" << dense.peak << '\n';
    require(refinement.nrms() < 2e-6, "fractional physical oracle needs further refinement");
    require(branchMismatch < 1e-10, "identical physical BBD branches diverged");
    require(Probe::nonlinear(*runtime).fallbackCount == 0, "ordinary input needed nonlinear fallback");
    // Numerical qualification, not a claim of exact nonlinear KCL between
    // endpoints: the accepted residual-current polynomial remains approximate.
    require(dense.nrms() < 2e-5 && captures.nrms() < 2e-5,
            "HQ dense captures exceed physical input integration error budget");
    require(dense.nrms() < 4 * (endpoint.nrms() + refinement.nrms()) + 1e-10,
            "dense interior error is separated from the accepted endpoint error floor");
    // The first three intervals deliberately use the pre-existing linear
    // current startup rule. Their tiny signal energy makes relative errors
    // larger; dense evaluation must remain at that measured endpoint floor,
    // not silently impose a new nonlinear startup method.
    require(priming.nrms() < 4 * (primingEndpoint.nrms() + primingRefinement.nrms()) + 1e-10,
            "dense startup error is separated from the accepted endpoint error floor");
    if (legacy.nrms() > 2e-5)
        require(dense.nrms() < .1 * legacy.nrms(), "dense input did not remove interior interpolation error");
}

bool sameNonlinear(const Chorus::SupportChain::NonlinearState& a,
                   const Chorus::SupportChain::NonlinearState& b)
{
    return a.previousCurrent == b.previousCurrent && a.previous2Current == b.previous2Current
        && a.previous3Current == b.previous3Current && a.junctionThermalVolts == b.junctionThermalVolts
        && a.currentHistoryDepth == b.currentHistoryDepth && a.fallbackCount == b.fallbackCount
        && a.maximumIterations == b.maximumIterations && a.topology == b.topology && a.valid == b.valid;
}
void lifecycleAndQueries()
{
    auto c = device(192000);
    require(!Probe::capture(*c).enabled, "prepare retained a stale input interval");
    for (int frame = 0; frame < 48; ++frame)
    {
        const double initial = Probe::inputState(*c)[5];
        allocations = 0; counting = true;
        const float result = Probe::input(*c, float(.2 * std::sin(frame * .61)));
        counting = false;
        require(allocations == 0, "dense interval preparation allocated on the audio path");
        const auto beforeState = Probe::inputState(*c);
        const auto beforeHistory = Probe::inputHistory(*c);
        const auto beforeNonlinear = Probe::nonlinear(*c);
        const auto interval = Probe::capture(*c);
        require(interval.valueAt(0) == initial && interval.valueAt(1) == beforeState[5],
                "dense query does not preserve accepted endpoints exactly");
        require(float(interval.valueAt(1)) == result, "dense final endpoint changed returned support value");
        allocations = 0; counting = true;
        double sum = 0;
        for (double fraction : {.83, .1, .9999, 0.0, .5, .1, 1.0, .0001})
        {
            const double a = Probe::capture(*c).valueAt(fraction);
            require(a == interval.valueAt(fraction), "dense query depends on query order");
            sum += a;
        }
        counting = false;
        require(std::isfinite(sum) && allocations == 0, "dense input query allocates or is nonfinite");
        require(Probe::inputState(*c) == beforeState && Probe::inputHistory(*c) == beforeHistory
                && sameNonlinear(Probe::nonlinear(*c), beforeNonlinear),
                "fractional query advanced capacitor or residual-current history");
    }
    c->reset();
    require(!Probe::capture(*c).enabled, "reset retained a stale dense interval");
    for (double rate : {48000., 96000., 176400., 768000., 192000.})
    {
        c->prepare(rate, true);
        require(!Probe::capture(*c).enabled, "rate change retained a stale dense interval");
        Probe::input(*c, .1f);
        require(Probe::capture(*c).enabled == (rate >= 176400), "dense capture selected on wrong numerical grid");
        Probe::input(*c, .2f, false);
        require(!Probe::capture(*c).enabled, "unneeded capture interval was prepared or remained stale");
    }
}

void captureSelectionParity()
{
    for (double rate : {48000., 96000., 192000.})
    {
        auto prepared = device(rate), skipped = device(rate);
        Chorus::InputCaptureInterval disabled;
        disabled.outputByPower.fill(10);
        disabled.initialOutput = disabled.finalOutput = 10;
        for (int frame = 0; frame < 384; ++frame)
        {
            const float input = float(.2 * std::sin(frame * .71));
            const float a = Probe::input(*prepared, input, true);
            const float b = Probe::input(*skipped, input, false);
            require(a == b && Probe::inputState(*prepared) == Probe::inputState(*skipped)
                    && Probe::inputHistory(*prepared) == Probe::inputHistory(*skipped)
                    && sameNonlinear(Probe::nonlinear(*prepared), Probe::nonlinear(*skipped)),
                    "capture preparation changed accepted input-filter state");
            require(!Probe::capture(*skipped).enabled, "skipped dense preparation left stale output");
            if (rate < 176400) require(!Probe::capture(*prepared).enabled, "low-grid capture behavior changed");
            const float nullOutput = Probe::line(*prepared, false, a, 79999, nullptr);
            const float disabledOutput = Probe::line(*prepared, true, a, 79999, &disabled);
            require(nullOutput == disabledOutput && Probe::cells(*prepared) == Probe::cells(*prepared, true),
                    "disabled capture pointer changed legacy Line processing");
        }
    }
}

float acquired(double value)
{
    const float input = float(value);
    const double v = std::abs(double(input)) / double(1.1246614f);
    return float(input / std::pow(1 + double(1.2044546f) * v * v
                    + std::pow(v, double(12.9395323f)), 1 / double(12.9395323f)));
}
void asynchronousCaptures()
{
    auto c = device(192000);
    // A known cubic is an independent event-scheduling fixture, not a copy
    // of the dense builder. Both clocks must use the SAME immutable interval.
    std::array<double, 2> phase {};
    std::array<int, 2> index {};
    std::array<std::array<float, 128>, 2> cells {};
    for (int frame = 0; frame < 400; ++frame)
    {
        Chorus::InputCaptureInterval interval;
        interval.enabled = true;
        interval.outputByPower[0] = .13 * std::sin(frame * .2);
        interval.outputByPower[1] = .31;
        // Separate linear and cubic intervals exercise the event sampler
        // without relying on any output from production's polynomial builder.
        const double quadratic = frame < 200 ? 0 : -.17;
        const double cubic = frame < 200 ? 0 : .09;
        interval.outputByPower[2] = quadratic;
        interval.outputByPower[3] = cubic;
        interval.initialOutput = interval.outputByPower[0];
        interval.finalOutput = interval.initialOutput + .31 + quadratic + cubic;
        for (int side = 0; side < 2; ++side)
        {
            const double clock = frame >= 71 && frame < 83 ? 0 : side ? 199999 : 37001;
            const double step = clock / 192000, end = phase[side] + step;
            for (double edge = 1; edge <= end; edge += 1)
            {
                const double f = (edge - phase[side]) / step;
                const double value = interval.initialOutput + .31 * f + quadratic * f * f + cubic * f * f * f;
                index[side] = (index[side] + 1) % 128;
                cells[side][index[side]] = acquired(value);
            }
            allocations = 0; counting = true;
            const float output = Probe::line(*c, side != 0, float(interval.finalOutput), clock, &interval);
            counting = false;
            phase[side] = end - std::floor(end);
            require(allocations == 0 && std::isfinite(output), "dense asynchronous capture allocated or became nonfinite");
            require(Probe::index(*c, side != 0) == index[side], "dense capture changed physical input event count");
            require(std::abs(Probe::phase(*c, side != 0) - phase[side]) < 1e-12, "dense capture changed clock phase");
            const auto actual = Probe::cells(*c, side != 0);
            for (int i = 0; i < 128; ++i)
                require(std::abs(actual[i] - cells[side][i]) < 6e-8f,
                        "BBD line did not acquire dense physical input at its own edge");
        }
    }
}
}

int main()
{
    try
    {
        std::cout << std::setprecision(12);
        lifecycleAndQueries();
        captureSelectionParity();
        asynchronousCaptures();
        physicalFractions(176400, 1, true);
        physicalFractions(192000, 1, true);
        physicalFractions(192000, 1, false);
        physicalFractions(192000, 3, true);
        physicalFractions(192000, 4, true);
        physicalFractions(384000, 2, true);
        physicalFractions(768000, 0, true);
        std::cout << "Dense pre-BBD input contracts passed\n";
    }
    catch (const std::exception& e)
    { counting = false; std::cerr << e.what() << '\n'; return 1; }
}
