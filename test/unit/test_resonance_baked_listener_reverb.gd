extends GutTest

## Baked listener reverb must play the reverb probes baked at runtime: audible with probes,
## silent without listener reverb and silent when the scene has no baked probes.

const ResonanceRuntimeExporter = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_exporter.gd"
)
const ResonanceRuntimeBaker = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_baker.gd"
)
const ReverbBusRoom = preload("res://test/helpers/reverb_bus_room.gd")
const ROOM_SIZE := Vector3(10, 4, 10)
const SILENT_DB := -60.0
const MEASURE_SECONDS := 1.5
const BAKE_RAYS := 1024
const BAKE_THREADS := 4

var _root: Node3D
var _static_scene: Node
var _listener: ResonanceListener
var _player: ResonancePlayer
var _baker: ResonanceRuntimeBaker


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "BakedListenerReverbRoot"
	add_child_autoqfree(_root)

	# Realtime rays on, so a fallback from Baked to realtime reverb would be audible.
	var config := ResonanceRuntimeConfig.new()
	config.realtime_rays = 1024
	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	runtime.set("runtime", config)
	_root.add_child(runtime)

	_static_scene = ClassDB.instantiate("ResonanceStaticScene")
	_root.add_child(_static_scene)
	_static_scene.owner = _root

	ReverbBusRoom.add_closed_room(_root, ROOM_SIZE)

	var camera := Camera3D.new()
	camera.current = true
	camera.position = Vector3(0, 1.6, 2)
	_root.add_child(camera)
	_listener = ClassDB.instantiate("ResonanceListener") as ResonanceListener
	_listener.reverb_type = ResonanceListener.REVERB_BAKED
	camera.add_child(_listener)

	_player = ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	_player.player_config = ResonancePlayerConfig.new()
	_player.stream = ReverbBusRoom.make_looping_tone_stream()
	_player.position = Vector3(0, 1.6, -2)
	_root.add_child(_player)

	await wait_process_frames(4)


func after_each() -> void:
	if _baker:
		_baker.shutdown()
		_baker = null
	if not is_instance_valid(_player):
		return
	if _player.is_playing():
		_player.stop()
	_player.player_config = null
	await wait_process_frames(2)


func test_baked_listener_reverb_is_audible_with_baked_probes() -> void:
	var volume := await _bake_reverb_probes()
	assert_gt(volume.probe_data.get_data().size(), 0, "bake must produce probe data")

	_listener.apply_reverb = true
	var peak_db := await _play_and_measure_reverb_bus()
	assert_gt(peak_db, SILENT_DB, "baked listener reverb must reach the reverb bus")


func test_reverb_bus_is_silent_without_listener_reverb() -> void:
	await _bake_reverb_probes()

	_listener.apply_reverb = false
	var peak_db := await _play_and_measure_reverb_bus()
	assert_lt(peak_db, SILENT_DB, "without listener reverb nothing may reach the reverb bus")


func test_baked_listener_reverb_is_silent_without_baked_probes() -> void:
	assert_eq(await _export_static({}), OK)

	_listener.apply_reverb = true
	var peak_db := await _play_and_measure_reverb_bus()
	assert_lt(peak_db, SILENT_DB, "Baked must not fall back to realtime reverb")


func _bake_reverb_probes() -> ResonanceProbeVolume:
	var volume := ClassDB.instantiate("ResonanceProbeVolume") as ResonanceProbeVolume
	volume.position = Vector3(0, ROOM_SIZE.y * 0.5, 0)
	volume.region_size = ROOM_SIZE * 0.9
	volume.headless_baking_mode = true
	var bake_config := ResonanceBakeConfig.new()
	bake_config.bake_num_rays = BAKE_RAYS
	bake_config.bake_num_threads = BAKE_THREADS
	volume.bake_config = bake_config
	volume.probe_data = ResonanceProbeData.new()
	_root.add_child(volume)

	_baker = ResonanceRuntimeBaker.new()
	assert_eq(await _export_static({"bake_probes": true, "baker": _baker}), OK)
	return volume


func _export_static(extra_opts: Dictionary) -> int:
	var opts := {"static_scene_node": _static_scene}
	opts.merge(extra_opts)
	return await ResonanceRuntimeExporter.export_static_async(_root, opts)


func _play_and_measure_reverb_bus() -> float:
	_player.play()
	await wait_process_frames(4)
	return await ReverbBusRoom.peak_reverb_bus_db(get_tree(), MEASURE_SECONDS)
