extends GutTest

## Bake runner teardown must roll back mid-bake probe_data (plugin disable / editor exit).

const BakeRunner = preload("res://addons/nexus_resonance/editor/resonance_bake_runner.gd")
const BakeBackup = preload("res://addons/nexus_resonance/editor/resonance_bake_backup.gd")


func test_shutdown_restores_mutated_probe_data_from_active_bake() -> void:
	var runner = BakeRunner.new(null)
	var vol := ClassDB.instantiate("ResonanceProbeVolume")
	add_child_autoqfree(vol)

	var pd := ClassDB.instantiate("ResonanceProbeData") as Resource
	assert_not_null(pd)
	var original := PackedByteArray([10, 20, 30])
	pd.set_data(original)
	pd.set("bake_params_hash", 11)
	pd.set("pathing_params_hash", 22)
	vol.set_probe_data(pd)

	var volumes: Array[Node] = [vol]
	runner._backup = BakeBackup.new()
	runner._backup.create_backups(volumes, false)
	runner._active_bake_volumes = volumes
	runner._bake_in_progress = true

	# Mid-pipeline mutation (reflections done, pathing/static not finished).
	pd.set_data(PackedByteArray([99]))
	pd.set("bake_params_hash", 0)
	pd.set("pathing_params_hash", 0)

	runner.shutdown()

	assert_eq(pd.get_data(), original, "shutdown must restore pre-bake probe blob")
	assert_eq(int(pd.get("bake_params_hash")), 11)
	assert_eq(int(pd.get("pathing_params_hash")), 22)
	assert_true(runner.is_shutdown())
	assert_false(runner._bake_in_progress)
	assert_eq(runner._active_bake_volumes.size(), 0)


func test_finish_pipeline_is_noop_after_shutdown() -> void:
	var runner = BakeRunner.new(null)
	runner._shutdown_completed = true
	runner._bake_in_progress = true
	# Must not clear flags via deferred finish after teardown already restored.
	runner._finish_pipeline(false, null, [] as Array[Node])
	assert_true(runner._bake_in_progress, "finish after shutdown must not race teardown state")
