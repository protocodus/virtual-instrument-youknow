#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace youknow
{
// Roland JUNO-106 Service Notes p15: C17/C20 10uF, R54/R57 1.5k,
// VR1 10KB, the still-connected 33k+6.8k+1.5k LINE selector ladder,
// IC7 input R77/R78 1k and R83/R84 100k, feedback 39k over15k,
// C26/C27 47uF NP and R79/R80 220 ohm into JA3 PHONES.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// The external headphone load is declared and noiseless; these are electrical
// volts, not a headphone's acoustic response. LINE jacks are assumed unplugged.
// IC7 is an ideal noninverting amplifier (3.6): no installed current limit,
// clipping, op-amp noise, bandwidth or reactive headphone impedance is inferred.
// In particular the M5218L's +/-50mA ABSOLUTE maximum is not a clipping law.
// Mitsubishi1984 data book, printed p2-45:
// https://www.bitsavers.org/components/mitsubishi/_dataBooks/1984_Mitsubishi_General_Purpose_ICs.pdf
//
// The passive input's Thevenin resistance has one pole from C17. Its real
// part is Zhf+(Zdc-Zhf)/(1+(f/fc)^2), so one white draw through the matching
// shelf reproduces ALL input resistor Johnson powers. Add IC7's feedback
// resistors' 4kT*(39k+39k^2/15k) and the220-ohm output resistor at IC7's
// output, then pass them through C26, once. No external receiver noise is added.
// Both signal high-passes and the noise shelf use TPT at host rate; histories
// survive coefficient changes. Phase/capacitor trajectories on changing pot
// or load are numerical approximations, not a contact/charge switching model.
class HeadphoneOutput
{
public:
    enum class Route { Line, Headphones };
    struct Coefficients
    {
        double inputPoleHz {}, headphonePoleHz {}, passbandGain {};
        double noiseLowGain {}, noiseDensityPerRootKelvin {};
    };

    // No allocation or implicit reset.1..1e9 ohm is a numerical domain, not
    // a headphone measurement; product choices declare32/80/300/600 ohm.
    [[nodiscard]] bool prepare(double sampleRate, double loadOhms = 32.0) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 768000
            || !validLoad(loadOhms)) return false;
        sampleRate_ = sampleRate;
        loadOhms_ = loadOhms;
        rebuild();
        return true;
    }
    [[nodiscard]] bool setLoad(double loadOhms) noexcept
    {
        if (!validLoad(loadOhms)) return false;
        if (loadOhms != loadOhms_) { loadOhms_ = loadOhms; rebuild(); }
        return true;
    }
    [[nodiscard]] bool setVolume(double position) noexcept
    {
        if (!std::isfinite(position)) return false;
        position = std::clamp(position, 0.0, 1.0);
        if (position != volume_) { volume_ = position; rebuild(); }
        return true;
    }
    [[nodiscard]] bool setNoise(double temperatureKelvin, double amount) noexcept
    {
        if (!std::isfinite(temperatureKelvin) || temperatureKelvin < 0
            || temperatureKelvin > 1000 || !std::isfinite(amount)
            || amount < 0 || amount > 16) return false;
        if (temperatureKelvin != temperature_ || amount != noiseAmount_)
        {
            temperature_ = temperatureKelvin;
            noiseAmount_ = amount;
            noiseScale_ = coefficients_.noiseDensityPerRootKelvin
                        * std::sqrt(0.5 * sampleRate_ * temperature_) * noiseAmount_;
        }
        return true;
    }
    void reset() noexcept { channels_ = {}; }
    [[nodiscard]] std::array<double,2> process(double leftVolts, double rightVolts,
        double leftUnitNoise = 0.0, double rightUnitNoise = 0.0) noexcept
    {
        return { step(channels_[0], leftVolts, leftUnitNoise),
                 step(channels_[1], rightVolts, rightUnitNoise) };
    }
    [[nodiscard]] const Coefficients& coefficients() const noexcept { return coefficients_; }
    [[nodiscard]] double loadOhms() const noexcept { return loadOhms_; }
    [[nodiscard]] double volume() const noexcept { return volume_; }
    [[nodiscard]] std::complex<double> analogResponse(double frequencyHz) const noexcept
    {
        const std::complex<double> s {0, twoPi * frequencyHz};
        return coefficients_.passbandGain * s / (s + twoPi * coefficients_.inputPoleHz)
             * s / (s + twoPi * coefficients_.headphonePoleHz);
    }
    [[nodiscard]] double analogNoisePsd(double frequencyHz, double temperatureKelvin) const noexcept
    {
        const double w = twoPi * frequencyHz;
        const double p = twoPi * coefficients_.inputPoleHz;
        const double q = twoPi * coefficients_.headphonePoleHz;
        const double z = p * coefficients_.noiseLowGain;
        return temperatureKelvin * std::pow(coefficients_.noiseDensityPerRootKelvin,2)
             * (w*w + z*z)/(w*w + p*p) * w*w/(w*w + q*q);
    }
    // Looking back into the PHONES jack with the ideal IC7 source grounded.
    // This excludes the declared headphone itself and never shorts L to R.
    [[nodiscard]] static std::complex<double> analogOutputImpedance(double frequencyHz) noexcept
    {
        return {220.0, frequencyHz > 0 ? -1.0/(twoPi*frequencyHz*47e-6)
                                      : -std::numeric_limits<double>::infinity()};
    }

private:
    static constexpr double twoPi = 2.0 * std::numbers::pi;
    struct Channel { double inputLow {}, headphoneLow {}, noiseLow {}; };
    std::array<Channel,2> channels_ {};
    Coefficients coefficients_ {};
    double sampleRate_ {48000}, loadOhms_ {32}, volume_ {1};
    double inputG_ {}, headphoneG_ {}, temperature_ {}, noiseAmount_ {}, noiseScale_ {};
    static bool validLoad(double load) noexcept
    { return std::isfinite(load) && load >= 1 && load <= 1e9; }
    static double parallel(double a, double b) noexcept { return a*b/(a+b); }
    static double lowPass(double input, double& state, double g) noexcept
    {
        const double delta = (input-state)*g/(1+g);
        const double output = state+delta;
        state = output+delta;
        return output;
    }
    void rebuild() noexcept
    {
        constexpr double amplifierGain = 1.0+39000.0/15000.0;
        const double upper = 1500.0+(1-volume_)*10000.0;
        const double base = parallel(volume_*10000.0,41300.0);
        const double lower = parallel(base,101000.0);
        const double inputPole = 1.0/(twoPi*10e-6*(upper+lower));
        const double headphonePole = 1.0/(twoPi*47e-6*(220.0+loadOhms_));
        const double outputDivider = loadOhms_/(220.0+loadOhms_);
        const double zdc = parallel(100000.0,1000.0+base);
        const double zhf = parallel(100000.0,1000.0+parallel(base,upper));
        constexpr double feedbackResistance = 39000.0+39000.0*39000.0/15000.0;
        const double noiseDc = amplifierGain*amplifierGain*zdc+feedbackResistance+220.0;
        const double noiseHf = amplifierGain*amplifierGain*zhf+feedbackResistance+220.0;
        coefficients_ = {inputPole,headphonePole,
            lower/(upper+lower)*(100000.0/101000.0)*amplifierGain*outputDivider,
            std::sqrt(noiseDc/noiseHf),
            std::sqrt(4*1.380649e-23*noiseHf)*outputDivider};
        inputG_ = std::tan(std::numbers::pi*inputPole/sampleRate_);
        headphoneG_ = std::tan(std::numbers::pi*headphonePole/sampleRate_);
        noiseScale_ = coefficients_.noiseDensityPerRootKelvin
                    * std::sqrt(0.5*sampleRate_*temperature_)*noiseAmount_;
    }
    double step(Channel& state, double input, double unitNoise) noexcept
    {
        if (!std::isfinite(input)) input = 0;
        if (!std::isfinite(unitNoise)) unitNoise = 0;
        const double coupled = (input-lowPass(input,state.inputLow,inputG_))
                             * coefficients_.passbandGain;
        const double white = unitNoise*noiseScale_;
        const double noise = white+(coefficients_.noiseLowGain-1)
                           * lowPass(white,state.noiseLow,inputG_);
        const double mixed = coupled+noise;
        const double output = mixed-lowPass(mixed,state.headphoneLow,headphoneG_);
        if (!std::isfinite(output)) { state = {}; return 0; }
        return output;
    }
};
} // namespace youknow
