#pragma once

namespace youknow
{
// Roland JUNO-106 Service Notes, July 31 1984, module board p.13:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
//
// C66 -> IC18d follower -> R117 100k -> IC17a inverting summing node.
// R116 560k injects -15V into that node. The feedback is R118 56k +
// VR31 20k, in parallel with C62 .047uF. R119 47k then drives C63 .0047uF
// at the shared PWM CV output. Thus, with the opamp's virtual ground:
//
// C62*dVo/dt = -Vin/R117 + 15/R116 - Vo/(R118+VR31)
// C63*dVpwm/dt = (Vo-Vpwm)/R119.
//
// R117 and R116 are NOT the two capacitor discharge resistances. Using
// R117*C62 and R116*C63 overstated both smoothing times (4.7/2.632ms).
struct PwmControlCircuit
{
    static constexpr double inputOhms = 100.0e3;
    static constexpr double biasOhms = 560.0e3;
    static constexpr double feedbackFixedOhms = 56.0e3;
    static constexpr double feedbackTrimOhms = 20.0e3;
    static constexpr double feedbackFarads = 47.0e-9;
    static constexpr double outputOhms = 47.0e3;
    static constexpr double outputFarads = 4.7e-9;

    // p.8: IC28a's PWM S/H branch spans approximately +4 to -6V. p.9/19:
    // the shared VR31 sets the 50% point to approximately +6V PWM CV.
    // Solving the DC KCL at that point gives Rf=69.136k, VR31=13.136k.
    // This is an inferred nominal service operating point, not a measured
    // installed pot position. Across VR31's full travel tau1 is 2.632--3.572ms.
    // The printed voltages are rounded: preserve the engine's established
    // +6/-0.8V DC calibration and change only the normalized dynamics here.
    static constexpr double nominalFeedbackOhms =
        6.0 / (6.0 / inputOhms + 15.0 / biasOhms);
    static constexpr double feedbackPoleSeconds =
        nominalFeedbackOhms * feedbackFarads;
    static constexpr double outputPoleSeconds = outputOhms * outputFarads;
};
static_assert(PwmControlCircuit::nominalFeedbackOhms
                  >= PwmControlCircuit::feedbackFixedOhms
              && PwmControlCircuit::nominalFeedbackOhms
                  <= PwmControlCircuit::feedbackFixedOhms
                         + PwmControlCircuit::feedbackTrimOhms);
} // namespace youknow
