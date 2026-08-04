@tool
extends ResourceFormatLoader
class_name ResonanceProbeDataLoader

## Custom text loader for ResonanceProbeData [.tres] / [.bak] headers. [.res] stays on the default loader.
## Caps: [code]data[/code] expression max 256 MiB; [code]probe_positions[/code] max 1 MiB (str_to_var safety).


func _read_tres_header(path: String) -> String:
	var pf := FileAccess.open(path, FileAccess.READ)
	if pf == null:
		return ""
	var header := pf.get_line()
	pf.close()
	return header


func _is_tres_resonance_probe_data(path: String) -> bool:
	var ext := path.get_extension().to_lower()
	if ext != "tres" and ext != "bak":
		return false
	return "ResonanceProbeData" in _read_tres_header(path)


func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["tres", "bak"])


func _handles_type(type: StringName) -> bool:
	return type == &"ResonanceProbeData"


func _recognize_path(path: String, type_hint: StringName) -> bool:
	if not (type_hint.is_empty() or type_hint == &"ResonanceProbeData"):
		return false
	return _is_tres_resonance_probe_data(path)


func _get_resource_type(path: String) -> String:
	if _is_tres_resonance_probe_data(path):
		return "ResonanceProbeData"
	return ""


func _load(
	path: String, _original_path: String, _use_sub_threads: bool, _cache_mode: int
) -> Variant:
	if _is_tres_resonance_probe_data(path):
		return _load_tres(path)
	return ERR_FILE_UNRECOGNIZED


func _load_tres(path: String) -> Variant:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return ERR_CANT_OPEN
	var content: String = f.get_as_text()
	f.close()
	var parsed := _parse_tres_data(content)
	if parsed == null:
		return ERR_PARSE_ERROR
	var data_val = parsed.data
	if data_val is not PackedByteArray:
		data_val = PackedByteArray()
	var probe_pos_val = parsed.get("probe_positions", PackedVector3Array())
	if probe_pos_val is not PackedVector3Array:
		probe_pos_val = PackedVector3Array()
	var res := ClassDB.instantiate("ResonanceProbeData") as Resource
	if res == null:
		return ERR_CANT_CREATE
	res.set("data", data_val)
	res.set("probe_positions", probe_pos_val)
	res.set("bake_params_hash", parsed.bake_params_hash)
	res.set("baked_reflection_type", parsed.baked_reflection_type)
	res.set("pathing_params_hash", parsed.pathing_params_hash)
	res.set("static_source_params_hash", parsed.static_source_params_hash)
	res.set("static_listener_params_hash", parsed.static_listener_params_hash)
	res.set("static_scene_params_hash", parsed.static_scene_params_hash)
	res.take_over_path(path)
	return res


## Parses .tres file; returns dict with data, probe_positions, bake_params_hash, etc. or null on error.
func _parse_tres_data(content: String) -> Variant:
	var in_resource := false
	var data_expr := ""
	var probe_positions_expr := ""
	var bake_params_hash := 0
	var baked_reflection_type := -1
	var pathing_params_hash := 0
	var static_source_params_hash := 0
	var static_listener_params_hash := 0
	var static_scene_params_hash := 0
	for line in content.split("\n"):
		var stripped := line.strip_edges()
		if stripped == "[resource]":
			if in_resource:
				break  # Second [resource] block - use first block only
			in_resource = true
			continue
		if not in_resource:
			continue
		if stripped.begins_with("data = "):
			data_expr = stripped.substr(7).strip_edges()
		elif stripped.begins_with("probe_positions = "):
			probe_positions_expr = stripped.substr(18).strip_edges()
		elif stripped.begins_with("bake_params_hash = "):
			bake_params_hash = int(stripped.substr(19))
		elif stripped.begins_with("baked_reflection_type = "):
			baked_reflection_type = int(stripped.substr(23))
		elif stripped.begins_with("pathing_params_hash = "):
			pathing_params_hash = int(stripped.substr(21))
		elif stripped.begins_with("static_source_params_hash = "):
			static_source_params_hash = int(stripped.substr(29))
		elif stripped.begins_with("static_listener_params_hash = "):
			static_listener_params_hash = int(stripped.substr(31))
		elif stripped.begins_with("static_scene_params_hash = "):
			static_scene_params_hash = int(stripped.substr(27))
	var data_result = PackedByteArray()
	# Cap str_to_var payload size (class doc).
	const MAX_DATA_EXPR_LEN := 256 * 1024 * 1024
	if not data_expr.is_empty() and data_expr.length() < MAX_DATA_EXPR_LEN:
		var r = str_to_var(data_expr)
		if r is PackedByteArray:
			data_result = r
		elif r != null:
			push_warning(
				(
					"Nexus Resonance: ResonanceProbeDataLoader: invalid data (expected PackedByteArray), type %s"
					% typeof(r)
				)
			)
	var probe_positions_result = PackedVector3Array()
	# Cap probe_positions expression size.
	const MAX_PROBE_POSITIONS_EXPR_LEN := 1024 * 1024
	if (
		not probe_positions_expr.is_empty()
		and probe_positions_expr.length() < MAX_PROBE_POSITIONS_EXPR_LEN
	):
		var r = str_to_var(probe_positions_expr)
		if r is PackedVector3Array:
			probe_positions_result = r
		elif r != null:
			push_warning(
				(
					"Nexus Resonance: ResonanceProbeDataLoader: invalid probe_positions (expected PackedVector3Array), type %s"
					% typeof(r)
				)
			)
	return {
		"data": data_result,
		"probe_positions": probe_positions_result,
		"bake_params_hash": bake_params_hash,
		"baked_reflection_type": baked_reflection_type,
		"pathing_params_hash": pathing_params_hash,
		"static_source_params_hash": static_source_params_hash,
		"static_listener_params_hash": static_listener_params_hash,
		"static_scene_params_hash": static_scene_params_hash
	}
