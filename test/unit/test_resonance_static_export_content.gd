extends GutTest

## The static export check counts the same geometry the static exporter exports.

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")


func _scene_with_geometry(dynamic: bool) -> Node:
	var root: Node3D = autofree(Node3D.new())
	var geom := ClassDB.instantiate("ResonanceGeometry") as Node
	geom.set("dynamic", dynamic)
	root.add_child(geom)
	return root


func test_plain_static_geometry_is_exportable() -> void:
	var root := _scene_with_geometry(false)
	assert_true(ResonanceSceneUtils.scene_has_exportable_resonance_content(root, "static"))


func test_dynamic_geometry_is_not_static_content() -> void:
	var root := _scene_with_geometry(true)
	assert_false(ResonanceSceneUtils.scene_has_exportable_resonance_content(root, "static"))
