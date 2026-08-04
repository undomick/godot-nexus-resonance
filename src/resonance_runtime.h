#ifndef RESONANCE_RUNTIME_H
#define RESONANCE_RUNTIME_H

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

class Viewport;
class Camera3D;
class ResonanceServer;

// C++ port of the Nexus Resonance runtime node. Orchestrates server init, config, the per-frame tick and debug UI.
//
// Helpers stay in GDScript and are driven from here: ResonanceRuntimeBus (bus wiring), ResonanceReverbActivator
// (wet-bus keep-alive), the FMOD/Coda bridges and the config/bake resources. They are instantiated via script.new()
// and held as Variant so the RefCounted instances stay alive for the node lifetime.
//
// Translation units: resonance_runtime.cpp (lifecycle + bus), resonance_runtime_tick.cpp (per-frame hot path),
// resonance_runtime_init.cpp (server init/reinit, config, signals, static scenes, bridges).
//
// Multiple ResonanceRuntime nodes may exist (scene handoff). Only the primary instance runs flush+tick;
// live_game_runtime_count gates ResonanceServer shutdown when the last live runtime exits.
class ResonanceRuntime : public Node {
    GDCLASS(ResonanceRuntime, Node)

  private:
    Ref<Resource> runtime;

    Variant runtime_bus;
    Variant reverb_activator;
    Variant fmod_bridge;
    Variant coda_bridge;

    // Config-affecting node exports (mirrors the GDScript runtime).
    bool fmod_bridge_enabled = false;
    bool coda_bridge_enabled = false;
    int context_simd_level = -1;
    bool context_validation = false;

    // Debug overlays + input (driven from resonance_runtime_debug.cpp). When enable_debug is off the overlay keys
    // do nothing (hide dev tools from players). player_overlay_visible (F3) also feeds debug_occlusion.
    bool enable_debug = false;
    bool debug_overlay_visible = false;
    bool performance_overlay_visible = false;
    bool player_overlay_visible = false;
    int64_t debug_overlay_toggle_key = KEY_F1;
    int64_t performance_overlay_toggle_key = KEY_F2;
    int64_t player_overlay_toggle_key = KEY_F3;

    // Debugger performance monitors + audio-aggregate sampling (GDScript ResonanceRuntimePerfMonitors instance).
    Variant perf_monitors;

    bool static_scene_reload_pending = false;

    // Live game runtimes; server shutdown only when this reaches 0.
    static int live_game_runtime_count;
    // Instance that owns flush+tick (last _ready wins; elect on exit).
    static ObjectID primary_runtime_id;

    // Gates per-phase frame timing (Full only). Standard by default.
    int performance_custom_monitors = 2;

    // Per-frame timing snapshot (telemetry; read by ResonanceRuntimePerfMonitors).
    int64_t main_thread_last_tick_usec = 0;
    int64_t main_thread_activator_usec = 0;
    int64_t main_thread_reinit_usec = 0;
    int64_t main_thread_viewport_usec = 0;
    int64_t main_thread_tick_usec = 0;
    int64_t main_thread_flush_usec = 0;
    int64_t runtime_physics_tick_usec = 0;
    int64_t runtime_physics_viewport_usec = 0;
    int64_t runtime_physics_server_tick_usec = 0;
    int64_t runtime_physics_flush_usec = 0;

    // Viewport sync cache: skip redundant world / exclude RIDs / listener updates when unchanged.
    bool vp_sync_cache_valid = false;
    RID vp_sync_last_world_rid;
    PackedInt64Array vp_sync_last_exclude_ids;
    Transform3D vp_sync_last_cam_xform;
    bool vp_sync_last_had_listener_nodes = false;

    // Per-frame group cache (avoids duplicate get_nodes_in_group in the viewport sync).
    int64_t group_cache_frame = -1;
    bool group_cache_use_physics = false;
    TypedArray<Node> cached_listener_nodes;

    bool custom_tracer_main_sim_warned = false;

    static bool editor_hint();
    static Variant script_new(const String& script_path);

    void claim_primary_runtime();
    void release_primary_and_elect_successor();
    void ensure_primary_side_effects();

    void setup_activator();
    void cleanup_reverb_activator();

    // Tick path (resonance_runtime_tick.cpp).
    bool uses_full_frame_timing() const;
    void refresh_group_caches_for_frame(bool use_physics_frame);
    TypedArray<RID> collect_listener_physics_exclude_rids(Viewport* vp, const TypedArray<Node>& listener_nodes) const;
    PackedInt64Array sorted_rid_int_ids(const TypedArray<RID>& rids) const;
    bool camera_listener_xform_changed(Camera3D* cam) const;
    void apply_resonance_viewport_to_server(Viewport* vp, bool use_physics_frame);
    void warn_custom_tracer_main_thread_sim(ResonanceServer* srv);
    void fill_activator_buffer();
    void handle_pending_reinit_frame_size();

    // Init / reinit / config / signals / scenes / bridges (resonance_runtime_init.cpp).
    Dictionary get_bake_params_for_runtime();
    void reload_static_scenes_from_tree(Node* tree_root);
    void prepare_geometry_before_reinit();
    void reinit_for_config_change();
    void apply_debug_flags();
    void apply_perspective_correction();
    void connect_runtime_signals();
    void disconnect_runtime_signals();
    void notify_volumes_runtime_config_changed();
    void warn_restart_if_needed();
    void init_fmod_bridge();
    void init_coda_bridge();
    void apply_primary_handoff_without_reinit();

    // Debug overlays + profiler driving (resonance_runtime_debug.cpp).
    void create_debug_overlay();
    void toggle_debug_overlay();
    void create_performance_overlay();
    void toggle_performance_overlay();
    void toggle_player_overlay();
    void disable_runtime_debug_ui();
    void refresh_resonance_geometry_for_debug_viz();
    void update_debug_overlay_visibility();
    void refresh_performance_custom_monitors_if_ready();
    void unregister_nexus_performance_monitors();
    void disable_performance_overlay_node();
    void tick_performance_monitors();

  protected:
    static void _bind_methods();

  public:
    void _ready() override;
    void _exit_tree() override;
    void _process(double delta) override;
    void _physics_process(double delta) override;
    void _input(const Ref<InputEvent>& event) override;

    void set_enable_debug(bool p_enabled);
    bool is_enable_debug() const { return enable_debug; }
    void set_performance_custom_monitors(int p_level);
    int get_performance_custom_monitors() const { return performance_custom_monitors; }
    void set_debug_overlay_toggle_key(int64_t p_key) { debug_overlay_toggle_key = p_key; }
    int64_t get_debug_overlay_toggle_key() const { return debug_overlay_toggle_key; }
    void set_performance_overlay_toggle_key(int64_t p_key) { performance_overlay_toggle_key = p_key; }
    int64_t get_performance_overlay_toggle_key() const { return performance_overlay_toggle_key; }
    void set_player_overlay_toggle_key(int64_t p_key) { player_overlay_toggle_key = p_key; }
    int64_t get_player_overlay_toggle_key() const { return player_overlay_toggle_key; }

    void set_runtime(const Ref<Resource>& p_config);
    Ref<Resource> get_runtime() const { return runtime; }

    void set_fmod_bridge_enabled(bool p_enabled);
    bool is_fmod_bridge_enabled() const { return fmod_bridge_enabled; }
    void set_coda_bridge_enabled(bool p_enabled);
    bool is_coda_bridge_enabled() const { return coda_bridge_enabled; }
    void set_context_simd_level(int p_level);
    int get_context_simd_level() const { return context_simd_level; }
    void set_context_validation(bool p_enabled);
    bool get_context_validation() const { return context_validation; }

    static int get_live_game_runtime_count();
    bool is_primary_runtime() const;

    // Callable targets for ResonanceRuntimeBus and the debug overlay.
    StringName get_bus_effective() const;
    StringName get_reverb_bus_name() const;
    StringName get_reverb_bus_send() const;

    void refresh_player_bus_routing();
    void apply_bus_to_players();

    Dictionary get_activator_instrumentation() const;

    void reset_viewport_sync_cache();
    void sync_physics_process_for_custom_tracer();
    Dictionary get_frame_timings() const;

    Dictionary get_config_dict() const;
    void initialize_server();
    void request_static_scene_reload();
    void perform_deferred_static_scene_reload();
    void reload_after_reinit();
    void deferred_reset_spatial_audio_warmup_passes();

    void on_reflection_type_changed(const Variant& arg);
    void on_audio_frame_size_changed(const Variant& arg);
    void on_runtime_affecting_probes_changed(const Variant& arg);

    Ref<RefCounted> get_fmod_bridge() const;
    Ref<RefCounted> get_coda_bridge() const;

    void on_scene_tree_exiting();

    // Hard-stop players + detach ResonanceAudioEffect so AudioServer can retire them before
    // GDExtension deinit. Call a few frames before SceneTree.quit(); _exit_tree is too late.
    void prepare_for_shutdown();
};

} // namespace godot

#endif // RESONANCE_RUNTIME_H
