// Qualification and audition of a partial B-2 DCO scan reconstruction.
//
// Protocol: independently walk the recovered 0493 -> next 0493 instruction
// path, using NEC's *state* counts, for each reset/clamp/voice branch. Match
// the closed-form engine law and check prior PIT anchors as cross-checks.
// The instruction subset is from the pinned B-2 disassembly:
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L681-L794
// Counts and skipped-instruction rules: NEC uPD7810/11 pp.17-26:
// https://datasheet4u.com/pdf/298676/UPD7810.pdf#page=17
// One state is three nominal 12 MHz resonator clocks (250 ns), as in the
// existing PIT staging. Table counts identify instruction STARTS, not PA4 or
// /WR pin edges. No NMOS entry delay, arbitrary MIDI latency or random jitter.
//
// --self-test qualifies these paths, pass-boundary snapshots, profile latching
// and host/block invariance. --render DIR produces raw and whole-file-RMS
// matched A/B stabs and chords through shipping kernels at 48 kHz / 1x.
// Absolute first-DCO/non-DCO offsets, the full data-dependent main loop,
// mid-pass controller branch changes and interrupt/wire timing remain open.
// The existing original recording's steady windows cannot qualify this change.
#include "DSP/YouKnowEngine.h"
#include "RealismComparisonSupport.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace youknow
{
struct YouKnowTestAccess
{
    static auto phases(const YouKnowEngine& engine) { return engine.converterEventPhases_; }
    static void snapshot(YouKnowEngine& engine) { engine.refreshFirmwareDcoTiming(); }
    static void card(YouKnowEngine& engine, int index, bool reset, int midi)
    {
        auto& v = engine.voices_[static_cast<std::size_t>(index)];
        v.rootMidi = -1;
        v.currentMidi = v.targetMidi = static_cast<float>(midi);
        v.dcoResetPending = reset;
    }
};
}
namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
using Profile = YouKnowEngine::ConverterTimingProfile;
void require(bool pass, const std::string& reason)
{
    if (!pass) throw std::runtime_error(reason);
}
struct Instruction { int pc, bytes, states, target; bool call, ret; };
// A path fixture, not an implementation of the engine's interval formula.
// Every executed instruction remains individually inspectable against ROM and
// NEC, including the skipped JRE's seven idle states (not its ten run states).
constexpr Instruction program[] {
    { 0x041c, 2, 14, 0x0000, false, false }, // LDEAX (DE++)
    { 0x041e, 4, 20, 0x0000, false, false }, // LBCD $FF6F_tuneLfoBend
    { 0x0422, 2, 11, 0x0000, false, false }, // DADD EA,BC
    { 0x0424, 1, 4, 0x0000, false, false }, // MOV A,EAL
    { 0x0425, 2, 10, 0x0000, false, false }, // STAW $FF63_midiModDepth
    { 0x0427, 1, 4, 0x0000, false, false }, // MOV A,EAH
    { 0x0428, 2, 7, 0x0000, false, false }, // GTI A,$2F
    { 0x042a, 2, 10, 0x04a5, false, false }, // JRE $04A5
    { 0x042c, 2, 7, 0x0000, false, false }, // LTI A,$97
    { 0x042e, 2, 10, 0x04ac, false, false }, // JRE $04AC
    { 0x0430, 2, 7, 0x0000, false, false }, // SUI A,$30
    { 0x0432, 2, 8, 0x0000, false, false }, // SLL A
    { 0x0434, 3, 10, 0x0000, false, false }, // LXI EA,$0E60_noteCvDacTbl
    { 0x0437, 2, 11, 0x0000, false, false }, // EADD EA,A
    { 0x0439, 1, 13, 0x0000, false, false }, // PUSH EA
    { 0x043a, 3, 10, 0x0000, false, false }, // LXI EA,$0F30_noteClkDivTbl
    { 0x043d, 2, 11, 0x0000, false, false }, // EADD EA,A
    { 0x043f, 1, 4, 0x0000, false, false }, // DMOV HL,EA
    { 0x0440, 3, 20, 0x0000, false, false }, // LDEAX (HL+$02)
    { 0x0443, 1, 4, 0x0000, false, false }, // DMOV BC,EA
    { 0x0444, 2, 14, 0x0000, false, false }, // LDEAX (HL)
    { 0x0446, 1, 13, 0x0000, false, false }, // PUSH EA
    { 0x0447, 2, 11, 0x0000, false, false }, // DSUB EA,BC
    { 0x0449, 1, 4, 0x0000, false, false }, // DMOV BC,EA
    { 0x044a, 2, 10, 0x0000, false, false }, // LDAW $FF63_midiModDepth
    { 0x044c, 2, 32, 0x0000, false, false }, // MUL C
    { 0x044e, 1, 4, 0x0000, false, false }, // MOV A,EAH
    { 0x044f, 1, 4, 0x0000, false, false }, // MOV C,A
    { 0x0450, 2, 10, 0x0000, false, false }, // LDAW $FF63_midiModDepth
    { 0x0452, 2, 32, 0x0000, false, false }, // MUL B
    { 0x0454, 2, 11, 0x0000, false, false }, // EADD EA,C
    { 0x0456, 1, 4, 0x0000, false, false }, // DMOV BC,EA
    { 0x0457, 1, 10, 0x0000, false, false }, // POP EA
    { 0x0458, 2, 11, 0x0000, false, false }, // DSUB EA,BC
    { 0x045a, 2, 10, 0x0000, false, false }, // LDAW $FF0F_voicePtr
    { 0x045c, 1, 4, 0x0000, false, false }, // MOV B,A
    { 0x045d, 2, 10, 0x0000, false, false }, // LDAW $FF34_actvVoiceBit
    { 0x045f, 3, 14, 0x0000, false, false }, // OFFAW $FF00_resetVoiceBits
    { 0x0462, 2, 10, 0x04b3, false, false }, // JRE $04B3
    { 0x0464, 1, 4, 0x0000, false, false }, // DI 
    { 0x0465, 3, 10, 0x0000, false, false }, // LXI HL,$0B50
    { 0x0468, 1, 13, 0x0000, false, false }, // LDAX (HL+B)
    { 0x0469, 1, 4, 0x0000, false, false }, // MOV H,A
    { 0x046a, 2, 7, 0x0000, false, false }, // MVI L,$00
    { 0x046c, 1, 4, 0x0000, false, false }, // MOV A,EAL
    { 0x046d, 1, 7, 0x0000, false, false }, // STAX (HL)
    { 0x046e, 1, 4, 0x0000, false, false }, // MOV A,EAH
    { 0x046f, 1, 7, 0x0000, false, false }, // STAX (HL)
    { 0x0470, 1, 4, 0x0000, false, false }, // EI 
    { 0x0471, 1, 10, 0x0000, false, false }, // POP HL
    { 0x0472, 2, 14, 0x0000, false, false }, // LDEAX (HL++)
    { 0x0474, 1, 4, 0x0000, false, false }, // DMOV BC,EA
    { 0x0475, 1, 13, 0x0000, false, false }, // PUSH EA
    { 0x0476, 2, 14, 0x0000, false, false }, // LDEAX (HL)
    { 0x0478, 2, 11, 0x0000, false, false }, // DSUB EA,BC
    { 0x047a, 1, 4, 0x0000, false, false }, // DMOV BC,EA
    { 0x047b, 2, 10, 0x0000, false, false }, // LDAW $FF63_midiModDepth
    { 0x047d, 2, 32, 0x0000, false, false }, // MUL C
    { 0x047f, 1, 4, 0x0000, false, false }, // MOV A,EAH
    { 0x0480, 1, 4, 0x0000, false, false }, // MOV C,A
    { 0x0481, 2, 10, 0x0000, false, false }, // LDAW $FF63_midiModDepth
    { 0x0483, 2, 32, 0x0000, false, false }, // MUL B
    { 0x0485, 2, 11, 0x0000, false, false }, // EADD EA,C
    { 0x0487, 1, 4, 0x0000, false, false }, // DMOV BC,EA
    { 0x0488, 1, 10, 0x0000, false, false }, // POP EA
    { 0x0489, 2, 11, 0x0000, false, false }, // DADD EA,BC
    { 0x048b, 2, 8, 0x0000, false, false }, // DSLL EA
    { 0x048d, 2, 8, 0x0000, false, false }, // DSLL EA
    { 0x048f, 2, 10, 0x0000, false, false }, // LDAW $FF0F_voicePtr
    { 0x0491, 2, 13, 0x082f, true, false }, // CALF $082F_loadDac
    { 0x0493, 3, 20, 0x0000, false, false }, // ANI PA,$EF
    { 0x0496, 2, 10, 0x0000, false, false }, // LDAW $FF34_actvVoiceBit
    { 0x0498, 2, 8, 0x0000, false, false }, // SLL A
    { 0x049a, 2, 10, 0x0000, false, false }, // STAW $FF34_actvVoiceBit
    { 0x049c, 2, 16, 0x0000, false, false }, // INRW $FF0F_voicePtr
    { 0x049e, 3, 13, 0x0000, false, false }, // EQIW $FF0F_voicePtr,$06
    { 0x04a1, 2, 10, 0x041c, false, false }, // JRE $041C
    { 0x04a3, 2, 10, 0x04d5, false, false }, // JRE $04D5
    { 0x04a5, 3, 13, 0x0000, false, false }, // MVIW $FF63_midiModDepth,$00
    { 0x04a8, 2, 7, 0x0000, false, false }, // MVI A,$00
    { 0x04aa, 2, 10, 0x0432, false, false }, // JRE $0432
    { 0x04ac, 3, 13, 0x0000, false, false }, // MVIW $FF63_midiModDepth,$00
    { 0x04af, 2, 7, 0x0000, false, false }, // MVI A,$66
    { 0x04b1, 2, 10, 0x0432, false, false }, // JRE $0432
    { 0x04b3, 2, 7, 0x0000, false, false }, // XRI A,$FF
    { 0x04b5, 3, 14, 0x0000, false, false }, // ANAW $FF00_resetVoiceBits
    { 0x04b8, 2, 10, 0x0000, false, false }, // STAW $FF00_resetVoiceBits
    { 0x04ba, 3, 10, 0x0000, false, false }, // LXI HL,$0B56
    { 0x04bd, 1, 13, 0x0000, false, false }, // LDAX (HL+B)
    { 0x04be, 2, 7, 0x0000, false, false }, // MVI H,$23
    { 0x04c0, 3, 11, 0x0000, false, false }, // LTI B,$03
    { 0x04c3, 2, 7, 0x0000, false, false }, // MVI H,$13
    { 0x04c5, 2, 7, 0x0000, false, false }, // MVI L,$00
    { 0x04c7, 1, 4, 0x0000, false, false }, // DI 
    { 0x04c8, 1, 7, 0x0000, false, false }, // STAX (HL)
    { 0x04c9, 2, 10, 0x0465, false, false }, // JRE $0465
    { 0x082f, 3, 20, 0x0000, false, false }, // ORI PA,$F0
    { 0x0832, 2, 7, 0x0000, false, false }, // ORI A,$F0
    { 0x0834, 2, 10, 0x0000, false, false }, // MOV PA,A
    { 0x0836, 1, 4, 0x0000, false, false }, // MOV A,EAH
    { 0x0837, 2, 10, 0x0000, false, false }, // MOV PB,A
    { 0x0839, 1, 4, 0x0000, false, false }, // MOV A,EAL
    { 0x083a, 2, 10, 0x0000, false, false }, // MOV PC,A
    { 0x083c, 1, 10, 0x0000, false, true }, // RET 
};
const Instruction& at(int pc)
{
    for (const auto& instruction : program)
        if (instruction.pc == pc) return instruction;
    throw std::runtime_error("instruction trace left the qualified path");
}
struct Trace { int total, lsb, msb, control; };
Trace trace(bool reset, int high, int nextVoice)
{
    int pc = 0x0493, states = 0, returnPc = 0;
    Trace result {};
    for (int step = 0; step < 200; ++step)
    {
        if (pc == 0x0493 && states > 0)
        {
            result.total = states;
            return result;
        }
        if (pc == 0x046d) result.lsb = states;
        if (pc == 0x046f) result.msb = states;
        if (pc == 0x04c8) result.control = states;
        const auto& i = at(pc);
        int next = pc + i.bytes;
        bool skip = (pc == 0x0428 && high > 47)
            || (pc == 0x042c && high < 151)
            || (pc == 0x045f && !reset)
            || (pc == 0x04c0 && nextVoice < 3);
        states += i.states;
        if (skip)
        {
            const auto& omitted = at(next);
            require(omitted.bytes == 2, "unexpected skipped instruction width");
            states += 7; // NEC starred two-byte JRE/MVI: p.26 note (1).
            next += omitted.bytes;
        }
        if (i.target != 0)
        {
            if (i.call) returnPc = next;
            next = i.target;
        }
        if (i.ret) next = returnPc;
        pc = next;
    }
    throw std::runtime_error("unbounded instruction trace");
}
EngineParameters patch()
{
    EngineParameters p;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.calibration = 1.0f;
    p.aging = 0.5f;
    p.sawEnabled = true; p.pulseEnabled = true;
    p.subLevel = 0.28f; p.noiseLevel = 0.0f;
    p.cutoff = 0.26f; p.resonance = 0.74f;
    p.envDepth = 0.48f; p.keyFollow = 0.5f;
    p.attack = 0.0f; p.decay = 0.22f;
    p.sustain = 0.24f; p.release = 0.12f;
    p.chorus = ChorusMode::Off; p.chorusNoise = 0.0f;
    return p;
}
StereoBuffer render(Profile profile, bool unison, int block = 128,
                    double rate = 48000.0, int quality = 1)
{
    YouKnowEngine engine;
    engine.selectConverterTimingProfile(profile);
    engine.prepare(rate, block, quality);
    auto p = patch();
    p.keyMode = unison ? KeyMode::Unison : KeyMode::Poly1;
    engine.setParameters(p);
    struct Event { int frame, note; bool on; };
    std::vector<Event> events;
    for (int stab = 0; stab < 5; ++stab)
    {
        const int root = std::array { 48, 55, 60, 53, 57 }[static_cast<std::size_t>(stab)];
        for (int voice = 0; voice < (unison ? 1 : 6); ++voice)
        {
            const int note = root + std::array { 0, 7, 12, 15, 19, 24 }[static_cast<std::size_t>(voice)];
            events.push_back({ static_cast<int>(std::llround(rate * (0.050 + 0.420 * stab))), note, true });
            events.push_back({ static_cast<int>(std::llround(rate * (0.125 + 0.420 * stab))), note, false });
        }
    }
    std::stable_sort(events.begin(), events.end(), [](auto a, auto b) { return a.frame < b.frame; });
    StereoBuffer result;
    result.left.resize(static_cast<std::size_t>(std::llround(rate * 2.25)));
    result.right.resize(result.left.size());
    std::size_t event = 0;
    for (int frame = 0; frame < static_cast<int>(result.left.size());)
    {
        while (event < events.size() && events[event].frame == frame)
        {
            const auto& e = events[event++];
            if (e.on) engine.noteOn(e.note, 1.0f); else engine.noteOff(e.note);
        }
        int count = std::min(block, static_cast<int>(result.left.size()) - frame);
        if (event < events.size()) count = std::min(count, events[event].frame - frame);
        engine.process(result.left.data() + frame, result.right.data() + frame, count);
        frame += count;
    }
    std::string error;
    require(validate(result, error), error);
    return result;
}
void selfTest()
{
    for (int reset = 0; reset < 2; ++reset)
        for (int high = 0; high < 256; ++high)
            for (int nextVoice = 1; nextVoice < 6; ++nextVoice)
            {
                const auto t = trace(reset != 0, high, nextVoice);
                require(t.total == YouKnowEngine::firmwareDcoInterWriteStates(
                    reset != 0, static_cast<std::uint8_t>(high)), "ROM interval mismatch");
                require(t.total - t.lsb == 334 && t.total - t.msb == 323,
                        "independent trace contradicts established PIT anchors");
                if (reset) require(t.total - t.control == 389, "reset anchor mismatch");
            }
    const auto shipping = YouKnowEngine::converterEventPhases(Profile::MeasuredChartGeometry);
    for (double rate : { 8000.0, 44100.0, 48000.0, 96000.0 })
        for (int quality : { 1, 2, 4 })
        {
            YouKnowEngine engine;
            engine.selectConverterTimingProfile(Profile::FirmwareDcoNoInterrupt);
            engine.prepare(rate, 128, quality);
            engine.setParameters(patch());
            for (int reset = 0; reset < 2; ++reset)
                for (int note : { 0, 60, 127 })
                {
                    for (int voice = 0; voice < 6; ++voice)
                        YouKnowTestAccess::card(engine, voice, reset != 0, note);
                    YouKnowTestAccess::snapshot(engine);
                    const auto actual = YouKnowTestAccess::phases(engine);
                    const auto interval = trace(reset != 0, note + 24, 1).total / 4.0e6;
                    for (std::size_t ordinal = 0; ordinal < actual.size(); ++ordinal)
                    {
                        if (ordinal < 4 || ordinal > 8)
                            require(actual[ordinal] == shipping[ordinal], "candidate moved an unresolved anchor");
                        else
                            require(std::abs((actual[ordinal] - actual[ordinal - 1]) * 0.0042 - interval)
                                < 1.0e-15, "live schedule does not use the qualified instruction interval");
                        if (ordinal > 0) require(actual[ordinal] > actual[ordinal - 1], "candidate reordered converter writes");
                    }
                }
            const auto beforeSelection = YouKnowTestAccess::phases(engine);
            engine.selectConverterTimingProfile(Profile::MeasuredChartGeometry);
            YouKnowTestAccess::snapshot(engine);
            require(YouKnowTestAccess::phases(engine) == beforeSelection, "selection changed a live profile");
            engine.reset();
            require(YouKnowTestAccess::phases(engine) == shipping, "reset did not apply profile selection");
        }
    for (bool unison : { false, true })
    {
        const auto a = render(Profile::FirmwareDcoNoInterrupt, unison, 1);
        const auto b = render(Profile::FirmwareDcoNoInterrupt, unison, 128);
        require(a.left == b.left && a.right == b.right, "host block partition changed timing/audio");
    }
    {
        YouKnowEngine engine;
        engine.selectConverterTimingProfile(Profile::FirmwareDcoNoInterrupt);
        engine.prepare(48000.0, 128, false);
        auto p = patch();
        p.keyMode = KeyMode::Unison;
        engine.setParameters(p);
        engine.noteOn(60, 1.0f);
        std::array<float, 256> left {}, right {};
        engine.process(left.data(), right.data(), 1);
        const auto cold = YouKnowTestAccess::phases(engine);
        // A POLY-mode change may still have its real assignment-table rescan
        // queued; let that bounded follow-up finish before testing steady run.
        for (int block = 0; block < 10; ++block)
            engine.process(left.data(), right.data(), 256);
        const auto running = YouKnowTestAccess::phases(engine);
        require(std::abs((cold[4] - cold[3]) * 0.0042 - 243.25e-6) < 1e-15,
                "real unison onset did not take the reset instruction path");
        require(std::abs((running[4] - running[3]) * 0.0042 - 216.75e-6) < 1e-15,
                "subsequent scan did not return to the running instruction path");
    }
    std::cout << "PASS: 2560 independent B-2 paths; PIT anchors; 72 rate/quality/reset/clamp snapshots; profile latching; block-invariant shipping-kernel stabs/chords.\n";
}
void audition(const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    std::ofstream csv(output / "metrics.csv");
    csv << "scenario,version,peak_dbfs,rms_dbfs,listening_trim_db,difference_rms_dbfs\n" << std::setprecision(12);
    for (bool unison : { true, false })
    {
        const std::string name = unison ? "stabs" : "chords";
        const auto directory = output / name;
        std::filesystem::create_directories(directory);
        const auto a = render(Profile::MeasuredChartGeometry, unison);
        const auto b = render(Profile::FirmwareDcoNoInterrupt, unison);
        StereoBuffer delta;
        std::string error;
        require(difference(a, b, delta, error), error);
        const auto la = measure(a), lb = measure(b), ld = measure(delta);
        const double target = std::min(la.rms, lb.rms);
        const double commonSafety = std::min(1.0, 0.8 / std::max(
            la.peak * target / la.rms, lb.peak * target / lb.rms));
        for (int version = 0; version < 2; ++version)
        {
            const auto& audio = version == 0 ? a : b;
            const auto level = version == 0 ? la : lb;
            const std::string letter = version == 0 ? "A" : "B";
            require(!std::filesystem::exists(directory / (letter + ".wav")), "refusing to overwrite an audition");
            const double gain = commonSafety * target / level.rms;
            require(writeFloatWav(directory / (letter + ".wav"), applyGain(audio, gain), error), error);
            require(writeFloatWav(directory / (letter + ".raw.wav"), audio, error), error);
            csv << name << ',' << letter << ',' << decibels(level.peak) << ',' << decibels(level.rms)
                << ',' << decibels(gain) << ',' << decibels(ld.rms) << '\n';
        }
        require(writeFloatWav(directory / "difference.raw.wav", delta, error), error);
    }
    std::ofstream key(output / "key.md");
    key << "A: unchanged shipping MeasuredChartGeometry profile.\n\n"
           "B: comparison-only FirmwareDcoNoInterrupt profile; relative DCO write intervals use the B-2 instruction trace (including reset and clamp branches).\n\n"
           "Both: identical score, 48 kHz, block 128, 1x, Poly/Cubic/RK4 x1, Unit Character 100%, Aging 50%, chorus off. "
           "A.wav and B.wav are matched on whole-file stereo RMS; exact trims and unscaled differences are in metrics.csv. Raw archives preserve physical level.\n\n"
           "The first DCO and non-DCO chart anchors, full-loop data-dependent time, physical mux edges and MIDI serial/entry timing remain unresolved. "
           "Timing branches are predicted at the logical pass boundary; mid-pass host parameter changes are not qualified. "
           "These files measure candidate differences, not proof of agreement with an original-unit recording. No shipping default changes.\n";
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--self-test") selfTest();
        else if (argc == 3 && std::string(argv[1]) == "--render") audition(argv[2]);
        else throw std::runtime_error("usage: --self-test | --render DIRECTORY");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
