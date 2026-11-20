extends GutTest

## A probe volume holds its probe batch exactly while it is in the tree, also when the same
## instance is removed and added back, as with pooled or streamed level chunks. Players keep
## pointing at it for pathing meanwhile.

const ResonanceRuntimeExporter = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_exporter.gd"
)
const ResonanceRuntimeBaker = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_baker.gd"
)
const ReverbBusRoom = preload("res://test/helpers/reverb_bus_room.gd")
const ROOM_SIZE := Vector3(10, 4, 10)
const BAKE_RAYS := 256
const BAKE_THREADS := 4
const SETTLE_FRAMES := 4

var _root: Node3D
var _static_scene: Node
var _baker: ResonanceRuntimeBaker


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "ProbeVolumeReenterRoot"
	add_child_autoqfree(_root)

	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	runtime.set("runtime", ResonanceRuntimeConfig.new())
	_root.add_child(runtime)

	_static_scene = ClassDB.instantiate("ResonanceStaticScene")
	_root.add_child(_static_scene)
	_static_scene.owner = _root

	ReverbBusRoom.add_closed_room(_root, ROOM_SIZE)
	await wait_process_frames(SETTLE_FRAMES)


func after_each() -> void:
	if _baker:
		_baker.shutdown()
		_baker = null


func test_readded_volume_restores_its_probe_batch() -> void:
	var volume := await _bake_volume()
	assert_gte(volume.get_probe_batch_handle(), 0, "the bake must leave a live probe batch")
	var baked_count := _server().get_active_probe_batch_count()

	_root.remove_child(volume)
	await wait_process_frames(SETTLE_FRAMES)
	assert_lt(volume.get_probe_batch_handle(), 0, "a removed volume releases its batch")
	assert_eq(_server().get_active_probe_batch_count(), baked_count - 1)

	_root.add_child(volume)
	await wait_process_frames(SETTLE_FRAMES)
	assert_gte(volume.get_probe_batch_handle(), 0, "a re-added volume reloads its batch")
	assert_eq(_server().get_active_probe_batch_count(), baked_count)


func test_volume_readded_in_the_same_frame_keeps_one_batch() -> void:
	var volume := await _bake_volume()
	var baked_count := _server().get_active_probe_batch_count()

	_root.remove_child(volume)
	_root.add_child(volume)
	_root.remove_child(volume)
	_root.add_child(volume)
	await wait_process_frames(SETTLE_FRAMES)
	assert_gte(volume.get_probe_batch_handle(), 0)
	assert_eq(_server().get_active_probe_batch_count(), baked_count)


func test_new_volume_loads_its_batch_exactly_once() -> void:
	var baked := await _bake_volume()
	var baked_count := _server().get_active_probe_batch_count()

	var volume := ClassDB.instantiate("ResonanceProbeVolume") as ResonanceProbeVolume
	volume.region_size = baked.region_size
	volume.probe_data = baked.probe_data
	_root.add_child(volume)
	await wait_process_frames(SETTLE_FRAMES)
	assert_gte(volume.get_probe_batch_handle(), 0)
	assert_eq(_server().get_active_probe_batch_count(), baked_count + 1)


func test_player_keeps_its_pathing_volume_across_remove_and_readd() -> void:
	var volume := await _bake_volume()
	volume.name = "PathingVolume"
	var player := ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	player.player_config = ResonancePlayerConfig.new()
	player.stream = ReverbBusRoom.make_looping_tone_stream()
	_root.add_child(player)
	player.pathing_probe_volume = player.get_path_to(volume)
	var pathing_path := player.pathing_probe_volume
	player.play()
	await wait_process_frames(SETTLE_FRAMES)

	_root.remove_child(volume)
	await wait_process_frames(SETTLE_FRAMES)
	assert_eq(player.pathing_probe_volume, pathing_path, "removing the volume keeps the reference")

	_root.add_child(volume)
	await wait_process_frames(SETTLE_FRAMES)
	assert_eq(player.get_node_or_null(player.pathing_probe_volume), volume)
	assert_gte(volume.get_probe_batch_handle(), 0)

	player.stop()
	player.player_config = null
	await wait_process_frames(2)


func _bake_volume() -> ResonanceProbeVolume:
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
	var opts := {"static_scene_node": _static_scene, "bake_probes": true, "baker": _baker}
	assert_eq(await ResonanceRuntimeExporter.export_static_async(_root, opts), OK)
	await wait_process_frames(SETTLE_FRAMES)
	return volume


func _server() -> Object:
	return ResonanceServerAccess.get_server_if_initialized()
