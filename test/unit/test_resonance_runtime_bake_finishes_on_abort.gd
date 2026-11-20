extends GutTest

## A runtime bake that stops before the pipeline starts still emits bake_finished,
## so callers awaiting it (export_static_async) do not hang.

const ResonanceRuntimeBaker = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_baker.gd"
)


func test_bake_without_exported_static_scene_finishes() -> void:
	var scene_root := Node3D.new()
	add_child_autoqfree(scene_root)
	var vol := ClassDB.instantiate("ResonanceProbeVolume") as Node
	scene_root.add_child(vol)
	var baker := ResonanceRuntimeBaker.new()

	baker.bake_volumes_to_ram([vol] as Array[Node], scene_root)

	assert_true(await wait_for_signal(baker.bake_finished, 1.0))
	assert_push_error("Static scene not exported")
	baker.shutdown()


func test_bake_without_volumes_finishes() -> void:
	var baker := ResonanceRuntimeBaker.new()

	baker.bake_volumes_to_ram([] as Array[Node], null)

	assert_true(await wait_for_signal(baker.bake_finished, 1.0))
	assert_push_error("Runtime bake needs volumes")
	baker.shutdown()
