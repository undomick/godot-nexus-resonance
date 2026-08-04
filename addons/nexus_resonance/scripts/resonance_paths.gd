extends RefCounted
class_name ResonancePaths

## Logical [code]res://[/code] paths, project-setting keys, and default bus names (bake/export/editor).

const PATH_RESONANCE_DATA := "res://resonance_data/"
const PATH_RESONANCE_DATA_LEGACY := "res://audio_data/"
const SUBDIR_STATICS := "statics/"
const SUBDIR_DYNAMICS := "dynamics/"
const SUBDIR_BATCHES := "batches/"

const SETTINGS_PREFIX := "nexus/resonance/"
const LEGACY_SETTINGS_PREFIX := "audio/nexus_resonance/"

const SETTING_BAKE_OUTPUT_DIR := SETTINGS_PREFIX + "bake/default_output_directory"
const SETTING_BAKE_OUTPUT_DIR_LEGACY := SETTINGS_PREFIX + "bake/output_dir"
const SETTING_RESONANCE_ASSET_FORMAT := SETTINGS_PREFIX + "export/resonance_asset_format"
const SETTING_RESONANCE_ASSET_FORMAT_LEGACY := SETTINGS_PREFIX + "export/static_scene_asset_format"
const SETTING_PROBE_DATA_FORMAT := SETTINGS_PREFIX + "export/probe_data_format"
const SETTING_LOGGER_PREFIX := SETTINGS_PREFIX + "logger/"
const SETTING_EDITOR_AUTO_CONVERT_ANIMATION := (
	SETTINGS_PREFIX + "editor/auto_convert_animation_audio_on_save"
)

## Default wet / reverb bus (editor plugin + runtime config fallbacks).
const DEFAULT_REVERB_BUS_NAME := &"ResonanceReverb"
## Default dry / send target when config bus is empty.
const DEFAULT_OUTPUT_BUS_NAME := &"Master"


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


static func _normalize_dir(dir: String) -> String:
	if dir.is_empty():
		return PATH_RESONANCE_DATA
	return dir if dir.ends_with("/") else dir + "/"


## Project Setting output root (with legacy key fallback). Trailing [code]/[/code]. Default [constant PATH_RESONANCE_DATA].
static func get_resonance_data_dir() -> String:
	var key := SETTING_BAKE_OUTPUT_DIR
	if (
		not ProjectSettings.has_setting(key)
		and ProjectSettings.has_setting(SETTING_BAKE_OUTPUT_DIR_LEGACY)
	):
		key = SETTING_BAKE_OUTPUT_DIR_LEGACY
	if ProjectSettings.has_setting(key):
		var dir: String = ProjectSettings.get_setting(key, PATH_RESONANCE_DATA)
		if not dir.is_empty():
			return _normalize_dir(dir)
	return PATH_RESONANCE_DATA


## [code]{{root}}statics/[/code]
static func get_statics_dir() -> String:
	return get_resonance_data_dir() + SUBDIR_STATICS


## [code]{{root}}dynamics/[/code]
static func get_dynamics_dir() -> String:
	return get_resonance_data_dir() + SUBDIR_DYNAMICS


## [code]{{root}}batches/[/code]
static func get_batches_dir() -> String:
	return get_resonance_data_dir() + SUBDIR_BATCHES


## [code].tres[/code] vs [code].res[/code] for static scene and dynamic mesh export (project setting).
static func get_resonance_asset_extension() -> String:
	return "res" if _export_setting_use_res(SETTING_RESONANCE_ASSET_FORMAT) else "tres"


## [code]{{root}}statics/{{scene}}_static.{{ext}}[/code]
static func static_scene_asset_save_path(scene_basename: String) -> String:
	return get_statics_dir() + scene_basename + "_static." + get_resonance_asset_extension()


## [code]{{root}}dynamics/{{stem}}_dynamic.{{ext}}[/code]
static func dynamic_mesh_asset_save_path(name_stem: String) -> String:
	return get_dynamics_dir() + name_stem + "_dynamic." + get_resonance_asset_extension()


## Probe batch file extension (project setting; matches custom loader/saver).
static func get_probe_data_asset_extension() -> String:
	return "res" if _export_setting_use_res(SETTING_PROBE_DATA_FORMAT) else "tres"


## [code]{{root}}batches/{{scene}}_{{node}}_batch.{{ext}}[/code] (same as bake pipeline / editor).
static func probe_data_save_path(scene_basename: String, node_key: String) -> String:
	return (
		get_batches_dir()
		+ scene_basename
		+ "_"
		+ node_key
		+ "_batch."
		+ get_probe_data_asset_extension()
	)
