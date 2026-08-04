extends GutTest

## Clear Unreferenced must treat live edited-scene probe_data as referenced.

const ProbeRefIndex = preload(
	"res://addons/nexus_resonance/editor/resonance_probe_reference_index.gd"
)


func test_canonical_probe_resource_path_strips_bak_suffix() -> void:
	assert_eq(
		ProbeRefIndex.canonical_probe_resource_path(
			"res://resonance_data/batches/level_vol_batch.res.bak"
		),
		"res://resonance_data/batches/level_vol_batch.res"
	)


func test_collect_live_probe_data_paths_from_edited_tree() -> void:
	var root := Node.new()
	add_child_autoqfree(root)
	var vol := ClassDB.instantiate("ResonanceProbeVolume")
	root.add_child(vol)

	var pd := ClassDB.instantiate("ResonanceProbeData") as Resource
	assert_not_null(pd)
	var path := "res://resonance_data/batches/unsaved_vol_batch.res"
	if pd.has_method("take_over_path"):
		pd.take_over_path(path)
	else:
		pd.resource_path = path
	vol.set_probe_data(pd)

	var live: PackedStringArray = ProbeRefIndex.collect_live_probe_data_paths(root)
	assert_eq(live.size(), 1)
	assert_eq(live[0], path)


func test_merge_keeps_live_only_paths_out_of_delete_set() -> void:
	# Disk .tscn scan missed the bake because the scene was still unsaved.
	var disk_referenced := PackedStringArray([])
	var live_referenced := PackedStringArray(["res://resonance_data/batches/unsaved_vol_batch.res"])
	var merged: PackedStringArray = ProbeRefIndex.merge_referenced_paths(
		disk_referenced, live_referenced
	)
	var probe_files := PackedStringArray(
		[
			"res://resonance_data/batches/unsaved_vol_batch.res",
			"res://resonance_data/batches/orphan_batch.res",
		]
	)
	var to_delete: PackedStringArray = []
	for p in probe_files:
		if p not in merged:
			to_delete.append(p)
	assert_eq(to_delete.size(), 1)
	assert_eq(to_delete[0], "res://resonance_data/batches/orphan_batch.res")
