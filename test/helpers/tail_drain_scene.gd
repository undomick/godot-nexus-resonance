extends RefCounted

## Runtime, listener, closed room and short-clip players for tail drain tests.

const ReverbBusRoom := preload("res://test/helpers/reverb_bus_room.gd")

const REFLECTION_CONVOLUTION := 0
const REFLECTION_PARAMETRIC := 1
const REFLECTION_HYBRID := 2
const REFLECTION_TAN := 3
const ALL_REFLECTION_TYPES := [REFLECTION_CONVOLUTION, REFLECTION_PARAMETRIC, REFLECTION_HYBRID, REFLECTION_TAN]

const SAMPLE_RATE := 44100
const TAIL_SECONDS := 0.5
const REALTIME_RAYS := 1024
const ROOM_SIZE := Vector3(8, 4, 8)


static func build(parent: Node, reflection_type: int) -> Node3D:
	var root := Node3D.new()
	parent.add_child(root)
	var config := ResonanceRuntimeConfig.new()
	config.reflection_type = reflection_type
	config.realtime_rays = REALTIME_RAYS
	config.realtime_simulation_duration = TAIL_SECONDS
	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	runtime.name = "ResonanceRuntime"
	runtime.set("runtime", config)
	root.add_child(runtime)
	ReverbBusRoom.add_closed_room(root, ROOM_SIZE)
	var camera := Camera3D.new()
	camera.current = true
	camera.position = Vector3(0, 1.7, 0)
	root.add_child(camera)
	var listener: Node = ClassDB.instantiate("ResonanceListener")
	listener.set("apply_reverb", true)
	camera.add_child(listener)
	return root


## Mono 16-bit clip; frames == 0 gives an empty stream.
static func make_clip(frames: int) -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = SAMPLE_RATE
	wav.stereo = false
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, int(sin(float(i) * 0.3) * 20000.0))
	wav.data = data
	return wav


static func make_player(root: Node3D, stream: AudioStream, with_reflections: bool = true) -> ResonancePlayer:
	var config := ResonancePlayerConfig.new()
	config.pathing_enabled_override = 0
	config.reflections_enabled = with_reflections
	var player := ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	player.player_config = config
	player.stream = stream
	player.position = Vector3(2, 1.5, -2)
	root.add_child(player)
	return player


## Generous upper bound for natural EOS plus tail drain: the IR length, the grace window and frame jitter.
static func drain_deadline_sec() -> float:
	return TAIL_SECONDS * 4.0 + 3.0


## Waits until the player is no longer playing. Returns false on timeout.
static func wait_until_stopped(tree: SceneTree, player: ResonancePlayer, timeout_sec: float) -> bool:
	var end_ms := Time.get_ticks_msec() + int(timeout_sec * 1000.0)
	while Time.get_ticks_msec() < end_ms:
		if not is_instance_valid(player) or not player.playing:
			return true
		await tree.process_frame
	return not player.playing


static func wait_seconds(tree: SceneTree, seconds: float) -> void:
	var end_ms := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < end_ms:
		await tree.process_frame


static func output_is_finite(player: ResonancePlayer) -> bool:
	var info: Dictionary = player.get_audio_instrumentation()
	for key in ["last_output_rms", "pathing_sh_rms", "pathing_sh_energy"]:
		if not info.has(key):
			continue
		var value: float = info[key]
		if is_nan(value) or is_inf(value):
			return false
	return true
