#include "resonance_runtime.h"
#include "resonance_server.h"

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>

using namespace godot;

namespace {
const char* DEBUG_OVERLAY_PATH = "res://addons/nexus_resonance/scripts/debug_overlay.gd";
const char* PERF_OVERLAY_PATH = "res://addons/nexus_resonance/scripts/performance_overlay.gd";
const char* PERF_MONITORS_PATH = "res://addons/nexus_resonance/scripts/resonance_runtime_perf_monitors.gd";
} // namespace

void ResonanceRuntime::create_debug_overlay() {
    if (get_node_or_null(NodePath("DebugOverlay"))) {
        return;
    }
    Node* overlay = Object::cast_to<Node>(script_new(DEBUG_OVERLAY_PATH));
    if (overlay == nullptr) {
        return;
    }
    overlay->set_name("DebugOverlay");
    add_child(overlay);
}

void ResonanceRuntime::update_debug_overlay_visibility() {
    Node* overlay = get_node_or_null(NodePath("DebugOverlay"));
    if (overlay) {
        overlay->set("visible", debug_overlay_visible);
    }
}

void ResonanceRuntime::toggle_debug_overlay() {
    debug_overlay_visible = !debug_overlay_visible;
    update_debug_overlay_visibility();
}

void ResonanceRuntime::create_performance_overlay() {
    if (editor_hint() || get_node_or_null(NodePath("PerformanceOverlay"))) {
        return;
    }
    Node* overlay = Object::cast_to<Node>(script_new(PERF_OVERLAY_PATH));
    if (overlay == nullptr) {
        return;
    }
    overlay->set_name("PerformanceOverlay");
    overlay->set("visible", false);
    add_child(overlay);
}

void ResonanceRuntime::toggle_performance_overlay() {
    performance_overlay_visible = !performance_overlay_visible;
    Node* overlay = get_node_or_null(NodePath("PerformanceOverlay"));
    if (overlay == nullptr && performance_overlay_visible) {
        create_performance_overlay();
        overlay = get_node_or_null(NodePath("PerformanceOverlay"));
    }
    if (overlay) {
        overlay->set("visible", performance_overlay_visible);
        if (performance_overlay_visible) {
            overlay->set_process_mode(Node::PROCESS_MODE_INHERIT);
        }
    }
}

void ResonanceRuntime::toggle_player_overlay() {
    player_overlay_visible = !player_overlay_visible;
    apply_debug_flags();
    refresh_resonance_geometry_for_debug_viz();
}

void ResonanceRuntime::disable_performance_overlay_node() {
    Node* perf = get_node_or_null(NodePath("PerformanceOverlay"));
    if (perf) {
        perf->set("visible", false);
        perf->set_process_mode(Node::PROCESS_MODE_DISABLED);
    }
}

void ResonanceRuntime::disable_runtime_debug_ui() {
    debug_overlay_visible = false;
    performance_overlay_visible = false;
    player_overlay_visible = false;
    update_debug_overlay_visibility();
    disable_performance_overlay_node();
    apply_debug_flags();
    refresh_resonance_geometry_for_debug_viz();
}

void ResonanceRuntime::refresh_resonance_geometry_for_debug_viz() {
    if (!is_inside_tree()) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        return;
    }
    // sync_reflection_debug_viz only; a full refresh_geometry() breaks Embree dynamic occlusion on the F3 toggle.
    get_tree()->call_group_flags(
        SceneTree::GROUP_CALL_DEFERRED, "resonance_geometry", "sync_reflection_debug_viz");
}

void ResonanceRuntime::refresh_performance_custom_monitors_if_ready() {
    if (editor_hint() || !is_inside_tree()) {
        return;
    }
    if (Object::cast_to<Object>(perf_monitors) == nullptr) {
        perf_monitors = script_new(PERF_MONITORS_PATH);
    }
    Object* pm = Object::cast_to<Object>(perf_monitors);
    if (pm) {
        pm->call("register", this, performance_custom_monitors);
    }
}

void ResonanceRuntime::unregister_nexus_performance_monitors() {
    Object* pm = Object::cast_to<Object>(perf_monitors);
    if (pm) {
        pm->call("unregister_all");
    }
}

void ResonanceRuntime::tick_performance_monitors() {
    Object* pm = Object::cast_to<Object>(perf_monitors);
    if (pm) {
        pm->call("tick", this);
    }
}

void ResonanceRuntime::set_enable_debug(bool p_enabled) {
    if (enable_debug == p_enabled) {
        return;
    }
    const bool was_on = enable_debug;
    enable_debug = p_enabled;
    if (was_on && !p_enabled) {
        disable_runtime_debug_ui();
    }
}

void ResonanceRuntime::set_performance_custom_monitors(int p_level) {
    const int nv = p_level < 0 ? 0 : (p_level > 3 ? 3 : p_level);
    if (performance_custom_monitors == nv) {
        return;
    }
    performance_custom_monitors = nv;
    refresh_performance_custom_monitors_if_ready();
}

void ResonanceRuntime::_input(const Ref<InputEvent>& event) {
    if (editor_hint() || !enable_debug) {
        return;
    }
    Ref<InputEventKey> key = event;
    if (key.is_null() || !key->is_pressed() || key->is_echo()) {
        return;
    }
    const int64_t kc = key->get_keycode();
    if (kc == debug_overlay_toggle_key) {
        toggle_debug_overlay();
        get_viewport()->set_input_as_handled();
    } else if (kc == performance_overlay_toggle_key) {
        toggle_performance_overlay();
        get_viewport()->set_input_as_handled();
    } else if (kc == player_overlay_toggle_key) {
        toggle_player_overlay();
        get_viewport()->set_input_as_handled();
    }
}
