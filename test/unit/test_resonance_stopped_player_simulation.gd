extends GutTest

## A ResonancePlayer that is in the tree but not playing must not keep its source in the
## reflection simulation. Playing re-arms it; after stop it drops out once the tail has drained.

const REALTIME_CONVOLUTION := {"realtime_rays": 256, "reflection_type": 0}
const ROOM_HALF := 5.0
const WAIT_FRAMES := 240
const TAIL_DRAIN_FRAMES := 600

var _root: Node3D
var _player: ResonancePlayer


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "StoppedPlayerSimRoot"
	add_child_autoqfree(_root)

	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	var cfg := ResonanceRuntimeConfig.new()
	for key in REALTIME_CONVOLUTION:
		cfg.set(key, REALTIME_CONVOLUTION[key])
	runtime.set("runtime", cfg)
	_root.add_child(runtime)

	var camera := Camera3D.new()
	camera.current = true
	_root.add_child(camera)
	camera.add_child(ClassDB.instantiate("ResonanceListener"))

	_add_room()

	var config := ResonancePlayerConfig.new()
	config.reflections_enabled = true
	config.pathing_enabled_override = 0
	_player = ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	_player.player_config = config
	_player.stream = _make_looping_noise()
	_player.position = Vector3(0, 0, -3)
	_root.add_child(_player)


func after_each() -> void:
	if is_instance_valid(_player) and _player.is_playing():
		_player.stop()
	await wait_process_frames(2)


func _add_room() -> void:
	var size := Vector3(ROOM_HALF * 2.0, ROOM_HALF, ROOM_HALF * 2.0)
	var t := 0.3
	_add_wall(Vector3(0, -size.y * 0.5, 0), Vector3(size.x, t, size.z))
	_add_wall(Vector3(0, size.y * 0.5, 0), Vector3(size.x, t, size.z))
	_add_wall(Vector3(-ROOM_HALF, 0, 0), Vector3(t, size.y, size.z))
	_add_wall(Vector3(ROOM_HALF, 0, 0), Vector3(t, size.y, size.z))
	_add_wall(Vector3(0, 0, -ROOM_HALF), Vector3(size.x, size.y, t))
	_add_wall(Vector3(0, 0, ROOM_HALF), Vector3(size.x, size.y, t))


func _add_wall(pos: Vector3, size: Vector3) -> void:
	var box := BoxMesh.new()
	box.size = size
	var mi := MeshInstance3D.new()
	mi.mesh = box
	mi.position = pos
	mi.add_child(ClassDB.instantiate("ResonanceStaticGeometry"))
	_root.add_child(mi)


func _make_looping_noise() -> AudioStreamWAV:
	var frames := 22050
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, rng.randi_range(-8000, 8000))
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 44100
	wav.stereo = false
	wav.data = data
	wav.loop_mode = AudioStreamWAV.LOOP_FORWARD
	wav.loop_end = frames
	return wav


func _active_reflection_sources() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null:
		return -1
	return int(srv.get_simulation_worker_timing().get("active_reflection_sources", -1))


func _await_active_reflection_sources(expected: int, max_frames: int) -> bool:
	for _i in max_frames:
		if _active_reflection_sources() == expected:
			return true
		await wait_process_frames(1)
	return false


func test_stopped_player_source_leaves_and_rejoins_reflection_simulation() -> void:
	await wait_process_frames(WAIT_FRAMES)
	assert_eq(
		_active_reflection_sources(), 0, "a player that was never played must not count as a reflection source"
	)

	_player.play()
	assert_true(
		await _await_active_reflection_sources(1, WAIT_FRAMES),
		"play() must put the source back into the reflection simulation"
	)

	_player.stop()
	var dropped_during_tail := false
	for _i in TAIL_DRAIN_FRAMES:
		if not _player.is_playing():
			break
		if _active_reflection_sources() == 0:
			dropped_during_tail = true
		await wait_process_frames(1)
	assert_false(dropped_during_tail, "reflections must keep running while the reverb tail drains")
	assert_true(
		await _await_active_reflection_sources(0, WAIT_FRAMES),
		"after stop and tail drain the source must leave the reflection simulation"
	)
