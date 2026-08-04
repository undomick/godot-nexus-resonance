#include "resonance_fmod_bridge.h"
#include "resonance_constants.h"
#include "resonance_server.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace godot {

void ResonanceFMODBridge::_bind_methods() {
    ClassDB::bind_method(D_METHOD("init_bridge"), &ResonanceFMODBridge::init_bridge);
    ClassDB::bind_method(D_METHOD("shutdown_bridge"), &ResonanceFMODBridge::shutdown_bridge);
    ClassDB::bind_method(D_METHOD("rebind_after_reinit"), &ResonanceFMODBridge::rebind_after_reinit);
    ClassDB::bind_method(D_METHOD("is_bridge_loaded"), &ResonanceFMODBridge::is_bridge_loaded);
    ClassDB::bind_method(D_METHOD("is_bridge_initialized"), &ResonanceFMODBridge::is_bridge_initialized);
    ClassDB::bind_method(D_METHOD("add_fmod_source", "resonance_source_handle"), &ResonanceFMODBridge::add_fmod_source);
    ClassDB::bind_method(D_METHOD("remove_fmod_source", "fmod_handle"), &ResonanceFMODBridge::remove_fmod_source);
}

ResonanceFMODBridge::~ResonanceFMODBridge() {
    shutdown_bridge();
}

void* ResonanceFMODBridge::get_proc(const char* name) {
#if defined(_WIN32) || defined(_WIN64)
    return (void*)GetProcAddress((HMODULE)plugin_handle_, name);
#else
    return dlsym(plugin_handle_, name);
#endif
}

bool ResonanceFMODBridge::load_plugin() {
    if (plugin_handle_ != nullptr)
        return true;

#if defined(_WIN32) || defined(_WIN64)
    const char* plugin_name = "phonon_fmod.dll";
#elif defined(__APPLE__)
    const char* plugin_name = "libphonon_fmod.dylib";
#else
    const char* plugin_name = "libphonon_fmod.so";
#endif

#if defined(_WIN32) || defined(_WIN64)
    plugin_handle_ = LoadLibraryA(plugin_name);
    if (!plugin_handle_) {
        // Try loading from next to executable (e.g. project root or addons/nexus_resonance/bin)
        char path[resonance::kFmodPluginMaxModulePathLen];
        if (GetModuleFileNameA(NULL, path, sizeof(path))) {
            std::string dir(path);
            size_t last = dir.find_last_of("/\\");
            if (last != std::string::npos)
                dir = dir.substr(0, last + 1);
            plugin_handle_ = LoadLibraryA((dir + plugin_name).c_str());
        }
        if (!plugin_handle_) {
            // Try fmod_plugin deploy path: addons/nexus_resonance/bin/fmod_plugin/windows-x64/
            plugin_handle_ = LoadLibraryA((std::string(resonance::kFmodPluginPathWindows) + plugin_name).c_str());
        }
    }
#else
    plugin_handle_ = dlopen(plugin_name, RTLD_NOW);
    if (!plugin_handle_) {
#if defined(__APPLE__)
        const char* paths[] = {
            resonance::kFmodPluginPathNexusOsx,
            resonance::kFmodPluginPathFmodOsx,
            resonance::kFmodPluginPathFmodLib,
            nullptr};
#else
        const char* paths[] = {
            resonance::kFmodPluginPathNexusLinux,
            resonance::kFmodPluginPathFmodLinux,
            resonance::kFmodPluginPathFmodLib,
            nullptr};
#endif
        for (int i = 0; paths[i]; i++) {
            std::string full = std::string(paths[i]) + plugin_name;
            plugin_handle_ = dlopen(full.c_str(), RTLD_NOW);
            if (plugin_handle_)
                break;
        }
    }
#endif

    if (!plugin_handle_)
        return false;

    fn_iplFMODInitialize_ = (void (*)(IPLContext))get_proc("iplFMODInitialize");
    fn_iplFMODTerminate_ = (void (*)())get_proc("iplFMODTerminate");
    fn_iplFMODSetHRTF_ = (void (*)(IPLHRTF))get_proc("iplFMODSetHRTF");
    fn_iplFMODSetSimulationSettings_ = (void (*)(IPLSimulationSettings))get_proc("iplFMODSetSimulationSettings");
    fn_iplFMODSetReverbSource_ = (void (*)(IPLSource))get_proc("iplFMODSetReverbSource");
    fn_iplFMODAddSource_ = (int32_t(*)(IPLSource))get_proc("iplFMODAddSource");
    fn_iplFMODRemoveSource_ = (void (*)(int32_t))get_proc("iplFMODRemoveSource");

    if (!fn_iplFMODInitialize_ || !fn_iplFMODTerminate_ || !fn_iplFMODSetHRTF_ ||
        !fn_iplFMODSetSimulationSettings_ || !fn_iplFMODSetReverbSource_ ||
        !fn_iplFMODAddSource_ || !fn_iplFMODRemoveSource_) {
        unload_plugin();
        return false;
    }
    return true;
}

void ResonanceFMODBridge::unload_plugin() {
    if (plugin_handle_) {
#if defined(_WIN32) || defined(_WIN64)
        FreeLibrary((HMODULE)plugin_handle_);
#else
        dlclose(plugin_handle_);
#endif
        plugin_handle_ = nullptr;
    }
    fn_iplFMODInitialize_ = nullptr;
    fn_iplFMODTerminate_ = nullptr;
    fn_iplFMODSetHRTF_ = nullptr;
    fn_iplFMODSetSimulationSettings_ = nullptr;
    fn_iplFMODSetReverbSource_ = nullptr;
    fn_iplFMODAddSource_ = nullptr;
    fn_iplFMODRemoveSource_ = nullptr;
    initialized_ = false;
}

bool ResonanceFMODBridge::init_bridge() {
    if (initialized_)
        return true;

    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized()) {
        UtilityFunctions::push_warning("ResonanceFMODBridge: ResonanceServer not initialized.");
        return false;
    }

    if (!load_plugin()) {
        UtilityFunctions::push_warning("ResonanceFMODBridge: Failed to load Steam Audio FMOD plugin. Ensure phonon_fmod.dll (or .so/.dylib) is in the FMOD plugin path.");
        return false;
    }

    IPLContext ctx = server->get_context_handle();
    IPLHRTF hrtf = server->get_hrtf_handle();
    const IPLSimulationSettings* sim_settings = server->get_simulation_settings_for_fmod();
    server->ensure_fmod_reverb_source();
    IPLSource reverb_src = nullptr;
    int32_t reverb_handle = server->get_fmod_reverb_source_handle();
    if (reverb_handle >= 0) {
        reverb_src = server->get_source_from_handle(reverb_handle);
    }

    if (!ctx || !hrtf || !sim_settings) {
        UtilityFunctions::push_warning("ResonanceFMODBridge: ResonanceServer context, HRTF or simulation settings not ready.");
        unload_plugin();
        return false;
    }

    fn_iplFMODInitialize_(ctx);
    fn_iplFMODSetHRTF_(hrtf);
    fn_iplFMODSetSimulationSettings_(*sim_settings);
    if (reverb_src) {
        fn_iplFMODSetReverbSource_(reverb_src);
    }

    initialized_ = true;
    UtilityFunctions::print_rich("[color=cyan]ResonanceFMODBridge:[/color] Initialized. Steam Audio FMOD plugins can use Nexus Resonance context.");
    return true;
}

void ResonanceFMODBridge::shutdown_bridge() {
    if (initialized_ && fn_iplFMODTerminate_) {
        fn_iplFMODTerminate_();
    }
    initialized_ = false;
    unload_plugin();
}

bool ResonanceFMODBridge::rebind_after_reinit() {
    // Engine reinit destroys the IPLContext that iplFMODInitialize retained. Terminate and
    // re-init against the new context; keep the shared library loaded to avoid dlopen churn.
    if (initialized_ && fn_iplFMODTerminate_) {
        fn_iplFMODTerminate_();
        initialized_ = false;
    }
    return init_bridge();
}

int32_t ResonanceFMODBridge::add_fmod_source(int32_t resonance_source_handle) {
    if (!initialized_ || !fn_iplFMODAddSource_ || resonance_source_handle < 0)
        return -1;

    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server)
        return -1;

    IPLSource src = server->get_source_from_handle(resonance_source_handle);
    if (!src)
        return -1;

    int32_t handle = fn_iplFMODAddSource_(src);
    return handle;
}

void ResonanceFMODBridge::remove_fmod_source(int32_t fmod_handle) {
    if (initialized_ && fn_iplFMODRemoveSource_ && fmod_handle >= 0) {
        fn_iplFMODRemoveSource_(fmod_handle);
    }
}

} // namespace godot
