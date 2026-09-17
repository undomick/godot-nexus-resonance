#ifndef RESONANCE_LISTENER_SYNC_POLICY_H
#define RESONANCE_LISTENER_SYNC_POLICY_H

namespace resonance {

/// Only the viewport camera's active ResonanceListener may publish validity to ResonanceServer.
inline bool listener_node_should_publish_validity(bool drives_server) {
    return drives_server;
}

/// Camera-only viewport sync (no ResonanceListener nodes) still drives a valid listener pose.
inline bool camera_fallback_listener_validity() {
    return true;
}

/// When no ResonanceListener drives the viewport camera this frame, use camera pose + validity fallback.
inline bool listener_sync_should_use_camera_fallback(bool any_driver_published) {
    return !any_driver_published;
}

/// Server validity when no driver published: camera fallback when available, otherwise invalid.
inline bool listener_sync_validity_when_no_driver(bool has_camera_fallback) {
    return has_camera_fallback ? camera_fallback_listener_validity() : false;
}

/// ResonanceRuntime::apply_resonance_viewport_to_server is SSOT for listener pose when a primary runtime exists.
inline bool listener_node_should_push_pose(bool primary_runtime_syncs_viewport) {
    return !primary_runtime_syncs_viewport;
}

} // namespace resonance

#endif // RESONANCE_LISTENER_SYNC_POLICY_H
