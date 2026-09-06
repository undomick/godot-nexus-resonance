// GDExtension class registration for ResonancePlayer-related types (split from resonance_player.cpp).

#include "resonance_player.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void ResonanceStreamPlayback::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_base_playback"), &ResonanceStreamPlayback::get_base_playback);
    ClassDB::bind_method(D_METHOD("get_inner_stream_playback"), &ResonanceStreamPlayback::get_inner_stream_playback);
}

void ResonanceStream::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_base_stream", "stream"), &ResonanceStream::set_base_stream);
    ClassDB::bind_method(D_METHOD("get_base_stream"), &ResonanceStream::get_base_stream);
    ClassDB::bind_method(D_METHOD("get_inner_stream"), &ResonanceStream::get_inner_stream);
}

void ResonanceReverbPlayback::_bind_methods() {
}

void ResonanceReverbStream::_bind_methods() {
}

void ResonancePlayer::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_stream", "stream"), &ResonancePlayer::set_stream);
    ClassDB::bind_method(D_METHOD("get_stream"), &ResonancePlayer::get_stream);
    ClassDB::bind_method(D_METHOD("get_inner_stream"), &ResonancePlayer::get_inner_stream);
    ClassDB::bind_method(D_METHOD("get_inner_stream_playback"), &ResonancePlayer::get_inner_stream_playback);
    ClassDB::bind_method(D_METHOD("play", "from_position"), &ResonancePlayer::play, DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("stop"), &ResonancePlayer::stop);
    ClassDB::bind_method(D_METHOD("play_stream", "from_position"), &ResonancePlayer::play_stream, DEFVAL(0.0));
    ClassDB::bind_method(D_METHOD("play_animation_audio_clip", "stream", "from_position"), &ResonancePlayer::play_animation_audio_clip,
                         DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("_deferred_try_ensure_source_after_config"), &ResonancePlayer::_deferred_try_ensure_source_after_config);
    ClassDB::bind_method(D_METHOD("reload_source_after_reinit"), &ResonancePlayer::reload_source_after_reinit);
    ClassDB::bind_method(D_METHOD("set_pathing_probe_volume", "p_path"), &ResonancePlayer::set_pathing_probe_volume);
    ClassDB::bind_method(D_METHOD("get_pathing_probe_volume"), &ResonancePlayer::get_pathing_probe_volume);
    ClassDB::bind_method(D_METHOD("set_exclude_from_debug", "p_exclude"), &ResonancePlayer::set_exclude_from_debug);
    ClassDB::bind_method(D_METHOD("get_exclude_from_debug"), &ResonancePlayer::get_exclude_from_debug);
    ClassDB::bind_method(D_METHOD("set_auto_exclude_colliders", "p_enable"), &ResonancePlayer::set_auto_exclude_colliders);
    ClassDB::bind_method(D_METHOD("get_auto_exclude_colliders"), &ResonancePlayer::get_auto_exclude_colliders);
    ClassDB::bind_method(D_METHOD("set_player_config", "p_config"), &ResonancePlayer::set_player_config);
    ClassDB::bind_method(D_METHOD("get_player_config"), &ResonancePlayer::get_player_config);
    ClassDB::bind_method(D_METHOD("set_reverb_split_output", "p_enable", "p_reverb_bus"), &ResonancePlayer::set_reverb_split_output, DEFVAL(StringName()));
    ClassDB::bind_method(D_METHOD("get_reverb_split_output"), &ResonancePlayer::get_reverb_split_output);
    ClassDB::bind_method(D_METHOD("get_audio_instrumentation"), &ResonancePlayer::get_audio_instrumentation);
    ClassDB::bind_method(D_METHOD("reset_audio_instrumentation"), &ResonancePlayer::reset_audio_instrumentation);
    ClassDB::bind_method(D_METHOD("get_effective_volume_linear_cached"), &ResonancePlayer::get_effective_volume_linear_cached);
    ClassDB::bind_method(D_METHOD("_deferred_push_playback_parameters"), &ResonancePlayer::_deferred_push_playback_parameters);
    ClassDB::bind_method(D_METHOD("_nexus_deferred_spawn_anim_audio_helper"), &ResonancePlayer::_nexus_deferred_spawn_anim_audio_helper);
    ClassDB::bind_method(D_METHOD("_nexus_deferred_emit_finished"), &ResonancePlayer::_nexus_deferred_emit_finished);
    ClassDB::bind_method(D_METHOD("set_convert_anim_audio_runtime", "p_enable"), &ResonancePlayer::set_convert_anim_audio_runtime);
    ClassDB::bind_method(D_METHOD("get_convert_anim_audio_runtime"), &ResonancePlayer::get_convert_anim_audio_runtime);
    ClassDB::bind_method(D_METHOD("set_show_directivity_gizmo", "p_enable"), &ResonancePlayer::set_show_directivity_gizmo);
    ClassDB::bind_method(D_METHOD("get_show_directivity_gizmo"), &ResonancePlayer::get_show_directivity_gizmo);
    ClassDB::bind_method(D_METHOD("_on_player_config_changed_refresh_gizmo"), &ResonancePlayer::_on_player_config_changed_refresh_gizmo);
    ClassDB::bind_static_method("ResonancePlayer",
                                D_METHOD("build_directivity_gizmo_lines", "enabled", "input_mode", "weight", "power", "user_value", "size"),
                                &ResonancePlayer::build_directivity_gizmo_lines);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "player_config", PROPERTY_HINT_RESOURCE_TYPE, "ResonancePlayerConfig"), "set_player_config", "get_player_config");
    ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "pathing_probe_volume", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "ResonanceProbeVolume"), "set_pathing_probe_volume", "get_pathing_probe_volume");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_exclude_colliders"), "set_auto_exclude_colliders", "get_auto_exclude_colliders");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "convert_anim_audio_runtime"), "set_convert_anim_audio_runtime", "get_convert_anim_audio_runtime");
    ADD_GROUP("Debug", "");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "exclude_from_debug"), "set_exclude_from_debug", "get_exclude_from_debug");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_directivity_gizmo"), "set_show_directivity_gizmo", "get_show_directivity_gizmo");

    BIND_ENUM_CONSTANT(ATTENUATION_INVERSE);
    BIND_ENUM_CONSTANT(ATTENUATION_LINEAR);
    BIND_ENUM_CONSTANT(ATTENUATION_CUSTOM_CURVE);
    BIND_ENUM_CONSTANT(ATTENUATION_DISABLED);
}
