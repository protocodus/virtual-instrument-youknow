#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace youknow
{
// Comparison circuit for IC3 and the complete C14/HPF passive network.
// Roland Service Notes p.15: C14=10uF, R39=33k; C11=4.7nF,
// C10=15nF, R21/R23=1M, R25..R29=47k; C9=47nF, C8=10nF,
// R22=47k, R18=100k, R19=10k, C6=22nF and R24=220k.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf
//
// Toshiba TC4052BP's current data gives 240/110/80 ohms typical at
// 5/10/15V total supply, 25C; those are comparison coordinates, not an
// installed-part measurement. The Juno uses +5V and the Tr3 negative rail,
// not +/-7.5V. R13/R14 put Tr3's base near -2.63V; its emitter rail depends
// on the junction. Therefore this API requires an explicit resistance and
// never silently installs one of the published supply points as the Juno's.
// https://toshiba.semicon-storage.com/info/TC4052BP_datasheet_en_20160115.pdf?did=18603&prodName=TC4052BP
// Charge injection, voltage-dependent Ron, leakage and rail clipping are
// deliberately uncalibrated. All deselected capacitor stores remain live.
class HighPassSwitchCircuit
{
public:
    static constexpr int stores = 5;
    using State = std::array<double, stores>;

    void prepare(double rate, double resistance) noexcept
    {
        // Only setup builds/inverts matrices; no allocation or inversion in
        // process(). Retain the physical capacitor voltages on rate changes.
        const double fastTau = resistance * (47e-9 * 10e-9 / (47e-9 + 10e-9));
        switchSteps_ = std::clamp(static_cast<int>(std::ceil(10.0 / (rate * fastTau))), 1, 4096);
        for (int mode = 0; mode != 4; ++mode)
        {
            ordinary_[mode] = makeKernel(mode, resistance, 1.0 / rate);
            switching_[mode] = makeKernel(mode, resistance, 1.0 / (rate * switchSteps_));
        }
    }

    void reset() noexcept { voltage_.fill(0); feedbackVoltage_ = 0; previousMode_ = -1; }
    [[nodiscard]] const State& capacitorVoltages() const noexcept { return voltage_; }
    [[nodiscard]] double feedbackVoltage() const noexcept { return feedbackVoltage_; }

    template <typename Clip>
    double process(double input, int mode, Clip clip) noexcept
    {
        mode = std::clamp(mode, 0, 3); // panel order: Boost, One, Two, Three
        double output = 0;
        if (mode != previousMode_)
        {
            // Resolve the fast Ron/C9/C8 redistribution on a switch event.
            // An ordinary coarse trapezoidal step would invent a long
            // alternating residue from this sub-sample physical mode.
            for (int i = 0; i != switchSteps_; ++i)
                output += step(input, switching_[mode], clip);
            output /= switchSteps_;
            previousMode_ = mode;
        }
        else
            output = step(input, ordinary_[mode], clip);
        if (!std::isfinite(output)) { reset(); return 0; }
        return output;
    }

private:
    friend struct HighPassSwitchTestAccess;
    struct Kernel
    {
        std::array<State, stores> transition {};
        State input {}, direct {}, boostNode {};
        double directInput {}, boostInput {}, feedbackLeak {}, feedbackDrive {};
    };
    std::array<Kernel, 4> ordinary_ {}, switching_ {};
    State voltage_ {};
    double feedbackVoltage_ {};
    int previousMode_ {-1}, switchSteps_ {1};

    static Kernel makeKernel(int mode, double ron, double h) noexcept
    {
        // Midpoint nodes: common, Y0, C11-right, Y1, C10-right, Y2, Y3, N.
        // Each capacitor carries its physical start-of-interval voltage.
        // Midpoint I = 2C/h * (Vmid - Vstart); Vnext = 2Vmid - Vstart.
        constexpr int nodes = 8, columns = nodes + stores + 1;
        std::array<std::array<double, columns>, nodes> a {};
        const auto resistor = [&](int p, int q, double r) {
            const double g = 1.0 / r;
            a[p][p] += g;
            if (q >= 0) { a[q][q] += g; a[p][q] -= g; a[q][p] -= g; }
        };
        resistor(0, -1, 33000);
        resistor(1, -1, 1e6); resistor(3, -1, 1e6);
        for (int node : {2, 4, 5, 6}) resistor(node, -1, 47000);
        resistor(6, 7, 47000);
        constexpr std::array<int, 4> selected {6, 5, 3, 1};
        resistor(0, selected[mode], ron);
        // p=-2 denotes the driven source, q=-1 denotes ground.
        constexpr std::array<int, stores> p {-2, 1, 3, 6, 7}, q {0, 2, 4, 7, -1};
        constexpr State capacitance {10e-6, 4.7e-9, 15e-9, 47e-9, 10e-9};
        for (int c = 0; c != stores; ++c)
        {
            const double g = 2.0 * capacitance[c] / h;
            if (p[c] >= 0)
            {
                a[p[c]][p[c]] += g; a[p[c]][nodes + c] += g;
                if (q[c] >= 0) a[p[c]][q[c]] -= g;
            }
            if (q[c] >= 0)
            {
                a[q[c]][q[c]] += g; a[q[c]][nodes + c] -= g;
                if (p[c] >= 0) a[q[c]][p[c]] -= g;
                else if (p[c] == -2) a[q[c]][columns - 1] += g;
            }
        }
        // Fixed-size partial-pivot Gauss-Jordan elimination, setup only.
        for (int c = 0; c != nodes; ++c)
        {
            int pivot = c;
            for (int row = c + 1; row != nodes; ++row)
                if (std::abs(a[row][c]) > std::abs(a[pivot][c])) pivot = row;
            std::swap(a[c], a[pivot]);
            const double diagonal = a[c][c];
            for (int col = c; col != columns; ++col) a[c][col] /= diagonal;
            for (int row = 0; row != nodes; ++row)
                if (row != c)
                {
                    const double factor = a[row][c];
                    for (int col = c; col != columns; ++col) a[row][col] -= factor * a[c][col];
                }
        }
        Kernel k;
        for (int c = 0; c != stores; ++c)
            for (int v = 0; v != stores + 1; ++v)
            {
                double mid = 0;
                if (p[c] >= 0) mid += a[p[c]][nodes + v];
                else if (p[c] == -2 && v == stores) mid += 1;
                if (q[c] >= 0) mid -= a[q[c]][nodes + v];
                if (v == stores) k.input[c] = 2 * mid;
                else k.transition[c][v] = 2 * mid - (c == v ? 1 : 0);
            }
        for (int v = 0; v != stores + 1; ++v)
        {
            double direct = 0;
            for (int node : {2, 4, 5, 6}) direct += a[node][nodes + v];
            if (v == stores) { k.directInput = direct; k.boostInput = a[7][nodes + v]; }
            else { k.direct[v] = direct; k.boostNode[v] = a[7][nodes + v]; }
        }
        k.feedbackLeak = h / (2 * 100e3 * 22e-9);
        k.feedbackDrive = h / (2 * 10e3 * 22e-9);
        return k;
    }

    template <typename Clip>
    double step(double input, const Kernel& k, Clip clip) noexcept
    {
        double direct = k.directInput * input, n = k.boostInput * input;
        State next {};
        for (int i = 0; i != stores; ++i)
        {
            direct += k.direct[i] * voltage_[i]; n += k.boostNode[i] * voltage_[i];
            next[i] = k.input[i] * input;
            for (int j = 0; j != stores; ++j) next[i] += k.transition[i][j] * voltage_[j];
        }
        double c6 = (feedbackVoltage_ + k.feedbackDrive * n) / (1 + k.feedbackLeak);
        double out = clip(n + c6);
        if (std::abs(out - n - c6) > 1e-6 * std::max(1.0, std::abs(n + c6)))
        {
            // Include the feedback capacitor's changed current when IC4b
            // approaches the existing amplifier swing policy. This fixed
            // point is contractive (ratio <.23 at the minimum 8kHz rate).
            for (int i = 0; i != 12; ++i)
                c6 = (feedbackVoltage_ + k.feedbackDrive * clip(n + c6))
                   / (1 + k.feedbackLeak + k.feedbackDrive);
            out = clip(n + c6);
        }
        feedbackVoltage_ = 2 * c6 - feedbackVoltage_;
        voltage_ = next;
        return direct + (47.0 / 220.0) * out;
    }
};
} // namespace youknow
