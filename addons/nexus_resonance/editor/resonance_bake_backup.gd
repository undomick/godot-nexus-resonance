@tool
extends RefCounted

## Backs up probe data before a bake, on disk for Undo and in memory for rolling back a failed bake.
##
## Disk backups are plain file copies: saving through the probe saver could call
## [code]take_over_path[/code] and redirect later saves to the .bak file.
## They are keyed by instance id because preparing a bake can move [member Resource.resource_path].

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)

var _backup_paths: Dictionary = {}  # original resource_path -> backup file path
var _backup_by_instance_id: Dictionary = {}  # instance_id -> { original_path, backup_path }
var _memory_snapshots: Dictionary = {}


## "foo.res.bak.bak" -> "foo.res"
static func _strip_bak_suffixes(path: String) -> String:
	var p := path
	while p.ends_with(".bak"):
		p = p.get_basename()
	return p


## Looks up the disk backup by instance id first, which survives path migration, then by
## resource_path. Returns an empty Dictionary on a miss.
static func resolve_disk_backup(
	backup_by_instance_id: Dictionary, backup_by_path: Dictionary, pd: Resource
) -> Dictionary:
	if pd == null:
		return {}
	var entry: Variant = backup_by_instance_id.get(pd.get_instance_id(), null)
	if entry is Dictionary:
		var backup_path: String = str(entry.get("backup_path", ""))
		var original_path: String = str(entry.get("original_path", ""))
		var save_path: String = ""
		if pd.resource_path and pd.resource_path.get_file().length() > 0:
			save_path = _strip_bak_suffixes(pd.resource_path)
		if save_path.is_empty():
			save_path = original_path
		if backup_path.is_empty() or save_path.is_empty():
			return {}
		return {"backup_path": backup_path, "save_path": save_path, "original_path": original_path}
	if not pd.resource_path or pd.resource_path.get_file().length() == 0:
		return {}
	var lookup_path: String = _strip_bak_suffixes(pd.resource_path)
	var path_backup: String = str(backup_by_path.get(lookup_path, ""))
	if path_backup.is_empty():
		return {}
	return {"backup_path": path_backup, "save_path": lookup_path, "original_path": lookup_path}


static func _snapshot_probe_data(pd: Resource) -> Dictionary:
	var data_val: PackedByteArray = PackedByteArray()
	if pd.has_method("get_data"):
		var raw: Variant = pd.get_data()
		if raw is PackedByteArray:
			data_val = (raw as PackedByteArray).duplicate()
	var positions: PackedVector3Array = PackedVector3Array()
	if "probe_positions" in pd:
		var pos_raw: Variant = pd.get("probe_positions")
		if pos_raw is PackedVector3Array:
			positions = (pos_raw as PackedVector3Array).duplicate()
	return {
		"data": data_val,
		"probe_positions": positions,
		"bake_params_hash": pd.get("bake_params_hash") if "bake_params_hash" in pd else 0,
		"baked_reflection_type":
		pd.get("baked_reflection_type") if "baked_reflection_type" in pd else -1,
		"baked_ambisonics_order":
		pd.get("baked_ambisonics_order") if "baked_ambisonics_order" in pd else -1,
		"pathing_params_hash": pd.get("pathing_params_hash") if "pathing_params_hash" in pd else 0,
		"static_source_params_hash":
		pd.get("static_source_params_hash") if "static_source_params_hash" in pd else 0,
		"static_listener_params_hash":
		pd.get("static_listener_params_hash") if "static_listener_params_hash" in pd else 0,
		"static_scene_params_hash":
		pd.get("static_scene_params_hash") if "static_scene_params_hash" in pd else 0,
	}


static func _apply_probe_data_snapshot(pd: Resource, snap: Dictionary) -> void:
	if pd.has_method("set_data") and snap.has("data"):
		pd.set_data(snap["data"])
	for prop in [
		"probe_positions",
		"bake_params_hash",
		"baked_reflection_type",
		"baked_ambisonics_order",
		"pathing_params_hash",
		"static_source_params_hash",
		"static_listener_params_hash",
		"static_scene_params_hash",
	]:
		if snap.has(prop) and prop in pd:
			pd.set(prop, snap[prop])


func create_backups(volumes: Array[Node], make_disk_copies: bool = true) -> void:
	_backup_paths.clear()
	_backup_by_instance_id.clear()
	_memory_snapshots.clear()
	for vol in volumes:
		var pd = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		if not pd:
			continue
		_memory_snapshots[pd.get_instance_id()] = _snapshot_probe_data(pd)
		if not make_disk_copies:
			continue
		if not pd.resource_path or pd.resource_path.get_file().length() == 0:
			continue
		var original_path: String = _strip_bak_suffixes(pd.resource_path)
		# A resource_path that points at a .bak file would send later saves into the backup.
		if original_path != pd.resource_path and pd.has_method("take_over_path"):
			pd.take_over_path(original_path)
		if not FileAccess.file_exists(original_path):
			continue
		var backup_path: String = original_path + ".bak"
		var err: int = DirAccess.copy_absolute(original_path, backup_path)
		if err == OK:
			_backup_paths[original_path] = backup_path
			_backup_by_instance_id[pd.get_instance_id()] = {
				"original_path": original_path,
				"backup_path": backup_path,
			}
		else:
			push_warning(
				(
					"Nexus Resonance: Failed to create probe data backup at %s (error %d)."
					% [backup_path, err]
				)
			)


func has_backups() -> bool:
	return not _backup_paths.is_empty() or not _backup_by_instance_id.is_empty()


func has_memory_snapshots() -> bool:
	return not _memory_snapshots.is_empty()


func clear_memory_snapshots() -> void:
	_memory_snapshots.clear()


func discard_backups() -> void:
	for backup_path in _backup_paths.values():
		_remove_backup_file(backup_path)
	# The path map owns the files; the instance-id map points at the same ones.
	_backup_paths.clear()
	_backup_by_instance_id.clear()
	_memory_snapshots.clear()


static func _remove_backup_file(backup_path: String) -> void:
	if FileAccess.file_exists(backup_path):
		DirAccess.remove_absolute(backup_path)
	# Backups written with ResourceSaver can leave a .uid file behind.
	var uid_path := backup_path + ".uid"
	if FileAccess.file_exists(uid_path):
		DirAccess.remove_absolute(uid_path)


## Rolls probe data back to the pre-bake snapshots. Returns true if any volume was restored.
func restore_memory_snapshots(volumes: Array[Node], on_reload: Callable = Callable()) -> bool:
	var restored_any := false
	for vol in volumes:
		var pd = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		if not pd:
			continue
		var snap: Variant = _memory_snapshots.get(pd.get_instance_id(), null)
		if snap == null or not (snap is Dictionary):
			continue
		_apply_probe_data_snapshot(pd, snap)
		restored_any = true
		if on_reload.is_valid():
			on_reload.call(pd, volumes)
	_memory_snapshots.clear()
	return restored_any


func restore(
	volumes: Array[Node],
	editor_interface: EditorInterface,
	on_reload: Callable,
	on_complete: Callable
) -> void:
	var restored_any := false
	var failed_any := false
	var restored_backup_paths: Dictionary = {}  # backup_path -> true
	for vol in volumes:
		var pd = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		if not pd:
			continue
		var resolved: Dictionary = resolve_disk_backup(_backup_by_instance_id, _backup_paths, pd)
		if resolved.is_empty():
			continue
		var backup_path: String = str(resolved.get("backup_path", ""))
		var save_path: String = str(resolved.get("save_path", ""))
		if (
			backup_path.is_empty()
			or save_path.is_empty()
			or not FileAccess.file_exists(backup_path)
		):
			failed_any = true
			continue
		var backup: Resource = _load_disk_backup_resource(backup_path, save_path)
		if backup == null:
			failed_any = true
			continue
		if pd.has_method("copy_from"):
			pd.copy_from(backup)
		else:
			_copy_probe_data_properties(pd, backup)
		if pd.has_method("take_over_path"):
			pd.take_over_path(save_path)
		var save_err: int = ResourceSaver.save(pd, save_path)
		if save_err != OK:
			push_warning(
				(
					"Nexus Resonance: Failed to restore probe data to %s (error %d); keeping backup."
					% [save_path, save_err]
				)
			)
			failed_any = true
			continue
		on_reload.call(pd, volumes)
		restored_any = true
		restored_backup_paths[backup_path] = true
	if restored_any and not failed_any:
		ResonanceEditorDialogs.show_success_toast(editor_interface, UIStrings.INFO_BACKUP_RESTORED)
	elif restored_any and failed_any:
		ResonanceEditorDialogs.show_warning(editor_interface, UIStrings.WARN_BACKUP_RESTORE_PARTIAL)
	else:
		ResonanceEditorDialogs.show_warning(editor_interface, UIStrings.WARN_BACKUP_RESTORE_FAILED)
	# Backups that failed to restore stay for a retry.
	for backup_path in restored_backup_paths.keys():
		_remove_backup_file(str(backup_path))
	var remaining_by_id: Dictionary = {}
	for id in _backup_by_instance_id.keys():
		var entry: Variant = _backup_by_instance_id[id]
		if entry is Dictionary:
			var bp: String = str(entry.get("backup_path", ""))
			if not restored_backup_paths.has(bp) and FileAccess.file_exists(bp):
				remaining_by_id[id] = entry
	_backup_by_instance_id = remaining_by_id
	var remaining_by_path: Dictionary = {}
	for path in _backup_paths.keys():
		var bp2: String = str(_backup_paths[path])
		if not restored_backup_paths.has(bp2) and FileAccess.file_exists(bp2):
			remaining_by_path[path] = bp2
	_backup_paths = remaining_by_path
	_memory_snapshots.clear()
	on_complete.call()


## Binary .res.bak files have no loader, so every backup is copied onto the canonical path and loaded there.
static func _load_disk_backup_resource(backup_path: String, save_path: String) -> Resource:
	if save_path.is_empty() or not FileAccess.file_exists(backup_path):
		return null
	var copy_err: int = DirAccess.copy_absolute(backup_path, save_path)
	if copy_err != OK:
		push_warning(
			(
				"Nexus Resonance: Failed to copy probe backup %s -> %s (error %d)."
				% [backup_path, save_path, copy_err]
			)
		)
		return null
	return ResourceLoader.load(save_path, "", ResourceLoader.CACHE_MODE_IGNORE) as Resource


func _copy_probe_data_properties(dst: Resource, src: Resource) -> void:
	if dst.has_method("set_data") and src.has_method("get_data"):
		dst.set_data(src.get_data())
	# Keep in sync with the stored properties of ResonanceProbeData.
	for prop in [
		"probe_positions",
		"bake_params_hash",
		"baked_reflection_type",
		"baked_ambisonics_order",
		"pathing_params_hash",
		"static_source_params_hash",
		"static_listener_params_hash",
		"static_scene_params_hash",
	]:
		if prop in src and prop in dst:
			dst.set(prop, src.get(prop))


func shutdown() -> void:
	_backup_paths.clear()
	_backup_by_instance_id.clear()
	_memory_snapshots.clear()
