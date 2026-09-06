// Load the shipping CLAP binary through its public C ABI. This catches missing
// exports, wrong format metadata and wrapper MIDI/audio failures without
// linking the host probe to JUCE or opening an editor.
#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

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

struct Host
{
    const std::thread::id mainThread = std::this_thread::get_id();
    bool processing = false;
    std::atomic<bool> callbackRequested { false };

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

    static const void* CLAP_ABI getExtension (const clap_host_t*, const char* id)
    {
        static const clap_host_thread_check_t threads { isMainThread, isAudioThread };
        return matches (id, CLAP_EXT_THREAD_CHECK) ? &threads : nullptr;
    }

    static void CLAP_ABI request (const clap_host_t*) {}
    static void CLAP_ABI requestCallback (const clap_host_t* host)
    {
        from (host).callbackRequested.store (true);
    }

    clap_host_t interface { CLAP_VERSION, this, "YouKnow CLAP smoke test",
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

    clap_input_events_t interface { this, size, get };
};
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

        Host host;
        const auto* plugin = factory->create_plugin (factory, &host.interface, descriptor->id);
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
                                 &events.interface, &outputEvents };
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
        std::cout << "CLAP descriptor, lifecycle and MIDI rendering passed (peak "
                  << peak << ")\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
