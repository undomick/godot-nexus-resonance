#ifndef RESONANCE_STATIC_SCENE_H
#define RESONANCE_STATIC_SCENE_H

#include "resonance_geometry_asset.h"
#include <godot_cpp/classes/node3d.hpp>

namespace godot {

/// Holds the exported static scene asset (merged geometry from iplStaticMeshSave) for baking and runtime.
class ResonanceStaticScene : public Node3D {
    GDCLASS(ResonanceStaticScene, Node3D)

  private:
    Ref<ResonanceGeometryAsset> static_scene_asset;
    String scene_name_when_exported;
    int64_t export_hash = 0; // Geometry hash at the last export; an unchanged hash skips re-export.

    static constexpr int kMaxServerInitRetries = 64;
    bool server_init_retry_pending_ = false;
    int server_init_retry_count_ = 0;

    void _register_static_pack();
    void _unregister_static_pack();
    void _sync_live_static_geometry();
    void _schedule_retry_register_when_server_ready();
    void _deferred_retry_register_static_pack();

  protected:
    static void _bind_methods();
    void _notification(int p_what);

  public:
    ResonanceStaticScene();
    ~ResonanceStaticScene();

    void set_static_scene_asset(const Ref<ResonanceGeometryAsset>& p_asset);
    Ref<ResonanceGeometryAsset> get_static_scene_asset() const;

    void set_scene_name_when_exported(const String& p_name);
    String get_scene_name_when_exported() const;

    bool has_valid_asset() const;

    void set_export_hash(int64_t p_hash);
    int64_t get_export_hash() const;
};

enum class StaticSceneQuery {
    Any,
    WithValidAsset,
};

bool subtree_has_static_scene(Node* p_node, StaticSceneQuery p_query);

} // namespace godot

#endif
