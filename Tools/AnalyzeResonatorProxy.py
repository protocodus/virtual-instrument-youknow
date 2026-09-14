#!/usr/bin/env python3
"""Calculate passive CSA8.00MT resonance versus explicit external loading.

This offline proxy uses Murata Erie P-04-A, PDF p.12, typical equivalent-circuit
table: R1=4.888 ohm, L1=0.07176 mH, C1=5.97208 pF, C0=39.8679 pF.
https://dn710301.ca.archive.org/0/items/Murata-ErieCeramicResonatorsForTimingControlOCR/Murata-ErieCeramicResonatorsForTimingControlOCR.pdf

Its p.15 CSA-MT standard test uses CD4069UBE at 12 V, CL1=CL2=30 pF and
Rf=1 Mohm. These are NOT established Juno C109/IC38 conditions. CSA8.00MT
is a named substitute; no KMFC1034T1 equivalence is claimed. The separately
printed Fr/Fa/Qm do not exactly reproduce from this table's RLC values, so
they are retained separately, without fitting or normalizing to 8 MHz.

Model: a series R1-L1-C1 motional branch, shunted by C0 and the differential
load CL=CL1*CL2/(CL1+CL2)+Cstray. CL1/CL2 are ideal capacitors to a common
ground; Cstray is an explicit differential capacitance across the resonator,
not an automatically inferred per-pin capacitance. This reduction omits the
inverter's gain, phase, output impedance and drive. The upper Im(Y)=0 root
is a passive parallel phase resonance, NOT a complete oscillator prediction
or an impedance-magnitude peak. No temperature curve, warm-up time constant,
jitter spectrum, population distribution or aging trajectory follows from
these constants. This tool neither fits nor modifies the synthesizer.

Examples (Python standard library only):
  python3 Tools/AnalyzeResonatorProxy.py --self-test
  python3 Tools/AnalyzeResonatorProxy.py --caps-pf 30 30 --json out/proxy.json
  python3 Tools/AnalyzeResonatorProxy.py --caps-pf 30 30 --caps-pf 31 30

The first pair above reproduces only the catalogue's capacitor values; the
second pair is an arbitrary loading experiment, not a measured thermal step.
Repeated pairs are compared to the first. No capacitor values are defaulted.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import unittest


SOURCE_URL = (
    "https://dn710301.ca.archive.org/0/items/"
    "Murata-ErieCeramicResonatorsForTimingControlOCR/"
    "Murata-ErieCeramicResonatorsForTimingControlOCR.pdf"
)
R1_OHM = 4.888
L1_H = 0.07176e-3
C1_F = 5.97208e-12
C0_F = 39.8679e-12


def finite_value(value: float, name: str, *, positive: bool = False) -> float:
    if not math.isfinite(value) or value < 0.0 or (positive and value == 0.0):
        bound = "positive" if positive else "nonnegative"
        raise ValueError(f"{name} must be finite and {bound}")
    return value


def effective_load_pf(cl1: float, cl2: float, stray: float = 0.0) -> float:
    finite_value(cl1, "CL1", positive=True)
    finite_value(cl2, "CL2", positive=True)
    finite_value(stray, "differential stray capacitance")
    small, large = sorted((cl1, cl2))
    # Equivalent to CL1*CL2/(CL1+CL2), without an overflowing product.
    return finite_value(small / (1.0 + small / large) + stray,
                        "effective load", positive=True)


def parallel_resonance_hz(load_pf: float, resistance_ohm: float = R1_OHM) -> float:
    finite_value(load_pf, "effective load")
    finite_value(resistance_ohm, "motional resistance")
    cp = C0_F + load_pf * 1e-12
    if load_pf > 0.0 and load_pf * 1e-12 == 0.0:
        raise ValueError("effective load is too small to represent in farads")
    # Y = j*w*Cp + 1 / (R1 + j*(w*L1 - 1/(w*C1))). Set Im(Y)=0.
    # With u=w*w*L1*C1, k=C1/Cp and d=R1*R1*C1/L1:
    #     u*u - (2+k-d)*u + (1+k) = 0.
    # Use the upper root. The lower root becomes the singular series short
    # when R1=0, and is not the parallel resonance. This form of the
    # discriminant avoids subtracting nearly equal quantities near four.
    k = C1_F / cp
    d = resistance_ohm * resistance_ohm * C1_F / L1_H
    discriminant = k * k - 2.0 * d * (2.0 + k) + d * d
    if not math.isfinite(discriminant) or discriminant <= 0.0:
        raise ValueError("this load/loss has no distinct parallel phase resonance")
    u = (2.0 + k - d + math.sqrt(discriminant)) / 2.0
    if u <= 1.0:
        raise ValueError("this load/loss has no parallel phase resonance above series")
    return math.sqrt(u / (L1_H * C1_F)) / math.tau


def analyze(capacitor_pairs: list[list[float]], stray_pf: float = 0.0) -> dict:
    if not capacitor_pairs:
        raise ValueError("specify at least one CL1/CL2 capacitor pair")
    rows = []
    for cl1, cl2 in capacitor_pairs:
        load = effective_load_pf(cl1, cl2, stray_pf)
        frequency = parallel_resonance_hz(load)
        reference = rows[0]["parallel_phase_resonance_hz"] if rows else frequency
        fractional_change = (frequency - reference) / reference
        rows.append({
            "cl1_pf": cl1,
            "cl2_pf": cl2,
            "differential_stray_pf": stray_pf,
            "effective_load_pf": load,
            "parallel_phase_resonance_hz": frequency,
            "change_from_first_ppm": fractional_change * 1e6,
            "change_from_first_cents": 1200.0 * math.log1p(fractional_change) / math.log(2.0),
        })
    return {
        "schema_version": 1,
        "component_proxy": "Murata CSA8.00MT",
        "source": {"url": SOURCE_URL, "catalogue": "Murata Erie P-04-A", "pdf_page": 12},
        "typical_circuit": {"r1_ohm": R1_OHM, "l1_h": L1_H, "c1_f": C1_F, "c0_f": C0_F},
        "separately_printed_catalogue_values": {
            "fr_hz": 7_688_626.0, "fa_hz": 8_244_443.0, "qm": 731.518,
            "note": "Not used to fit the RLC model; the tabulated values do not reproduce exactly.",
        },
        "derived_unloaded_circuit": {
            "series_branch_resonance_hz": 1.0 / (math.tau * math.sqrt(L1_H * C1_F)),
            "parallel_phase_resonance_hz": parallel_resonance_hz(0.0),
            "motional_q": math.sqrt(L1_H / C1_F) / R1_OHM,
        },
        "interpretation": {
            "load_model": "Ideal CL1 and CL2 to common ground; differential stray added across resonator.",
            "comparison_reference": "First input capacitor pair; all other circuit values remain fixed.",
            "scope": "Passive parallel phase resonance, not installed oscillator frequency or Juno calibration.",
            "missing_for_oscillator": "Inverter gain/phase, impedance, drive and actual installed loading.",
            "thermal_and_noise_parameters": "Unset: these source constants do not determine them.",
        },
        "scenarios": rows,
    }


class ProxyTests(unittest.TestCase):
    def test_independent_admittance_roots(self):
        # Bisect the complex network's susceptance directly, with a bracket
        # chosen independently of the quadratic. All these roots lie in it.
        for load_pf in (0.0, 1.0, 15.0, 16.0, 32.0, 100.0):
            def susceptance(frequency):
                w = math.tau * frequency
                return (1j * w * (C0_F + load_pf * 1e-12)
                        + 1.0 / (R1_OHM + 1j * (w * L1_H - 1.0 / (w * C1_F)))).imag

            lo, hi = 7.70e6, 8.50e6
            self.assertLess(susceptance(lo), 0.0)
            self.assertGreater(susceptance(hi), 0.0)
            for _ in range(60):
                mid = (lo + hi) / 2.0
                if susceptance(mid) < 0.0:
                    lo = mid
                else:
                    hi = mid
            self.assertAlmostEqual(parallel_resonance_hz(load_pf), (lo + hi) / 2.0, delta=1e-6)

    def test_lossless_limit(self):
        for load_pf in (0.0, 15.0, 100.0):
            cp = C0_F + load_pf * 1e-12
            # At zero resistance, L1 resonates against the series combination
            # of C1 and Cp. This independently supplies the parallel frequency.
            series_capacitance = 1.0 / (1.0 / C1_F + 1.0 / cp)
            expected = 1.0 / (math.tau * math.sqrt(L1_H * series_capacitance))
            self.assertAlmostEqual(parallel_resonance_hz(load_pf, 0.0), expected, delta=1e-6)
            self.assertLess(parallel_resonance_hz(load_pf), expected)

    def test_capacitor_reduction(self):
        self.assertEqual(effective_load_pf(30.0, 30.0), 15.0)
        self.assertEqual(effective_load_pf(20.0, 60.0, 2.0), 17.0)
        self.assertEqual(effective_load_pf(60.0, 20.0, 2.0), 17.0)

    def test_scenarios_and_units(self):
        result = analyze([[30.0, 30.0], [31.0, 30.0], [30.0, 30.0]])
        first, increased, repeated = result["scenarios"]
        self.assertEqual(first, repeated)
        self.assertEqual(first["change_from_first_ppm"], 0.0)
        self.assertLess(increased["parallel_phase_resonance_hz"], first["parallel_phase_resonance_hz"])
        self.assertLess(increased["change_from_first_cents"], 0.0)
        ratio = increased["parallel_phase_resonance_hz"] / first["parallel_phase_resonance_hz"]
        self.assertAlmostEqual(2.0 ** (increased["change_from_first_cents"] / 1200.0), ratio)
        self.assertAlmostEqual(1.0 + increased["change_from_first_ppm"] / 1e6, ratio)
        self.assertEqual(json.loads(json.dumps(result, allow_nan=False)), result)

    def test_invalid_inputs(self):
        for value in (-1.0, math.nan, math.inf, -math.inf):
            for args in ((value, 30.0, 0.0), (30.0, value, 0.0), (30.0, 30.0, value)):
                with self.assertRaises(ValueError):
                    effective_load_pf(*args)
            with self.assertRaises(ValueError):
                parallel_resonance_hz(value)
        with self.assertRaises(ValueError):
            effective_load_pf(0.0, 30.0)
        with self.assertRaises(ValueError):
            analyze([])
        with self.assertRaises(ValueError):
            parallel_resonance_hz(1e308)
        with self.assertRaises(ValueError):
            parallel_resonance_hz(1e-320)
        with self.assertRaises(ValueError):
            parallel_resonance_hz(15.0, 1e200)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--caps-pf", type=float, nargs=2, action="append", metavar=("CL1", "CL2"),
                      help="explicit capacitor pair; repeat to compare with the first")
    mode.add_argument("--self-test", action="store_true", help="run independent offline numerical checks")
    parser.add_argument("--stray-pf", type=float, default=0.0, help="differential stray across resonator (default: ideal 0)")
    parser.add_argument("--json", type=Path, help="write JSON here instead of stdout")
    args = parser.parse_args(argv)
    if args.self_test:
        if args.json is not None or args.stray_pf != 0.0:
            parser.error("--self-test does not accept scenario/output options")
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ProxyTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    try:
        result = json.dumps(analyze(args.caps_pf, args.stray_pf), indent=2, allow_nan=False) + "\n"
        if args.json is None:
            print(result, end="")
        else:
            args.json.write_text(result, encoding="utf-8")
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
