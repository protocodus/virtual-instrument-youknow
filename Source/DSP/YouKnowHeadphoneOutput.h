#pragma once

#include "YouKnowOutputJack.h"

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
// The reference keeps IC7 ideal. Optional nominal dynamics use the ORIGINAL
// M5218L's typical fT=7MHz and SR=2.2V/us at25C,+/-15V (SR: unity gain,
// RL=2k). Do not substitute the later M5218A's3V/us. A dominant-pole,
// infinite-DC-gain reduction gives7MHz/(1+39k/15k)=1.944MHz. It is a
// component prior, not installed loaded bandwidth/slew calibration.
// No installed current limit, clipping or reactive headphone impedance is inferred.
// In particular the M5218L's +/-50mA ABSOLUTE maximum is not a clipping law.
// Mitsubishi1984 data book, printed pp2-45/46 (PDFpp86/87):
// https://www.bitsavers.org/components/mitsubishi/_dataBooks/1984_Mitsubishi_General_Purpose_ICs.pdf
// Archival mirror of the same manufacturer scan:
// https://archive.decromancer.ca/bitsavers.org/components/mitsubishi/_dataBooks/1984_Mitsubishi_General_Purpose_ICs.pdf#page=87
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
    static constexpr double amplifierGain = 1.0+39000.0/15000.0;
    static constexpr double amplifierGainBandwidthHz = 7.0e6;
    static constexpr double amplifierCornerHz = amplifierGainBandwidthHz/amplifierGain;
    static constexpr double amplifierSlewVoltsPerSecond = 2.2e6;
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
        amplifierPole_ = OutputJackLowPass::coefficients(amplifierCornerHz,sampleRate_);
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
            rebuildAmplifierNoise();
        }
        return true;
    }
    // Changes neither capacitor nor amplifier histories. The inactive ideal
    // path primes response/slew history, and the engine advances this route
    // even while LINE is selected. Not a stored tone/session parameter.
    void setAmplifierDynamics(bool enabled) noexcept { amplifierDynamics_ = enabled; }
    [[nodiscard]] bool amplifierDynamics() const noexcept { return amplifierDynamics_; }
    [[nodiscard]] std::array<double,2> amplifierVoltages() const noexcept
    { return {channels_[0].amplifierOutput,channels_[1].amplifierOutput}; }
    void reset() noexcept { channels_ = {}; }
    [[nodiscard]] std::array<double,2> process(double leftVolts, double rightVolts,
        double leftUnitNoise = 0.0, double rightUnitNoise = 0.0,
        double leftOutputUnitNoise = 0.0, double rightOutputUnitNoise = 0.0) noexcept
    {
        return { step(channels_[0], leftVolts, leftUnitNoise,leftOutputUnitNoise),
                 step(channels_[1], rightVolts, rightUnitNoise,rightOutputUnitNoise) };
    }
    [[nodiscard]] const Coefficients& coefficients() const noexcept { return coefficients_; }
    [[nodiscard]] double loadOhms() const noexcept { return loadOhms_; }
    [[nodiscard]] double volume() const noexcept { return volume_; }
    [[nodiscard]] std::complex<double> analogResponse(double frequencyHz) const noexcept
    {
        const std::complex<double> s {0, twoPi * frequencyHz};
        const auto response = coefficients_.passbandGain * s / (s + twoPi * coefficients_.inputPoleHz)
             * s / (s + twoPi * coefficients_.headphonePoleHz);
        return amplifierDynamics_ ? response/(1.0+s/(twoPi*amplifierCornerHz)) : response;
    }
    [[nodiscard]] double analogNoisePsd(double frequencyHz, double temperatureKelvin) const noexcept
    {
        const double w = twoPi * frequencyHz;
        const double p = twoPi * coefficients_.inputPoleHz;
        const double q = twoPi * coefficients_.headphonePoleHz;
        const double z = p * coefficients_.noiseLowGain;
        const double legacy = temperatureKelvin * std::pow(coefficients_.noiseDensityPerRootKelvin,2)
                            * (w*w + z*z)/(w*w + p*p);
        if (!amplifierDynamics_) return legacy*w*w/(w*w+q*q);
        // Only IC7 input/feedback noise traverses its response. R79/R80's
        // independent220-ohm Johnson source is after the amplifier.
        const double series = 4*1.380649e-23*temperatureKelvin*220*outputDivider_*outputDivider_;
        const double ratio = frequencyHz/amplifierCornerHz;
        return ((legacy-series)/(1+ratio*ratio)+series)*w*w/(w*w+q*q);
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
    struct Channel
    {
        double inputLow {}, headphoneLow {}, noiseLow {}, amplifierNoiseLow {};
        double amplifierPreviousInput {}, amplifierDifference {}, amplifierOutput {};
    };
    std::array<Channel,2> channels_ {};
    Coefficients coefficients_ {};
    double sampleRate_ {48000}, loadOhms_ {32}, volume_ {1};
    double inputG_ {}, headphoneG_ {}, temperature_ {}, noiseAmount_ {}, noiseScale_ {};
    bool amplifierDynamics_ {};
    OutputJackLowPass::Coefficients amplifierPole_ {};
    double outputDivider_ {}, amplifierNoiseLowGain_ {}, amplifierNoiseScale_ {}, outputNoiseScale_ {};
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
        const double upper = 1500.0+(1-volume_)*10000.0;
        const double base = parallel(volume_*10000.0,41300.0);
        const double lower = parallel(base,101000.0);
        const double inputPole = 1.0/(twoPi*10e-6*(upper+lower));
        const double headphonePole = 1.0/(twoPi*47e-6*(220.0+loadOhms_));
        const double outputDivider = loadOhms_/(220.0+loadOhms_);
        outputDivider_ = outputDivider;
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
        amplifierNoiseLowGain_ = std::sqrt((noiseDc-220)/(noiseHf-220));
        noiseScale_ = coefficients_.noiseDensityPerRootKelvin
                    * std::sqrt(0.5*sampleRate_*temperature_)*noiseAmount_;
        rebuildAmplifierNoise();
    }
    void rebuildAmplifierNoise() noexcept
    {
        const double scale = std::sqrt(0.5*sampleRate_*temperature_)*noiseAmount_;
        outputNoiseScale_ = std::sqrt(4*1.380649e-23*220)*scale;
        const double total = coefficients_.noiseDensityPerRootKelvin/outputDivider_;
        amplifierNoiseScale_ = std::sqrt(std::max(0.0,total*total-4*1.380649e-23*220))*scale;
    }
    double step(Channel& state, double input, double unitNoise,double outputUnitNoise) noexcept
    {
        if (!std::isfinite(input)) input = 0;
        if (!std::isfinite(unitNoise)) unitNoise = 0;
        if (!std::isfinite(outputUnitNoise)) outputUnitNoise = 0;
        const double coupled = (input-lowPass(input,state.inputLow,inputG_))
                             * coefficients_.passbandGain;
        const double white = unitNoise*noiseScale_;
        const double noise = white+(coefficients_.noiseLowGain-1)
                           * lowPass(white,state.noiseLow,inputG_);
        double mixed = coupled+noise;
        const double amplifierWhite = unitNoise*amplifierNoiseScale_;
        const double amplifierNoise = amplifierWhite+(amplifierNoiseLowGain_-1)
            *lowPass(amplifierWhite,state.amplifierNoiseLow,inputG_);
        const double idealAmplifier = coupled/outputDivider_+amplifierNoise;
        if (amplifierDynamics_)
        {
            // The corner is above host Nyquist. The existing matched one-pole
            // preserves its tiny audible magnitude instead of aliasing an
            // exponential impulse into unity. Its minimum-phase history is
            // numerical, not the actual compensation capacitor trajectory.
            state.amplifierDifference = amplifierPole_.correction
                *(idealAmplifier-state.amplifierPreviousInput)
                -amplifierPole_.a1*state.amplifierDifference;
            state.amplifierPreviousInput = idealAmplifier;
            const double target = idealAmplifier+state.amplifierDifference;
            const double maximumStep = amplifierSlewVoltsPerSecond/sampleRate_;
            // Nominal sampled output-voltage slew AFTER response and BEFORE
            // C26/220ohm/load attenuation. This bounds sampled secant slopes,
            // not unresolved intra-frame slewing/THD or an oversampled diode
            // law. No rail clipping is invented from absolute maxima.
            state.amplifierOutput += std::clamp(target-state.amplifierOutput,-maximumStep,maximumStep);
            mixed = (state.amplifierOutput+outputUnitNoise*outputNoiseScale_)*outputDivider_;
        }
        else
        {
            state.amplifierPreviousInput = idealAmplifier;
            state.amplifierDifference = 0;
            state.amplifierOutput = idealAmplifier;
        }
        const double output = mixed-lowPass(mixed,state.headphoneLow,headphoneG_);
        if (!std::isfinite(output)) { state = {}; return 0; }
        return output;
    }
};
} // namespace youknow
