// Deterministic product-path review of a set of hardware-realism changes.
//
//   YouKnowRenderRealismRound2 render <output-dir> <label> <git-revision> [1|4]
//   YouKnowRenderRealismRound2 compare <output-dir> <before-label> <after-label>
//
// Each render writes immutable raw stereo float32 WAVs and a source manifest.
// The short musical montage is assembled from independently prepared engines,
// which leave isolated noise, VCA, performance and cable excerpts. Baseline
// builds compile the SAME source/score against archived pre-change headers and
// library, omitting unavailable feature APIs through explicit capability macros.
// Each take trims ONLY the engine's declared processing latency, appending the
// same number of trailing frames. No correlation, pitch or timing fit is used.
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
#ifndef YOUKNOW_ROUND2_SOURCE_SHA256
#define YOUKNOW_ROUND2_SOURCE_SHA256 "unavailable"
#endif
#ifndef YOUKNOW_ROUND2_HAVE_CABLE
#define YOUKNOW_ROUND2_HAVE_CABLE 0
#endif
#ifndef YOUKNOW_ROUND2_HAVE_ORIGINAL
#define YOUKNOW_ROUND2_HAVE_ORIGINAL 0
#endif
#ifndef YOUKNOW_ROUND2_HAVE_VCA
#define YOUKNOW_ROUND2_HAVE_VCA 0
#endif

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr std::array<std::string_view, 10> slugs {
    "01-main-noise-bytes", "02-noise-bright", "03-vca-high-notes",
    "04-performance-chords", "05-performance-retriggers", "06-performance-unison",
    "07-performance-hold", "08-cable-3m", "09-cable-6m", "montage"
};
constexpr std::string_view protocol = "youknow-realism-round2-v1";
std::array<int, slugs.size()> reportedLatency {};
std::string capabilities()
{
    return std::string(YOUKNOW_ROUND2_HAVE_CABLE ? "1" : "0")
         + (YOUKNOW_ROUND2_HAVE_ORIGINAL ? "1" : "0")
         + (YOUKNOW_ROUND2_HAVE_VCA ? "1" : "0");
}

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
#if YOUKNOW_ROUND2_HAVE_VCA
    require(p.enableVoiceVcaAntialias, "product profile did not select the VCA correction");
#endif
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
    Performance(EngineParameters p, int quality, bool original = false)
        : engine_(std::make_unique<YouKnowEngine>())
    {
        ProductFidelityProfile::configureBeforePrepare(*engine_);
        require(engine_->configureThermalStart(true), "cannot configure settled thermal start");
        engine_->selectConverterTimingProfile(
            YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine_->setParameters(p);
#if YOUKNOW_ROUND2_HAVE_ORIGINAL
        engine_->setOriginalPerformanceMode(original);
#else
        (void) original;
#endif
        engine_->prepare(comparisonSampleRate, comparisonBlockSize, quality);
        latency_ = engine_->getProcessingLatencySamples();
        require(latency_ >= 0 && latency_ < 4096, "invalid declared processing latency");
        run(0.25, false);
    }

    void run(double seconds, bool retain = true)
    {
        auto remaining = static_cast<int>(std::llround(seconds * comparisonSampleRate));
        while (remaining > 0)
        {
            const int count = std::min(remaining, comparisonBlockSize);
            engine_->process(left_.data(), right_.data(), count);
#if YOUKNOW_ROUND2_HAVE_ORIGINAL
            require(engine_->originalPerformanceHealthy(), "original-performance renderer lost firmware support");
#endif
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
    void hold(bool value) { engine_->setSustainPedal(value); }
    StereoBuffer take(std::size_t section)
    {
        require(section < reportedLatency.size(), "invalid scene index");
        // Pull the complete delayed tail, then remove the declared leading
        // latency. MIDI and serial timing differences remain in the audio.
        run(double(latency_) / comparisonSampleRate);
        require(audio_.left.size() > std::size_t(latency_), "scene shorter than latency");
        audio_.left.erase(audio_.left.begin(), audio_.left.begin() + latency_);
        audio_.right.erase(audio_.right.begin(), audio_.right.begin() + latency_);
        reportedLatency[section] = latency_;
        return std::move(audio_);
    }

private:
    std::unique_ptr<YouKnowEngine> engine_;
    std::array<float, comparisonBlockSize> left_ {}, right_ {};
    StereoBuffer audio_;
    int latency_ {};
};

StereoBuffer noiseBytes(int quality)
{
    auto p = panel();
    p.sawEnabled = p.pulseEnabled = false;
    p.subLevel = 0;
    p.noiseLevel = 0;
    p.cutoff = 1;
    p.resonance = p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate;
    p.chorusNoise = 0;
    Performance take(p, quality);
    take.run(.12);
    take.on(60);
    // Byte 6 is a useful supported soft-junction contrast; the legacy law's
    // dead zone masks 1/4, whereas 16/32 and full supply audible references.
    for (const int byte : {0, 1, 4, 6, 16, 32, 127})
    {
        p.noiseLevel = float(byte) / 127.f;
        take.set(p);
        take.run(byte == 6 ? .8 : .5);
    }
    take.off(60); take.run(.3);
    return take.take(0); //4.22s
}

StereoBuffer noiseBright(int quality)
{
    auto p = panel();
    p.sawEnabled = p.pulseEnabled = false;
    p.subLevel = 0;
    p.noiseLevel = 16.f / 127.f;
    p.cutoff = 1;
    p.resonance = p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate;
    p.chorusNoise = 0;
    Performance take(p, quality);
    take.run(.12);
    take.hit(60, 2.8, .4);
    return take.take(1); //3.32s; C41/noise-control/VCA changes share this path
}

StereoBuffer vcaHighNotes(int quality)
{
    auto p = panel();
    p.pulseEnabled = true;
    p.pwmDepth = .29f;
    p.subLevel = .25f;
    p.noiseLevel = 0;
    p.cutoff = 1;
    p.resonance = p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate;
    Performance take(p, quality);
    take.run(.12);
    take.chord({84, 91, 96}, .9, .14);
    for (const int note : {96, 108, 117, 123, 103, 91}) take.hit(note, .15, .07);
    take.chord({84, 91, 96, 103}, 1.0, .3);
    return take.take(2); //3.78s; bright nonlinear VCA drive and gate edges
}

EngineParameters performancePanel()
{
    auto p = panel();
    p.pulseEnabled = true;
    p.pwmSource = PwmSource::Lfo;
    p.pwmDepth = .29f;
    p.subLevel = .3f;
    p.cutoff = .53f;
    p.resonance = .32f;
    p.envDepth = .31f;
    p.attack = YouKnowEngine::panelPositionForAttack(.012f);
    p.decay = YouKnowEngine::panelPositionForDecay(.4f);
    p.sustain = .55f;
    p.release = YouKnowEngine::panelPositionForRelease(.42f);
    p.chorus = ChorusMode::One;
    return p;
}

StereoBuffer performanceChords(int quality)
{
    auto p = performancePanel();
    Performance take(p, quality, true);
    take.run(.12);
    // Same-time host ordering stays identical. The original descending scan
    // may assign these chord notes to different voice cards.
    take.chord({48, 55, 60, 64}, .85, .16);
    take.chord({41, 48, 53, 57}, .85, .16);
    take.chord({43, 50, 55, 59}, .85, .16);
    take.chord({48, 55, 60, 64}, .6, .4);
    return take.take(3); //4.15s
}

StereoBuffer performanceRetriggers(int quality)
{
    auto p = performancePanel();
    p.keyMode = KeyMode::Poly2;
    p.chorus = ChorusMode::Off;
    p.attack = YouKnowEngine::panelPositionForAttack(.07f);
    p.release = YouKnowEngine::panelPositionForRelease(.85f);
    Performance take(p, quality, true);
    take.run(.12);
    for (const int note : {60, 60, 67, 67, 64, 64}) take.hit(note, .25, .08);
    // These intentionally short notes expose the phase-dependent real scan,
    // not an invented universal 5 ms rejection threshold.
    for (const int note : {72, 74, 76, 79, 76, 74}) take.hit(note, .001, .055);
    take.hit(72, .45, .75);
    return take.take(4); //3.636s
}

StereoBuffer performanceUnison(int quality)
{
    auto p = performancePanel();
    p.keyMode = KeyMode::Unison;
    p.chorus = ChorusMode::Off;
    p.attack = 0;
    p.sustain = .6f;
    p.release = YouKnowEngine::panelPositionForRelease(.22f);
    p.cutoff = .72f;
    Performance take(p, quality, true);
    take.run(.12);
    for (const int note : {48, 55, 60, 55, 48}) take.hit(note, .38, .12);
    take.run(.55);
    return take.take(5); //3.17s; serialized six-card gate/PIT/CV evolution
}

StereoBuffer performanceHold(int quality)
{
    auto p = performancePanel();
    Performance take(p, quality, true);
    take.run(.12);
    take.hold(true);
    take.chord({48, 55, 60}, .48, .25);
    take.chord({53, 60, 65}, .48, .35);
    take.hold(false); take.run(.6);
    take.chord({43, 50, 55, 59}, .85, .65);
    return take.take(6); //3.78s
}

StereoBuffer cable(int quality, float picofarads, std::size_t section)
{
    auto p = panel();
#if YOUKNOW_ROUND2_HAVE_CABLE
    p.outputCapacitancePf = picofarads;
#else
    (void) picofarads;
#endif
    // Explicit receiver resistance, common to both builds. Each final stereo
    // jack sees one declared cable. 0 pF is the frozen baseline connection.
    p.outputLoadOhms = 47000;
    p.pulseEnabled = true;
    p.pwmDepth = .31f;
    p.subLevel = .15f;
    p.cutoff = 1;
    p.resonance = p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate;
    p.noiseLevel = 0;
    Performance take(p, quality);
    take.run(.12);
    for (const int note : {60, 84, 96}) take.hit(note, .6, .10);
    take.chord({60, 67, 72}, .85, .4);
    return take.take(section); //3.47s
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

std::vector<std::uint8_t> playbackBytes(const StereoBuffer& audio)
{
    // Native players commonly reject IEEE float WAVs. Raw provenance remains
    // float32; review copies use ordinary signed PCM24 without dither or gain.
    std::string error;
    require(validate(audio, error), error);
    require(measure(audio).peak <= 1.0, "PCM24 playback would clip");
    require(audio.left.size() <= (std::numeric_limits<std::uint32_t>::max() - 36u) / 6u,
            "PCM24 playback exceeds RIFF size");
    std::vector<std::uint8_t> bytes;
    const auto dataBytes = static_cast<std::uint32_t>(audio.left.size() * 6u);
    bytes.reserve(44u + dataBytes);
    const auto tag = [&](const char* value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value[i]));
    };
    tag("RIFF"); appendLittleEndian(bytes, 36u + dataBytes, 4); tag("WAVE"); tag("fmt ");
    appendLittleEndian(bytes, 16u, 4); appendLittleEndian(bytes, 1u, 2);
    appendLittleEndian(bytes, 2u, 2); appendLittleEndian(bytes, comparisonSampleRate, 4);
    appendLittleEndian(bytes, comparisonSampleRate * 6u, 4);
    appendLittleEndian(bytes, 6u, 2); appendLittleEndian(bytes, 24u, 2);
    tag("data"); appendLittleEndian(bytes, dataBytes, 4);
    for (std::size_t i = 0; i < audio.left.size(); ++i)
        for (const float value : {audio.left[i], audio.right[i]})
        {
            const auto sample = static_cast<std::int32_t>(std::llround(double(value) * 8388607.0));
            appendLittleEndian(bytes, static_cast<std::uint32_t>(sample), 3);
        }
    return bytes;
}

void writePlayback(const std::filesystem::path& path, const StereoBuffer& audio)
{
    const auto bytes = playbackBytes(audio);
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    require(!fsError, "cannot create playback directory");
    std::ofstream stream(path, std::ios::binary);
    require(bool(stream), "cannot open PCM24 playback file");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(bool(stream), "cannot write PCM24 playback file");
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
                         std::string_view rendererFingerprint = YOUKNOW_ROUND2_SOURCE_SHA256)
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
        << "\"available_feature_apis\":\"" << capabilities() << "\",\n"
        << "\"feature_api_order\":\"cable,original-performance,VCA-antialias\",\n"
        << "\"latency_compensation\":\"declared integer processing latency only\",\n"
        << "\"discarded_preroll_seconds\":0.25,\n"
        << "\"thermal_start\":\"settled\",\n"
        << "\"noise_seeds\":\"prepare/reset defined fixed seeds\",\n"
        << "\"product_profile\":true,\n"
        << "\"numerical_modes\":\"PolyZoned/Cubic/Rk4Single\",\n"
        << "\"unit_character\":1,\"aging\":0.5,\"volume\":0.6,\n"
        << "\"velocity\":1,\"velocity_depth\":0,\"polyphony\":6,\n"
        << "\"declared_latency_samples\":\"";
    for (std::size_t index = 0; index + 1 < slugs.size(); ++index)
        manifest << reportedLatency[index] << (index + 2 < slugs.size() ? "," : "");
    manifest << "\",\n\"sections\":{\n";
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
    const auto api = manifestString(manifest, "available_feature_apis");
    require(api.size() == 3 && api.find_first_not_of("01") == std::string::npos,
            "invalid feature capability declaration");
    require(manifestString(manifest, "feature_api_order") == "cable,original-performance,VCA-antialias",
            "wrong feature capability order");
    require(manifestString(manifest, "latency_compensation") == "declared integer processing latency only",
            "wrong latency compensation policy");
    const auto latency = manifestString(manifest, "declared_latency_samples");
    std::istringstream latencies(latency);
    std::string element;
    std::size_t latencyCount = 0;
    while (std::getline(latencies, element, ','))
    {
        require(!element.empty() && element.find_first_not_of("0123456789") == std::string::npos,
                "invalid declared latency value");
        require(element.size() <= 4 && std::stoi(element) < 4096, "declared latency out of range");
        ++latencyCount;
    }
    require(latencyCount + 1 == slugs.size(), "declared latency list has wrong size");
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
    require(isFingerprint(YOUKNOW_ROUND2_SOURCE_SHA256),
        "compile the renderer with the CMake renderer source fingerprint");
    const auto output = directory / label;
    require(!std::filesystem::exists(output), "render label already exists; use a new immutable label");
    std::array<StereoBuffer, slugs.size()> audio {
        noiseBytes(quality), noiseBright(quality), vcaHighNotes(quality),
        performanceChords(quality), performanceRetriggers(quality), performanceUnison(quality),
        performanceHold(quality), cable(quality, 480, 7), cable(quality, 960, 8), {}
    };
    constexpr std::size_t montageIndex = slugs.size() - 1;
    constexpr std::array<std::size_t, 5> montageSections {0, 2, 3, 5, 8};
    for (std::size_t index = 0; index < montageSections.size(); ++index)
    {
        append(audio[montageIndex], audio[montageSections[index]]);
        if (index + 1 < montageSections.size()) silence(audio[montageIndex], 0.3);
    }
    const auto manifest = manifestJson(audio, label, revision, quality, YOUKNOW_DSP_SOURCE_SHA256);
    validateManifest(manifest, label, audio);
    for (std::size_t index = 0; index < slugs.size(); ++index)
        writeAudio(output / (std::string(slugs[index]) + "-raw.wav"), audio[index]);
    std::string error;
    require(writeText(output / "manifest.json", manifest, error), error);
    require(writeText(output / "score.md", R"score(All scenes use 48 kHz, 128-frame event-split blocks, settled thermal start, fixed reset seeds, the shipping ProductFidelityProfile, PolyZoned/Cubic/Rk4Single and zero velocity sensitivity. Each scene discards 0.25 s pre-roll, then compensates only its declared integer engine latency.

Requested new cable/mode APIs are absent in the frozen baseline build; its manifest records 000. The final build records the available APIs. Cable scenes use 0 pF on baseline and the requested 480/960 pF on final. Performance scenes request Original Performance on final and use the historical direct note adapter on baseline. These are modeled alternatives, not hardware captures.

The 19.99 s montage starts noise bytes at 0.00 s, high-note VCA at 4.52 s, original chord scan at 8.60 s, unison at 13.05 s, and the 6 m cable at 16.52 s.

- 01 main noise bytes, 4.22 s: note/gate starts at 0.12 s; byte 0 at 0.12, 1 at 0.62, 4 at 1.12, 6 at 1.62, 16 at 2.42, 32 at 2.92, and 127 at 3.42 s; gate off at 3.92 s. Byte 6 exposes the soft-junction onset.
- 02 bright noise, 3.32 s: byte 16, open filter, saw/pulse/sub off; note from 0.12 to 2.92 s.
- 03 high-note VCA, 3.78 s: saw/pulse/sub, open filter; MIDI chord 84/91/96 at 0.12 s; six fast gate strokes 96/108/117/123/103/91 from 1.16 s; chord 84/91/96/103 at 2.48 s. The MIDI 123 stroke exposes the stronger third-harmonic VCA fold; the chord passages retain the musical body.
- 04 original chords, 4.15 s: chords at 0.12/1.13/2.14/3.15 s, in identical ascending host insertion order.
- 05 retriggers, 3.636 s: repeated MIDI 60/67/64 notes from 0.12 s; six 1 ms notes from 2.10 s; long MIDI 72 at 2.436 s.
- 06 unison, 3.17 s: MIDI 48/55/60/55/48 starts at 0.12/0.62/1.12/1.62/2.12 s, held for 0.38 s each.
- 07 HOLD, 3.78 s: pedal on at 0.12 s, two chords released under HOLD; pedal off at 1.68 s; final chord at 2.28 s.
- 08/09 cables, 3.47 s: High output and 47 kOhm receiver; MIDI 60/84/96 at 0.12/0.82/1.52 s; chord 60/67/72 at 2.22 s; 3 m / 480 pF or 6 m / 960 pF.
)score", error), error);
    std::cout << "Frozen " << label << ": " << audio[montageIndex].left.size()
              << " montage frames (" << audio[montageIndex].left.size() / 48000.0 << " s)\n";
}

void compare(const std::filesystem::path& directory,
             const std::string& beforeLabel, const std::string& afterLabel)
{
    const auto output = directory / "listen";
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
        << "\",\"after\":\"" << afterLabel << "\",\"rms_matching\":\"whole-file stereo\","
        << "\"playback_encoding\":\"PCM24, no dither or extra gain\","
        << "\"matched_metrics_domain\":\"float before PCM24 quantization\","
        << "\"before_dsp_source_sha256\":\"" << manifestString(beforeManifest, "dsp_source_sha256") << "\","
        << "\"after_dsp_source_sha256\":\"" << manifestString(afterManifest, "dsp_source_sha256") << "\","
        << "\"renderer_source_sha256\":\"" << manifestString(beforeManifest, "renderer_source_sha256") << "\","
        << "\"sections\":{\n";
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
        // Leave enough PCM24 headroom for even an opposite-polarity B-A
        // difference. Both matched takes still sit below the -6dBFS limit.
        const double sharedGain = std::min(listeningTargetPeak, .49)
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
        writePlayback(sectionOutput / "before.wav", matchedBefore);
        writePlayback(sectionOutput / "after.wav", matchedAfter);
        writePlayback(sectionOutput / "A.wav", matchedBefore);
        writePlayback(sectionOutput / "B.wav", matchedAfter);
        writeAudio(sectionOutput / "difference-raw.wav", rawDelta);
        writePlayback(sectionOutput / "difference.wav", delta);
        writePlayback(sectionOutput / "difference-audible.wav", applyGain(delta, audibleGain));
        // Convenient direct listen: one complete before, 0.5 s silence,
        // one complete after. Identical section offsets within each half.
        StereoBuffer audition = matchedBefore;
        silence(audition, 0.5);
        append(audition, matchedAfter);
        writePlayback(sectionOutput / "before-then-after.wav", audition);
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
    std::cout << "Native PCM24 review files: " << std::filesystem::absolute(output) << '\n';
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
    auto wrongApis = after;
    const auto apiPosition = wrongApis.find("\"available_feature_apis\":\"") + 26;
    wrongApis[apiPosition] = '2';
    reject([&] { validateManifest(wrongApis, "after", audio); }, "malformed feature capabilities");
    auto wrongLatency = after;
    const auto latencyPosition = wrongLatency.find("\"declared_latency_samples\":\"") + 28;
    wrongLatency.insert(latencyPosition, "-1,");
    reject([&] { validateManifest(wrongLatency, "after", audio); }, "malformed latency declaration");
    const StereoBuffer pcmTest { { -1.f, 0.f, .5f }, { 1.f, -.5f, .25f } };
    const auto pcm = playbackBytes(pcmTest);
    require(pcm.size() == 62 && readU32(pcm.data() + 4) == 54
        && readU16(pcm.data() + 20) == 1 && readU16(pcm.data() + 22) == 2
        && readU32(pcm.data() + 24) == 48000 && readU16(pcm.data() + 32) == 6
        && readU16(pcm.data() + 34) == 24 && readU32(pcm.data() + 40) == 18,
        "PCM24 playback header is invalid");
    // Hand-written signed little-endian codes verify stereo ordering, sign,
    // full scale and fractional values independently of the encoder loop.
    constexpr std::array<std::uint8_t, 18> expectedPcm {
        0x01, 0x00, 0x80, 0xff, 0xff, 0x7f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x40, 0x00, 0x00, 0x20
    };
    require(std::equal(expectedPcm.begin(), expectedPcm.end(), pcm.begin() + 44),
        "PCM24 playback sample encoding is invalid");
    auto clippingPcm = pcmTest;
    clippingPcm.left[0] = 1.000001f;
    reject([&] { playbackBytes(clippingPcm); }, "clipping PCM24 playback");
    auto nonfinitePcm = pcmTest;
    nonfinitePcm.right[0] = std::numeric_limits<float>::quiet_NaN();
    reject([&] { playbackBytes(nonfinitePcm); }, "non-finite PCM24 playback");
    auto unevenPcm = pcmTest;
    unevenPcm.right.pop_back();
    reject([&] { playbackBytes(unevenPcm); }, "unequal PCM24 channels");
    std::cout << "Realism review manifest contract: valid pair passed; " << rejected
              << " invalid comparisons/playback inputs rejected; PCM24 byte oracle passed.\n";
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
        require(argc >= 5, "usage: YouKnowRenderRealismRound2 render dir label git-revision [1|4] | compare dir before-label after-label");
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
