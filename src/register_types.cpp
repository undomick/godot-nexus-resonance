#include "register_types.h"
#include "resonance_ambisonic_player.h"
#include "resonance_audio_effect.h"
#include "resonance_coda_event_emitter.h"
#include "resonance_dynamic_geometry.h"
#include "resonance_fmod_bridge.h"
#include "resonance_fmod_event_emitter.h"
#include "resonance_geometry.h"
#include "resonance_geometry_asset.h"
#include "resonance_listener.h"
#include "resonance_material.h"
#include "resonance_player.h"
#include "resonance_probe_data.h"
#include "resonance_probe_volume.h"
#include "resonance_runtime.h"
#include "resonance_server.h"
#include "resonance_sofa_asset.h"
#include "resonance_static_geometry.h"
#include "resonance_static_scene.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static ResonanceServer* _resonance_server = nullptr;

void initialize_nexus_resonance_module(godot::ModuleInitializationLevel p_level) {
    if (p_level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        // Core Classes
        ClassDB::register_class<ResonanceServer>();
        ClassDB::register_class<ResonanceFMODBridge>();
        ClassDB::register_class<ResonanceMaterial>();
        ClassDB::register_class<ResonanceGeometryAsset>();
        ClassDB::register_class<ResonanceSOFAAsset>();
        ClassDB::register_class<ResonanceGeometry>();
        ClassDB::register_class<ResonanceStaticGeometry>();
        ClassDB::register_class<ResonanceDynamicGeometry>();
        ClassDB::register_class<ResonanceStaticScene>();
        ClassDB::register_class<ResonanceListener>();
        ClassDB::register_class<ResonanceFmodEventEmitter>();
        ClassDB::register_class<ResonanceCodaEventEmitter>();
        ClassDB::register_class<ResonanceProbeData>();

        // Audio Effect Registration
        ClassDB::register_class<ResonanceAudioEffect>();
        ClassDB::register_internal_class<ResonanceAudioEffectInstance>();

        // Player & Internal DSP
        ClassDB::register_class<ResonancePlayer>();
        ClassDB::register_internal_class<ResonanceStream>();
        ClassDB::register_internal_class<ResonanceStreamPlayback>();
        ClassDB::register_class<ResonanceReverbStream>();
        ClassDB::register_internal_class<ResonanceReverbPlayback>();

        // Ambisonics
        ClassDB::register_class<ResonanceAmbisonicPlayer>();
        ClassDB::register_class<ResonanceAmbisonicInternalStream>();
        ClassDB::register_internal_class<ResonanceAmbisonicInternalPlayback>();

        // Probes
        ClassDB::register_class<ResonanceProbeVolume>();

        // Runtime orchestrator (native port of the runtime node)
        ClassDB::register_class<ResonanceRuntime>();

        _resonance_server = memnew(ResonanceServer);
        Engine::get_singleton()->register_singleton("ResonanceServer", ResonanceServer::get_singleton());
    }
}

void uninitialize_nexus_resonance_module(godot::ModuleInitializationLevel p_level) {
    if (p_level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        // Shutdown Steam Audio BEFORE unregistering/destroying. Ensures clean teardown order
        // and avoids SIGSEGV when AudioServer/engine still reference the server during exit.
        if (_resonance_server) {
            _resonance_server->shutdown();
        }
        Engine* eng = Engine::get_singleton();
        if (eng && eng->has_singleton("ResonanceServer")) {
            eng->unregister_singleton("ResonanceServer");
        }
        if (_resonance_server) {
            memdelete(_resonance_server);
            _resonance_server = nullptr;
        }
    }
}

extern "C" {
GDExtensionBool GDE_EXPORT nexus_resonance_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization* r_initialization) {
    godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_nexus_resonance_module);
    init_obj.register_terminator(uninitialize_nexus_resonance_module);
    init_obj.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}