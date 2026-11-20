extends GutTest

## A source created after the scene is ready stays gated until its first direct result, and a hard
## scene reload closes its gate again until the next result.

var _runtime: Node
var _root: Node3D
var _handle: int = -1


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "SourceGateRoot"
	add_child_autoqfree(_root)

	_runtime = ClassDB.instantiate("ResonanceRuntime")
	_root.add_child(_runtime)

	var camera := Camera3D.new()
	camera.current = true
	_root.add_child(camera)
	camera.add_child(ClassDB.instantiate("ResonanceListener") as Node3D)

	await wait_process_frames(4)


func after_each() -> void:
	var srv: Variant = _server()
	if srv != null and _handle >= 0:
		srv.destroy_source_handle(_handle)
	_handle = -1
	await wait_process_frames(2)


func _server() -> Variant:
	return ResonanceServerAccess.get_server_if_initialized()


func _await_scene_ready(max_frames: int = 180) -> bool:
	for _i in max_frames:
		var srv: Variant = _server()
		if srv != null and srv.is_spatial_audio_output_ready():
			return true
		await wait_process_frames(1)
	return false


func _await_source_ready(max_frames: int = 180) -> bool:
	for _i in max_frames:
		var srv: Variant = _server()
		if srv != null and srv.is_source_spatial_output_ready(_handle):
			return true
		await wait_process_frames(1)
	return false


func test_source_created_after_scene_ready_waits_for_first_direct_result() -> void:
	assert_true(await _await_scene_ready(), "empty scene should become ready")
	var srv: Variant = _server()
	_handle = srv.create_source_handle(Vector3(0, 0, -2), 1.0, "", -1, false)
	assert_true(_handle >= 0, "server should hand out a source handle")

	assert_false(srv.is_source_spatial_output_ready(_handle), "a fresh source has no direct result yet")
	assert_true(await _await_source_ready(), "the source should open after its first direct pass")


func test_hard_scene_reload_closes_source_gate_until_next_direct_result() -> void:
	assert_true(await _await_scene_ready(), "empty scene should become ready")
	var srv: Variant = _server()
	_handle = srv.create_source_handle(Vector3(0, 0, -2), 1.0, "", -1, false)
	assert_true(await _await_source_ready(), "the source should open after its first direct pass")

	srv.arm_spatial_audio_output_gate()

	assert_false(srv.is_source_spatial_output_ready(_handle), "a hard reload closes the gate at once")
	assert_true(await _await_source_ready(), "the source should open again after the reload")
