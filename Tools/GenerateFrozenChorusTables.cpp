#include "FrozenTableWriter.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <vector>
#if !defined(YOUKNOW_VERIFY_FROZEN_TABLES)
#undef YOUKNOW_EMBEDDED_TARGET
#endif
#define private public
#include "../Source/DSP/YouKnowChorus.cpp"
#undef private

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    using namespace youknow;
    using namespace frozen_table_writer;
    destination = argv[1];
    std::filesystem::create_directories(destination);
    const auto bbd = [](const char* name, const auto& table) {
        file(name, [&](auto& out) {
            out << "{{\n";
            for (const auto& node : table) {
                out << '{'; value(out, node.value); out << ',';
                value(out, node.slope); out << "},\n";
            }
            out << "}}";
        });
    };
    bbd("BbdTransfer", bbdTransferHermiteTable);
    bbd("BbdServicedBias", bbdServicedBiasHermiteTable);
    scalar("ChorusIdealNoiseMoment", noiseMomentsFor(ChorusSupportProfile::IdealFollowers));
    scalar("ChorusFiniteNoiseMoment", noiseMomentsFor(ChorusSupportProfile::Nominal2SA1015));
}
