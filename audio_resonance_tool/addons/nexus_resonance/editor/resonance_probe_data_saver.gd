@tool
extends ResourceFormatSaver
class_name ResonanceProbeDataSaver

## Text [.tres] saver for ResonanceProbeData (VC-friendly). [.res] defers to the default saver.
## [.bak] saves are rejected (backups are file copies; saving here caused take_over_path → .bak chains).
## Writes via tmp + rename. Per-path Mutex serializes concurrent saves to the same file.


static var _path_mutexes: Dictionary = {}


static func _mutex_for_path(target_path: String) -> Mutex:
	if not _path_mutexes.has(target_path):
		_path_mutexes[target_path] = Mutex.new()
	return _path_mutexes[target_path]


func _recognize(resource: Resource) -> bool:
	return resource != null and resource.get_class() == "ResonanceProbeData"


func _get_recognized_extensions(_resource: Resource) -> PackedStringArray:
	return PackedStringArray(["tres"])


func _save(resource: Resource, path: String, _flags: int) -> Error:
	if resource.get_class() != "ResonanceProbeData":
		return ERR_INVALID_PARAMETER
	# Refuse .bak: backups are copies; take_over_path on .bak used to break subsequent saves.
	if path.ends_with(".bak"):
		push_warning(
			"Nexus Resonance: Refusing to save ResonanceProbeData to a .bak path (%s)." % path
		)
		return ERR_INVALID_PARAMETER
	if path.get_extension().to_lower() == "res":
		return ResourceSaver.save(resource, path, _flags)
	var data: PackedByteArray = resource.get("data")
	if data == null:
		data = PackedByteArray()
	var probe_positions_raw = (
		resource.get("probe_positions") if "probe_positions" in resource else null
	)
	var probe_positions: PackedVector3Array = (
		probe_positions_raw if probe_positions_raw is PackedVector3Array else PackedVector3Array()
	)
	var bake_params_hash: int = resource.get("bake_params_hash")
	var baked_reflection_type: int = resource.get("baked_reflection_type")
	var pathing_params_hash: int = (
		resource.get("pathing_params_hash") if "pathing_params_hash" in resource else 0
	)
	var static_source_params_hash: int = (
		resource.get("static_source_params_hash") if "static_source_params_hash" in resource else 0
	)
	var static_listener_params_hash: int = (
		resource.get("static_listener_params_hash")
		if "static_listener_params_hash" in resource
		else 0
	)
	var static_scene_params_hash: int = (
		resource.get("static_scene_params_hash") if "static_scene_params_hash" in resource else 0
	)
	var target_path: String = path if path.ends_with(".tres") else path.get_basename() + ".tres"
	var save_mutex: Mutex = _mutex_for_path(target_path)
	save_mutex.lock()
	var tmp_path := target_path + ".tmp"
	var data_str := var_to_str(data)
	var probe_pos_str := var_to_str(probe_positions)
	var content := (
		'[gd_resource type="ResonanceProbeData" format=3]\n\n[resource]\ndata = %s\nprobe_positions = %s\nbake_params_hash = %d\nbaked_reflection_type = %d\npathing_params_hash = %d\nstatic_source_params_hash = %d\nstatic_listener_params_hash = %d\nstatic_scene_params_hash = %d\n'
		% [
			data_str,
			probe_pos_str,
			bake_params_hash,
			baked_reflection_type,
			pathing_params_hash,
			static_source_params_hash,
			static_listener_params_hash,
			static_scene_params_hash
		]
	)
	var f := FileAccess.open(tmp_path, FileAccess.WRITE)
	if f == null:
		save_mutex.unlock()
		return FileAccess.get_open_error()
	f.store_string(content)
	f.close()
	var rename_err := DirAccess.rename_absolute(tmp_path, target_path)
	if rename_err != OK:
		if FileAccess.file_exists(tmp_path):
			DirAccess.remove_absolute(tmp_path)
		push_warning(
			(
				"Nexus Resonance: Atomic probe save failed (rename), error %d. Temp removed."
				% rename_err
			)
		)
		save_mutex.unlock()
		return rename_err
	resource.take_over_path(target_path)
	save_mutex.unlock()
	return OK
