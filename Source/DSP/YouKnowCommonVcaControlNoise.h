#pragma once

#include <cmath>

namespace youknow
{
// Roland JUNO-106 jack board p15: R30=2.2k, C7=10uF to ground,
// R32=1.5k to IC5 GC1, R31=47ohm to ground and R165=15k to +15V.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// With ideal quiet IC28/+15V sources and negligible GC1 current, eliminate
// the resistor nodes: r=R31||R165, Zgc=r||(R32+(R30||1/sC7)). Thus
// Re(Zgc)=Rhigh+(Rlow-Rhigh)/(1+(w*tau)^2), where Rhigh=r||R32,
// Rlow=r||(R30+R32), tau=(R30||(R32+r))*C7. Fluctuation-dissipation
// gives one-sided voltage PSD4kT*Re(Zgc), equivalent to independent white
// and OU sources. The OU stationary variance is kT*(Rlow-Rhigh)/tau;
// exact interval decay/innovation retain this stationary PSD-equivalent
// network state across rate changes. This does not identify the circuit's
// full nonstationary covariance under spatially different temperatures.
// Thermal-noise law: https://www.analog.com/media/en/training-seminars/tutorials/MT-047.pdf
//
// This EXTERNAL network is absent from NEC's -94dBV measurement: its p259
// VNO test grounds GC1 pin3. The existing intrinsic output floor remains.
// NEC p257's -5.9mV/dB at25C gives delta(g)/g=-ln(10)/(20*5.9mV)*deltaVgc;
// apply its existing T/298.15 scaling. This is small-noise linearization of
// that same gain law, not added device-CV/flicker/supply noise. At25C the
// 10Hz..20k white-dominated modulation is about2.39ppm RMS; omitted quadratic
// terms are order1e-11 at these noise voltages. Finite IC28 impedance,
// active GC1 loading, installed temperatures and component spreads are open.
// https://archive.org/download/bitsavers_necdataBooCircuitsforConsumerUse_42422169/1983_NEC_Integrated_Circuits_for_Consumer_Use.pdf#page=262
struct CommonVcaControlNoise
{
    static constexpr double boltzmann = 1.380649e-23;
    static constexpr double referenceKelvin = 298.15;
    static constexpr double r30 = 2200.0, r32 = 1500.0;
    static constexpr double r31 = 47.0, r165 = 15000.0, c7 = 10e-6;
    static constexpr double biasResistance = r31*r165/(r31+r165);
    static constexpr double highResistance = biasResistance*r32/(biasResistance+r32);
    static constexpr double lowResistance = biasResistance*(r30+r32)/(biasResistance+r30+r32);
    static constexpr double timeConstant = r30*(r32+biasResistance)/(r30+r32+biasResistance)*c7;
    static constexpr double referenceGainSensitivity = 2.302585092994045684/(20*5.9e-3);

    struct Coefficients
    {
        double decay {}, whiteRms {}, innovationRms {};
    };
    [[nodiscard]] static Coefficients coefficients(double rate) noexcept
    {
        const double interval = 1.0/rate;
        const double variance = boltzmann*referenceKelvin
            *(lowResistance-highResistance)/timeConstant;
        return {std::exp(-interval/timeConstant),
            std::sqrt(2*boltzmann*referenceKelvin*highResistance*rate),
            std::sqrt(variance*-std::expm1(-2*interval/timeConstant))};
    }
    void reset() noexcept { slowVolts = 0; }
    [[nodiscard]] double process(const Coefficients& c, double thermalRmsScale,
        double whiteNormal, double slowNormal) noexcept
    {
        // Only the innovation follows current T; preserve the equivalent
        // voltage state when temperature or rate changes, rather than
        // rescaling its history. This follows the uniform-board-T model.
        slowVolts = c.decay*slowVolts+c.innovationRms*thermalRmsScale*slowNormal;
        return c.whiteRms*thermalRmsScale*whiteNormal+slowVolts;
    }
    double slowVolts {};
};
}
