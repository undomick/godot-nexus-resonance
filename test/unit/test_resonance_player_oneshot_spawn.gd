extends GutTest

## A one-shot ResonancePlayer added to the tree and played in the same frame must play the whole
## stream and emit `finished` once, near the stream length.

const STREAM_SEC := 0.4
const MIX_RATE := 44100
const EARLY_FRAMES := 5
const MIN_FINISH_SEC := STREAM_SEC * 0.75
const MAX_FINISH_SEC := 3.0
const SETTLE_FRAMES := 60

var _root: Node3D
var _finished_count := 0
var _finished_at_usec := 0


func before_each() -> void:
	_finished_count = 0
	_finished_at_usec = 0
	_root = Node3D.new()
	add_child_autoqfree(_root)
	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	runtime.set("runtime", ResonanceRuntimeConfig.new())
	_root.add_child(runtime)
	var camera := Camera3D.new()
	camera.current = true
	_root.add_child(camera)
	camera.add_child(ClassDB.instantiate("ResonanceListener"))
	_add_floor()
	await wait_process_frames(SETTLE_FRAMES)


func _add_floor() -> void:
	var box := BoxMesh.new()
	box.size = Vector3(10, 0.3, 10)
	var floor_mesh := MeshInstance3D.new()
	floor_mesh.mesh = box
	floor_mesh.position = Vector3(0, -2, 0)
	floor_mesh.add_child(ClassDB.instantiate("ResonanceStaticGeometry"))
	_root.add_child(floor_mesh)


func _make_oneshot_stream() -> AudioStreamWAV:
	var frames := int(STREAM_SEC * MIX_RATE)
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, int(sin(float(i) * 0.05) * 12000.0))
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = MIX_RATE
	wav.stereo = false
	wav.data = data
	return wav


func _on_finished() -> void:
	_finished_count += 1
	if _finished_at_usec == 0:
		_finished_at_usec = Time.get_ticks_usec()


func test_oneshot_played_in_spawn_frame_plays_to_the_end() -> void:
	var config := ResonancePlayerConfig.new()
	config.pathing_enabled_override = 0
	var player := ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	player.player_config = config
	player.stream = _make_oneshot_stream()
	player.position = Vector3(0, 0, -2)
	player.finished.connect(_on_finished)

	_root.add_child(player)
	var started_usec := Time.get_ticks_usec()
	player.play()

	await wait_process_frames(EARLY_FRAMES)
	assert_true(player.is_playing(), "the one-shot must still be playing a few frames after play()")
	assert_eq(_finished_count, 0, "`finished` must not fire right after play()")

	var deadline_usec := started_usec + int(MAX_FINISH_SEC * 1000000.0)
	while _finished_count == 0 and Time.get_ticks_usec() < deadline_usec:
		await wait_process_frames(1)

	assert_eq(_finished_count, 1, "`finished` must fire once")
	var elapsed_sec := float(_finished_at_usec - started_usec) / 1000000.0
	assert_gt(
		elapsed_sec, MIN_FINISH_SEC, "`finished` must not fire before the stream has played out"
	)
	await wait_process_frames(4)
	assert_eq(_finished_count, 1, "`finished` must not fire again after the tail")
