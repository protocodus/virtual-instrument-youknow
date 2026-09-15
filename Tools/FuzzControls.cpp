// Seeded shipping-processor/editor control torture, without an audio device.
// Run --quick for CI, otherwise a longer sweep; --seed N reproduces the control
// and MIDI script. --output DIR writes callback and scenario CSVs. UI variation
// buttons use the product's own random generator; their values, concurrent
// scheduling and the instrument's analogue noise remain intentionally unfrozen.
// Warmup, control writes, allocation, validation and CSV I/O are outside callback
// timing. Timing is diagnostic (never a CI threshold); nonfinite output, failed
// panic/recovery, exceptions and a 30-second progress watchdog are hard failures.
#define DONT_SET_USING_JUCE_NAMESPACE 1
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <time.h>
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;
using namespace youknow;
constexpr int preparedFrames = 256;
std::atomic<std::uint64_t> progress { 0 };
std::atomic<std::uint64_t> uiProgress { 0 };
std::atomic<bool> editorPhase { false };
std::atomic<int> currentScenario { -1 }, currentBlock { -1 };

struct Options
{
    bool quick = false;
    std::uint32_t seed = 0x594b2026u;
    std::filesystem::path output;
};

class ProgressWatchdog
{
public:
    explicit ProgressWatchdog (std::uint32_t watchdogSeed)
        : thread ([this, watchdogSeed]
          {
              auto previous = progress.load();
              auto previousUi = uiProgress.load();
              auto lastProgress = Clock::now();
              auto lastUiProgress = Clock::now();
              while (! stop.load())
              {
                  std::this_thread::sleep_for (std::chrono::milliseconds (100));
                  const auto current = progress.load();
                  if (current != previous)
                  {
                      previous = current;
                      lastProgress = Clock::now();
                  }
                  const auto ui = uiProgress.load();
                  if (! editorPhase.load() || ui != previousUi)
                  {
                      previousUi = ui;
                      lastUiProgress = Clock::now();
                  }
                  if (Clock::now() - lastProgress > std::chrono::seconds (30)
                      || Clock::now() - lastUiProgress > std::chrono::seconds (30))
                  {
                      std::cerr << "FAIL watchdog: seed=" << watchdogSeed << " scenario="
                                << currentScenario.load() << " block=" << currentBlock.load() << '\n';
                      std::_Exit (EXIT_FAILURE);
                  }
              }
          })
    {
    }

    ~ProgressWatchdog()
    {
        stop.store (true);
        if (thread.joinable())
            thread.join();
    }

    ProgressWatchdog (const ProgressWatchdog&) = delete;
    ProgressWatchdog& operator= (const ProgressWatchdog&) = delete;

private:
    std::atomic<bool> stop { false };
    std::thread thread;
};

double cpuSeconds()
{
#if defined(_WIN32)
    FILETIME creation {}, exit {}, kernel {}, user {};
    if (GetThreadTimes (GetCurrentThread(), &creation, &exit, &kernel, &user))
    {
        const auto ticks = [] (FILETIME t) {
            return (static_cast<std::uint64_t> (t.dwHighDateTime) << 32u)
                 | static_cast<std::uint64_t> (t.dwLowDateTime);
        };
        return static_cast<double> (ticks (kernel) + ticks (user)) * 1.0e-7;
    }
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    timespec value {};
    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &value) == 0)
        return static_cast<double> (value.tv_sec)
             + static_cast<double> (value.tv_nsec) * 1.0e-9;
#endif
    throw std::runtime_error ("thread CPU clock unavailable");
}

void set (YouKnowAudioProcessor& p, const char* id, float value)
{
    auto* parameter = p.parameters.getParameter (id);
    if (parameter == nullptr)
        throw std::runtime_error (std::string ("missing parameter: ") + id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

float randomValue (std::mt19937& rng)
{
    // Deliberately bias towards maximum changes, while also visiting interiors.
    const auto value = rng();
    if (value % 4u == 0u) return 0.0f;
    if (value % 4u == 1u) return 1.0f;
    return static_cast<float> (value >> 8u) / 16777215.0f;
}

void chord (juce::MidiBuffer& midi, int voices)
{
    midi.addEvent (juce::MidiMessage::allSoundOff (1), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
    for (int voice = 0; voice < voices; ++voice)
        midi.addEvent (juce::MidiMessage::noteOn (
            1, 42 + voice * 2, static_cast<juce::uint8> (100)), 0);
}

struct Sample
{
    int block, frames, voices, factor;
    double wallUs, cpuUs, peak;
};

struct FuzzResult
{
    std::string name;
    double rate;
    int channels;
    std::vector<Sample> samples;
};

class Renderer
{
public:
    Renderer (YouKnowAudioProcessor& processor, FuzzResult& result)
        : p (processor), r (result), buffer (result.channels, 1024)
    {
        midi.ensureSize (16384);
    }

    void block (int index, int frames, bool measure = true)
    {
        currentBlock.store (index);
        buffer.setSize (r.channels, frames, false, false, true);
        buffer.clear();
        const auto wallStart = Clock::now();
        const auto cpuStart = cpuSeconds();
        p.processBlock (buffer, midi);
        const auto cpuUs = (cpuSeconds() - cpuStart) * 1.0e6;
        const auto wallUs = std::chrono::duration<double, std::micro> (
            Clock::now() - wallStart).count();
        double peak = 0.0;
        for (int channel = 0; channel < r.channels; ++channel)
            for (int i = 0; i < frames; ++i)
            {
                const auto sample = buffer.getSample (channel, i);
                if (! std::isfinite (sample))
                    throw std::runtime_error (r.name + " nonfinite audio at block "
                                              + std::to_string (index));
                peak = std::max (peak, static_cast<double> (std::abs (sample)));
            }
        if (measure)
            r.samples.push_back ({ index, frames, p.getActiveVoiceCount(),
                p.getOversamplingFactorForDisplay(), wallUs, cpuUs, peak });
        midi.clear();
        ++progress;
    }

    void warmup (int voices = 6)
    {
        chord (midi, voices);
        for (int i = 0; i < 48; ++i)
            block (-48 + i, preparedFrames, false);
    }

    void recover()
    {
        p.flushPendingMidiEvents();
        p.requestPanic();
        block (-3, 256, false);
        if (p.getActiveVoiceCount() != 0)
            throw std::runtime_error (r.name + ": panic left active voices");
        p.setCurrentProgram (0);
        set (p, parameters::volume, 0.8f);
        set (p, parameters::quality, 0.0f);
        set (p, parameters::polyphony, 6.0f);
        set (p, parameters::pitchBend, 0.0f);
        set (p, parameters::modulation, 0.0f);
        chord (midi, 6);
        block (-2, 1024, false);
        if (p.getActiveVoiceCount() == 0 || buffer.getMagnitude (0, 1024) < 1.0e-6f)
            throw std::runtime_error (r.name + ": instrument did not recover");
        p.requestPanic();
        block (-1, 256, false);
    }

    juce::MidiBuffer midi;

private:
    YouKnowAudioProcessor& p;
    FuzzResult& r;
    juce::AudioBuffer<float> buffer;
};

bool changesCostConfiguration (const juce::RangedAudioParameter& parameter)
{
    const auto& id = parameter.paramID;
    return id == parameters::quality || id == parameters::legacyHq
        || id == parameters::polyphony || id == parameters::vcfTanhMode
        || id == parameters::vcfFastEarlyMode || id == parameters::vcfSolverMode;
}

std::vector<juce::RangedAudioParameter*> parameterList (YouKnowAudioProcessor& p,
                                                      bool all)
{
    std::vector<juce::RangedAudioParameter*> list;
    for (auto* raw : p.getParameters())
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (raw))
            if (all || ! changesCostConfiguration (*parameter))
                list.push_back (parameter);
    return list;
}

void mutate (const std::vector<juce::RangedAudioParameter*>& list,
             std::mt19937& rng, int count)
{
    for (int i = 0; i < count; ++i)
        list[rng() % list.size()]->setValueNotifyingHost (randomValue (rng));
}

void midiStorm (juce::MidiBuffer& midi, std::mt19937& rng, int block, int frames)
{
    const auto position = [&] { return frames > 0 ? static_cast<int> (rng() %
                                   static_cast<unsigned> (frames)) : 0; };
    for (int event = 0; event < 12; ++event)
    {
        const int note = static_cast<int> (rng() % 128u);
        midi.addEvent (event % 2 == 0
            ? juce::MidiMessage::noteOn (1, note, static_cast<juce::uint8> (
                1u + rng() % 127u))
            : juce::MidiMessage::noteOff (1, note), position());
    }
    midi.addEvent (juce::MidiMessage::pitchWheel (1,
        static_cast<int> (rng() % 16384u)), position());
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 1,
        static_cast<int> (rng() % 128u)), position());
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64,
        block % 2 == 0 ? 127 : 0), position());
    if (block % 17 == 0)
        midi.addEvent (juce::MidiMessage::allNotesOff (1), position());
    if (block % 19 == 0)
        midi.addEvent (juce::MidiMessage::programChange (1,
            static_cast<int> (rng() % 128u)), position());
    // Real hardware-style parameter dumps, with periodic bursts exceeding the
    // reflection FIFO's capacity, exercise its bounded overflow/recovery path.
    const int messages = block % 37 == 0 ? 128 : 1;
    for (int message = 0; message < messages; ++message)
    {
        std::array<std::uint8_t, sysex::parameterMessageBytes> bytes {};
        const auto length = sysex::writeParameterMessage (
            static_cast<int> (rng() % 18u), static_cast<int> (rng() % 128u),
            0, bytes.data(), bytes.size());
        midi.addEvent (juce::MidiMessage::createSysExMessage (bytes.data() + 1,
            static_cast<int> (length) - 2), position());
    }
}

void verifyMidiBurstRecovery (YouKnowAudioProcessor& p, Renderer& render, int& block)
{
    p.flushPendingMidiEvents();
    constexpr int marker = 91;
    for (int event = 0; event < 130; ++event)
    {
        std::array<std::uint8_t, sysex::parameterMessageBytes> bytes {};
        const auto length = sysex::writeParameterMessage (
            static_cast<int> (sysex::ToneParameter::VcfFreq),
            event == 129 ? marker : event % 128, 0, bytes.data(), bytes.size());
        render.midi.addEvent (juce::MidiMessage::createSysExMessage (
            bytes.data() + 1, static_cast<int> (length) - 2), event);
    }
    render.block (block++, 256);
    p.flushPendingMidiEvents();
    render.block (block++, 256);
    p.flushPendingMidiEvents();
    if (std::abs (p.currentPatch().cutoff - static_cast<float> (marker) / 127.0f) > 1.0e-6f)
        throw std::runtime_error ("SysEx overflow recovery lost the final cutoff marker");
}

FuzzResult sequential (const Options& options, std::string name, double rate,
                   int channels, int fixedFrames, const char* focused = nullptr)
{
    FuzzResult result { std::move (name), rate, channels, {} };
    result.samples.reserve (10000);
    YouKnowAudioProcessor p;
    p.setPlayConfigDetails (0, channels, rate, preparedFrames);
    const bool corner = result.name == "corner_16voice_4x";
    const bool broad = result.name == "legal_parameter_midi_fuzz";
    const bool changing = result.name == "controls_six_voice";
    const bool matched = changing || result.name == "baseline_six_voice";
    if (corner)
    {
        set (p, parameters::polyphony, 16.0f);
        set (p, parameters::quality, 2.0f);
        set (p, parameters::vcfTanhMode, 0.0f);
        set (p, parameters::vcfFastEarlyMode, 0.0f);
        set (p, parameters::vcfSolverMode, 0.0f);
        set (p, parameters::resonance, 1.0f);
        set (p, parameters::cutoff, 0.8f);
        set (p, parameters::chorusI, 1.0f);
        set (p, parameters::chorusII, 1.0f);
    }
    // Quality is fixed before prepare, since a live switch waits for silence.
    p.prepareToPlay (rate, preparedFrames);
    Renderer render (p, result);
    render.warmup (corner ? 16 : 6);
    if (corner && (p.getOversamplingFactorForDisplay() != 4
                   || p.getActiveVoiceCount() != 16))
        throw std::runtime_error ("corner stress did not reach 16 voices at 4x");
    if (matched && p.getActiveVoiceCount() != 6)
        throw std::runtime_error ("comparison did not start with six sustained voices");
    // Seed derived from configuration, keeping baseline/control MIDI identical.
    std::mt19937 rng (options.seed ^ static_cast<std::uint32_t> (rate)
                     ^ static_cast<std::uint32_t> (channels * 1237));
    auto list = parameterList (p, broad);
    if (changing)
    {
        // Keep the chord sounding throughout the matched CPU comparison. The
        // broader scenario independently visits every one of these controls.
        constexpr auto fixed = std::to_array<const char*> ({
            parameters::volume, parameters::vcaLevel, parameters::vcaMode,
            parameters::attack, parameters::decay, parameters::sustain,
            parameters::release, parameters::vcfEnv, parameters::envPolarity,
            parameters::saw, parameters::pulse, parameters::poly1,
            parameters::poly2, parameters::legacyKeyMode });
        std::erase_if (list, [&] (const auto* parameter) {
            return std::any_of (fixed.begin(), fixed.end(), [&] (const char* id) {
                return parameter->paramID == id;
            });
        });
    }
    int block = 0;
    if (broad)
    {
        // Every parameter gets both legal endpoints and an interior; every
        // finite menu/boolean/integer value is exercised explicitly as well.
        for (auto* parameter : list)
        {
            for (const float value : { 0.0f, 1.0f, 0.5f })
            {
                parameter->setValueNotifyingHost (value);
                p.forwardLegacyModeParametersForTest();
                render.block (block++, 64);
            }
            const auto steps = parameter->getNumSteps();
            if (steps > 1 && steps <= 128)
                for (int step = 0; step < steps; ++step)
                {
                    parameter->setValueNotifyingHost (
                        static_cast<float> (step) / static_cast<float> (steps - 1));
                    p.forwardLegacyModeParametersForTest();
                    render.block (block++, 32);
                }
        }
        for (int program = 0; program < p.getNumPrograms(); ++program)
        {
            p.setCurrentProgram (program);
            chord (render.midi, 6);
            render.block (block++, 64);
        }
        p.setCurrentProgram (0);
        const auto settleQuality = [&] (int quality) {
            set (p, parameters::quality, static_cast<float> (quality));
            p.requestPanic();
            // Allow the 5 ms mute/rebuild/unmute path to finish after panic.
            for (int settle = 0; settle < 16; ++settle) render.block (block++, 256);
            const int expected = std::min (1 << quality, rate >= 96000.0 ? 2 : 4);
            if (p.getOversamplingFactorForDisplay() != expected)
                throw std::runtime_error ("idle quality change did not reach requested factor");
        };
        for (int quality = 0; quality < 3; ++quality)
        {
            settleQuality (quality);
            chord (render.midi, 6);
            render.block (block++, 256);
            const int oldFactor = p.getOversamplingFactorForDisplay();
            const int nextQuality = (quality + 1) % 3;
            set (p, parameters::quality, static_cast<float> (nextQuality));
            for (int busy = 0; busy < 4; ++busy) render.block (block++, 64);
            if (p.getOversamplingFactorForDisplay() != oldFactor)
                throw std::runtime_error ("busy quality switch rebuilt a sounding path");
            settleQuality (nextQuality);
        }
    }
    const int count = options.quick ? 192 : 1536;
    constexpr std::array<int, 10> variableFrames { 0, 1, 7, 16, 32, 64, 127,
                                                 256, 513, 1024 };
    juce::MemoryBlock saved;
    p.getStateInformation (saved);
    for (int iteration = 0; iteration < count; ++iteration, ++block)
    {
        const int frames = broad ? variableFrames[static_cast<std::size_t> (
            iteration) % variableFrames.size()] : fixedFrames;
        if (changing)
        {
            mutate (list, rng, 16);
            // A closed filter can make an otherwise active chord inaudible.
            // Preserve audible excitation while still moving cutoff widely.
            const auto cutoff = p.parameters.getRawParameterValue (parameters::cutoff)->load();
            set (p, parameters::cutoff, std::max (0.35f, cutoff));
        }
        if (focused != nullptr)
        {
            auto* parameter = p.parameters.getParameter (focused);
            parameter->setValueNotifyingHost (std::string (focused) == parameters::highPass
                ? static_cast<float> (iteration % 4) / 3.0f
                : static_cast<float> (iteration % 2));
        }
        if (broad)
        {
            mutate (list, rng, 24);
            midiStorm (render.midi, rng, iteration, frames);
            if (iteration % 29 == 0) p.requestPanic();
            if (iteration % 31 == 0)
                p.setCurrentProgram (static_cast<int> (rng() %
                    static_cast<unsigned> (p.getNumPrograms())));
            if (iteration % 43 == 0)
                p.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
        }
        p.forwardLegacyModeParametersForTest();
        render.block (block, frames);
        if (matched && p.getActiveVoiceCount() != 6)
            throw std::runtime_error ("comparison lost its six-voice load");
        p.flushPendingMidiEvents();
    }
    // The baseline must sound; legal filter/modulation edits can make the
    // changing case very quiet. Its six active voices above establish the
    // processing load without misdiagnosing a quiet random setting as failure.
    if (matched && ! changing && (result.samples.front().peak < 1.0e-6
                                  || result.samples.back().peak < 1.0e-6))
        throw std::runtime_error ("baseline was not audible at both endpoints");
    if (broad) verifyMidiBurstRecovery (p, render, block);
    render.recover();
    p.releaseResources();
    return result;
}

std::vector<juce::Component*> editorControls (juce::Component& root)
{
    std::vector<juce::Component*> controls;
    for (auto* child : root.getChildren())
    {
        // Do not descend into widgets: ComboBox's internal button opens a menu.
        if (dynamic_cast<juce::Slider*> (child) != nullptr
            || dynamic_cast<juce::ComboBox*> (child) != nullptr
            || dynamic_cast<YouKnowPerformanceLever*> (child) != nullptr)
            controls.push_back (child);
        else if (auto* button = dynamic_cast<juce::Button*> (child))
        {
            const auto name = (button->getName() + " " + button->getButtonText()).toLowerCase();
            const bool fileAction = (name.contains ("load") && ! name.contains ("reload"))
                                 || name.contains ("save");
            if (! fileAction && ! name.contains ("about") && ! name.contains ("help")
                && name.trim().isNotEmpty())
                controls.push_back (child);
        }
        else
        {
            const auto descendants = editorControls (*child);
            controls.insert (controls.end(), descendants.begin(), descendants.end());
        }
    }
    return controls;
}

FuzzResult concurrentEditor (const Options& options)
{
    FuzzResult result { "concurrent_editor_audio", 48000.0, 2, {} };
    result.samples.reserve (100000);
    YouKnowAudioProcessor p;
    p.setPlayConfigDetails (0, 2, result.rate, preparedFrames);
    p.prepareToPlay (result.rate, preparedFrames);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    if (! editor) throw std::runtime_error ("editor creation failed");
    auto controls = editorControls (*editor);
    if (controls.empty()) throw std::runtime_error ("editor has no controls");
    Renderer render (p, result);
    render.warmup();
    std::atomic<bool> finished { false }, failed { false };
    std::exception_ptr audioError;
    editorPhase = true;
    std::thread audio ([&] {
        try
        {
            std::mt19937 midiRng (options.seed ^ 0x41554449u);
            for (int block = 0; ! finished.load() || block < 256; ++block)
            {
                midiStorm (render.midi, midiRng, block, 128);
                render.block (block, 128);
            }
        }
        catch (...) { audioError = std::current_exception(); failed = true; }
    });
    try
    {
        std::mt19937 rng (options.seed ^ 0x45444954u);
        juce::MemoryBlock saved;
        p.getStateInformation (saved);
        const int count = options.quick ? 256 : 2048;
        int action = 0;
        bool loopFinished = false;
        std::exception_ptr messageError;
        struct Mutator final : juce::Timer
        {
            std::function<void()> callback;
            void timerCallback() override { callback(); }
        } timer;
        const auto stop = [&] {
            timer.stopTimer();
            loopFinished = true;
#if ! defined(__APPLE__)
            juce::MessageManager::getInstance()->stopDispatchLoop();
#endif
        };
        timer.callback = [&] {
          try
          {
            if (action >= count || failed.load()) { stop(); return; }
            // First visit every discovered control, then fuzz their order.
            auto* control = controls[static_cast<std::size_t> (action) < controls.size()
                ? static_cast<std::size_t> (action) : rng() % controls.size()];
            if (auto* lever = dynamic_cast<YouKnowPerformanceLever*> (control))
            {
                const auto eventAt = [&] (juce::Point<float> position, bool down) {
                    const auto now = juce::Time::getCurrentTime();
                    return juce::MouseEvent {
                        juce::Desktop::getInstance().getMainMouseSource(), position,
                        juce::ModifierKeys (down ? juce::ModifierKeys::leftButtonModifier : 0),
                        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, lever, lever,
                        now, position, now, 1, false };
                };
                lever->mouseDown (eventAt (lever->getLocalBounds().toFloat().getCentre(), true));
                for (int drag = 0; drag < 8; ++drag)
                    lever->mouseDrag (eventAt ({
                        (randomValue (rng) * 2.0f - 0.5f) * static_cast<float> (lever->getWidth()),
                        (randomValue (rng) * 2.0f - 0.5f) * static_cast<float> (lever->getHeight()) }, true));
                lever->mouseUp (eventAt ({}, false));
                if (std::abs (lever->getPitchBend()) > 1.0e-6f
                    || std::abs (lever->getModulation()) > 1.0e-6f)
                    throw std::runtime_error ("performance lever failed to spring back");
            }
            else if (auto* slider = dynamic_cast<juce::Slider*> (control))
                slider->setValue (slider->proportionOfLengthToValue (randomValue (rng)),
                                  juce::sendNotificationSync);
            else if (auto* combo = dynamic_cast<juce::ComboBox*> (control))
            {
                if (combo->getNumItems() > 0)
                    combo->setSelectedItemIndex (static_cast<int> (rng() %
                        static_cast<unsigned> (combo->getNumItems())), juce::sendNotificationSync);
            }
            else if (auto* button = dynamic_cast<juce::Button*> (control))
            {
                if (button->getClickingTogglesState())
                    button->setToggleState (! button->getToggleState(), juce::sendNotificationSync);
                else if (button->onClick) button->onClick();
            }
            if (action % 17 == 0)
                p.setCurrentProgram (static_cast<int> (rng() %
                    static_cast<unsigned> (p.getNumPrograms())));
            if (action % 29 == 0)
                p.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
            if (action % 31 == 0) p.getStateInformation (saved);
            if (action % 3 == 0)
            {
                const int note = 36 + (action / 3) % 60;
                p.keyboardState.noteOn (1, note, 0.8f);
                p.keyboardState.noteOff (1, note, 0.0f);
            }
            p.flushPendingMidiEvents();
            if (action % 32 == 0)
            {
                editor->setSize (action % 64 == 0 ? 1200 : 2280,
                                 action % 64 == 0 ? 633 : 1203);
                auto snapshot = editor->createComponentSnapshot (editor->getLocalBounds(), true, 0.5f);
                if (! snapshot.isValid()) throw std::runtime_error ("editor paint failed");
            }
            ++uiProgress;
            ++action;
          }
          catch (...) { messageError = std::current_exception(); stop(); }
        };
        // This final scenario owns the real event loop, so APVTS async
        // attachments, editor timers, layout and paint all run during audio.
        timer.startTimer (1);
#if defined(__APPLE__)
        // A JUCE console executable has no NSApplication. Pump the same native
        // run-loop mode used by JUCE's optional runDispatchLoopUntil instead:
        // timers and async attachments work without a window or native events.
        while (! loopFinished)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
#else
        static_cast<void> (loopFinished);
        juce::MessageManager::getInstance()->runDispatchLoop();
#endif
        timer.stopTimer();
        if (messageError) std::rethrow_exception (messageError);
        if (action != count && ! failed.load())
            throw std::runtime_error ("editor event loop ended before completing actions");
    }
    catch (...)
    {
        finished = true;
        audio.join();
        editorPhase = false;
        throw;
    }
    finished = true;
    audio.join();
    editorPhase = false;
    if (audioError) std::rethrow_exception (audioError);
    render.recover();
    editor.reset();
    p.releaseResources();
    std::cout << "  editor controls exercised: " << controls.size() << '\n';
    return result;
}

double percentile (std::vector<double> values, double fraction)
{
    std::sort (values.begin(), values.end());
    return values[static_cast<std::size_t> (std::ceil (
        fraction * static_cast<double> (values.size() - 1)))];
}

void report (const Options& options, const std::vector<FuzzResult>& results)
{
    std::ofstream callbacks, summary;
    if (! options.output.empty())
    {
        std::filesystem::create_directories (options.output);
        callbacks.open (options.output / "callbacks.csv");
        summary.open (options.output / "summary.csv");
        if (! callbacks || ! summary) throw std::runtime_error ("cannot write CSV output");
        callbacks << "seed,scenario,sample_rate,channels,block,frames,voices,factor,wall_us,thread_cpu_us,peak,deadline_us\n";
        summary << "seed,scenario,sample_rate,channels,blocks,wall_p50_us,wall_p99_us,wall_max_us,cpu_p50_us,cpu_p99_us,cpu_max_us,wall_over_deadline,cpu_over_deadline,peak\n";
    }
    for (const auto& r : results)
    {
        std::vector<double> walls, cpus;
        std::size_t wallOver = 0, cpuOver = 0;
        double peak = 0.0;
        for (const auto& s : r.samples)
        {
            walls.push_back (s.wallUs);
            cpus.push_back (s.cpuUs);
            const auto deadline = 1.0e6 * static_cast<double> (s.frames) / r.rate;
            if (s.frames > 0 && s.wallUs > deadline) ++wallOver;
            if (s.frames > 0 && s.cpuUs > deadline) ++cpuOver;
            peak = std::max (peak, s.peak);
            if (callbacks)
                callbacks << options.seed << ',' << r.name << ',' << r.rate << ',' << r.channels
                    << ',' << s.block << ',' << s.frames << ',' << s.voices << ',' << s.factor
                    << ',' << s.wallUs << ',' << s.cpuUs << ',' << s.peak << ',' << deadline << '\n';
        }
        const auto wall50 = percentile (walls, 0.50), wall99 = percentile (walls, 0.99);
        const auto cpu50 = percentile (cpus, 0.50), cpu99 = percentile (cpus, 0.99);
        const auto wallMax = percentile (walls, 1.0), cpuMax = percentile (cpus, 1.0);
        std::cout << r.name << " @ " << r.rate << " Hz/" << r.channels << " ch: "
            << r.samples.size() << " callbacks, CPU p50/p99/max " << cpu50 << '/' << cpu99
            << '/' << cpuMax << " us, wall max " << wallMax << " us, deadline overruns "
            << wallOver << " wall / " << cpuOver << " CPU, peak " << peak << '\n';
        if (summary)
            summary << options.seed << ',' << r.name << ',' << r.rate << ',' << r.channels
                << ',' << r.samples.size() << ',' << wall50 << ',' << wall99 << ',' << wallMax
                << ',' << cpu50 << ',' << cpu99 << ',' << cpuMax << ',' << wallOver
                << ',' << cpuOver << ',' << peak << '\n';
    }
}
} // namespace

int main (int argc, char** argv)
{
    Options options;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg (argv[i]);
            if (arg == "--quick") options.quick = true;
            else if (arg == "--seed" && i + 1 < argc)
                options.seed = static_cast<std::uint32_t> (std::stoul (argv[++i], nullptr, 0));
            else if (arg == "--output" && i + 1 < argc) options.output = argv[++i];
            else throw std::runtime_error ("usage: YouKnowControlFuzz [--quick] [--seed N] [--output DIR]");
        }
        juce::ScopedJuceInitialiser_GUI initialise;
        std::cout << "Control fuzz seed=" << options.seed << " mode="
                  << (options.quick ? "quick" : "full") << " clock=thread-cpu\n" << std::flush;
        ProgressWatchdog watchdog (options.seed);
        std::vector<FuzzResult> results;
        const auto run = [&] (std::string name, double rate, int channels, int frames,
                              const char* focused = nullptr) {
            currentScenario = static_cast<int> (results.size());
            std::cout << "Running " << currentScenario.load() << ": " << name << " "
                      << rate << " Hz/" << channels << " ch/" << frames << " frames\n" << std::flush;
            results.push_back (sequential (options, std::move (name), rate, channels, frames, focused));
        };
        for (const double rate : { 44100.0, 48000.0, 96000.0 })
            for (const int channels : { 1, 2 })
            {
                run ("baseline_six_voice", rate, channels, 256);
                run ("controls_six_voice", rate, channels, 256);
                run ("legal_parameter_midi_fuzz", rate, channels, 256);
            }
        for (const int frames : { 32, 64 })
        {
            const auto suffix = "_" + std::to_string (frames);
            run ("baseline" + suffix, 48000.0, 2, frames);
            run ("character_sweep" + suffix, 48000.0, 2, frames, parameters::calibration);
            run ("highpass_sweep" + suffix, 48000.0, 2, frames, parameters::highPass);
        }
        run ("corner_16voice_4x", 48000.0, 2, 256);
        currentScenario = static_cast<int> (results.size());
        std::cout << "Running " << currentScenario.load() << ": concurrent_editor_audio\n" << std::flush;
        results.push_back (concurrentEditor (options));
        report (options, results);
        std::cout << "PASS: finite audio, panic/recovery and concurrent editor stress. "
                     "Timing above is diagnostic; deadline overruns require inspection.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL seed=" << options.seed << " scenario=" << currentScenario.load()
                  << " block=" << currentBlock.load() << ": " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
