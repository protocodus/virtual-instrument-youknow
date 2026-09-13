#!/usr/bin/env python3
"""Qualify B-2 DAC sequencing and conditional sample/hold estimates.

This is a bounded circuit/firmware audit, not an audio-engine timing profile.
It walks both NOISE-to-RES branches, checks inhibited PB/PC writes for every
initial PA value and selected channel, and integrates an ideal RC independently
of the closed-form half-LSB settling calculation. Mutated paths and source
inputs must fail. No installed acquisition, charge injection or droop is fitted.

Primary evidence:
* Roland Juno-106 Service Notes, July 31 1984, pp.3/8/13: installed HD14051BP,
  DAC ordering, 10 nF holds and actual supply/buffer wiring:
  https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf
* B-2 ROM disassembly pinned to the commit and SHA256 below. Only instruction
  metadata is restated here; --listing or --fetch checks it against that source.
* NEC uPD7810/11, pp.17/18/21/23/25/26: MOV(word),A=17 states, port ANI/ORI=20,
  LDEAX(HL)=14, ORAW=14, BIT=10, CALF=13, RET/JR/JMP=10. A skipped one-byte JR
  consumes four idle states (p.26 note 1). A state is three 12 MHz clocks.
  https://datasheet4u.com/pdf/298676/UPD7810.pdf
* Hitachi HD14051B pp.2/3: 15 V table Ron=80 typical/280 max ohm at 25 C,
  300 max at 85 C; 25 mA is an ABSOLUTE MAXIMUM, not available charge current.
  https://akizukidenshi.com/goodsaffix/hd14051b_e.pdf
* TI TL082 legacy TL08xC bias table (65 pA typical at 25 C) and TL064C bias
  table (30 pA typical): https://www.ti.com/lit/ds/symlink/tl082.pdf and
  https://www.ti.com/lit/ds/symlink/tl064.pdf . These later family data are
  scale estimates, not measurements of an installed 1984 device.

Supply qualification: IC24/23 pin16=+5 V and pin8=ground, but pin7 is NOT
ground. It is Tr23's PNP emitter: R121=39k to ground and R122=10k to -15 V
bias the base near -11.94 V before base loading, with the emitter above that
by VBE. R120=39k/C90=10nF load/bypass that rail. IC26 pin16=+15 V and pin7
is the R132=10k to -15 V / R128=1k to ground divider, nominally -1.36 V.
Thus neither the 5 V logic supply nor an exact 15 V analog span specifies the
installed Ron. The 15 V numbers below are CONDITIONAL reference coordinates.
The TL082 source and 10 nF loading are absent from the mux's 50 pF switching
test; its delays, its 30 mV typical control-feedthrough test and its 0.18 pF
typical signal feedthrough capacitance cannot pin installed charge injection.

Examples (Python standard library only):
  python3 Tools/AuditControlDacTiming.py --self-test
  python3 Tools/AuditControlDacTiming.py --listing /path/to/ic29.txt
  python3 Tools/AuditControlDacTiming.py --fetch --json /path/to/results.json

--self-test checks the independent fixtures and negative controls offline;
it explicitly does not claim a freshly verified external listing. --listing
and --fetch additionally require the exact pinned SHA and matching metadata.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import hashlib
import json
import math
from pathlib import Path
import re
import urllib.error
import urllib.request


FIRMWARE_COMMIT = "26926a04ff1939106820313e71e34b4ca2f67070"
FIRMWARE_URL = (
    "https://raw.githubusercontent.com/ErroneousBosh/j106roms/"
    + FIRMWARE_COMMIT + "/ic29.txt"
)
FIRMWARE_SHA256 = "69c7a92de514b8a2021d80cff7314476e1096918c755bb8f22e9aaf3dd419122"
STATE_SECONDS = 3.0 / 12_000_000.0


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


@dataclass(frozen=True)
class Instruction:
    address: int
    size: int
    mnemonic: str
    operand: str
    states: int


# These two small paths are independent of the C++ engine and DCO timing law.
# Counts are instruction START intervals. Exact PA latch timing is unpublished.
PROGRAM = (
    Instruction(0x07B2, 3, "ANI", "PA,$BF", 20),
    Instruction(0x07B5, 3, "JMP", "$02EC", 10),
    Instruction(0x02EC, 2, "LDAW", "$FF46_ic40Latch", 10),
    Instruction(0x02EE, 4, "MOV", "($3000),A", 17),
    Instruction(0x02F2, 2, "BIT", "0,$FF1E_flags1", 10),
    Instruction(0x02F4, 1, "JR", "$02FD", 10),
    Instruction(0x02F5, 2, "LDAW", "$FF11_voiceRun", 10),
    Instruction(0x02F7, 3, "ORAW", "$FF10_noteGate", 14),
    Instruction(0x02FA, 2, "STAW", "$FF11_voiceRun", 10),
    Instruction(0x02FC, 1, "JR", "$0301", 10),
    Instruction(0x02FD, 2, "LDAW", "$FF10_noteGate", 10),
    Instruction(0x02FF, 2, "STAW", "$FF11_voiceRun", 10),
    Instruction(0x0301, 3, "LXI", "HL,$FF3F_vcfReso", 10),
    Instruction(0x0304, 2, "LDEAX", "(HL)", 14),
    Instruction(0x0306, 2, "MVI", "A,$06", 7),
    Instruction(0x0308, 2, "CALF", "$082F_loadDac", 13),
    Instruction(0x082F, 3, "ORI", "PA,$F0", 20),
    Instruction(0x0832, 2, "ORI", "A,$F0", 7),
    Instruction(0x0834, 2, "MOV", "PA,A", 10),
    Instruction(0x0836, 1, "MOV", "A,EAH", 4),
    Instruction(0x0837, 2, "MOV", "PB,A", 10),
    Instruction(0x0839, 1, "MOV", "A,EAL", 4),
    Instruction(0x083A, 2, "MOV", "PC,A", 10),
    Instruction(0x083C, 1, "RET", "", 10),
)


def check_listing(data: bytes) -> None:
    require(hashlib.sha256(data).hexdigest() == FIRMWARE_SHA256,
            "listing differs from the pinned B-2 source")
    parsed = {}
    for line in data.decode("utf-8").splitlines():
        match = re.match(r"^([0-9a-f]{4}):\s+((?:[0-9a-f]{2}\s+)+)([A-Z]+)\s*(.*)", line)
        if match:
            address, machine_bytes, mnemonic, operand = match.groups()
            parsed[int(address, 16)] = (
                len(machine_bytes.split()), mnemonic,
                operand.split("//", 1)[0].strip(),
            )
    for instruction in PROGRAM:
        expected = (instruction.size, instruction.mnemonic, instruction.operand)
        require(parsed.get(instruction.address) == expected,
                f"instruction mismatch at {instruction.address:04x}")


def walk_noise_to_res(sustain: bool, program=PROGRAM) -> tuple[int, list[dict]]:
    by_address = {instruction.address: instruction for instruction in program}
    pc, total, rows = 0x07B2, 0, []
    for _ in range(40):
        if pc == 0x082F:
            return total, rows
        require(pc in by_address, f"path escaped fixture at {pc:04x}")
        instruction = by_address[pc]
        rows.append({"address": f"{pc:04x}", "start_states": total,
                     "states": instruction.states, "executed": True})
        total += instruction.states
        pc += instruction.size
        if instruction.mnemonic in ("JMP", "JR", "CALF"):
            pc = int(instruction.operand[1:5], 16)
        elif instruction.mnemonic == "BIT" and sustain:
            skipped = by_address[pc]
            require(skipped.mnemonic == "JR" and skipped.size == 1,
                    "skip no longer qualifies for NEC's one-byte/four-state rule")
            rows.append({"address": f"{pc:04x}", "start_states": total,
                         "states": 4, "executed": False})
            total += 4
            pc += skipped.size
    raise AssertionError("instruction trace did not terminate")


def check_dac_inhibition(program=PROGRAM) -> dict:
    routine = [instruction for instruction in program if instruction.address >= 0x082F]
    starts, total = {}, 0
    for instruction in routine:
        starts[f"{instruction.address:04x}"] = total
        total += instruction.states
    # Exhaustive initial latch / selected channel check, including initially
    # active muxes. PA4/5/6 are IC24/23/26 inhibits; PA7 is not needed here.
    for initial_pa in range(256):
        for channel in range(8):
            pa, a, written = initial_pa, channel, []
            for instruction in routine:
                operation = (instruction.mnemonic, instruction.operand)
                if operation == ("ORI", "PA,$F0"):
                    pa |= 0xF0
                elif operation == ("ORI", "A,$F0"):
                    a |= 0xF0
                elif operation == ("MOV", "PA,A"):
                    require(pa & 0x70 == 0x70, "channel changes before inhibition")
                    pa = a
                elif operation in (("MOV", "PB,A"), ("MOV", "PC,A")):
                    require(pa & 0x70 == 0x70, "DAC byte written to an enabled mux")
                    written.append(instruction.operand[:2])
            require(written == ["PB", "PC"], "DAC byte order changed")
            require(pa & 7 == channel, "mux channel was not preserved")
    require(total == 75 and starts["0837"] == 41 and starts["083a"] == 55,
            "loadDac instruction-start timing changed")
    return {"routine_states": total, "instruction_starts": starts,
            "pb_to_pc_start_us": (starts["083a"] - starts["0837"]) * STATE_SECONDS * 1e6,
            "pc_to_caller_enable_start_us": (total - starts["083a"]) * STATE_SECONDS * 1e6,
            "initial_pa_channel_cases": 256 * 8}


def rc_residual_rk4(resistance: float, capacitance: float, seconds: float) -> float:
    """Numerically integrate C*dV/dt=(1-V)/R, tracking residual voltage.

    This reference never evaluates exp() or the production engine; dt <= tau/100
    resolves the corner and keeps the integration error below 1e-9 relatively.
    """
    tau = resistance * capacitance
    steps = max(1, math.ceil(seconds / (tau / 100.0)))
    dt, residual = seconds / steps, 1.0
    for _ in range(steps):
        k1 = -residual / tau
        k2 = -(residual + dt * k1 / 2.0) / tau
        k3 = -(residual + dt * k2 / 2.0) / tau
        k4 = -(residual + dt * k3) / tau
        residual += dt * (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0
    return residual


def conditional_circuit_estimates() -> dict:
    capacitance, bits, full_scale, refresh = 10e-9, 12, 10.0, 4.2e-3
    half_lsb_fraction = 0.5 / (2 ** bits)
    settling = []
    for resistance, label in ((80.0, "15V_25C_typical"),
                              (280.0, "15V_25C_maximum"),
                              (300.0, "15V_85C_maximum")):
        tau = resistance * capacitance
        seconds = -math.log(half_lsb_fraction) * tau
        numeric = rc_residual_rk4(resistance, capacitance, seconds)
        require(abs(numeric / half_lsb_fraction - 1.0) < 1e-9,
                "RC numerical solution disagrees with half-LSB threshold")
        require(rc_residual_rk4(resistance, capacitance, seconds * .999) > half_lsb_fraction
                > rc_residual_rk4(resistance, capacitance, seconds * 1.001),
                "half-LSB settling does not bracket the numerical crossing")
        settling.append({"coordinate": label, "resistance_ohms": resistance,
                         "tau_us": tau * 1e6, "half_lsb_settling_us": seconds * 1e6,
                         "numerical_relative_error": numeric / half_lsb_fraction - 1.0})
    residual_10us = rc_residual_rk4(280.0, capacitance, 10e-6)
    require(residual_10us * 2 ** bits > 100,
            "negative control: the invalid 10 us half-LSB claim was accepted")
    # A stress ceiling bounds charging time from BELOW if it is respected;
    # it cannot establish an upper time bound or available source current.
    minimum_time_at_absolute_max = capacitance * full_scale / .025
    leakage_typical = 10e-12
    lsb_volts = full_scale / (2 ** bits)
    droop = {family: (leakage_typical + bias) * refresh / capacitance * 1e6
             for family, bias in (("TL08xC_DCO", 65e-12), ("TL064C_NOISE", 30e-12))}
    # Solve the two supply dividers by conductance, then independently check
    # Kirchhoff currents; do not invent a Tr23 VBE or installed base current.
    tr23_base = (-15.0 / 10_000.0) / (1.0 / 39_000.0 + 1.0 / 10_000.0)
    ic26_vee = (-15.0 / 10_000.0) / (1.0 / 1_000.0 + 1.0 / 10_000.0)
    require(abs(tr23_base / 39_000 + (tr23_base + 15) / 10_000) < 1e-15,
            "Tr23 unloaded base-divider KCL failed")
    require(abs(ic26_vee / 1_000 + (ic26_vee + 15) / 10_000) < 1e-15,
            "IC26 VEE-divider KCL failed")
    return {"scope": "conditional ideal RC and typical-current scale estimates; not installed bounds",
            "settling": settling, "280ohm_residual_after_10us_lsb": residual_10us * 2 ** bits,
            "absolute_max_current_minimum_possible_time_us": minimum_time_at_absolute_max * 1e6,
            "typical_same_sign_droop_uv_per_4p2ms": droop, "lsb_mv_for_10V": lsb_volts * 1e3,
            "kt_over_c_rms_uv_at_298p15K": math.sqrt(1.380649e-23 * 298.15 / capacitance) * 1e6,
            "tr23_unloaded_base_volts": tr23_base,
            "ic24_ic23_vee": "Tr23 base plus PNP VBE; base-current loading unmeasured",
            "ic26_unloaded_vee_volts": ic26_vee,
            "ic26_unloaded_analog_span_volts": 15.0 - ic26_vee}


def rejected(action) -> bool:
    try:
        action()
    except AssertionError:
        return True
    return False


def audit(data: bytes | None) -> dict:
    if data is not None:
        check_listing(data)
        require(rejected(lambda: check_listing(data + b"\n")), "corrupt listing was accepted")
    noise = {}
    for sustain, expected in ((False, 141), (True, 159)):
        states, rows = walk_noise_to_res(sustain)
        require(states == expected, "NOISE-to-RES state count changed")
        noise["sustain_on" if sustain else "sustain_off"] = {
            "states": states, "instruction_start_interval_us": states * STATE_SECONDS * 1e6,
            "trace": rows,
        }
    # Negative controls: an invented jump width must fail the skipped-byte
    # rule; omission of either inhibit action must expose a live mux byte write.
    bad_skip = tuple(replace(i, size=2) if i.address == 0x02F4 else i for i in PROGRAM)
    require(rejected(lambda: walk_noise_to_res(True, bad_skip)), "bad skip-width fixture passed")
    for omitted in (0x082F, 0x0832):
        bad_inhibit = tuple(replace(i, mnemonic="NOP") if i.address == omitted else i
                            for i in PROGRAM)
        require(rejected(lambda: check_dac_inhibition(bad_inhibit)),
                f"missing inhibit action at {omitted:04x} passed")
    return {"status": "PASS", "firmware_commit": FIRMWARE_COMMIT,
            "firmware_sha256": FIRMWARE_SHA256,
            "external_listing_verified": data is not None,
            "scope": "no interrupts; instruction starts, not electrical pin edges; no DSP changes",
            "noise_to_res": noise, "load_dac": check_dac_inhibition(),
            "conditional_circuit": conditional_circuit_estimates(),
            "negative_controls_passed": ["skip width", "uninhibited DAC write", "10 us settling"]
                + (["corrupt source hash"] if data is not None else [])}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--self-test", action="store_true", help="offline fixture/numerical qualification")
    source.add_argument("--listing", type=Path, help="exact pinned ic29.txt source")
    source.add_argument("--fetch", action="store_true", help="download and hash-check the pinned listing")
    parser.add_argument("--json", type=Path, help="write complete traces and numerical results")
    args = parser.parse_args()
    data = args.listing.read_bytes() if args.listing else None
    if args.fetch:
        try:
            with urllib.request.urlopen(FIRMWARE_URL, timeout=30) as response:
                data = response.read()
        except urllib.error.URLError as error:
            parser.error(f"source fetch failed: {error}; use --listing with a trusted download of {FIRMWARE_URL}")
    result = audit(data)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Control DAC audit: PASS; external pinned listing:", "verified" if data is not None else "not requested")
    print("NOISE enable -> RES inhibit instruction starts: 141/159 states = 35.25/39.75 us (sustain off/on)")
    print("PB -> PC -> caller enable instruction starts: 3.5 us then 5 us; all 2048 latch/channel cases inhibited")
    for row in result["conditional_circuit"]["settling"]:
        print(f"Conditional {row['coordinate']}: tau={row['tau_us']:.3f} us; half-LSB settling={row['half_lsb_settling_us']:.6f} us")
    print("Installed acquisition, injection, droop and pin-edge timing remain unqualified; no audible behavior changes.")


if __name__ == "__main__":
    main()
