extends GutTest

## Material swaps on live geometry are applied by the simulation worker. A swap followed by freeing
## the geometry in the same frame must leave the queued change with valid handles.

var _root: Node3D


func before_each() -> void:
	_root = Node3D.new()
	add_child_autoqfree(_root)
	_root.add_child(ClassDB.instantiate("ResonanceRuntime"))
	var camera := Camera3D.new()
	camera.current = true
	_root.add_child(camera)
	camera.add_child(ClassDB.instantiate("ResonanceListener"))
	await wait_process_frames(4)


func _add_geometry(geometry_class: String, pos: Vector3) -> Node:
	var mesh_inst := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(2, 2, 0.4)
	mesh_inst.mesh = box
	mesh_inst.position = pos
	var geometry: Node = ClassDB.instantiate(geometry_class)
	mesh_inst.add_child(geometry)
	_root.add_child(mesh_inst)
	return geometry


func _make_material(absorption: float) -> Resource:
	var mat: Resource = ClassDB.instantiate("ResonanceMaterial")
	mat.set("absorption_low", absorption)
	mat.set("absorption_mid", absorption)
	mat.set("absorption_high", absorption)
	return mat


func _swap_then_free(geometry_class: String) -> void:
	var geometry := _add_geometry(geometry_class, Vector3(0, 0, -3))
	await wait_process_frames(4)
	var mat := _make_material(0.7)
	geometry.set("material", mat)
	geometry.set("material", _make_material(0.1))
	geometry.set("material", mat)
	assert_eq(geometry.get("material"), mat)
	geometry.get_parent().free()
	await wait_process_frames(10)
	assert_not_null(
		ResonanceServerAccess.get_server_if_initialized(), "server stays up after the swap"
	)


func test_swap_then_free_static_geometry() -> void:
	await _swap_then_free("ResonanceStaticGeometry")


func test_swap_then_free_dynamic_geometry() -> void:
	await _swap_then_free("ResonanceDynamicGeometry")
