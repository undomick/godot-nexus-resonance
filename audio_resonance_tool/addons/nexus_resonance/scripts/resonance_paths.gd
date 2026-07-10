extends RefCounted
class_name ResonancePaths

## Logical [code]res://[/code] paths and helpers (bake/export). Prefer [method get_audio_data_dir] for output root.

const PATH_AUDIO_DATA := "res://audio_data/"
const PATH_RESONANCE_MESHES := "res://resonance_meshes/"

const _OUTPUT_DIR_SETTING := "nexus/nexus_resonance/bake/default_output_directory"
const _STATIC_SCENE_FORMAT_SETTING := "nexus/nexus_resonance/export/static_scene_asset_format"
const _PROBE_DATA_FORMAT_SETTING := "nexus/nexus_resonance/export/probe_data_format"


static func _export_setting_use_res(setting_key: String) -> bool:
	var v: Variant = ProjectSettings.get_setting(setting_key, 0)
	if v == null:
		return false
	var t := typeof(v)
	if t == TYPE_INT:
		return int(v) == 1
	if t == TYPE_FLOAT:
		return clampi(int(round(v)), 0, 1) == 1
	return false


## Project Setting output dir (with legacy key fallback). Trailing [code]/[/code]. Default [constant PATH_AUDIO_DATA].
static func get_audio_data_dir() -> String:
	const LEGACY := "nexus/nexus_resonance/bake/output_dir"
	var key := _OUTPUT_DIR_SETTING
	if not ProjectSettings.has_setting(key) and ProjectSettings.has_setting(LEGACY):
		key = LEGACY
	if ProjectSettings.has_setting(key):
		var dir: String = ProjectSettings.get_setting(key, PATH_AUDIO_DATA)
		if not dir.is_empty():
			return dir if dir.ends_with("/") else dir + "/"
	return PATH_AUDIO_DATA


## [code].tres[/code] vs [code].res[/code] for static scene export (project setting).
static func get_static_scene_asset_extension() -> String:
	return "res" if _export_setting_use_res(_STATIC_SCENE_FORMAT_SETTING) else "tres"


## [code]audio_data/{{scene}}_static.{{ext}}[/code]
static func static_scene_asset_save_path(scene_basename: String) -> String:
	return get_audio_data_dir() + scene_basename + "_static." + get_static_scene_asset_extension()


## Probe batch file extension (project setting; matches custom loader/saver).
static func get_probe_data_asset_extension() -> String:
	return "res" if _export_setting_use_res(_PROBE_DATA_FORMAT_SETTING) else "tres"


## [code]audio_data/{{scene}}_{{node}}_batch.{{ext}}[/code] (same as bake pipeline / editor).
static func probe_data_save_path(scene_basename: String, node_key: String) -> String:
	return (
		get_audio_data_dir()
		+ scene_basename
		+ "_"
		+ node_key
		+ "_batch."
		+ get_probe_data_asset_extension()
	)
