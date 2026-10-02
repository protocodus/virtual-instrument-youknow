// Deterministic product-path review of a set of hardware-realism changes.
//
//   YouKnowRenderRealismReview render <output-dir> <label> <git-revision> [1|4]
//   YouKnowRenderRealismReview compare <output-dir> <before-label> <after-label>
//
// Each render writes immutable raw stereo float32 WAVs and a source manifest.
// The short musical montage is assembled from independently prepared engines,
// which also leave isolated envelope, oscillator/filter and chorus excerpts.
// Both sides use 48 kHz, event-split 128-frame blocks, the same score, the same
// reset-defined random seeds and the shipping ProductFidelityProfile. The
// default numerical modes/1x quality match a new plug-in instance. Thermal
// start is settled to keep warm-up drift out of the comparison. MIDI velocity
// is 1 and velocity sensitivity is zero, as on the JUNO-106.
//
// Comparison packaging matches whole-file stereo RMS separately for each
// excerpt. The baseline sets the loudness reference; one shared extra gain
// limits both sides to -6 dBFS peak. Raw differences preserve original engine
// levels; listening differences subtract the RMS-matched files. A separate
// boosted difference makes a subtle change audible and records its gain.
// These are model review renders, not captures or similarity measurements of
// an original JUNO-106. A new output label is required when the DSP changes.

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include "RealismComparisonSupport.h"

#include <bit>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#ifndef YOUKNOW_DSP_SOURCE_SHA256
#define YOUKNOW_DSP_SOURCE_SHA256 "unavailable"
#endif
#ifndef YOUKNOW_COMPARISON_COMPILER_ID
#define YOUKNOW_COMPARISON_COMPILER_ID "unknown"
#endif
#ifndef YOUKNOW_COMPARISON_COMPILER_VERSION
#define YOUKNOW_COMPARISON_COMPILER_VERSION "unknown"
#endif
#ifndef YOUKNOW_REVIEW_SOURCE_SHA256
#define YOUKNOW_REVIEW_SOURCE_SHA256 "unavailable"
#endif

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr std::array<std::string_view, 7> slugs {
    "01-release-reattack", "02-oscillator-filter", "03-chorus-output",
    "04-saw-coupling", "05-bright-dry", "06-chorus-idle-toggle", "montage"
};
constexpr std::string_view protocol = "youknow-realism-review-v1";

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string safeLabel(std::string_view value)
{
    require(!value.empty() && value.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
        == std::string_view::npos, "labels must contain only letters, numbers, -_.");
    require(value != "." && value != "..", "invalid label");
    return std::string(value);
}

std::string hashAudio(const StereoBuffer& audio)
{
    // FNV-1a over little-endian float32 samples: corruption/identity binding,
    // not a cryptographic substitute for the build's SHA-256 source identity.
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t frame = 0; frame < audio.left.size(); ++frame)
        for (const float sample : { audio.left[frame], audio.right[frame] })
        {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (int byte = 0; byte < 4; ++byte)
            {
                hash ^= (bits >> (byte * 8)) & 0xffu;
                hash *= 1099511628211ull;
            }
        }
    std::ostringstream result;
    result << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

EngineParameters panel()
{
    EngineParameters p;
    ProductFidelityProfile::applyTo(p);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.calibration = 1.0f;
    p.aging = 0.5f;
    p.volume = 0.6f;
    p.velocityDepth = 0.0f;
    p.polyphony = 6;
    p.highPass = HighPassMode::One;
    p.vcaLevel = 0.85f;
    p.chorus = ChorusMode::Off;
    return p;
}

class Performance
{
public:
    Performance(EngineParameters p, int quality)
        : engine_(std::make_unique<YouKnowEngine>())
    {
        ProductFidelityProfile::configureBeforePrepare(*engine_);
        require(engine_->configureThermalStart(true), "cannot configure settled thermal start");
        engine_->selectConverterTimingProfile(
            YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine_->prepare(comparisonSampleRate, comparisonBlockSize, quality);
        engine_->setParameters(p);
        run(0.25, false);
    }

    void run(double seconds, bool retain = true)
    {
        auto remaining = static_cast<int>(std::llround(seconds * comparisonSampleRate));
        while (remaining > 0)
        {
            const int count = std::min(remaining, comparisonBlockSize);
            engine_->process(left_.data(), right_.data(), count);
            if (retain)
            {
                audio_.left.insert(audio_.left.end(), left_.begin(), left_.begin() + count);
                audio_.right.insert(audio_.right.end(), right_.begin(), right_.begin() + count);
            }
            remaining -= count;
        }
    }

    void on(int note) { engine_->noteOn(note, 1.0f); }
    void off(int note) { engine_->noteOff(note); }
    void set(const EngineParameters& p) { engine_->setParameters(p); }
    void hit(int note, double hold, double gap)
    {
        on(note); run(hold); off(note); run(gap);
    }
    void chord(std::initializer_list<int> notes, double hold, double gap)
    {
        for (const int note : notes) on(note);
        run(hold);
        for (const int note : notes) off(note);
        run(gap);
    }
    StereoBuffer take() { return std::move(audio_); }

private:
    std::unique_ptr<YouKnowEngine> engine_;
    std::array<float, comparisonBlockSize> left_ {}, right_ {};
    StereoBuffer audio_;
};

StereoBuffer envelope(int quality)
{
    auto p = panel();
    // POLY 2 deliberately recycles the same assigned card while its analogue
    // envelope is still releasing; nonzero attack exposes the reattack slope.
    p.keyMode = KeyMode::Poly2;
    p.pulseEnabled = true;
    p.pwmDepth = 0.35f;
    p.subLevel = 0.28f;
    p.cutoff = 0.43f;
    p.resonance = 0.34f;
    p.envDepth = 0.45f;
    p.attack = YouKnowEngine::panelPositionForAttack(0.12f);
    p.decay = YouKnowEngine::panelPositionForDecay(0.45f);
    p.sustain = 0.24f;
    p.release = YouKnowEngine::panelPositionForRelease(1.4f);
    Performance take(p, quality);
    take.run(0.12);
    for (const int note : { 48, 48, 55, 55, 60, 60, 55, 48 })
        take.hit(note, 0.33, 0.10);
    take.run(1.15);
    return take.take(); // 4.71 s
}

StereoBuffer tone(int quality)
{
    auto p = panel();
    p.pulseEnabled = true;
    p.pwmDepth = 0.24f;
    p.subLevel = 0.4f;
    p.cutoff = 0.30f;
    p.resonance = 0.77f;
    p.envDepth = 0.56f;
    p.keyFollow = 0.5f;
    p.attack = YouKnowEngine::panelPositionForAttack(0.015f);
    p.decay = YouKnowEngine::panelPositionForDecay(0.75f);
    p.sustain = 0.14f;
    p.release = YouKnowEngine::panelPositionForRelease(0.28f);
    Performance take(p, quality);
    take.run(0.12);
    for (const int note : { 36, 43, 48, 55, 60, 67 })
        take.hit(note, 0.62, 0.15);
    take.run(0.75);
    return take.take(); // 5.49 s
}

StereoBuffer chorus(int quality)
{
    auto p = panel();
    p.pulseEnabled = true;
    p.pwmSource = PwmSource::Lfo;
    p.pwmDepth = 0.38f;
    p.lfoRate = YouKnowEngine::panelPositionForLfoRate(0.42f);
    p.subLevel = 0.32f;
    p.cutoff = 0.77f;
    p.resonance = 0.16f;
    p.envDepth = 0.18f;
    p.attack = YouKnowEngine::panelPositionForAttack(0.09f);
    p.decay = YouKnowEngine::panelPositionForDecay(0.8f);
    p.sustain = 0.76f;
    p.release = YouKnowEngine::panelPositionForRelease(0.45f);
    p.chorus = ChorusMode::One;
    Performance take(p, quality);
    take.run(0.12);
    take.chord({ 48, 55, 60, 64 }, 1.8, 0.18);
    p.chorus = ChorusMode::Two; take.set(p);
    take.chord({ 41, 48, 53, 57 }, 1.8, 0.18);
    for (const int note : { 43, 50, 55, 59 }) take.on(note);
    take.run(0.9);
    p.chorus = ChorusMode::Off; take.set(p); take.run(0.4);
    p.chorus = ChorusMode::One; take.set(p); take.run(1.1);
    for (const int note : { 43, 50, 55, 59 }) take.off(note);
    take.run(0.9);
    return take.take(); // 7.38 s
}

StereoBuffer sawCoupling(int quality)
{
    auto p = panel();
    // An enabled unipolar saw carries DC onto the WAVE node. Its switch
    // therefore charges/discharges the drawn C56 input coupling capacitor;
    // removing the saw's mean upstream erases the bass transient entirely.
    // Keep the note gate high so this tests the waveform switch independently
    // of the envelope, and keep every other waveform/noise leg switched off.
    p.pulseEnabled = false;
    p.subLevel = 0.0f;
    p.noiseLevel = 0.0f;
    p.vcaMode = VcaMode::Gate;
    p.cutoff = 0.77f;
    p.resonance = 0.08f;
    p.envDepth = 0.0f;
    p.keyFollow = 0.0f;
    Performance take(p, quality);
    take.run(0.12);
    take.on(48); take.run(0.65);
    p.sawEnabled = false; take.set(p); take.run(0.5);
    p.sawEnabled = true; take.set(p); take.run(0.6);
    p.sawEnabled = false; take.set(p); take.run(0.25);
    p.sawEnabled = true; take.set(p); take.run(1.0);
    take.off(48); take.run(0.6);
    return take.take(); // 3.72 s
}

StereoBuffer brightDry(int quality)
{
    auto p = panel();
    p.vcaMode = VcaMode::Gate;
    p.cutoff = 1.0f;
    p.resonance = 0.0f;
    p.envDepth = 0.0f;
    p.keyFollow = 0.0f;
    p.subLevel = 0.0f;
    Performance take(p, quality);
    take.run(0.12);
    for (const int note : { 60, 84, 96 }) take.hit(note, 0.62, 0.10);
    take.run(0.45);
    return take.take(); // 2.73 s; isolated diagnostic, outside the montage
}

StereoBuffer chorusIdleToggle(int quality)
{
    auto p = panel();
    p.cutoff = 0.82f;
    p.resonance = 0.10f;
    p.envDepth = 0.0f;
    p.attack = 0.0f;
    p.release = YouKnowEngine::panelPositionForRelease(0.18f);
    p.chorus = ChorusMode::One;
    Performance take(p, quality);
    take.run(0.3);
    take.chord({ 48, 55, 60 }, 1.0, 0.6);
    p.chorus = ChorusMode::Off; take.set(p); take.run(0.45);
    p.chorus = ChorusMode::One; take.set(p); take.run(1.2);
    p.chorus = ChorusMode::Two; take.set(p); take.run(1.2);
    return take.take(); // 4.75 s; isolated noise/switching diagnostic
}

void append(StereoBuffer& destination, const StereoBuffer& source)
{
    destination.left.insert(destination.left.end(), source.left.begin(), source.left.end());
    destination.right.insert(destination.right.end(), source.right.begin(), source.right.end());
}

void silence(StereoBuffer& destination, double seconds)
{
    const auto frames = static_cast<std::size_t>(std::llround(seconds * comparisonSampleRate));
    destination.left.insert(destination.left.end(), frames, 0.0f);
    destination.right.insert(destination.right.end(), frames, 0.0f);
}

void writeAudio(const std::filesystem::path& path, const StereoBuffer& audio)
{
    std::string error;
    require(validate(audio, error) && writeFloatWav(path, audio, error), error);
}

StereoBuffer readAudio(const std::filesystem::path& path)
{
    StereoBuffer audio;
    std::string error;
    require(readFloatWav(path, audio, error), error);
    return audio;
}

std::string levelJson(const StereoBuffer& audio)
{
    const auto level = measure(audio);
    return "{\"frames\":" + std::to_string(audio.left.size())
        + ",\"peak_dbfs\":" + jsonNumber(decibels(level.peak))
        + ",\"rms_dbfs\":" + jsonNumber(decibels(level.rms))
        + ",\"float_fnv1a64\":\"" + hashAudio(audio) + "\"}";
}

std::string manifestJson(const std::array<StereoBuffer, slugs.size()>& audio,
                         const std::string& label, const std::string& revision,
                         int quality, std::string_view dspFingerprint,
                         std::string_view compiler = YOUKNOW_COMPARISON_COMPILER_ID
                             " " YOUKNOW_COMPARISON_COMPILER_VERSION,
                         std::string_view rendererFingerprint = YOUKNOW_REVIEW_SOURCE_SHA256)
{
    std::ostringstream manifest;
    manifest << "{\n\"protocol\":\"" << protocol << "\",\n"
        << "\"label\":\"" << label << "\",\n"
        << "\"git_revision\":\"" << revision << "\",\n"
        << "\"dsp_source_sha256\":\"" << dspFingerprint << "\",\n"
        << "\"renderer_source_sha256\":\"" << rendererFingerprint << "\",\n"
        << "\"compiler\":\"" << compiler << "\",\n"
        << "\"sample_rate\":" << comparisonSampleRate << ",\n"
        << "\"block_size\":" << comparisonBlockSize << ",\n"
        << "\"quality\":" << quality << ",\n"
        << "\"discarded_preroll_seconds\":0.25,\n"
        << "\"thermal_start\":\"settled\",\n"
        << "\"noise_seeds\":\"prepare/reset defined fixed seeds\",\n"
        << "\"product_profile\":true,\n"
        << "\"numerical_modes\":\"PolyZoned/Cubic/Rk4Single\",\n"
        << "\"unit_character\":1,\"aging\":0.5,\"volume\":0.6,\n"
        << "\"velocity\":1,\"velocity_depth\":0,\"polyphony\":6,\n"
        << "\"sections\":{\n";
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        manifest << '\"' << slugs[index] << "\":" << levelJson(audio[index])
                 << (index + 1 < slugs.size() ? ",\n" : "\n");
    }
    manifest << "}\n}\n";
    return manifest.str();
}

void requireUniqueFragment(std::string_view manifest, const std::string& fragment)
{
    const auto start = manifest.find(fragment);
    require(start != std::string_view::npos
        && manifest.find(fragment, start + fragment.size()) == std::string_view::npos,
        "manifest missing or duplicating contract fragment: " + fragment);
}

// Manifests are the tool's deliberately narrow canonical format, not an
// arbitrary JSON ingestion API. Exact fragments avoid accepting one claimed
// quality/identity at the top and another duplicate farther down the file.
std::string manifestString(std::string_view manifest, std::string_view field)
{
    const auto key = "\"" + std::string(field) + "\":";
    requireUniqueFragment(manifest, key);
    const auto begin = manifest.find(key) + key.size();
    require(begin < manifest.size() && manifest[begin] == '\"',
        "manifest string expected: " + std::string(field));
    const auto end = manifest.find('\"', begin + 1);
    require(end != std::string_view::npos, "unterminated manifest string");
    const auto value = manifest.substr(begin + 1, end - begin - 1);
    require(value.find('\\') == std::string_view::npos, "non-canonical escaped manifest string");
    return std::string(value);
}

bool isFingerprint(std::string_view value)
{
    return value.size() == 64
        && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

void validateManifest(const std::string& manifest, const std::string& label,
                      const std::array<StereoBuffer, slugs.size()>& audio)
{
    require(manifestString(manifest, "protocol") == protocol, "wrong render protocol");
    require(manifestString(manifest, "label") == label, "raw render label does not match manifest");
    require(isFingerprint(manifestString(manifest, "dsp_source_sha256")),
        "render requires a 64-digit DSP SHA-256 fingerprint");
    require(isFingerprint(manifestString(manifest, "renderer_source_sha256")),
        "render requires a 64-digit renderer SHA-256 fingerprint");
    require(!manifestString(manifest, "git_revision").empty(), "missing render revision");
    require(manifestString(manifest, "compiler") != "unknown unknown", "missing compiler identity");
    const std::array<std::string, 13> required {
        "\"sample_rate\":48000,", "\"block_size\":128,",
        "\"discarded_preroll_seconds\":0.25,", "\"thermal_start\":\"settled\",",
        "\"noise_seeds\":\"prepare/reset defined fixed seeds\",", "\"product_profile\":true,",
        "\"numerical_modes\":\"PolyZoned/Cubic/Rk4Single\",", "\"unit_character\":1,",
        "\"aging\":0.5,", "\"volume\":0.6,", "\"velocity\":1,",
        "\"velocity_depth\":0,", "\"polyphony\":6,"
    };
    for (const auto& fragment : required)
    {
        requireUniqueFragment(manifest, fragment.substr(0, fragment.find(':') + 1));
        requireUniqueFragment(manifest, fragment);
    }
    const auto qualityKey = std::string("\"quality\":");
    requireUniqueFragment(manifest, qualityKey);
    const auto qualityPosition = manifest.find(qualityKey) + qualityKey.size();
    require(manifest.substr(qualityPosition, 2) == "1,"
        || manifest.substr(qualityPosition, 2) == "4,", "unsupported manifest quality");
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        requireUniqueFragment(manifest, "\"" + std::string(slugs[index]) + "\":");
        requireUniqueFragment(manifest,
            "\"" + std::string(slugs[index]) + "\":" + levelJson(audio[index]));
    }
}

void validatePair(const std::string& beforeManifest, const std::string& afterManifest)
{
    require(manifestString(beforeManifest, "compiler") == manifestString(afterManifest, "compiler"),
        "before/after compiler identity differs");
    require(manifestString(beforeManifest, "renderer_source_sha256")
        == manifestString(afterManifest, "renderer_source_sha256"),
        "before/after renderer source differs; score identity is not established");
    const auto quality = std::string("\"quality\":");
    const auto beforeQuality = beforeManifest.substr(beforeManifest.find(quality) + quality.size(), 2);
    const auto afterQuality = afterManifest.substr(afterManifest.find(quality) + quality.size(), 2);
    require(beforeQuality == afterQuality, "before/after quality differs");
    require(manifestString(beforeManifest, "dsp_source_sha256")
        != manifestString(afterManifest, "dsp_source_sha256"),
        "before/after DSP fingerprints are identical; no source change is being reviewed");
}

std::string readManifest(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot open render manifest " + path.string());
    std::ostringstream contents;
    contents << stream.rdbuf();
    require(stream.good() || stream.eof(), "cannot read render manifest " + path.string());
    return contents.str();
}

void render(const std::filesystem::path& directory, const std::string& label,
            const std::string& revision, int quality)
{
    require(isFingerprint(YOUKNOW_DSP_SOURCE_SHA256),
        "compile the renderer with the CMake DSP source fingerprint");
    require(isFingerprint(YOUKNOW_REVIEW_SOURCE_SHA256),
        "compile the renderer with the CMake renderer source fingerprint");
    const auto output = directory / label;
    require(!std::filesystem::exists(output), "render label already exists; use a new immutable label");
    std::array<StereoBuffer, slugs.size()> audio {
        envelope(quality), tone(quality), chorus(quality), sawCoupling(quality),
        brightDry(quality), chorusIdleToggle(quality), {}
    };
    constexpr std::size_t montageIndex = slugs.size() - 1;
    constexpr std::size_t musicalSections = 4;
    for (std::size_t index = 0; index < musicalSections; ++index)
    {
        append(audio[montageIndex], audio[index]);
        if (index + 1 < musicalSections) silence(audio[montageIndex], 0.3);
    }
    const auto manifest = manifestJson(audio, label, revision, quality, YOUKNOW_DSP_SOURCE_SHA256);
    validateManifest(manifest, label, audio);
    for (std::size_t index = 0; index < slugs.size(); ++index)
        writeAudio(output / (std::string(slugs[index]) + "-raw.wav"), audio[index]);
    std::string error;
    require(writeText(output / "manifest.json", manifest, error), error);
    std::cout << "Frozen " << label << ": " << audio[montageIndex].left.size()
              << " montage frames (" << audio[montageIndex].left.size() / 48000.0 << " s)\n";
}

void compare(const std::filesystem::path& directory,
             const std::string& beforeLabel, const std::string& afterLabel)
{
    const auto output = directory / (beforeLabel + "-vs-" + afterLabel);
    require(!std::filesystem::exists(output), "comparison already exists; keep review evidence frozen");
    const auto beforeManifest = readManifest(directory / beforeLabel / "manifest.json");
    const auto afterManifest = readManifest(directory / afterLabel / "manifest.json");
    std::array<StereoBuffer, slugs.size()> beforeAudio, afterAudio;
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        const auto name = std::string(slugs[index]) + "-raw.wav";
        beforeAudio[index] = readAudio(directory / beforeLabel / name);
        afterAudio[index] = readAudio(directory / afterLabel / name);
    }
    // Finish every identity and sample binding check before writing any audio.
    validateManifest(beforeManifest, beforeLabel, beforeAudio);
    validateManifest(afterManifest, afterLabel, afterAudio);
    validatePair(beforeManifest, afterManifest);
    std::ostringstream manifest;
    manifest << "{\"protocol\":\"" << protocol << "\",\"before\":\"" << beforeLabel
        << "\",\"after\":\"" << afterLabel << "\",\"rms_matching\":\"whole-file stereo\",\"sections\":{\n";
    std::string error;
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        const auto slug = std::string(slugs[index]);
        const auto& before = beforeAudio[index];
        const auto& after = afterAudio[index];
        const auto beforeLevel = measure(before);
        const auto afterLevel = measure(after);
        require(beforeLevel.rms > 0 && afterLevel.rms > 0, "cannot match silent audio");
        const double rmsTrim = beforeLevel.rms / afterLevel.rms;
        const double sharedGain = listeningTargetPeak
            / std::max(beforeLevel.peak, afterLevel.peak * rmsTrim);
        const auto matchedBefore = applyGain(before, sharedGain);
        const auto matchedAfter = applyGain(after, sharedGain * rmsTrim);
        StereoBuffer rawDelta, delta;
        require(difference(before, after, rawDelta, error), error);
        require(difference(matchedBefore, matchedAfter, delta, error), error);
        const auto deltaLevel = measure(delta);
        const double audibleGain = deltaLevel.peak > 0
            ? std::min(1000.0, listeningTargetPeak / deltaLevel.peak) : 1.0;
        const auto sectionOutput = output / slug;
        writeAudio(sectionOutput / "before.wav", matchedBefore);
        writeAudio(sectionOutput / "after.wav", matchedAfter);
        writeAudio(sectionOutput / "A.wav", matchedBefore);
        writeAudio(sectionOutput / "B.wav", matchedAfter);
        writeAudio(sectionOutput / "difference-raw.wav", rawDelta);
        writeAudio(sectionOutput / "difference.wav", delta);
        writeAudio(sectionOutput / "difference-audible.wav", applyGain(delta, audibleGain));
        // Convenient direct listen: one complete before, 0.5 s silence,
        // one complete after. Identical section offsets within each half.
        StereoBuffer audition = matchedBefore;
        silence(audition, 0.5);
        append(audition, matchedAfter);
        writeAudio(sectionOutput / "before-then-after.wav", audition);
        const auto key = std::string("A is ") + beforeLabel + " (DSP SHA-256 "
            + manifestString(beforeManifest, "dsp_source_sha256") + ").\n\nB is "
            + afterLabel + " (DSP SHA-256 "
            + manifestString(afterManifest, "dsp_source_sha256") + ").\n\n"
            + "Both are matched on whole-file stereo RMS. B trim: "
            + jsonNumber(decibels(rmsTrim)) + " dB; shared listening gain: "
            + jsonNumber(decibels(sharedGain)) + " dB. The signed difference is B minus A. "
            + "difference-audible.wav adds " + jsonNumber(decibels(audibleGain))
            + " dB to that residual; it is an exaggerated diagnostic.\n";
        require(writeText(sectionOutput / "key.md", key, error), error);
        manifest << '\"' << slug << "\":{\"before_raw\":" << levelJson(before)
            << ",\"after_raw\":" << levelJson(after)
            << ",\"raw_difference\":" << levelJson(rawDelta)
            << ",\"after_rms_trim_db\":" << jsonNumber(decibels(rmsTrim))
            << ",\"shared_gain_db\":" << jsonNumber(decibels(sharedGain))
            << ",\"matched_difference_rms_dbc\":"
            << jsonNumber(decibels(deltaLevel.rms / measure(matchedBefore).rms))
            << ",\"audible_difference_gain_db\":" << jsonNumber(decibels(audibleGain))
            << ",\"matched_before\":" << levelJson(matchedBefore)
            << ",\"matched_after\":" << levelJson(matchedAfter) << '}'
            << (index + 1 < slugs.size() ? ",\n" : "\n");
        std::cout << slug << ": after trim " << decibels(rmsTrim)
            << " dB, matched delta "
            << decibels(deltaLevel.rms / measure(matchedBefore).rms)
            << " dBc, audible delta boost " << decibels(audibleGain) << " dB\n";
    }
    manifest << "}}\n";
    require(writeText(output / "metrics.json", manifest.str(), error), error);
}

void selfTest()
{
    std::array<StereoBuffer, slugs.size()> audio;
    for (auto& section : audio)
        section = { { 0.1f, -0.2f, 0.15f }, { -0.05f, 0.2f, -0.1f } };
    const auto makeManifest = [&](std::string_view label, int quality, char fingerprint) {
        return manifestJson(audio, std::string(label), "test-revision", quality,
            std::string(64, fingerprint), "contract-test-compiler 1.0", std::string(64, 'c'));
    };
    const auto before = makeManifest("before", 1, 'a');
    const auto after = makeManifest("after", 1, 'b');
    validateManifest(before, "before", audio);
    validateManifest(after, "after", audio);
    validatePair(before, after);
    int rejected = 0;
    const auto reject = [&](auto&& check, std::string_view name) {
        bool didReject = false;
        try { check(); } catch (const std::runtime_error&) { didReject = true; }
        require(didReject, "self-test failed to reject " + std::string(name));
        ++rejected;
    };
    auto corrupted = audio;
    corrupted[0].left[0] = 0.100001f;
    reject([&] { validateManifest(before, "before", corrupted); }, "corrupt raw audio");
    auto wrongHash = before;
    const auto hashPosition = wrongHash.find(hashAudio(audio[0]));
    wrongHash[hashPosition] = wrongHash[hashPosition] == '0' ? '1' : '0';
    reject([&] { validateManifest(wrongHash, "before", audio); }, "wrong raw hash");
    reject([&] { validateManifest(before, "after", audio); }, "wrong label");
    const auto otherQuality = makeManifest("after", 4, 'b');
    validateManifest(otherQuality, "after", audio);
    reject([&] { validatePair(before, otherQuality); }, "quality mismatch");
    const auto sameSource = makeManifest("after", 1, 'a');
    reject([&] { validatePair(before, sameSource); }, "same DSP fingerprint");
    reject([&] { validateManifest(before + "\"protocol\":\"duplicate\"", "before", audio); },
        "duplicate protocol");
    reject([&] { validateManifest(before + "\"sample_rate\":44100", "before", audio); },
        "duplicate conflicting sample rate");
    auto wrongRate = before;
    wrongRate.replace(wrongRate.find("48000"), 5, "44100");
    reject([&] { validateManifest(wrongRate, "before", audio); }, "wrong sample rate");
    auto wrongCompiler = after;
    const auto compilerPosition = wrongCompiler.find("\"compiler\":\"") + 12;
    wrongCompiler.insert(compilerPosition, "other-");
    reject([&] { validatePair(before, wrongCompiler); }, "compiler mismatch");
    auto wrongRenderer = after;
    wrongRenderer.replace(wrongRenderer.find(std::string(64, 'c')), 64, std::string(64, 'd'));
    validateManifest(wrongRenderer, "after", audio);
    reject([&] { validatePair(before, wrongRenderer); }, "renderer source mismatch");
    std::cout << "Realism review manifest contract: valid pair passed; " << rejected
              << " invalid comparisons rejected.\n";
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test")
        {
            selfTest();
            return 0;
        }
        require(argc >= 5, "usage: YouKnowRenderRealismReview render dir label git-revision [1|4] | compare dir before-label after-label");
        const std::string_view command(argv[1]);
        const auto firstLabel = safeLabel(argv[3]);
        if (command == "render")
        {
            require(argc == 5 || argc == 6, "render expects 4 or 5 arguments");
            const auto revision = safeLabel(argv[4]);
            const std::string_view quality = argc == 6 ? argv[5] : "1";
            require(quality == "1" || quality == "4", "quality must be 1 or 4");
            render(argv[2], firstLabel, revision, quality == "4" ? 4 : 1);
        }
        else
        {
            require(command == "compare" && argc == 5, "invalid command or arguments");
            compare(argv[2], firstLabel, safeLabel(argv[4]));
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
