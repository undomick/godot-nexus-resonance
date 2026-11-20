extends RefCounted

## Builds and animates the node tree for one scenario from perf_scenarios.gd.

const ROOM_SIZE := Vector3(20.0, 6.0, 20.0)
const WALL_THICKNESS := 0.3
const DENSE_CLUTTER_COUNT := 25
const DENSE_SPHERE_SEGMENTS := 64
const DENSE_SPHERE_RINGS := 16
const LISTENER_YAW_SPEED := 1.5
const SOURCE_ORBIT_SPEED := 0.6
const MOVER_SPEED := 1.2
const ONESHOT_SECONDS := 0.4
const MIX_RATE := 44100
const ResonanceRuntimeExporter = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_exporter.gd"
)

var root: Node3D
var runtime: Node
var camera: Camera3D
var players: Array[Node] = []
var movers: Array[Node3D] = []
var churn_spawned := 0
## One entry per churn spawn, {"player": Node, "play_usec": int}; freed players stay listed.
var churn_starts: Array[Dictionary] = []
var spawn_us: Array[float] = []
## Per-phase split of spawn_us, keyed by the fields of ResonancePlayer.get_spawn_timing().
var spawn_phase_us := {
	"voice_dsp_us": [] as Array[float],
	"reflection_effect_us": [] as Array[float],
	"source_us": [] as Array[float],
	"params_push_us": [] as Array[float],
}
## Main-thread time of each streaming toggle: static pack swap and probe volume add/remove.
var stream_static_pack_us: Array[float] = []
var stream_probe_batch_us: Array[float] = []
## Main-thread time of assigning a new ResonanceMaterial to a live dynamic geometry.
var material_swap_us: Array[float] = []

var _scenario: Dictionary
var _time := 0.0
var _churn_accum := 0.0
var _loop_stream: AudioStreamWAV
var _oneshot_stream: AudioStreamWAV
var _static_scene: Node
var _probe_volume: Node3D
var _streamed_asset: Resource
var _stream_accum := 0.0
var _stream_toggles := 0
var _swap_geometry: Node
var _swap_materials: Array[Resource] = []
var _swap_accum := 0.0


func build(scenario: Dictionary, parent: Node) -> void:
	_scenario = scenario
	_loop_stream = _make_noise_stream(1.0, true)
	_oneshot_stream = _make_noise_stream(ONESHOT_SECONDS, false)

	root = Node3D.new()
	root.name = "Perf_" + str(scenario["id"])

	runtime = ClassDB.instantiate("ResonanceRuntime")
	runtime.set("runtime", _make_runtime_config(scenario.get("runtime", {})))
	runtime.set("performance_custom_monitors", 0)
	root.add_child(runtime)

	camera = Camera3D.new()
	camera.current = true
	root.add_child(camera)
	var listener: Node = ClassDB.instantiate("ResonanceListener")
	listener.set("apply_reverb", bool(scenario.get("listener_reverb", false)))
	listener.set(
		"reverb_type", int(scenario.get("listener_reverb_type", ResonanceListener.REVERB_REALTIME))
	)
	camera.add_child(listener)

	_add_geometry(str(scenario.get("geometry", "none")))
	for i in int(scenario.get("players", 0)):
		_add_player(i, int(scenario.get("players", 0)))
	for i in int(scenario.get("dynamic_movers", 0)):
		_add_mover(i)
	if bool(scenario.get("bake_probes", false)):
		_add_static_scene_and_probe_volume()
	if float(scenario.get("material_swaps_per_sec", 0.0)) > 0.0:
		_add_material_swap_panel()
	parent.add_child(root)


## Exports the static scene and bakes the probe volume into RAM; returns at once without bake_probes.
func bake_probes() -> void:
	if _static_scene == null:
		return
	var err: int = await ResonanceRuntimeExporter.export_static_async(
		root, {"static_scene_node": _static_scene, "bake_probes": true}
	)
	if err != OK:
		push_error("perf: probe bake export failed (%d)" % err)


func start_playback() -> void:
	if not bool(_scenario.get("playing", true)):
		return
	for p in players:
		p.call("play")


func step(delta: float) -> void:
	_time += delta
	match str(_scenario.get("motion", "none")):
		"listener":
			camera.rotation.y = _time * LISTENER_YAW_SPEED
		"sources":
			_orbit_sources()
	for i in movers.size():
		movers[i].position.x = sin(_time * MOVER_SPEED + i) * 4.0
	_spawn_churn(delta)
	_toggle_streaming(delta)
	_swap_material(delta)


func teardown() -> void:
	if is_instance_valid(_probe_volume) and not _probe_volume.is_inside_tree():
		_probe_volume.free()
	if is_instance_valid(root):
		root.queue_free()


func _orbit_sources() -> void:
	for i in players.size():
		players[i].position = _source_position(i, players.size(), _time * SOURCE_ORBIT_SPEED)


func _spawn_churn(delta: float) -> void:
	var rate := float(_scenario.get("churn_per_sec", 0.0))
	if rate <= 0.0:
		return
	_churn_accum += delta * rate
	while _churn_accum >= 1.0:
		_churn_accum -= 1.0
		var p := _make_player(_oneshot_stream)
		p.position = _source_position(churn_spawned, 8, 0.0)
		p.connect("finished", p.queue_free)
		var t0 := Time.get_ticks_usec()
		root.add_child(p)
		p.call("play")
		churn_starts.append({"player": p, "play_usec": t0})
		spawn_us.append(float(Time.get_ticks_usec() - t0))
		_record_spawn_phases(p)
		churn_spawned += 1


# Alternates between swapping the static pack and adding/removing the probe volume, the way level
# streaming would. Both run on the main thread and take simulation_mutex.
func _toggle_streaming(delta: float) -> void:
	var rate := float(_scenario.get("stream_toggles_per_sec", 0.0))
	if rate <= 0.0 or _static_scene == null:
		return
	_stream_accum += delta * rate
	while _stream_accum >= 1.0:
		_stream_accum -= 1.0
		_stream_toggles += 1
		if _stream_toggles % 2 == 1:
			stream_static_pack_us.append(_timed_us(_toggle_static_pack))
		else:
			stream_probe_batch_us.append(_timed_us(_toggle_probe_volume))


func _toggle_static_pack() -> void:
	var current: Resource = _static_scene.get("static_scene_asset")
	if current != null:
		_streamed_asset = current
		_static_scene.set("static_scene_asset", null)
	else:
		_static_scene.set("static_scene_asset", _streamed_asset)


# Keeps the same instance, as a pooled level chunk would.
func _toggle_probe_volume() -> void:
	if _probe_volume.is_inside_tree():
		root.remove_child(_probe_volume)
	else:
		root.add_child(_probe_volume)


# A dynamic panel, so its mesh stays live while the static pack covers the room.
func _add_material_swap_panel() -> void:
	var box := BoxMesh.new()
	box.size = Vector3(3.0, 2.0, 0.3)
	var panel := _add_mesh_with_geometry(box, Vector3(-3.0, 0.0, -3.0), "ResonanceDynamicGeometry")
	_swap_geometry = panel.get_child(0)
	for absorption in [0.05, 0.6]:
		var mat: Resource = ClassDB.instantiate("ResonanceMaterial")
		mat.set("absorption_low", absorption)
		mat.set("absorption_mid", absorption)
		mat.set("absorption_high", absorption)
		_swap_materials.append(mat)


func _swap_material(delta: float) -> void:
	var rate := float(_scenario.get("material_swaps_per_sec", 0.0))
	if rate <= 0.0 or _swap_geometry == null:
		return
	_swap_accum += delta * rate
	while _swap_accum >= 1.0:
		_swap_accum -= 1.0
		var next := _swap_materials[material_swap_us.size() % _swap_materials.size()]
		material_swap_us.append(_timed_us(func(): _swap_geometry.set("material", next)))


func _timed_us(action: Callable) -> float:
	var t0 := Time.get_ticks_usec()
	action.call()
	return float(Time.get_ticks_usec() - t0)


func _record_spawn_phases(p: Node) -> void:
	if not p.has_method("get_spawn_timing"):
		return
	var timing: Dictionary = p.call("get_spawn_timing")
	for key in spawn_phase_us:
		spawn_phase_us[key].append(float(timing.get(key, 0)))


func _add_player(index: int, count: int) -> void:
	var p := _make_player(_loop_stream)
	p.position = _source_position(index, count, 0.0)
	root.add_child(p)
	players.append(p)


func _make_player(stream: AudioStream) -> Node3D:
	var config := ResonancePlayerConfig.new()
	var overrides: Dictionary = _scenario.get("player", {})
	for key in overrides:
		config.set(key, overrides[key])
	var p: Node3D = ClassDB.instantiate("ResonancePlayer")
	p.set("player_config", config)
	p.set("stream", stream)
	return p


func _source_position(index: int, count: int, phase: float) -> Vector3:
	var radius := float(_scenario.get("source_distance", 5.0))
	var angle := TAU * float(index) / float(maxi(count, 1)) + phase
	return Vector3(cos(angle) * radius, 0.0, sin(angle) * radius)


func _make_runtime_config(overrides: Dictionary) -> Resource:
	var cfg := ResonanceRuntimeConfig.new()
	for key in overrides:
		cfg.set(key, overrides[key])
	return cfg


func _add_geometry(kind: String) -> void:
	if kind == "none":
		return
	_add_room()
	if kind == "dense_room":
		_add_dense_clutter()


func _add_room() -> void:
	var h := ROOM_SIZE * 0.5
	var t := WALL_THICKNESS
	_add_static_box(Vector3(0, -h.y, 0), Vector3(ROOM_SIZE.x, t, ROOM_SIZE.z))
	_add_static_box(Vector3(0, h.y, 0), Vector3(ROOM_SIZE.x, t, ROOM_SIZE.z))
	_add_static_box(Vector3(-h.x, 0, 0), Vector3(t, ROOM_SIZE.y, ROOM_SIZE.z))
	_add_static_box(Vector3(h.x, 0, 0), Vector3(t, ROOM_SIZE.y, ROOM_SIZE.z))
	_add_static_box(Vector3(0, 0, -h.z), Vector3(ROOM_SIZE.x, ROOM_SIZE.y, t))
	_add_static_box(Vector3(0, 0, h.z), Vector3(ROOM_SIZE.x, ROOM_SIZE.y, t))
	# Inner wall between listener and half of the sources, so occlusion has work to do.
	_add_static_box(Vector3(0, 0, 2.5), Vector3(8.0, ROOM_SIZE.y, t))


func _add_static_scene_and_probe_volume() -> void:
	_static_scene = ClassDB.instantiate("ResonanceStaticScene")
	root.add_child(_static_scene)
	_static_scene.owner = root
	_probe_volume = _make_probe_volume(ClassDB.instantiate("ResonanceProbeData"))
	root.add_child(_probe_volume)


func _make_probe_volume(probe_data: Resource) -> Node3D:
	var volume: Node3D = ClassDB.instantiate("ResonanceProbeVolume")
	volume.set("region_size", ROOM_SIZE * 0.9)
	volume.set("headless_baking_mode", true)
	volume.set("probe_data", probe_data)
	return volume


# Each sphere has segments * rings * 2 triangles (~2k); 25 of them give ~50k.
func _add_dense_clutter() -> void:
	var sphere := SphereMesh.new()
	sphere.radial_segments = DENSE_SPHERE_SEGMENTS
	sphere.rings = DENSE_SPHERE_RINGS
	sphere.radius = 0.5
	sphere.height = 1.0
	for i in DENSE_CLUTTER_COUNT:
		var pos := Vector3(float(i % 5) * 3.0 - 6.0, -2.0, float(i / 5) * 3.0 - 6.0)
		_add_mesh_with_geometry(sphere, pos, "ResonanceStaticGeometry")


func _add_static_box(pos: Vector3, size: Vector3) -> void:
	var box := BoxMesh.new()
	box.size = size
	_add_mesh_with_geometry(box, pos, "ResonanceStaticGeometry")


func _add_mover(index: int) -> void:
	var box := BoxMesh.new()
	box.size = Vector3(1.5, 2.0, 0.3)
	var pos := Vector3(0.0, 0.0, -2.0 - float(index) * 0.5)
	movers.append(_add_mesh_with_geometry(box, pos, "ResonanceDynamicGeometry"))


func _add_mesh_with_geometry(mesh: Mesh, pos: Vector3, geometry_class: String) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = pos
	mi.add_child(ClassDB.instantiate(geometry_class))
	root.add_child(mi)
	return mi


# Noise instead of a sine: reverb and HRTF filters see broadband input, as with real game sounds.
func _make_noise_stream(seconds: float, looping: bool) -> AudioStreamWAV:
	var frames := int(MIX_RATE * seconds)
	var rng := RandomNumberGenerator.new()
	rng.seed = 1234
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, rng.randi_range(-8000, 8000))
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = MIX_RATE
	wav.stereo = false
	wav.data = data
	if looping:
		wav.loop_mode = AudioStreamWAV.LOOP_FORWARD
		wav.loop_end = frames
	return wav
