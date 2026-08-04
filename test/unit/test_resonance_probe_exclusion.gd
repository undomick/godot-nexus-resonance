extends GutTest

## ResonanceProbeExclusion under ResonanceProbeVolume: collect + OBB filter.

var _root: Node3D
var _vol: Node
var _excl: Node


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "Root"
	add_child_autoqfree(_root)

	_vol = ClassDB.instantiate("ResonanceProbeVolume")
	_vol.name = "ProbeVolume"
	_vol.set("region_size", Vector3(10, 4, 10))
	_vol.set("generation_type", 2) # Volume grid
	_vol.set("spacing", 2.0)
	_root.add_child(_vol)

	_excl = ClassDB.instantiate("ResonanceProbeExclusion")
	_excl.name = "Exclusion"
	_excl.set("region_size", Vector3(2, 2, 2))
	_excl.position = Vector3(0, 0, 0)
	_vol.add_child(_excl)

	await wait_process_frames(1)


func test_collect_exclusion_boxes_finds_enabled_child() -> void:
	var boxes: Array = _vol.collect_exclusion_boxes()
	assert_eq(boxes.size(), 1)
	assert_eq(typeof(boxes[0]), TYPE_DICTIONARY)
	assert_eq(boxes[0]["size"], Vector3(2, 2, 2))


func test_disabled_exclusion_is_skipped() -> void:
	_excl.set("enabled", false)
	var boxes: Array = _vol.collect_exclusion_boxes()
	assert_eq(boxes.size(), 0)


func test_nested_exclusion_is_found() -> void:
	var mid := Node3D.new()
	mid.name = "Mid"
	_vol.add_child(mid)
	var nested = ClassDB.instantiate("ResonanceProbeExclusion")
	nested.name = "NestedExcl"
	nested.set("region_size", Vector3(1, 1, 1))
	mid.add_child(nested)
	await wait_process_frames(1)
	assert_eq(_vol.collect_exclusion_boxes().size(), 2)


func test_filter_via_bake_params_hash_changes_when_exclusion_moves() -> void:
	var h0: int = _vol.get_bake_params_hash()
	_excl.position = Vector3(3, 0, 0)
	await wait_process_frames(1)
	var h1: int = _vol.get_bake_params_hash()
	assert_ne(h0, h1, "moving exclusion must change bake_params_hash")
