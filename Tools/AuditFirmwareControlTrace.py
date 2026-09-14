#!/usr/bin/env python3
"""Qualify original nominal B-2 semantic descriptors against external evidence.

No ROM is shipped or downloaded. Pass the published, hash-pinned ic29 listing.
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
OPCODE_HASH = "42af19b05d7ae51572a24b6358d41fcc99cb59ce181ae1296c3bfdd14c2dd2e1"
LISTING_URL = "https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt"
NEC_URL = "https://datasheet4u.com/pdf/298676/UPD7810.pdf#page=17"


def checked(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"Unqualified reference hash: {path}")
    return data.decode()


def descriptors(path):
    rows = {}
    for address, op, argument, second, size, states, skipped in re.findall(
        r"\{0x([0-9a-f]+),Op::(\w+),0x([0-9a-f]+),0x([0-9a-f]+),(\d+),(\d+),(\d+)\}",
        path.read_text(),
    ):
        rows[int(address, 16)] = dict(op=op, argument=int(argument, 16), second=int(second, 16),
                                      size=int(size), states=int(states), skipped=int(skipped))
    return rows


def listing_rows(source):
    rows = {}
    for line in source.splitlines():
        match = re.match(r"([0-9a-f]{4}):\s+((?:[0-9a-f]{2}\s+)+)([A-Z]+)", line)
        if match:
            address = int(match[1], 16)
            if 0x02ec <= address <= 0x07b5 or 0x0800 <= address <= 0x0850:
                rows[address] = ([int(byte, 16) for byte in match[2].split()], match[3])
    return rows


def can_skip(op):
    return (op.startswith(("BIT_", "ON", "OFF", "EQ", "NEI", "DNE", "GT", "DGT", "LT", "DLT",
                           "DCR", "INRW", "SKIT", "ADDNC", "ADINC", "DADDNC", "SUBNB", "DSUBNB", "SUINB")))


def shortest_enable_gap(rows):
    enables = [address for address, row in rows.items() if row["op"] == "ANI_PA_xx"]
    minimum = 10**9
    for start in enables:
        first = rows[start]
        queue = [(first["states"], start + first["size"], ())]
        seen = set()
        while queue:
            elapsed, address, stack = heapq.heappop(queue)
            if (address, stack) in seen:
                continue
            seen.add((address, stack))
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
            elif op == "RET":
                if stack:
                    heapq.heappush(queue, (cost, stack[-1], stack[:-1]))
            else:
                heapq.heappush(queue, (cost, following, stack))
                if can_skip(op):
                    skipped = rows[following]
                    heapq.heappush(queue, (cost + skipped["skipped"], following + skipped["size"], stack))
    return minimum


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("listing", type=Path)
    parser.add_argument("--opcode-reference", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    expected = listing_rows(checked(args.listing, LISTING_HASH))
    actual = descriptors(root / "Source/DSP/YouKnowFirmwareProgram.h")
    assert actual.keys() == expected.keys(), "Instruction addresses differ from pinned bounded pass"
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
        if op == "JR":
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
            assert identity == (op, str(row["size"]), str(row["states"]), str(row["skipped"])), f"NMOS timing identity at {address:04X}"
    minimum = shortest_enable_gap(actual)
    assert minimum > 125, "32kHz at 4M CPU states/s needs converter spacing above 125 states"
    # MVI/LXI register overlay behavior is absent from the reachable paths;
    # the interpreter does not purport to cover the whole processor ISA.
    print(json.dumps(dict(instructions=len(actual), listing_sha256=LISTING_HASH,
        listing_url=LISTING_URL, primary_nmos_timing=NEC_URL,
        opcode_timing_crosscheck=bool(opcode_tables),
        conservative_enable_gap_states=minimum, nominal_state_hz=4000000,
        internal_rate_floor_hz=32000, passed=True), indent=2))


if __name__ == "__main__":
    main()
