extends Node3D

## Manual / headless regression for spatial cutout on ResonanceDynamicGeometry spawn.
## Plays a looping tone, waits [member spawn_delay_sec], then spawns a dynamic mesh.
## Passes if is_spatial_audio_output_ready() stays true and the player keeps playing.
##
## Run (from a prepared project/ with addon + this folder synced):
##   godot --path project --headless res://test/manual/dynamic_geometry_spawn_audio/main.tscn
## Or open main.tscn in the editor and press Play.

@export var spawn_delay_sec: float = 2.5
@export var observe_after_spawn_sec: float = 1.5
@export var auto_quit: bool = true

var _player: ResonancePlayer
var _status: Label
var _spawned := false
var _elapsed := 0.0
var _post_spawn_elapsed := 0.0
var _ready_before_spawn := false
var _saw_not_ready_after_spawn := false
var _finished := false


func _ready() -> void:
	_status = Label.new()
	_status.set_anchors_preset(Control.PRESET_TOP_WIDE)
	_status.offset_left = 16
	_status.offset_top = 16
	_status.offset_right = -16
	_status.offset_bottom = 120
	add_child(_status)

	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	add_child(runtime)

	var camera := Camera3D.new()
	camera.current = true
	camera.position = Vector3(0, 1.6, 3)
	add_child(camera)

	var listener: Node = ClassDB.instantiate("ResonanceListener")
	camera.add_child(listener)

	add_child(_make_dynamic_mesh_branch("BaselineBlock", Vector3(0, 1, -4), Vector3(3, 2, 0.4)))

	var config := ResonancePlayerConfig.new()
	config.reflections_enabled = 0
	config.pathing_enabled_override = 0

	_player = ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	_player.player_config = config
	_player.stream = _make_looping_tone_stream()
	_player.position = Vector3(0, 1, -1.5)
	add_child(_player)
	_player.play()

	_set_status("Playing tone… waiting %.1fs then spawning ResonanceDynamicGeometry" % spawn_delay_sec)


func _process(delta: float) -> void:
	if _finished:
		return

	_elapsed += delta
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	var spatial_ready: bool = false
	var simulating: bool = false
	if srv != null:
		spatial_ready = bool(srv.is_spatial_audio_output_ready())
		if srv.has_method("is_simulating"):
			simulating = bool(srv.is_simulating())
	var playing: bool = is_instance_valid(_player) and (_player.playing or _player.is_playing())

	# Progress log once per second while waiting for baseline readiness.
	if not _spawned and int(_elapsed) != int(_elapsed - delta):
		_set_status(
			"Waiting… t=%.1f srv=%s simulating=%s spatial_ready=%s playing=%s"
			% [_elapsed, str(srv != null), str(simulating), str(spatial_ready), str(playing)]
		)

	if not _spawned:
		# Require simulation (registered triangles) so we are past cold-start, not "ready" with empty scene.
		if spatial_ready and simulating:
			_ready_before_spawn = true
		if _elapsed >= spawn_delay_sec and _ready_before_spawn:
			add_child(_make_dynamic_mesh_branch("SpawnedBlock", Vector3(1.5, 1, -3), Vector3(1.2, 2.5, 0.4)))
			_spawned = true
			_set_status("Spawned ResonanceDynamicGeometry — observing for %.1fs…" % observe_after_spawn_sec)
		elif _elapsed >= maxf(spawn_delay_sec * 3.0, 10.0) and not _ready_before_spawn:
			_fail(
				"Timed out waiting for spatial_ready+simulating before spawn (srv=%s simulating=%s spatial_ready=%s)"
				% [str(srv != null), str(simulating), str(spatial_ready)]
			)
		return

	_post_spawn_elapsed += delta
	if not spatial_ready:
		_saw_not_ready_after_spawn = true
	if not playing:
		_fail("ResonancePlayer stopped playing after dynamic geometry spawn")
		return

	if _post_spawn_elapsed >= observe_after_spawn_sec:
		if _saw_not_ready_after_spawn:
			_fail("is_spatial_audio_output_ready went false after spawning ResonanceDynamicGeometry")
		else:
			_pass()


func _pass() -> void:
	_finished = true
	_set_status("PASS — tone kept playing; spatial output stayed ready after DynamicGeometry spawn")
	print("dynamic_geometry_spawn_audio: PASS")
	if auto_quit:
		await get_tree().create_timer(0.5).timeout
		get_tree().quit(0)


func _fail(reason: String) -> void:
	_finished = true
	_set_status("FAIL — %s" % reason)
	push_error("dynamic_geometry_spawn_audio: FAIL — %s" % reason)
	if auto_quit:
		await get_tree().create_timer(0.5).timeout
		get_tree().quit(1)


func _set_status(text: String) -> void:
	if is_instance_valid(_status):
		_status.text = text
	print(text)


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
