extends GutTest

## Regression: spawning ResonanceDynamicGeometry into a non-empty scene must not re-arm the
## spatial output gate (no hard mute of ongoing ResonancePlayer audio).
##
## Setup: baseline dynamic geometry (0->N arms once) + looping player. After the gate is ready,
## spawn a second dynamic mesh and assert is_spatial_audio_output_ready() stays true and the
## player keeps playing.

var _runtime: Node
var _root: Node3D
var _player: ResonancePlayer
var _listener: Node3D
var _camera: Camera3D


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "DynGeomSpawnRoot"
	add_child_autoqfree(_root)

	_runtime = ClassDB.instantiate("ResonanceRuntime")
	_root.add_child(_runtime)

	_camera = Camera3D.new()
	_camera.current = true
	_root.add_child(_camera)

	_listener = ClassDB.instantiate("ResonanceListener") as Node3D
	_camera.add_child(_listener)

	# Baseline acoustic mesh so triangle count becomes non-zero (cold-start arm once).
	_root.add_child(_make_dynamic_mesh_branch("BaselineBlock", Vector3(0, 0, -4), Vector3(2, 2, 0.5)))

	var config := ResonancePlayerConfig.new()
	config.reflections_enabled = 0
	config.pathing_enabled_override = 0

	_player = ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	_player.player_config = config
	_player.stream = _make_looping_tone_stream()
	_player.position = Vector3(0, 0, -2)
	_root.add_child(_player)

	await wait_process_frames(4)
	_player.play()
	await wait_process_frames(2)


func after_each() -> void:
	if not is_instance_valid(_player):
		return
	if _player.player_config != null:
		_player.player_config = null
	if _player.playing or _player.is_playing():
		_player.stop()
	await wait_process_frames(2)


func _server() -> Variant:
	return ResonanceServerAccess.get_server_if_initialized()


func _make_looping_tone_stream() -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 44100
	wav.stereo = false
	wav.loop_mode = AudioStreamWAV.LOOP_FORWARD
	wav.loop_begin = 0
	var frames: int = 44100
	wav.loop_end = frames
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		# ~440 Hz sine, moderate amplitude
		var sample: int = int(sin(float(i) * TAU * 440.0 / 44100.0) * 10000.0)
		data.encode_s16(i * 2, sample)
	wav.data = data
	return wav


func _make_dynamic_mesh_branch(branch_name: String, pos: Vector3, size: Vector3) -> Node3D:
	var mesh_inst := MeshInstance3D.new()
	mesh_inst.name = branch_name
	mesh_inst.position = pos
	var box := BoxMesh.new()
	box.size = size
	mesh_inst.mesh = box

	var dyn: Node = ClassDB.instantiate("ResonanceDynamicGeometry")
	dyn.name = "ResonanceDynamicGeometry"
	mesh_inst.add_child(dyn)
	return mesh_inst


func _await_spatial_ready(max_frames: int = 180) -> bool:
	for _i in max_frames:
		var srv: Variant = _server()
		if srv != null and srv.is_spatial_audio_output_ready():
			return true
		await wait_process_frames(1)
	return false


func test_spawn_dynamic_geometry_keeps_spatial_output_ready_and_player_playing() -> void:
	var srv: Variant = _server()
	assert_ne(srv, null, "ResonanceServer must initialize with ResonanceRuntime in the tree")

	var became_ready: bool = await _await_spatial_ready()
	assert_true(became_ready, "baseline geometry should make spatial output ready after commit+warmup")
	assert_true(_player.playing or _player.is_playing(), "looping tone should be playing before spawn")

	# Second dynamic mesh into a non-empty scene must not re-arm the gate.
	_root.add_child(_make_dynamic_mesh_branch("SpawnedBlock", Vector3(1.5, 0, -3), Vector3(1, 2, 0.4)))

	var saw_not_ready := false
	for _i in 90:
		srv = _server()
		if srv == null or not srv.is_spatial_audio_output_ready():
			saw_not_ready = true
			break
		await wait_process_frames(1)

	assert_false(
		saw_not_ready,
		"is_spatial_audio_output_ready must stay true after spawning ResonanceDynamicGeometry into a non-empty scene"
	)
	assert_true(
		_player.playing or _player.is_playing(),
		"ResonancePlayer must keep playing through the dynamic geometry spawn (no spatial cutout)"
	)
