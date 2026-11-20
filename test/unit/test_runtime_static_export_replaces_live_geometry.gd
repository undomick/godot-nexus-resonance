extends GutTest

## After a runtime static export, live ResonanceStaticGeometry meshes must leave the Steam Audio
## scene so the triangle count equals the static pack (no doubled room). Clearing the asset
## brings the live meshes back. Dynamic geometry stays registered throughout.

const ROOM_HALF_EXTENT := 4.0
const WALL_THICKNESS := 0.2
const BOX_TRIANGLES := 12
const ROOM_WALLS := 6
const SETTLE_FRAMES := 180

var _root: Node3D
var _static_scene: Node


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "StaticExportRoot"
	add_child_autoqfree(_root)

	_root.add_child(ClassDB.instantiate("ResonanceRuntime"))

	_static_scene = ClassDB.instantiate("ResonanceStaticScene")
	_static_scene.name = "ResonanceStaticScene"
	_root.add_child(_static_scene)
	_static_scene.owner = _root

	_add_room()


func _server() -> Variant:
	return ResonanceServerAccess.get_server_if_initialized()


func _triangle_count() -> int:
	var srv: Variant = _server()
	return srv.get_global_triangle_count() if srv != null else -1


func _await_triangle_count(expected: int) -> bool:
	for _i in SETTLE_FRAMES:
		if _triangle_count() == expected:
			return true
		await wait_process_frames(1)
	return false


func _add_room() -> void:
	var span := ROOM_HALF_EXTENT * 2.0
	var walls := [
		[Vector3(0, -ROOM_HALF_EXTENT, 0), Vector3(span, WALL_THICKNESS, span)],
		[Vector3(0, ROOM_HALF_EXTENT, 0), Vector3(span, WALL_THICKNESS, span)],
		[Vector3(-ROOM_HALF_EXTENT, 0, 0), Vector3(WALL_THICKNESS, span, span)],
		[Vector3(ROOM_HALF_EXTENT, 0, 0), Vector3(WALL_THICKNESS, span, span)],
		[Vector3(0, 0, -ROOM_HALF_EXTENT), Vector3(span, span, WALL_THICKNESS)],
		[Vector3(0, 0, ROOM_HALF_EXTENT), Vector3(span, span, WALL_THICKNESS)],
	]
	for i in walls.size():
		_root.add_child(
			_make_box("Wall%d" % i, walls[i][0], walls[i][1], "ResonanceStaticGeometry")
		)


func _make_box(box_name: String, pos: Vector3, size: Vector3, geometry_class: String) -> Node3D:
	var mesh_inst := MeshInstance3D.new()
	mesh_inst.name = box_name
	mesh_inst.position = pos
	var box := BoxMesh.new()
	box.size = size
	mesh_inst.mesh = box
	var geometry: Node = ClassDB.instantiate(geometry_class)
	geometry.name = geometry_class
	mesh_inst.add_child(geometry)
	return mesh_inst


func _export() -> int:
	return await ResonanceRuntimeExporter.export_static_async(
		_root, {"static_scene_node": _static_scene}
	)


func _pack_triangles() -> int:
	var asset: ResonanceGeometryAsset = _static_scene.static_scene_asset
	return asset.get_triangle_count() if asset != null else -1


func test_export_replaces_live_static_meshes_with_pack() -> void:
	assert_ne(_server(), null, "ResonanceServer must initialize with ResonanceRuntime in the tree")
	var live_room := ROOM_WALLS * BOX_TRIANGLES
	assert_true(await _await_triangle_count(live_room), "live room should register before export")

	assert_eq(await _export(), OK)
	assert_eq(_pack_triangles(), live_room, "pack should hold the whole room")

	assert_true(
		await _await_triangle_count(_pack_triangles()),
		"after export the scene must hold only the pack, got %d" % _triangle_count()
	)


func test_clearing_asset_restores_live_static_meshes() -> void:
	var live_room := ROOM_WALLS * BOX_TRIANGLES
	assert_true(await _await_triangle_count(live_room), "live room should register before export")
	assert_eq(await _export(), OK)
	assert_true(await _await_triangle_count(_pack_triangles()), "export should leave only the pack")

	_static_scene.static_scene_asset = null

	assert_true(
		await _await_triangle_count(live_room),
		"clearing the asset must bring the live room back, got %d" % _triangle_count()
	)


func test_export_keeps_dynamic_geometry() -> void:
	_root.add_child(
		_make_box("Mover", Vector3(1, 0, 1), Vector3(0.5, 0.5, 0.5), "ResonanceDynamicGeometry")
	)
	var live_room := ROOM_WALLS * BOX_TRIANGLES
	assert_true(
		await _await_triangle_count(live_room + BOX_TRIANGLES),
		"live room plus mover should register before export"
	)

	assert_eq(await _export(), OK)

	assert_true(
		await _await_triangle_count(_pack_triangles() + BOX_TRIANGLES),
		"dynamic mover must stay registered next to the pack, got %d" % _triangle_count()
	)
