#!/usr/bin/env python3
"""Compile the complete SDK-free DSP as C++17 and check its value/view adapters.

The equality fixture discovers EngineParameters members from the declaration,
so adding a control without extending the C++17 comparison fails this check.
Run with --cxx to select another standards-conforming host compiler.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / "Source"
    dsp = source / "DSP"
    header = (dsp / "YouKnowEngine.h").read_text()
    declaration = header.split("struct EngineParameters\n{", 1)[1].split(
        "// Hosts commonly present", 1)[0]
    declaration = re.sub(r"//[^\n]*", "", declaration)
    members = re.findall(r"^    (?!static\b)[\w:]+ (\w+)\s*\{", declaration, re.M)
    if not members:
        raise RuntimeError("EngineParameters declaration could not be inspected")
    checks = "\n".join(f'    checkMember(&EngineParameters::{name}, "{name}");'
                       for name in members)
    fixture = r'''
#include "DSP/YouKnowEngine.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
#if __cplusplus >= 202002L
#include <numbers>
#endif
using namespace youknow;
void require(bool condition, const char* name) {
    if (!condition) { std::cerr << "Portability failure: " << name << '\n'; std::exit(1); }
}
template <typename T>
void checkMember(T EngineParameters::* member, const char* name) {
    EngineParameters original, changed;
    if constexpr (std::is_same_v<T, bool>) changed.*member = !(original.*member);
    else if constexpr (std::is_enum_v<T>)
        changed.*member = static_cast<T>(static_cast<int>(original.*member) == 0 ? 1 : 0);
    else changed.*member = original.*member == T{} ? T{1} : T{};
    require(original != changed && changed != original, name);
    changed.*member = original.*member;
    require(original == changed && changed == original, name);
    if constexpr (std::is_floating_point_v<T>) {
        original.*member = -T{};
        changed.*member = T{};
        require(original == changed, "signed zero retains value equality");
        changed.*member = std::numeric_limits<T>::quiet_NaN();
        require(changed != changed, "NaN snapshot must reach sanitisation");
    }
}
int main() {
    Span<int> empty;
    require(empty.empty() && empty.begin() == empty.end(), "empty span");
    require(empty.subspan(0).empty() && empty.first(0).empty(), "empty subviews");
    int raw[] { 2, 3, 5 };
    std::array<int, 3> values {{ 7, 11, 13 }};
    const std::array<int, 2> immutable {{ 17, 19 }};
    std::vector<int> dynamic { 23, 29 };
    Span<int> view(values);
    Span<const int> readOnly(view);
    Span<int> rawView(raw);
    Span<int> dynamicView(dynamic);
    Span<const int> immutableView(immutable);
    static_assert(!std::is_constructible_v<Span<int>, const std::array<int, 2>&>);
    struct Base { int first; };
    struct Derived : Base { int second; };
    static_assert(!std::is_constructible_v<Span<Base>, std::array<Derived, 2>&>);
    require(readOnly.data() == values.data() && readOnly.size() == 3, "const view conversion");
    require(rawView[2] == 5 && dynamicView[1] == 29 && immutableView[1] == 19, "container views");
    view.subspan(1, 1)[0] = 31;
    require(values[1] == 31 && readOnly.first(2)[1] == 31, "subview keeps storage");
    require(view.subspan(3).empty() && view.subspan(3).data() == view.data() + view.size(), "end subview");
    require(bitCast<std::uint32_t>(numbers::pi_v<float>) == 0x40490fdbu, "float pi");
    require(bitCast<std::uint64_t>(numbers::pi) == UINT64_C(0x400921fb54442d18), "double pi");
    for (std::uint32_t bits : { 0u, 0x80000000u, 0x7f800000u, 0x7fc12345u, 0x3f800001u })
        require(bitCast<std::uint32_t>(bitCast<float>(bits)) == bits, "bit-exact cache keys");
#if __cplusplus >= 202002L
    static_assert(std::is_same_v<Span<const int>, std::span<const int>>);
    static_assert(numbers::pi == std::numbers::pi);
    static_assert(numbers::pi_v<float> == std::numbers::pi_v<float>);
#endif
    PARAMETER_CHECKS
    OutputNetwork::Configuration first, second;
    require(first == second, "output configuration equality");
    second.externalCapacitanceFarads = 1e-9;
    require(first != second, "output capacitance equality");
}
'''.replace("    PARAMETER_CHECKS", checks)
    common = [args.cxx, "-pedantic-errors", "-I", str(source)]
    subprocess.run(common + ["-std=c++17", "-fsyntax-only"]
                   + [str(path) for path in sorted(dsp.glob("*.cpp"))], check=True)
    with tempfile.TemporaryDirectory(prefix="youknow-portability-") as temporary:
        temporary = Path(temporary)
        test = temporary / "portability.cpp"
        test.write_text(fixture)
        for standard in (17, 20):
            executable = temporary / f"portability-{standard}"
            subprocess.run(common + [f"-std=c++{standard}", "-O2", str(test),
                                     "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
    print(f"C++17 DSP compilation and C++17/C++20 view, numeric, and {len(members)}-field equality checks passed")


if __name__ == "__main__":
    main()
