extends GutTest

## Player configs saved by 1.1 must load with the same per-source reflections type.

const TEMP_PATH := "user://test_player_config_reflections_type.tres"


func after_each() -> void:
	if FileAccess.file_exists(TEMP_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(TEMP_PATH))


func _load_config_with_reflections_type(value: int) -> ResonancePlayerConfig:
	var text := (
		'[gd_resource type="Resource" script_class="ResonancePlayerConfig" load_steps=2 format=3]\n\n'
		+ '[ext_resource type="Script" path="res://addons/nexus_resonance/scripts/resonance_player_config.gd" id="1"]\n\n'
		+ "[resource]\n"
		+ 'script = ExtResource("1")\n'
		+ "reflections_type = %d\n" % value
	)
	var file := FileAccess.open(TEMP_PATH, FileAccess.WRITE)
	file.store_string(text)
	file.close()
	return (
		ResourceLoader.load(TEMP_PATH, "", ResourceLoader.CACHE_MODE_IGNORE)
		as ResonancePlayerConfig
	)


func test_static_types_load_unchanged() -> void:
	assert_eq(
		_load_config_with_reflections_type(2).reflections_type,
		ResonancePlayerConfig.REFLECTIONS_STATIC_SOURCE
	)
	assert_eq(
		_load_config_with_reflections_type(3).reflections_type,
		ResonancePlayerConfig.REFLECTIONS_STATIC_LISTENER
	)


func test_removed_types_load_as_realtime() -> void:
	assert_eq(
		_load_config_with_reflections_type(-1).reflections_type,
		ResonancePlayerConfig.REFLECTIONS_REALTIME,
		"Use Global"
	)
	assert_eq(
		_load_config_with_reflections_type(1).reflections_type,
		ResonancePlayerConfig.REFLECTIONS_REALTIME,
		"Baked Reverb"
	)


func test_script_assignment_is_normalized() -> void:
	var cfg := ResonancePlayerConfig.new()
	cfg.reflections_type = 1
	assert_eq(cfg.reflections_type, ResonancePlayerConfig.REFLECTIONS_REALTIME)
	cfg.reflections_type = ResonancePlayerConfig.REFLECTIONS_STATIC_LISTENER
	assert_eq(cfg.reflections_type, ResonancePlayerConfig.REFLECTIONS_STATIC_LISTENER)
