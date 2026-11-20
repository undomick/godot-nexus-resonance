extends GutTest

## export_static(_async) with bake_probes must hand its volumes to the runtime baker.

const ResonanceRuntimeExporter = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_exporter.gd"
)
const ResonanceRuntimeBaker = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_baker.gd"
)


class RecordingBaker:
	extends ResonanceRuntimeBaker

	var baked_volumes: Array[Node] = []

	func bake_volumes_to_ram(volumes: Array[Node], _scene_root: Node) -> void:
		baked_volumes = volumes


func _volume_in_new_scene() -> Node:
	var scene_root := Node3D.new()
	add_child_autoqfree(scene_root)
	var vol := ClassDB.instantiate("ResonanceProbeVolume") as Node
	scene_root.add_child(vol)
	return vol


func _bake_planned_volumes(scene_root: Node, opts: Dictionary) -> Array[Node]:
	var baker := RecordingBaker.new()
	var volumes = ResonanceRuntimeExporter._probe_bake_volumes(scene_root, opts)
	ResonanceRuntimeExporter._start_probe_bake(scene_root, volumes, baker)
	baker.shutdown()
	return baker.baked_volumes


func test_collected_volumes_reach_the_baker() -> void:
	var vol := _volume_in_new_scene()
	var baked := _bake_planned_volumes(vol.get_parent(), {})
	assert_eq(baked, [vol] as Array[Node])


func test_untyped_probe_volumes_option_reaches_the_baker() -> void:
	var vol := _volume_in_new_scene()
	var baked := _bake_planned_volumes(vol.get_parent(), {"probe_volumes": [vol]})
	assert_eq(baked, [vol] as Array[Node])
