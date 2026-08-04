extends GutTest

## Clear-unreferenced must abort when the project scan was cancelled mid-way.

const ProbeClearPolicy = preload(
	"res://addons/nexus_resonance/editor/resonance_probe_clear_policy.gd"
)


func test_cancel_aborts_without_offering_deletes() -> void:
	var probe_files := PackedStringArray(
		[
			"res://resonance_data/batches/level_a_vol_batch.tres",
			"res://resonance_data/batches/level_b_vol_batch.tres",
		]
	)
	# Only level_a was scanned before cancel; level_b is still referenced by an unscanned scene.
	var referenced := PackedStringArray(["res://resonance_data/batches/level_a_vol_batch.tres"])
	var plan: Dictionary = ProbeClearPolicy.build_clear_plan(probe_files, referenced, true)
	assert_true(plan.get("aborted", false), "cancelled scan must abort clear")
	assert_eq(
		(plan.get("to_delete", PackedStringArray()) as PackedStringArray).size(),
		0,
		"cancelled scan must not list any deletes"
	)


func test_complete_scan_lists_only_unreferenced() -> void:
	var probe_files := PackedStringArray(
		[
			"res://resonance_data/batches/keep_batch.tres",
			"res://resonance_data/batches/orphan_batch.tres",
		]
	)
	var referenced := PackedStringArray(["res://resonance_data/batches/keep_batch.tres"])
	var plan: Dictionary = ProbeClearPolicy.build_clear_plan(probe_files, referenced, false)
	assert_false(plan.get("aborted", true), "complete scan must not abort")
	var to_delete: PackedStringArray = plan.get("to_delete", PackedStringArray())
	assert_eq(to_delete.size(), 1)
	assert_eq(to_delete[0], "res://resonance_data/batches/orphan_batch.tres")


func test_complete_scan_all_referenced_yields_empty_delete_list() -> void:
	var probe_files := PackedStringArray(["res://resonance_data/batches/only_batch.tres"])
	var referenced := PackedStringArray(["res://resonance_data/batches/only_batch.tres"])
	var plan: Dictionary = ProbeClearPolicy.build_clear_plan(probe_files, referenced, false)
	assert_false(plan.get("aborted", true))
	assert_eq((plan.get("to_delete", PackedStringArray()) as PackedStringArray).size(), 0)
