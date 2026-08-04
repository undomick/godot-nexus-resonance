@tool
extends EditorImportPlugin

## Imports .sofa HRTF files as ResonanceSOFAAsset.
## Volume and norm can be adjusted per-file in import options.


func _get_importer_name() -> String:
	return "nexus_resonance.sofa"


func _get_visible_name() -> String:
	return "Nexus Resonance SOFA HRTF"


func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["sofa"])


func _get_save_extension() -> String:
	return "tres"


func _get_resource_type() -> String:
	return "Resource"


func _get_priority() -> float:
	return 1.0


func _get_import_options(_path: String, _preset_index: int) -> Array:
	return [
		{
			"name": "volume_db",
			"default_value": 0.0,
			"property_hint": PROPERTY_HINT_RANGE,
			"hint_string": "-24,24,0.5"
		},
		{
			"name": "norm_type",
			"default_value": 0,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "None,RMS"
		}
	]


func _get_preset_count() -> int:
	return 0


func _get_preset_name(_preset_index: int) -> String:
	return ""


func _import(
	source_file: String,
	save_path: String,
	options: Dictionary,
	_platform_variants: Array,
	gen_files: Array
) -> Error:
	var data := FileAccess.get_file_as_bytes(source_file)
	if data.is_empty():
		return ERR_FILE_CANT_READ

	if not ClassDB.class_exists("ResonanceSOFAAsset"):
		push_error("Nexus Resonance: ResonanceSOFAAsset not available.")
		return ERR_UNAVAILABLE

	var asset: Resource = ClassDB.instantiate("ResonanceSOFAAsset")
	asset.set("sofa_data", data)
	asset.set("volume_db", options.get("volume_db", 0.0))
	asset.set("norm_type", options.get("norm_type", 0))

	var out_path := save_path + ".tres"
	var err := ResourceSaver.save(asset, out_path)
	if err != OK:
		return err
	# Also save beside .sofa for easy reference
	var beside_path := source_file.get_basename() + ".tres"
	if beside_path != out_path:
		var beside_err := ResourceSaver.save(asset, beside_path)
		if beside_err != OK:
			return beside_err
		gen_files.append(beside_path)
	return OK
