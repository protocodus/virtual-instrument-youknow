// Load the shipping CLAP binary through its public C ABI. This catches missing
// exports, wrong metadata, state notifications and wrapper MIDI/audio failures without
// linking the host probe to JUCE or opening an editor.
#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
 #define NOMINMAX
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

namespace
{
void require (bool condition, const char* message)
{
    if (! condition)
        throw std::runtime_error (message);
}

bool matches (const char* actual, const char* expected)
{
    return actual != nullptr && std::strcmp (actual, expected) == 0;
}

class Library
{
public:
    explicit Library (const char* path)
    {
#if defined(_WIN32)
        handle = LoadLibraryW (std::filesystem::path (path).c_str());
#else
        handle = dlopen (path, RTLD_NOW | RTLD_LOCAL);
#endif
        require (handle != nullptr, "could not load the CLAP binary");
    }

    ~Library()
    {
#if defined(_WIN32)
        FreeLibrary (handle);
#else
        dlclose (handle);
#endif
    }

    const clap_plugin_entry_t* entry() const
    {
#if defined(_WIN32)
        return reinterpret_cast<const clap_plugin_entry_t*> (
            GetProcAddress (handle, "clap_entry"));
#else
        return static_cast<const clap_plugin_entry_t*> (dlsym (handle, "clap_entry"));
#endif
    }

private:
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

struct EntryLifetime
{
    const clap_plugin_entry_t* entry;
    ~EntryLifetime() { entry->deinit(); }
};

struct InstanceLifetime
{
    const clap_plugin_t* plugin;
    bool active = false;
    ~InstanceLifetime()
    {
        if (active)
            plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
};

struct StateStream
{
    std::vector<uint8_t> bytes;
    std::size_t position = 0;
    uint64_t maximumTransfer = std::numeric_limits<uint64_t>::max();

    static int64_t CLAP_ABI write (const clap_ostream_t* stream,
                                   const void* data, uint64_t size)
    {
        auto& self = *static_cast<StateStream*> (stream->ctx);
        const auto count = std::min (size, self.maximumTransfer);
        const auto* source = static_cast<const uint8_t*> (data);
        self.bytes.insert (self.bytes.end(), source, source + count);
        return static_cast<int64_t> (count);
    }

    static int64_t CLAP_ABI read (const clap_istream_t* stream,
                                  void* data, uint64_t size)
    {
        auto& self = *static_cast<StateStream*> (stream->ctx);
        const auto count = std::min ({ size, self.maximumTransfer,
                                      static_cast<uint64_t> (self.bytes.size() - self.position) });
        std::memcpy (data, self.bytes.data() + self.position,
                     static_cast<std::size_t> (count));
        self.position += static_cast<std::size_t> (count);
        return static_cast<int64_t> (count);
    }

    clap_ostream_t output { this, write };
    clap_istream_t input { this, read };
};

struct Host
{
    const std::thread::id mainThread = std::this_thread::get_id();
    bool processing = false;
    std::atomic<bool> callbackRequested { false };
    clap_param_rescan_flags rescanFlags = 0;
    bool rescanOnMainThread = true;
    bool stateLoadInProgress = false;
    bool rescannedDuringLoad = false;
    bool rescanSaveSucceeded = false;
    const clap_plugin_t* loadedPlugin = nullptr;
    StateStream* rescanSnapshot = nullptr;

    static Host& from (const clap_host_t* host)
    {
        return *static_cast<Host*> (host->host_data);
    }

    static bool CLAP_ABI isMainThread (const clap_host_t* host)
    {
        return std::this_thread::get_id() == from (host).mainThread;
    }

    static bool CLAP_ABI isAudioThread (const clap_host_t* host)
    {
        return isMainThread (host) && from (host).processing;
    }

    static void CLAP_ABI rescan (const clap_host_t* host, clap_param_rescan_flags flags)
    {
        auto& self = from (host);
        self.rescanFlags |= flags;
        self.rescanOnMainThread = self.rescanOnMainThread && isMainThread (host);
        self.rescannedDuringLoad = self.rescannedDuringLoad || self.stateLoadInProgress;
        if (self.rescanSnapshot != nullptr)
        {
            self.rescanSnapshot->bytes.clear();
            const auto* state = static_cast<const clap_plugin_state_t*> (
                self.loadedPlugin->get_extension (self.loadedPlugin, CLAP_EXT_STATE));
            self.rescanSaveSucceeded = state != nullptr
                && state->save (self.loadedPlugin, &self.rescanSnapshot->output);
        }
    }

    static void CLAP_ABI clear (const clap_host_t*, clap_id, clap_param_clear_flags) {}

    static const void* CLAP_ABI getExtension (const clap_host_t*, const char* id)
    {
        static const clap_host_thread_check_t threads { isMainThread, isAudioThread };
        static const clap_host_params_t params { rescan, clear, request };
        if (matches (id, CLAP_EXT_THREAD_CHECK))
            return &threads;
        return matches (id, CLAP_EXT_PARAMS) ? &params : nullptr;
    }

    static void CLAP_ABI request (const clap_host_t*) {}
    static void CLAP_ABI requestCallback (const clap_host_t* host)
    {
        from (host).callbackRequested.store (true);
    }

    // Avoid `interface`, which the Windows SDK defines as a macro.
    clap_host_t clapHost { CLAP_VERSION, this, "YouKnow CLAP smoke test",
                          "Protocodus", "https://protocodus.cz", "1.0",
                          getExtension, request, request, requestCallback };
};

bool hasFeature (const clap_plugin_descriptor_t& descriptor, const char* feature)
{
    if (descriptor.features != nullptr)
        for (auto current = descriptor.features; *current != nullptr; ++current)
            if (matches (*current, feature))
                return true;
    return false;
}

struct Events
{
    clap_event_midi_t midi { { sizeof (clap_event_midi_t), 0,
                              CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 },
                            0, { 0x90, 60, 100 } };
    bool present = false;

    static uint32_t CLAP_ABI size (const clap_input_events_t* list)
    {
        return static_cast<const Events*> (list->ctx)->present ? 1u : 0u;
    }

    static const clap_event_header_t* CLAP_ABI get (const clap_input_events_t* list,
                                                    uint32_t index)
    {
        const auto& events = *static_cast<const Events*> (list->ctx);
        return events.present && index == 0 ? &events.midi.header : nullptr;
    }

    clap_input_events_t inputEvents { this, size, get };
};

void serviceCallbacks (Host& host, const clap_plugin_t* plugin)
{
    for (int count = 0; count < 16 && host.callbackRequested.exchange (false); ++count)
        plugin->on_main_thread (plugin);
}

void testStateRoundTrip (const clap_plugin_factory_t* factory,
                        const clap_plugin_descriptor_t* descriptor, bool buffered)
{
    StateStream saved;
    saved.maximumTransfer = buffered ? 23 : saved.maximumTransfer;
    std::vector<std::pair<clap_id, double>> expectedValues;
    {
        Host host;
        const auto* plugin = factory->create_plugin (factory, &host.clapHost, descriptor->id);
        require (plugin != nullptr, "CLAP state source instantiation failed");
        InstanceLifetime instance { plugin };
        require (plugin->init (plugin), "CLAP state source initialization failed");
        serviceCallbacks (host, plugin);
        const auto* params = static_cast<const clap_plugin_params_t*> (
            plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        const auto* state = static_cast<const clap_plugin_state_t*> (
            plugin->get_extension (plugin, CLAP_EXT_STATE));
        require (params != nullptr && state != nullptr, "CLAP parameter/state extension missing");

        unsigned changed = 0;
        for (uint32_t index = 0; index < params->count (plugin); ++index)
        {
            clap_param_info_t info {};
            require (params->get_info (plugin, index, &info), "CLAP parameter info failed");
            if (! matches (info.name, "VCF Freq") && ! matches (info.name, "VCA Level"))
                continue;
            clap_event_param_value_t event {
                { sizeof (clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                  CLAP_EVENT_PARAM_VALUE, 0 },
                info.id, info.cookie, -1, -1, -1, -1,
                info.min_value + (info.max_value - info.min_value) * 0.37 };
            clap_input_events_t input {
                &event, [] (const clap_input_events_t*) -> uint32_t { return 1; },
                [] (const clap_input_events_t* list, uint32_t eventIndex)
                    -> const clap_event_header_t*
                {
                    return eventIndex == 0
                        ? &static_cast<clap_event_param_value_t*> (list->ctx)->header : nullptr;
                } };
            const clap_output_events_t output {
                nullptr, [] (const clap_output_events_t*, const clap_event_header_t*) -> bool
                { return true; } };
            params->flush (plugin, &input, &output);
            ++changed;
        }
        require (changed == 2, "CLAP state fixture did not find both edited controls");
        serviceCallbacks (host, plugin);
        for (uint32_t index = 0; index < params->count (plugin); ++index)
        {
            clap_param_info_t info {};
            double value = 0.0;
            require (params->get_info (plugin, index, &info)
                         && params->get_value (plugin, info.id, &value),
                     "CLAP source parameter read failed");
            expectedValues.emplace_back (info.id, value);
        }
        require (state->save (plugin, &saved.output) && ! saved.bytes.empty(),
                 "CLAP state save failed");
    }

    Host host;
    const auto* plugin = factory->create_plugin (factory, &host.clapHost, descriptor->id);
    require (plugin != nullptr, "CLAP state destination instantiation failed");
    InstanceLifetime instance { plugin };
    require (plugin->init (plugin), "CLAP state destination initialization failed");
    serviceCallbacks (host, plugin);
    const auto* params = static_cast<const clap_plugin_params_t*> (
        plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    const auto* state = static_cast<const clap_plugin_state_t*> (
        plugin->get_extension (plugin, CLAP_EXT_STATE));
    require (params != nullptr && state != nullptr, "CLAP restored extensions missing");
    unsigned different = 0;
    for (const auto& [id, expected] : expectedValues)
    {
        double value = 0.0;
        require (params->get_value (plugin, id, &value), "CLAP fresh parameter read failed");
        different += std::abs (value - expected) > 1.0e-7 ? 1u : 0u;
    }
    require (different >= 2, "CLAP state fixture must restore changed parameter values");

    // CLAP's params.h preset scenario requires host.params.rescan when values
    // change. The pinned JUCE wrapper translates programChanged into VALUES.
    // CLAP 29ffcc273be7c7c651f6c9953b99e69700e2387a, ext/params.h;
    // clap-juce-extensions c1a5ad025f95d01e03267857fa8276ebeed16500,
    // src/wrapper/clap-juce-wrapper.cpp audioProcessorChanged/stateLoad.
    // Its callback is synchronous on the main thread: a host saving there must
    // see the complete transaction, without spinning on an unfinished write.
    StateStream reentrantSnapshot;
    host.loadedPlugin = plugin;
    host.rescanSnapshot = &reentrantSnapshot;
    host.rescanFlags = 0;
    host.stateLoadInProgress = true;
    saved.maximumTransfer = buffered ? 17 : std::numeric_limits<uint64_t>::max();
    const bool loaded = state->load (plugin, &saved.input);
    host.stateLoadInProgress = false;
    serviceCallbacks (host, plugin);
    require (loaded, "CLAP state load failed");
    require ((host.rescanFlags & CLAP_PARAM_RESCAN_VALUES) != 0,
             "CLAP state load changed values without a host rescan");
    require (host.rescanOnMainThread && host.rescannedDuringLoad,
             "CLAP state rescan did not run synchronously on the main thread");
    require (host.rescanSaveSucceeded && reentrantSnapshot.bytes == saved.bytes,
             "CLAP host rescan could not save the complete restored state");
    host.rescanSnapshot = nullptr;
    require (params->count (plugin) == expectedValues.size(),
             "CLAP state load changed the parameter count");
    for (const auto& [id, expected] : expectedValues)
    {
        double value = 0.0;
        require (params->get_value (plugin, id, &value)
                     && std::abs (value - expected) <= 1.0e-7,
                 "CLAP restored parameter differs from its saved value");
    }
    StateStream restored;
    require (state->save (plugin, &restored.output) && restored.bytes == saved.bytes,
             "CLAP restored state is not byte-for-byte reproducible");

    StateStream invalid;
    invalid.bytes = { 1, 2, 3, 4, 5, 6, 7, 8 };
    host.rescanFlags = 0;
    // JUCE's void restore API prevents the wrapper reporting malformed chunks
    // as false. They must still leave the processor and host cache untouched.
    state->load (plugin, &invalid.input);
    serviceCallbacks (host, plugin);
    StateStream afterInvalid;
    require (host.rescanFlags == 0 && state->save (plugin, &afterInvalid.output)
                 && afterInvalid.bytes == saved.bytes,
             "CLAP rejected state changed values or notified the host");
}
} // namespace

int main (int argc, char** argv)
{
    try
    {
        require (argc == 3, "expected CLAP binary path and product version");
        Library library { argv[1] };
        const auto* entry = library.entry();
        require (entry != nullptr, "CLAP binary does not export clap_entry");
        require (clap_version_is_compatible (entry->clap_version), "incompatible CLAP ABI");
        require (entry->init (argv[1]), "CLAP entry initialization failed");
        EntryLifetime entryLifetime { entry };
        const auto* factory = static_cast<const clap_plugin_factory_t*> (
            entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
        require (factory != nullptr, "CLAP plugin factory is missing");
        require (factory->get_plugin_count (factory) == 1, "expected one CLAP instrument");
        const auto* descriptor = factory->get_plugin_descriptor (factory, 0);
        require (descriptor != nullptr, "CLAP descriptor is missing");
        require (matches (descriptor->id, "cz.protocodus.youknow"), "wrong stable CLAP ID");
        require (matches (descriptor->name, "YouKnow"), "wrong CLAP product name");
        require (matches (descriptor->vendor, "Protocodus"), "wrong CLAP vendor");
        require (matches (descriptor->version, argv[2]), "wrong CLAP version");
        require (hasFeature (*descriptor, CLAP_PLUGIN_FEATURE_INSTRUMENT)
                     && hasFeature (*descriptor, CLAP_PLUGIN_FEATURE_SYNTHESIZER),
                 "CLAP instrument/synthesizer features are missing");

        testStateRoundTrip (factory, descriptor, false);
        testStateRoundTrip (factory, descriptor, true);

        Host host;
        const auto* plugin = factory->create_plugin (factory, &host.clapHost, descriptor->id);
        require (plugin != nullptr, "CLAP instantiation failed");
        InstanceLifetime instance { plugin };
        require (plugin->init (plugin), "CLAP plugin initialization failed");
        const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*> (
            plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS));
        require (audioPorts != nullptr, "CLAP audio ports extension is missing");
        require (audioPorts->count (plugin, true) == 0
                     && audioPorts->count (plugin, false) == 1,
                 "CLAP must expose one output and no audio input");
        clap_audio_port_info_t audioInfo {};
        require (audioPorts->get (plugin, 0, false, &audioInfo)
                     && audioInfo.channel_count == 2,
                 "CLAP output must be stereo");
        const auto* notePorts = static_cast<const clap_plugin_note_ports_t*> (
            plugin->get_extension (plugin, CLAP_EXT_NOTE_PORTS));
        require (notePorts != nullptr && notePorts->count (plugin, true) == 1
                     && notePorts->count (plugin, false) == 0,
                 "CLAP MIDI input/output capabilities are wrong");
        clap_note_port_info_t noteInfo {};
        require (notePorts->get (plugin, 0, true, &noteInfo)
                     && (noteInfo.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0,
                 "CLAP input does not accept MIDI");

        constexpr uint32_t blockSize = 256;
        require (plugin->activate (plugin, 48000.0, blockSize, blockSize),
                 "CLAP activation failed");
        instance.active = true;
        // CLAP allows the main OS thread to serve as its symbolic audio thread
        // provided audio callbacks never run concurrently for this instance.
        host.processing = true;
        const bool started = plugin->start_processing (plugin);
        host.processing = false;
        require (started, "CLAP start_processing failed");

        std::array<float, blockSize> left {}, right {};
        float* channels[] { left.data(), right.data() };
        clap_audio_buffer_t output { channels, nullptr, 2, 0, 0 };
        Events events;
        const clap_output_events_t outputEvents {
            nullptr, [] (const clap_output_events_t*, const clap_event_header_t*) -> bool
            { return true; } };
        clap_process_t process { 0, blockSize, nullptr, nullptr, &output, 0, 1,
                                 &events.inputEvents, &outputEvents };
        bool finite = true;
        bool succeeded = true;
        float peak = 0.0f;
        for (int block = 0; block < 32; ++block)
        {
            left.fill (0.0f);
            right.fill (0.0f);
            events.present = block == 0 || block == 31;
            events.midi.data[0] = block == 31 ? 0x80 : 0x90;
            output.constant_mask = 0;
            host.processing = true;
            succeeded = plugin->process (plugin, &process) != CLAP_PROCESS_ERROR && succeeded;
            host.processing = false;
            process.steady_time += blockSize;
            if (host.callbackRequested.exchange (false))
                plugin->on_main_thread (plugin);
            for (auto* channel : channels)
                for (uint32_t sample = 0; sample < blockSize; ++sample)
                {
                    finite = std::isfinite (channel[sample]) && finite;
                    peak = std::max (peak, std::abs (channel[sample]));
                }
        }
        host.processing = true;
        plugin->stop_processing (plugin);
        host.processing = false;
        require (succeeded, "CLAP process returned an error");
        require (finite, "CLAP rendered non-finite audio");
        require (peak > 1.0e-5f, "CLAP rendered silence for a MIDI note");
        std::cout << "CLAP descriptor, state/rescan, lifecycle and MIDI rendering passed (peak "
                  << peak << ")\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
