extends GutTest

## Exporting static geometry while the game runs must not change the convolution wet level.

const ResonanceRuntimeExporter = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_exporter.gd"
)
const ReverbBusRoom = preload("res://test/helpers/reverb_bus_room.gd")
const SILENT_DB := -60.0
const MAX_LEVEL_CHANGE_DB := 12.0
const MEASURE_SECONDS := 1.5
## Longer than the realtime IR, so the tail from before the export has died out.
const TAIL_SETTLE_SECONDS := 2.5

var _root: Node3D
var _player: ResonancePlayer


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "StaticExportReverbRoot"
	add_child_autoqfree(_root)

	var config := ResonanceRuntimeConfig.new()
	config.realtime_rays = 1024
	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	runtime.set("runtime", config)
	_root.add_child(runtime)

	ReverbBusRoom.add_closed_room(_root, Vector3(10, 4, 10))

	var camera := Camera3D.new()
	camera.current = true
	camera.position = Vector3(0, 1.6, 2)
	_root.add_child(camera)
	var listener: Node = ClassDB.instantiate("ResonanceListener")
	listener.set("apply_reverb", true)
	camera.add_child(listener)

	_player = ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	_player.player_config = ResonancePlayerConfig.new()
	_player.stream = ReverbBusRoom.make_looping_tone_stream()
	_player.position = Vector3(0, 1.6, -2)
	_root.add_child(_player)

	await wait_process_frames(4)
	_player.play()


func after_each() -> void:
	if not is_instance_valid(_player):
		return
	if _player.is_playing():
		_player.stop()
	_player.player_config = null
	await wait_process_frames(2)


func test_reverb_bus_level_survives_runtime_static_export() -> void:
	var before_db := await ReverbBusRoom.peak_reverb_bus_db(get_tree(), MEASURE_SECONDS)
	assert_gt(before_db, SILENT_DB, "wet path must be audible before export")

	var asset := ResonanceRuntimeExporter.export_asset(_root)
	assert_not_null(asset)

	await ReverbBusRoom.peak_reverb_bus_db(get_tree(), TAIL_SETTLE_SECONDS)
	var after_db := await ReverbBusRoom.peak_reverb_bus_db(get_tree(), MEASURE_SECONDS)
	assert_lt(absf(after_db - before_db), MAX_LEVEL_CHANGE_DB, "export changed the wet level")
