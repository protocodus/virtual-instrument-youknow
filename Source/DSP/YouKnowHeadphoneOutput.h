#pragma once

#include "YouKnowOutputJack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include "YouKnowCompatibility.h"
#include "YouKnowFinite.h"

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
// Optional IC7 intrinsic noise uses that ORIGINAL sheet's integrated input-referred
//2uVrms at Rs=1k,10Hz..30kHz. This one integrated reading does not identify
// white/flicker/current noise separately or its installed source dependence.
// Choose a flat equivalent PSD over29990Hz, conservatively subtracting the
// test1k resistor's4kTR at25C so it is not counted again in the drawn network.
// The sheet does not explicitly say whether VNI has already deembedded Rs:
// this subtraction is a qualified conservative convention, not a claim.
// The resulting10.813nV/sqrt(Hz) is fixed at the datasheet25C coordinate;
// no unknown device temperature coefficient is assigned. Character scales
// it once; zero Character/zero temperature retain diagnostic exact silence.
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
    static constexpr double intrinsicInputNoisePsd = 4e-12/29990.0
        -4*1.380649e-23*298.15*1000.0;
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
        if (!finiteValue(sampleRate) || sampleRate < 8000 || sampleRate > 768000
            || !validLoad(loadOhms)) return false;
        sampleRate_ = sampleRate;
        loadOhms_ = loadOhms;
        amplifierPole_ = OutputJackLowPass::coefficients(amplifierCornerHz,sampleRate_);
        intrinsicNoiseRateScale_ = std::sqrt(intrinsicInputNoisePsd*0.5*sampleRate_)*amplifierGain;
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
        if (!finiteValue(position)) return false;
        position = std::clamp(position, 0.0, 1.0);
        if (position != volume_) { volume_ = position; rebuild(); }
        return true;
    }
    [[nodiscard]] bool setNoise(double temperatureKelvin, double amount) noexcept
    {
        if (!finiteValue(temperatureKelvin) || temperatureKelvin < 0
            || temperatureKelvin > 1000 || !finiteValue(amount)
            || amount < 0 || amount > 16) return false;
        if (temperatureKelvin != temperature_ || amount != noiseAmount_)
        {
            temperature_ = temperatureKelvin;
            noiseAmount_ = amount;
            noiseScale_ = coefficients_.noiseDensityPerRootKelvin
                        * inlineSqrt(0.5 * sampleRate_ * temperature_) * noiseAmount_;
            rebuildAmplifierNoise();
        }
        return true;
    }
    // Changes neither capacitor nor amplifier histories. The inactive ideal
    // path primes response/slew history, and the engine advances this route
    // even while LINE is selected. Not a stored tone/session parameter.
    void setAmplifierDynamics(bool enabled) noexcept { amplifierDynamics_ = enabled; }
    void setIntrinsicNoise(bool enabled) noexcept { intrinsicNoise_ = enabled; }
    [[nodiscard]] std::array<double,2> amplifierVoltages() const noexcept
    { return {channels_[0].amplifierOutput,channels_[1].amplifierOutput}; }
    void reset() noexcept { channels_ = {}; }
    [[nodiscard]] std::array<double,2> process(double leftVolts, double rightVolts,
        double leftUnitNoise = 0.0, double rightUnitNoise = 0.0,
        double leftOutputUnitNoise = 0.0, double rightOutputUnitNoise = 0.0,
        double leftIntrinsicUnitNoise = 0.0, double rightIntrinsicUnitNoise = 0.0) noexcept
    {
#if defined(YOUKNOW_HAS_DOUBLE2_SIMD) && !defined(YOUKNOW_WORK_AUDIT)
        const double in0 = finiteValue(leftVolts) ? leftVolts : 0.0;
        const double in1 = finiteValue(rightVolts) ? rightVolts : 0.0;
        const double un0 = finiteValue(leftUnitNoise) ? leftUnitNoise : 0.0;
        const double un1 = finiteValue(rightUnitNoise) ? rightUnitNoise : 0.0;
        const double oun0 = finiteValue(leftOutputUnitNoise) ? leftOutputUnitNoise : 0.0;
        const double oun1 = finiteValue(rightOutputUnitNoise) ? rightOutputUnitNoise : 0.0;
        const double iun0 = finiteValue(leftIntrinsicUnitNoise) ? leftIntrinsicUnitNoise : 0.0;
        const double iun1 = finiteValue(rightIntrinsicUnitNoise) ? rightIntrinsicUnitNoise : 0.0;
        const auto lowPass2 = [](Double2 x, Double2& s, Double2 g, Double2 denom) noexcept -> Double2 {
            const Double2 delta = (x - s) * g / denom;
            const Double2 out = s + delta;
            s = out + delta;
            return out;
        };
        const Double2 input = double2Pack(in0, in1);
        const Double2 unitNoise = double2Pack(un0, un1);
        const Double2 inG = double2Splat(inputG_);
        const Double2 inDenom = double2Splat(1.0 + inputG_);
        Double2 inputLow = double2Pack(channels_[0].inputLow, channels_[1].inputLow);
        Double2 noiseLow = double2Pack(channels_[0].noiseLow, channels_[1].noiseLow);
        Double2 ampNoiseLow = double2Pack(channels_[0].amplifierNoiseLow, channels_[1].amplifierNoiseLow);
        Double2 headphoneLow = double2Pack(channels_[0].headphoneLow, channels_[1].headphoneLow);
        const Double2 coupled = (input - lowPass2(input, inputLow, inG, inDenom))
            * double2Splat(coefficients_.passbandGain);
        const Double2 white = unitNoise * double2Splat(noiseScale_);
        const Double2 noise = double2MulAdd(
            white, double2Splat(coefficients_.noiseLowGain - 1.0),
            lowPass2(white, noiseLow, inG, inDenom));
        const Double2 deviceNoise = intrinsicNoise_
            ? double2Pack(iun0, iun1) * double2Splat(intrinsicNoiseScale_)
            : double2Splat(0.0);
        const Double2 outDiv = double2Splat(outputDivider_);
        Double2 mixed = double2MulAdd(coupled + noise, deviceNoise, outDiv);
        const Double2 amplifierWhite = unitNoise * double2Splat(amplifierNoiseScale_);
        const Double2 amplifierNoise = double2MulAdd(
            amplifierWhite, double2Splat(amplifierNoiseLowGain_ - 1.0),
            lowPass2(amplifierWhite, ampNoiseLow, inG, inDenom));
        const Double2 idealAmplifier = coupled / outDiv + amplifierNoise + deviceNoise;
        Double2 ampPrevNext = idealAmplifier;
        Double2 ampDiffNext = double2Splat(0.0);
        Double2 ampOutNext = idealAmplifier;
        if (amplifierDynamics_)
        {
            const Double2 ampPrev = double2Pack(channels_[0].amplifierPreviousInput,
                                                channels_[1].amplifierPreviousInput);
            const Double2 ampDiff = double2Pack(channels_[0].amplifierDifference,
                                                channels_[1].amplifierDifference);
            ampDiffNext = double2MulSub(
                double2Splat(amplifierPole_.correction) * (idealAmplifier - ampPrev),
                double2Splat(amplifierPole_.a1), ampDiff);
            const Double2 target = idealAmplifier + ampDiffNext;
            const double maximumStep = amplifierSlewVoltsPerSecond / sampleRate_;
            const Double2 ampOutPrev = double2Pack(channels_[0].amplifierOutput,
                                                   channels_[1].amplifierOutput);
            const Double2 stepDelta = target - ampOutPrev;
            const Double2 clampedStep = double2Pack(
                std::clamp(double2Low(stepDelta), -maximumStep, maximumStep),
                std::clamp(double2High(stepDelta), -maximumStep, maximumStep));
            ampOutNext = ampOutPrev + clampedStep;
            mixed = double2MulAdd(
                ampOutNext, double2Pack(oun0, oun1), double2Splat(outputNoiseScale_)) * outDiv;
        }
        const Double2 hpG = double2Splat(headphoneG_);
        const Double2 hpDenom = double2Splat(1.0 + headphoneG_);
        const Double2 output = mixed - lowPass2(mixed, headphoneLow, hpG, hpDenom);
        const double out0 = double2Low(output);
        const double out1 = double2High(output);
        if (finiteValue(out0) && finiteValue(out1))
        {
            channels_[0].inputLow = double2Low(inputLow);
            channels_[1].inputLow = double2High(inputLow);
            channels_[0].headphoneLow = double2Low(headphoneLow);
            channels_[1].headphoneLow = double2High(headphoneLow);
            channels_[0].noiseLow = double2Low(noiseLow);
            channels_[1].noiseLow = double2High(noiseLow);
            channels_[0].amplifierNoiseLow = double2Low(ampNoiseLow);
            channels_[1].amplifierNoiseLow = double2High(ampNoiseLow);
            channels_[0].amplifierPreviousInput = double2Low(ampPrevNext);
            channels_[1].amplifierPreviousInput = double2High(ampPrevNext);
            channels_[0].amplifierDifference = double2Low(ampDiffNext);
            channels_[1].amplifierDifference = double2High(ampDiffNext);
            channels_[0].amplifierOutput = double2Low(ampOutNext);
            channels_[1].amplifierOutput = double2High(ampOutNext);
            return { out0, out1 };
        }
#endif
        return { step(channels_[0], leftVolts, leftUnitNoise,leftOutputUnitNoise,leftIntrinsicUnitNoise),
                 step(channels_[1], rightVolts, rightUnitNoise,rightOutputUnitNoise,rightIntrinsicUnitNoise) };
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
        const double device = intrinsicNoise_ && temperatureKelvin>0
            ? intrinsicInputNoisePsd*amplifierGain*amplifierGain*outputDivider_*outputDivider_ : 0;
        if (!amplifierDynamics_) return (legacy+device)*w*w/(w*w+q*q);
        // Only IC7 input/feedback noise traverses its response. R79/R80's
        // independent220-ohm Johnson source is after the amplifier.
        const double series = 4*1.380649e-23*temperatureKelvin*220*outputDivider_*outputDivider_;
        const double ratio = frequencyHz/amplifierCornerHz;
        return ((legacy-series+device)/(1+ratio*ratio)+series)*w*w/(w*w+q*q);
    }
    // Looking back into the PHONES jack with the ideal IC7 source grounded.
    // This excludes the declared headphone itself and never shorts L to R.
    [[nodiscard]] static std::complex<double> analogOutputImpedance(double frequencyHz) noexcept
    {
        return {220.0, frequencyHz > 0 ? -1.0/(twoPi*frequencyHz*47e-6)
                                      : -std::numeric_limits<double>::infinity()};
    }

private:
    static constexpr double twoPi = 2.0 * numbers::pi;
    struct Channel
    {
        double inputLow {}, headphoneLow {}, noiseLow {}, amplifierNoiseLow {};
        double amplifierPreviousInput {}, amplifierDifference {}, amplifierOutput {};
    };
    std::array<Channel,2> channels_ {};
    Coefficients coefficients_ {};
    double sampleRate_ {48000}, loadOhms_ {32}, volume_ {1};
    double inputG_ {}, headphoneG_ {}, temperature_ {}, noiseAmount_ {}, noiseScale_ {};
    bool amplifierDynamics_ {}, intrinsicNoise_ {};
    OutputJackLowPass::Coefficients amplifierPole_ {};
    double outputDivider_ {}, amplifierNoiseLowGain_ {}, amplifierNoiseScale_ {}, outputNoiseScale_ {};
    double intrinsicNoiseScale_ {}, intrinsicNoiseRateScale_ {};
    static bool validLoad(double load) noexcept
    { return finiteValue(load) && load >= 1 && load <= 1e9; }
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
            inlineSqrt(noiseDc/noiseHf),
            inlineSqrt(4*1.380649e-23*noiseHf)*outputDivider};
        inputG_ = std::tan(numbers::pi*inputPole/sampleRate_);
        headphoneG_ = std::tan(numbers::pi*headphonePole/sampleRate_);
        amplifierNoiseLowGain_ = inlineSqrt((noiseDc-220)/(noiseHf-220));
        noiseScale_ = coefficients_.noiseDensityPerRootKelvin
                    * inlineSqrt(0.5*sampleRate_*temperature_)*noiseAmount_;
        rebuildAmplifierNoise();
    }
    void rebuildAmplifierNoise() noexcept
    {
        const double scale = inlineSqrt(0.5*sampleRate_*temperature_)*noiseAmount_;
        outputNoiseScale_ = inlineSqrt(4*1.380649e-23*220)*scale;
        const double total = coefficients_.noiseDensityPerRootKelvin/outputDivider_;
        amplifierNoiseScale_ = inlineSqrt(std::max(0.0,total*total-4*1.380649e-23*220))*scale;
        // Device density stays at its measured25C coordinate, unlike Johnson
        // noise. Zero-K suppression is only an exact-silence diagnostic policy.
        intrinsicNoiseScale_ = temperature_>0
            ? intrinsicNoiseRateScale_*noiseAmount_ : 0;
    }
    double step(Channel& state, double input, double unitNoise,double outputUnitNoise,double intrinsicUnitNoise) noexcept
    {
        if (!finiteValue(input)) input = 0;
        if (!finiteValue(unitNoise)) unitNoise = 0;
        if (!finiteValue(outputUnitNoise)) outputUnitNoise = 0;
        if (!finiteValue(intrinsicUnitNoise)) intrinsicUnitNoise = 0;
        const double coupled = (input-lowPass(input,state.inputLow,inputG_))
                             * coefficients_.passbandGain;
        const double white = unitNoise*noiseScale_;
        const double noise = white+(coefficients_.noiseLowGain-1)
                           * lowPass(white,state.noiseLow,inputG_);
        const double deviceNoise = intrinsicNoise_ ? intrinsicUnitNoise*intrinsicNoiseScale_ : 0;
        double mixed = coupled+noise+deviceNoise*outputDivider_;
        const double amplifierWhite = unitNoise*amplifierNoiseScale_;
        const double amplifierNoise = amplifierWhite+(amplifierNoiseLowGain_-1)
            *lowPass(amplifierWhite,state.amplifierNoiseLow,inputG_);
        const double idealAmplifier = coupled/outputDivider_+amplifierNoise+deviceNoise;
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
        if (!finiteValue(output)) { state = {}; return 0; }
        return output;
    }
};
} // namespace youknow
