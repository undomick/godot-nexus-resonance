extends GutTest

## ResonanceBakeBackup memory snapshots roll back mid-pipeline probe_data mutations.

const BakeBackup = preload("res://addons/nexus_resonance/editor/resonance_bake_backup.gd")


func test_restore_memory_snapshots_rolls_back_mutated_probe_data() -> void:
	var backup = BakeBackup.new()
	var vol := ClassDB.instantiate("ResonanceProbeVolume")
	add_child_autoqfree(vol)

	var pd := ClassDB.instantiate("ResonanceProbeData") as Resource
	assert_not_null(pd, "ResonanceProbeData should instantiate")
	var original := PackedByteArray([1, 2, 3, 4])
	pd.set_data(original)
	pd.set("bake_params_hash", 42)
	pd.set("pathing_params_hash", 7)
	pd.set("static_source_params_hash", 3)
	pd.set("static_listener_params_hash", 5)
	pd.set("static_scene_params_hash", 9)
	pd.set("baked_reflection_type", 2)
	vol.set_probe_data(pd)

	var volumes: Array[Node] = [vol]
	backup.create_backups(volumes)
	assert_true(backup.has_memory_snapshots(), "create_backups should retain memory snapshots")

	# Simulate reflections bake mutating live resource, then a later step failing.
	pd.set_data(PackedByteArray([9, 9, 9]))
	pd.set("bake_params_hash", 99)
	pd.set("pathing_params_hash", 0)
	pd.set("static_source_params_hash", 0)
	pd.set("static_listener_params_hash", 0)
	pd.set("static_scene_params_hash", 0)
	pd.set("baked_reflection_type", 1)

	assert_true(
		backup.restore_memory_snapshots(volumes),
		"restore_memory_snapshots should apply pre-bake state"
	)
	assert_eq(pd.get_data(), original, "probe blob should match pre-bake snapshot")
	assert_eq(int(pd.get("bake_params_hash")), 42)
	assert_eq(int(pd.get("pathing_params_hash")), 7)
	assert_eq(int(pd.get("static_source_params_hash")), 3)
	assert_eq(int(pd.get("static_listener_params_hash")), 5)
	assert_eq(int(pd.get("static_scene_params_hash")), 9)
	assert_eq(int(pd.get("baked_reflection_type")), 2)
	assert_false(backup.has_memory_snapshots(), "snapshots should clear after restore")


func test_discard_backups_clears_memory_snapshots() -> void:
	var backup = BakeBackup.new()
	var vol := ClassDB.instantiate("ResonanceProbeVolume")
	add_child_autoqfree(vol)
	var pd := ClassDB.instantiate("ResonanceProbeData") as Resource
	pd.set_data(PackedByteArray([1]))
	vol.set_probe_data(pd)
	backup.create_backups([vol] as Array[Node])
	assert_true(backup.has_memory_snapshots())
	backup.discard_backups()
	assert_false(backup.has_memory_snapshots())
	assert_false(backup.has_backups())


func test_resolve_disk_backup_survives_resource_path_migration() -> void:
	var pd := ClassDB.instantiate("ResonanceProbeData") as Resource
	assert_not_null(pd, "ResonanceProbeData should instantiate")
	var original := "res://audio_data/batches/old_scene_vol.res"
	var migrated := "res://resonance_data/batches/new_scene_vol.res"
	var bak := original + ".bak"
	var by_id := {
		pd.get_instance_id(): {"original_path": original, "backup_path": bak},
	}
	var by_path := {original: bak}

	# Simulate _prepare_probe_data_for_bake take_over_path to the new canonical layout.
	if pd.has_method("take_over_path"):
		pd.take_over_path(migrated)
	else:
		pd.resource_path = migrated

	var resolved: Dictionary = BakeBackup.resolve_disk_backup(by_id, by_path, pd)
	assert_false(resolved.is_empty(), "instance-id key must find backup after path change")
	assert_eq(str(resolved.get("backup_path", "")), bak)
	assert_eq(
		str(resolved.get("save_path", "")),
		migrated,
		"restore should write into the post-migration canonical path"
	)

	# Path-only lookup must miss after migration (this was the pre-fix Undo failure).
	var path_only: Dictionary = BakeBackup.resolve_disk_backup({}, by_path, pd)
	assert_true(path_only.is_empty(), "path-keyed lookup alone cannot find migrated resources")


func test_load_disk_backup_resource_falls_back_to_file_copy_for_binary_bak() -> void:
	# Binary .res.bak cannot be ResourceLoader.load'd by extension; restore copies onto
	# the canonical .res path and loads that instead.
	var dir := "user://nexus_resonance_bake_backup_test"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dir))
	var save_path := dir + "/probe_batch.res"
	var bak_path := save_path + ".bak"

	var pd := ClassDB.instantiate("ResonanceProbeData") as Resource
	assert_not_null(pd)
	var blob := PackedByteArray([7, 8, 9, 10])
	pd.set_data(blob)
	pd.set("bake_params_hash", 123)
	var save_err: int = ResourceSaver.save(pd, save_path)
	assert_eq(save_err, OK, "seed canonical .res for binary bak test")
	assert_eq(DirAccess.copy_absolute(save_path, bak_path), OK)

	# Corrupt the live file so a successful bak restore is observable.
	pd.set_data(PackedByteArray([1]))
	ResourceSaver.save(pd, save_path)

	var loaded: Resource = BakeBackup._load_disk_backup_resource(bak_path, save_path)
	assert_not_null(loaded, "binary .res.bak must restore via copy+load fallback")
	assert_eq(loaded.get_data(), blob, "restored binary bak must match pre-bake bytes")
	assert_eq(int(loaded.get("bake_params_hash")), 123)

	DirAccess.remove_absolute(ProjectSettings.globalize_path(bak_path))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(save_path))
