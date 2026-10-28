// The HPF Boost leg's untrimmed parts, Roland jack board p. 15: IC4b's R18
// 100k / C6 22n feedback over R19 10k, returned into IC4a's R29 47k summing
// node through R24 220k. The oracle is IC4b's analogue gain,
// 1 + (R18 || C6) / R19, evaluated at the trapezoid's prewarped frequency.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static HighPassSwitchCircuit boost(const YouKnowEngine& engine) { return engine.highPassSwitch_; }
};
}

namespace
{
using namespace youknow;
using Complex = std::complex<double>;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

constexpr int boostMode = 0;

double hardClip(double x) { return std::clamp(x, -1.0, 1.0); }
double noClip(double x) { return x; }

std::vector<double> probe(HighPassSwitchCircuit circuit, double amplitude, double (*clip)(double))
{
    circuit.reset();
    std::vector<double> out(6000);
    for (std::size_t n = 0; n < out.size(); ++n)
    {
        // A bass tone with a step, through Boost and out to Two and back.
        const int mode = n < 4000 || n >= 5000 ? boostMode : 2;
        const double x = amplitude * (std::sin(0.013 * static_cast<double>(n)) + (n >= 1500 ? 0.5 : 0.0));
        out[n] = circuit.process(x, mode, clip);
    }
    return out;
}

void nominalIsThePrintedCircuit()
{
    HighPassSwitchCircuit printed;
    printed.prepare(96000.0, 110.0);
    auto installed = printed;
    installed.setBoostResistors(100e3, 10e3, 220e3);
    for (double amplitude : { 0.05, 0.6 })
        require(probe(printed, amplitude, hardClip) == probe(installed, amplitude, hardClip),
                "the printed Boost parts changed the circuit's samples");
}

Complex response(HighPassSwitchCircuit circuit, double rate, double hz)
{
    // Settle C14's 0.33 s coupling, then correlate whole periods.
    const int settle = static_cast<int>(4.0 * rate), span = static_cast<int>(2.0 * rate);
    Complex sum {};
    for (int n = 0; n < settle + span; ++n)
    {
        const double phase = 2.0 * std::numbers::pi * hz * n / rate;
        const double y = circuit.process(0.01 * std::sin(phase), boostMode, noClip);
        if (n >= settle) sum += y * Complex(std::sin(phase), std::cos(phase));
    }
    return sum;
}

void shelfLaw()
{
    // Node N feeds IC4b's high-impedance input and the direct legs never see
    // these parts, so per frequency y = D + (R29/R24) G N with D and N fixed.
    // Drawing only R24 isolates N; the full draw must then follow G alone.
    constexpr double rate = 48000.0, hz = 40.0;
    const double h = 1.0 / rate;
    const double omega = 2.0 / h * std::tan(std::numbers::pi * hz * h);
    const auto gain = [&](double r18, double r19) {
        const Complex z = r18 / Complex(1.0, omega * r18 * 22e-9);
        return 1.0 + z / r19;
    };
    HighPassSwitchCircuit nominal;
    nominal.prepare(rate, 110.0);
    const Complex yNominal = response(nominal, rate, hz);
    for (float calibration : { 1.0f, 2.0f })
    {
        const double r18 = Chorus::drawnResistance(100e3, 18u, calibration);
        const double r19 = Chorus::drawnResistance(10e3, 19u, calibration);
        const double r24 = Chorus::drawnResistance(220e3, 24u, calibration);
        auto drawn = nominal, returnOnly = nominal;
        drawn.setBoostResistors(r18, r19, r24);
        returnOnly.setBoostResistors(100e3, 10e3, r24);
        const Complex measured = (response(drawn, rate, hz) - yNominal)
                               / (response(returnOnly, rate, hz) - yNominal);
        const double rho = 47e3 / r24, rho0 = 47e3 / 220e3;
        const Complex g0 = gain(100e3, 10e3);
        const Complex expected = (rho * gain(r18, r19) - rho0 * g0) / ((rho - rho0) * g0);
        require(std::abs(measured - expected) < 1e-6 * std::abs(expected),
                "the Boost shelf does not follow its drawn R18, R19 and R24");

        // Low-band boost height against nominal, and the class bound on it.
        const double heightDb = 20.0 * std::log10(rho * (1.0 + r18 / r19) / (rho0 * 11.0));
        const double t = Chorus::carbonResistorToleranceClass * calibration;
        const double boundDb = 20.0 * std::log10((1.0 + 10.0 * (1.0 + t) / (1.0 - t)) / (1.0 - t) / 11.0);
        require(heightDb != 0.0 && std::abs(heightDb) <= boundDb,
                "the Boost height left the drawn resistor class");
        std::cout << "Boost leg at Character " << calibration << ": R18 " << r18 << ", R19 " << r19
                  << ", R24 " << r24 << "; low-band return " << heightDb << " dB, C6 corner x"
                  << 100e3 / r18 << '\n';
    }
}

EngineParameters productParameters()
{
    EngineParameters parameters;
    ActiveProductFidelityProfile::applyTo(parameters);
    return parameters;
}

std::unique_ptr<YouKnowEngine> productEngine(float calibration, bool spread)
{
    auto engine = std::make_unique<YouKnowEngine>();
    ActiveProductFidelityProfile::configureBeforePrepare(*engine);
    auto parameters = productParameters();
    parameters.calibration = calibration;
    parameters.enableHighPassBoostSpread = spread;
    engine->setParameters(parameters);
    engine->prepare(48000.0, 128, 1);
    return engine;
}

void reachesTheEngine()
{
    using Probe = YouKnowTestAccess;
    const auto off = Probe::boost(*productEngine(1.0f, false));
    const auto expectedAt = [&](float calibration) {
        auto circuit = off;
        circuit.setBoostResistors(Chorus::drawnResistance(100e3, 18u, calibration),
                                  Chorus::drawnResistance(10e3, 19u, calibration),
                                  Chorus::drawnResistance(220e3, 24u, calibration));
        return probe(circuit, 0.3, noClip);
    };
    auto engine = productEngine(1.0f, true);
    require(probe(Probe::boost(*engine), 0.3, noClip) == expectedAt(1.0f),
            "the product engine did not install its drawn Boost parts");
    require(probe(Probe::boost(*engine), 0.3, noClip) != probe(off, 0.3, noClip),
            "the drawn Boost parts do not reach the signal");

    // A live Unit Character move redraws them; zero restores the printed parts.
    float left[128] {}, right[128] {};
    auto parameters = productParameters();
    for (float calibration : { 2.0f, 0.0f })
    {
        parameters.calibration = calibration;
        engine->setParameters(parameters);
        engine->process(left, right, 128);
        require(probe(Probe::boost(*engine), 0.3, noClip) == expectedAt(calibration),
                "a Unit Character change did not redraw the Boost parts");
    }
    require(probe(Probe::boost(*engine), 0.3, noClip) == probe(off, 0.3, noClip),
            "Unit Character zero did not restore the printed Boost parts");
}
}

int main()
{
    try
    {
        nominalIsThePrintedCircuit();
        shelfLaw();
        reachesTheEngine();
        std::cout << "HPF Boost parts passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
