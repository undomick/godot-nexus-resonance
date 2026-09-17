#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_runtime_config_policy.h"

using namespace resonance;

TEST_CASE("cfg-01 audit fields require engine reinit", "[runtime_config][cfg-01]") {
    const char* audit_fields[] = {
        "ambisonic_order",
        "scene_type",
        "hrtf_sofa_selected_index",
        "hrtf_volume_db",
        "realtime_simulation_duration",
        "reverb_binaural",
        "reflection_type",
    };
    for (const char* name : audit_fields) {
        REQUIRE(runtime_config_property_requires_engine_reinit(name));
    }
}

TEST_CASE("sample_rate and audio_frame_size are not user reinit properties", "[runtime_config]") {
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("sample_rate_override"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("audio_frame_size"));
}

TEST_CASE("sim scheduling intervals are live-patchable", "[runtime_config][cfg-01]") {
    REQUIRE(runtime_config_property_is_live_patchable("reflections_sim_interval"));
    REQUIRE(runtime_config_property_is_live_patchable("pathing_sim_interval"));
    REQUIRE(runtime_config_property_is_live_patchable("direct_sim_interval"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("reflections_sim_interval"));
}

TEST_CASE("routing buses refresh without engine reinit", "[runtime_config]") {
    REQUIRE(runtime_config_property_requires_routing_refresh("bus"));
    REQUIRE(runtime_config_property_requires_routing_refresh("reverb_bus_name"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("bus"));
}

TEST_CASE("live dict keys include baked probe mode", "[runtime_config]") {
    REQUIRE(runtime_config_property_is_live_patchable("perspective_correction_enabled"));
    REQUIRE(runtime_config_dict_key_is_live_patchable("perspective_correction_factor"));
    REQUIRE_FALSE(runtime_config_dict_key_is_live_patchable("ambisonic_order"));
    REQUIRE_FALSE(runtime_config_property_is_live_patchable("reflections_sampling_mode"));
    REQUIRE_FALSE(runtime_config_dict_key_is_live_patchable("baked_reverb_use_listener_probe"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("pathing_num_vis_samples"));
    REQUIRE(runtime_config_property_requires_engine_reinit("pathing_num_samples"));
    REQUIRE(runtime_config_property_is_live_patchable("pathing_vis_range"));
    REQUIRE(runtime_config_property_is_live_patchable("pathing_vis_radius"));
    REQUIRE(runtime_config_property_is_live_patchable("pathing_vis_threshold"));
    REQUIRE(runtime_config_dict_key_is_live_patchable("pathing_vis_range"));
    REQUIRE_FALSE(runtime_config_property_is_live_patchable("pathing_path_range"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("pathing_vis_range"));
    REQUIRE(runtime_config_property_is_live_patchable("pathing_enabled"));
    REQUIRE(runtime_config_property_is_live_patchable("pathing_normalize_eq"));
    REQUIRE(runtime_config_property_is_live_patchable("path_validation_enabled"));
    REQUIRE(runtime_config_property_is_live_patchable("find_alternate_paths"));
    REQUIRE(runtime_config_dict_key_is_live_patchable("pathing_enabled"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("pathing_enabled"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("pathing_normalize_eq"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("path_validation_enabled"));
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("find_alternate_paths"));
}

TEST_CASE("bake_ambisonic_order does not require engine reinit", "[runtime_config]") {
    REQUIRE_FALSE(runtime_config_property_requires_engine_reinit("bake_ambisonic_order"));
}
