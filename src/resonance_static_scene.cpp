#include "resonance_static_scene.h"
#include "resonance_server.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

using namespace godot;

ResonanceStaticScene::ResonanceStaticScene() {}

ResonanceStaticScene::~ResonanceStaticScene() {}

void ResonanceStaticScene::_register_static_pack() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        return;
    }
    if (!is_inside_tree()) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        return;
    }
    const uint64_t id = get_instance_id();
    if (!has_valid_asset()) {
        srv->remove_static_pack(id);
        return;
    }
    srv->add_or_replace_static_pack(id, static_scene_asset, get_global_transform());
}

void ResonanceStaticScene::_unregister_static_pack() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        return;
    }
    srv->remove_static_pack(get_instance_id());
}

void ResonanceStaticScene::_notification(int p_what) {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        return;
    }
    if (p_what == NOTIFICATION_ENTER_TREE) {
        // Deferred so global_transform is final after parent placement.
        call_deferred("_register_static_pack");
    } else if (p_what == NOTIFICATION_EXIT_TREE) {
        _unregister_static_pack();
    }
}

void ResonanceStaticScene::set_static_scene_asset(const Ref<ResonanceGeometryAsset>& p_asset) {
    if (static_scene_asset == p_asset)
        return;
    static_scene_asset = p_asset;
    if (is_inside_tree()) {
        _register_static_pack();
    }
}

Ref<ResonanceGeometryAsset> ResonanceStaticScene::get_static_scene_asset() const {
    return static_scene_asset;
}

void ResonanceStaticScene::set_scene_name_when_exported(const String& p_name) {
    scene_name_when_exported = p_name;
}

String ResonanceStaticScene::get_scene_name_when_exported() const {
    return scene_name_when_exported;
}

bool ResonanceStaticScene::has_valid_asset() const {
    return static_scene_asset.is_valid() && static_scene_asset->is_valid();
}

void ResonanceStaticScene::set_export_hash(int64_t p_hash) {
    export_hash = p_hash;
}

int64_t ResonanceStaticScene::get_export_hash() const {
    return export_hash;
}

void ResonanceStaticScene::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_static_scene_asset", "p_asset"), &ResonanceStaticScene::set_static_scene_asset);
    ClassDB::bind_method(D_METHOD("get_static_scene_asset"), &ResonanceStaticScene::get_static_scene_asset);

    ClassDB::bind_method(D_METHOD("set_scene_name_when_exported", "p_name"), &ResonanceStaticScene::set_scene_name_when_exported);
    ClassDB::bind_method(D_METHOD("get_scene_name_when_exported"), &ResonanceStaticScene::get_scene_name_when_exported);

    ClassDB::bind_method(D_METHOD("has_valid_asset"), &ResonanceStaticScene::has_valid_asset);

    ClassDB::bind_method(D_METHOD("set_export_hash", "p_hash"), &ResonanceStaticScene::set_export_hash);
    ClassDB::bind_method(D_METHOD("get_export_hash"), &ResonanceStaticScene::get_export_hash);

    ClassDB::bind_method(D_METHOD("_register_static_pack"), &ResonanceStaticScene::_register_static_pack);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "static_scene_asset", PROPERTY_HINT_RESOURCE_TYPE, "ResonanceGeometryAsset"),
                 "set_static_scene_asset", "get_static_scene_asset");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "scene_name_when_exported"), "set_scene_name_when_exported", "get_scene_name_when_exported");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "export_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_export_hash", "get_export_hash");
}
