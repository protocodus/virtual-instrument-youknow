#!/usr/bin/env python3
"""Qualify nominal B-2 main-pass, ADC and optional receiver descriptors.

No ROM is shipped or downloaded. Pass the published, hash-pinned ic29 listing.
Optional --rom checks every listed instruction against an independently
archived B-2 dump. This catches transcription differences without copying
the binary into the instrument. The listing's inverted-mask table label is
wrong: the raw dump has EI/RETI at 0008/0009 and the six masks at 000A..000F.
Optional --opcode-reference cross-checks execution/skip timing against the
specified MAME NMOS table (facts only; its source is not part of the engine).
The unconditional graph lower bound deliberately permits infeasible branches:
that makes it conservative for the single physical-enable-per-interval bound.
"""
import argparse
import hashlib
import heapq
import json
import re
from pathlib import Path

LISTING_HASH = "69c7a92de514b8a2021d80cff7314476e1096918c755bb8f22e9aaf3dd419122"
ROM_HASH = "f3c48e14434e29e264407e9163f30c0473f94c20957799ecf75746099d1bd2a2"
OPCODE_HASH = "42af19b05d7ae51572a24b6358d41fcc99cb59ce181ae1296c3bfdd14c2dd2e1"
LISTING_URL = "https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt"
NEC_URL = "https://datasheet4u.com/pdf/298676/UPD7810.pdf#page=17"
NEC_MANUAL_URL = "https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view"
INSTRUCTION_LINE = r"([0-9a-fA-F]{4})(?:_[^\s:]+)?:\s+((?:[0-9a-f]{2}\s+)+)([A-Z]+)"
# Semantic handler entry addresses, not a distributed firmware image. The
# graph allows every dispatch, independently of RAM/data predicates.
PARAMETER_TARGETS = (0x014f, 0x0184, 0x0198, 0x0200, 0x023a, 0x01ab, 0x01c8,
                     0x01cc, 0x01d0, 0x01af, 0x01b3, 0x01b7, 0x01d4, 0x01e2,
                     0x01ed, 0x01c4, 0x01e9, 0x01c0, 0x0223, 0x0227, 0x01a7, 0x0246)


def checked(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"Unqualified reference hash: {path}")
    return data.decode()


def descriptors(path, serial=False):
    rows = {}
    pattern = (r"\{\s*0x([0-9a-f]+),\s*Op::(\w+),\s*0x([0-9a-f]+),\s*"
               r"0x([0-9a-f]+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}")
    if serial:
        pattern += r",\s*Extra::(\w+)"
    for fields in re.findall(pattern, path.read_text()):
        address, op, argument, second, size, states, skipped = fields[:7]
        if serial and fields[7] != "Normal":
            assert op == "NOP", "Extended serial operations must replace the NOP placeholder"
            op = fields[7]
        assert int(address, 16) not in rows, f"Duplicate descriptor at {address}"
        rows[int(address, 16)] = dict(op=op, argument=int(argument, 16), second=int(second, 16),
                                      size=int(size), states=int(states), skipped=int(skipped))
    return rows


def check_rom(path, source):
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == ROM_HASH, "Unqualified B-2 ROM hash"
    # MAME's juno106 driver independently identifies this 8192-byte dump as
    # voice B-2, CRC e0dc721e / SHA1 below. No runtime dependency.
    # https://github.com/mamedev/mame/blob/master/src/mame/roland/juno106.cpp
    assert len(data) == 8192
    assert hashlib.sha1(data).hexdigest() == "892b919a2476b269d916ba01dd5a81a25e044171"
    count = 0
    for line in source.splitlines():
        match = re.match(INSTRUCTION_LINE, line)
        if match:
            address = int(match[1], 16)
            instruction = bytes.fromhex(match[2])
            assert data[address:address + len(instruction)] == instruction, f"ROM bytes at {address:04X}"
            count += 1
    assert data[8:10] == bytes((0xaa, 0x62)), "External INT0 vector"
    for card in range(6):
        assert data[0x0a + card] == (0xff ^ (1 << card)), "Voice-off inverted mask"
        assert data[0x12 + card] == 1 << card, "Voice-on mask"
    for i, target in enumerate(PARAMETER_TARGETS):
        assert int.from_bytes(data[0xda + 2*i:0xdc + 2*i], 'little') == target
    return count


def listing_rows(source, serial=False, parameters=False):
    rows = {}
    for line in source.splitlines():
        match = re.match(INSTRUCTION_LINE, line)
        if match:
            address = int(match[1], 16)
            in_main = (address == 0x0020 or 0x0070 <= address <= 0x008d
                       or 0x02ec <= address <= 0x07b5 or 0x0800 <= address <= 0x0850)
            # The old descriptor set stops at TABLE00D7. The normal-parameter
            # set includes unarmed A3, stopping at024B before diagnostic writes
            # when armed; boot/test-mode code remains outside this contract.
            in_serial = (address in (0x0028, 0x02eb) or 0x008e <= address <= 0x00d5
                         or 0x0109 <= address <= 0x014c)
            in_parameters = (0x00d7 <= address <= 0x00d9 or 0x0106 <= address <= 0x0108
                             or 0x014f <= address <= 0x0248)
            if in_main or (serial and in_serial) or (parameters and in_parameters):
                rows[address] = ([int(byte, 16) for byte in match[2].split()], match[3])
    return rows


def can_skip(op):
    return (op.startswith(("BIT_", "ON", "OFF", "EQ", "NEI", "NEA", "DNE", "GT", "DGT", "LT", "DLT",
                           "DCR", "INRW", "SKIT", "ADDNC", "ADINC", "DADDNC", "SUBNB", "DSUBNB", "SUINB")))


def shortest_enable_gap(rows, entry=None):
    enables = [address for address, row in rows.items() if row["op"] == "ANI_PA_xx"]
    minimum = 10**9
    for start in enables if entry is None else [entry]:
        first = rows[start]
        queue = ([(first["states"], start + first["size"], ())] if entry is None
                 else [(0, start, ())])
        seen = set()
        while queue:
            elapsed, address, stack = heapq.heappop(queue)
            if (address, stack) in seen:
                continue
            seen.add((address, stack))
            # Missing instructions (notably parameter dispatch at 00D7) stop
            # this bounded receiver. They cannot enter the audio replay.
            if address not in rows:
                continue
            row = rows[address]
            if address in enables:
                minimum = min(minimum, elapsed)
                break
            following = address + row["size"]
            cost = elapsed + row["states"]
            op = row["op"]
            if op in {"JR", "JRE", "JMP_w"}:
                heapq.heappush(queue, (cost, row["argument"], stack))
            elif op == "CALF":
                heapq.heappush(queue, (cost, row["argument"], stack + (following,)))
            elif op == "JB":
                assert address == 0x00d9
                for target in PARAMETER_TARGETS:
                    heapq.heappush(queue, (cost, target, stack))
            elif op == "RET":
                if stack:
                    heapq.heappush(queue, (cost, stack[-1], stack[:-1]))
            elif op == "RETI":
                # A returning ISR only adds time to the interrupted path.
                continue
            else:
                if op == "LXI_S_w":
                    assert row["argument"] == 0xffff
                    stack = ()  # Note handlers abandon the interrupted calls.
                heapq.heappush(queue, (cost, following, stack))
                if can_skip(op):
                    skipped = rows[following]
                    heapq.heappush(queue, (cost + skipped["skipped"], following + skipped["size"], stack))
    return minimum


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("listing", type=Path)
    parser.add_argument("--opcode-reference", type=Path)
    parser.add_argument("--rom", type=Path,
                        help="Optional locally supplied, hash-qualified original B-2 binary")
    parser.add_argument("--program", type=Path,
                        help="Descriptor file to qualify; defaults to the maintained program")
    parser.add_argument("--serial-program", type=Path,
                        help="Also qualify the bounded note/sustain receiver descriptors")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = checked(args.listing, LISTING_HASH)
    rom_instructions = check_rom(args.rom, source) if args.rom else 0
    actual = descriptors(args.program or root / "Source/DSP/YouKnowFirmwareProgram.h")
    serial_instructions = 0
    if args.serial_program:
        serial_rows = descriptors(args.serial_program, serial=True)
        assert not actual.keys() & serial_rows.keys(), "Serial descriptors overlap the main/ADC descriptors"
        serial_instructions = len(serial_rows)
        assert serial_instructions in (81, 208), "Incomplete bounded serial descriptor set"
        actual.update(serial_rows)
    parameters = serial_instructions == 208
    expected = listing_rows(source, serial=bool(args.serial_program), parameters=parameters)
    assert actual.keys() == expected.keys(), "Instruction addresses differ from the selected bounded paths"
    opcode_tables = {}
    if args.opcode_reference:
        reference = re.sub(r"/\*.*?\*/|//[^\n]*", "", checked(args.opcode_reference, OPCODE_HASH), flags=re.S)
        for name in ("XX_7810", "48", "4C", "4D", "60", "64", "70", "74"):
            body = re.search("s_op" + name + r"\[256\] =\s*\{(.*?)\n\};", reference, re.S)[1]
            values = re.findall(r"::(\w+),\s*(\d+),\s*(\d+),\s*(\d+),", body)
            assert len(values) == 256
            opcode_tables[name] = values
    for address, (data, mnemonic) in expected.items():
        row = actual[address]
        assert row["size"] == len(data), f"Instruction length at {address:04X}"
        assert row["op"].split("_")[0] == mnemonic, f"Semantic operation at {address:04X}"
        op = row["op"]
        argument = second = 0
        if op == "MOV_A_CR":
            assert data[0] == 0x4c and 0xe0 <= data[1] <= 0xe3
            argument = data[1] - 0xe0
        elif op == "JR":
            displacement = data[0] & 63
            argument = address + 1 + (displacement - 64 if displacement & 32 else displacement)
        elif op == "JRE":
            argument = address + 2 + data[1] - (256 if data[0] == 0x4f else 0)
        elif op == "CALF":
            argument = ((data[0] & 7) + 8) * 256 + data[1]
        elif op.endswith("_wa_xx"):
            argument, second = data[-2:]
        elif op.endswith(("_w", "_s")) or "_w_" in op:
            argument = data[-2] + 256 * data[-1]
        elif op.endswith(("_wa", "_xx")):
            argument = data[-1]
        assert (argument, second) == (row["argument"], row["second"]), f"Raw-byte operands at {address:04X}"
        if opcode_tables:
            prefix = f"{data[0]:02X}" if data[0] in (0x48, 0x4c, 0x4d, 0x60, 0x64, 0x70, 0x74) else "XX_7810"
            identity = opcode_tables[prefix][data[0] if prefix == "XX_7810" else data[1]]
            table_op = f"MOV_A_CR{argument}" if op == "MOV_A_CR" else op
            assert identity == (table_op, str(row["size"]), str(row["states"]), str(row["skipped"])), f"NMOS timing identity at {address:04X}"
    # Separate primary-source ledger: original NMOS shortsheet pp.4-91..4-100.
    # This is the real vector plus ISR, not a synthetic per-pass delay. The
    # original-family manual (stock500375, pp.9-7..9-8) adds 16 entry states.
    adc_timing = {
        0x20: 10, 0x70: 4, 0x71: 4, 0x72: 10, 0x74: 20, 0x77: 10,
        0x7a: 7, 0x7b: 10, 0x7d: 7, 0x7e: 10, 0x80: 7, 0x81: 10,
        0x83: 7, 0x84: 10, 0x86: 7, 0x87: 14, 0x8a: 4, 0x8b: 4,
        0x8c: 4, 0x8d: 13,
    }
    for address, states in adc_timing.items():
        assert actual[address]["states"] == states, f"Primary ADC timing at {address:04X}"
    assert sum(adc_timing.values()) == 172
    if parameters:
        # Original-family manual printed12-30/12-67: JB loads PC from BC;
        # TABLE reads C/B from opcodePC+3+A/+4+A. Both preserve CY.
        assert actual[0x00d7]["states"] == 17 and actual[0x00d7]["skipped"] == 8
        assert actual[0x00d9]["states"] == 4 and actual[0x00d9]["skipped"] == 4
    minimum = shortest_enable_gap(actual)
    restart_minimum = None
    if serial_instructions:
        # A destructive note restart is the only supported interrupt path
        # which can shorten the remaining scan. The old enable instruction
        # must finish (20 states), followed by automatic entry (16), the
        # receiver, and the restarted main pass's first enable. Permitting
        # every skip outcome independently makes this a conservative bound.
        enable_costs = {row["states"] for row in actual.values() if row["op"] == "ANI_PA_xx"}
        assert enable_costs == {20}
        restart_minimum = 20 + 16 + shortest_enable_gap(actual, entry=0x0028)
        minimum = min(minimum, restart_minimum)
    assert minimum > 125, "32kHz at 4M CPU states/s needs converter spacing above 125 states"
    # MVI/LXI register overlay behavior is absent from the reachable paths;
    # the interpreter does not purport to cover the whole processor ISA.
    print(json.dumps(dict(instructions=len(actual), listing_sha256=LISTING_HASH,
        listing_url=LISTING_URL, primary_nmos_timing=NEC_URL,
        primary_nmos_entry=NEC_MANUAL_URL, adc_handler_states=172,
        adc_automatic_entry_states=16, adc_occupied_states=188,
        opcode_timing_crosscheck=bool(opcode_tables),
        rom_sha256=ROM_HASH if args.rom else None,
        rom_instruction_byte_crosschecks=rom_instructions,
        serial_instructions=serial_instructions,
        normal_parameter_instructions=127 if parameters else 0,
        conservative_restart_enable_gap_states=restart_minimum,
        conservative_enable_gap_states=minimum, nominal_state_hz=4000000,
        internal_rate_floor_hz=32000, passed=True), indent=2))


if __name__ == "__main__":
    main()
