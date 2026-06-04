@tool
extends EditorPlugin

## This EditorPlugin registers gizmos, inspectors, menus, and probe resource I/O. Disabling it removes
## that editor integration (menus, custom gizmos, inspector extensions, import/export hooks, autoload).
## Native node types (ResonanceProbeVolume, etc.) come from GDExtension (nexus_resonance.gdextension)
## and stay in ClassDB until the editor is restarted, so existing nodes remain movable like any Node3D.

const LOGGER_AUTOLOAD_PATH = "res://addons/nexus_resonance/scripts/resonance_logger.tscn"
const GIZMO_SCRIPT = "res://addons/nexus_resonance/editor/resonance_probe_gizmo.gd"
const PLAYER_GIZMO_SCRIPT = "res://addons/nexus_resonance/editor/resonance_player_gizmo.gd"
const PLAYER_GIZMO_CLASS_NAME = "ResonancePlayer"

const BUS_NAME = "ResonanceReverb"
const EFFECT_CLASS = "ResonanceAudioEffect"
## After plugin re-enable, GDExtension can register native classes a frame later; retry bus effect + gizmo refresh.
const _DEFERRED_EDITOR_INTEGRATION_MAX_FRAMES := 10
const SOFA_IMPORTER_SCRIPT = "res://addons/nexus_resonance/editor/sofa_importer.gd"
const RESONANCE_GEOMETRY_INSPECTOR_SCRIPT = "res://addons/nexus_resonance/editor/resonance_geometry_inspector.gd"
const RESONANCE_BAKE_RUNNER_SCRIPT = "res://addons/nexus_resonance/editor/resonance_bake_runner.gd"
const RESONANCE_PROBE_VOLUME_INSPECTOR_SCRIPT = "res://addons/nexus_resonance/editor/resonance_probe_volume_inspector.gd"
const RESONANCE_EXPORT_HANDLER_SCRIPT = "res://addons/nexus_resonance/editor/resonance_export_handler.gd"
const RESONANCE_EXPORT_PLUGIN_SCRIPT = "res://addons/nexus_resonance/editor/nexus_resonance_export_plugin.gd"
const RESONANCE_FMOD_EVENT_EMITTER_INSPECTOR_SCRIPT = "res://addons/nexus_resonance/editor/resonance_fmod_event_emitter_inspector.gd"
const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceLoggerScript = preload("res://addons/nexus_resonance/scripts/resonance_logger.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)
const ResonanceAnimationAudioConverter = preload(
	"res://addons/nexus_resonance/editor/resonance_animation_audio_converter.gd"
)

enum ToolMenuId {
	EXPORT_ACTIVE_SCENE,
	EXPORT_ALL_OPEN_SCENES,
	EXPORT_ACTIVE_SCENE_OBJ,
	EXPORT_DYNAMIC_OBJECTS_ACTIVE,
	EXPORT_DYNAMIC_OBJECTS_IN_BUILD,
	EXPORT_DYNAMIC_OBJECTS_IN_PROJECT,
	BAKE_ALL_PROBE_VOLUMES,
	CLEAR_PROBE_BATCHES,
	CLEAR_UNREFERENCED_PROBE_DATA,
	UNLINK_PROBE_VOLUME_REFS,
	CONVERT_ANIMATION_AUDIO_RESONANCE,
	CONVERT_ANIMATION_AUDIO_ALL_SCENES,
}

var gizmo_instance: EditorNode3DGizmoPlugin = null
var player_gizmo_instance: EditorNode3DGizmoPlugin = null
## False during/after _exit_tree so gizmo registration does not run after the plugin is off.
var _editor_plugin_ui_active: bool = false
var resonance_geometry_inspector: EditorInspectorPlugin = null
var resonance_fmod_event_emitter_inspector: EditorInspectorPlugin = null
var resonance_probe_volume_inspector: EditorInspectorPlugin = null
var bake_runner = null  # ResonanceBakeRunner
var sofa_importer: EditorImportPlugin = null
var export_handler = null  # ResonanceExportHandler
var steam_audio_export_plugin: EditorExportPlugin = null
const _tool_submenu_name := "Nexus Resonance"
var _tool_submenu: PopupMenu = null
var _gizmo_refresh_pending: bool = false

## Project settings root: Nexus → Resonance (bundles with other Nexus addons under [nexus]).
const SETTINGS_PREFIX := "nexus/resonance/"
const LEGACY_SETTINGS_PREFIX := "audio/nexus_resonance/"


func _enter_tree() -> void:
	_editor_plugin_ui_active = true
	# Export format keys before any other registration (avoids "nonexistent project setting" if the engine
	# touches keys during later steps). Idempotent with _register_export_project_settings.
	_ensure_export_format_project_settings()
	_migrate_legacy_project_settings()
	_migrate_nexus_bake_output_dir()
	_clear_obsolete_resonance_project_settings()
	_clear_bus_project_settings()
	_register_logger_project_settings()
	_register_editor_project_settings()
	_register_bake_project_settings()
	_register_export_project_settings()
	_init_editor_plugin_ui()
	_warn_if_gdextension_missing()


func _warn_if_gdextension_missing() -> void:
	if ResonanceServerAccess.has_server():
		return
	var msg := (
		"GDExtension did not load. Native Nexus Resonance features and baking will not work. "
		+ "Verify addons/nexus_resonance/bin matches your OS and CPU (e.g. Linux x86_64), "
		+ "and check the Output panel for loader errors."
	)
	ResonanceEditorDialogs.show_warning(get_editor_interface(), msg)


func _migrate_legacy_project_settings() -> void:
	var keys: Array[String] = [
		"logger/categories_enabled",
		"logger/output_to_file",
		"logger/output_to_debug",
		"logger/file_path",
		"logger/steam_audio_verbose",
		"bake_num_rays",
		"bake_num_bounces",
		"bake_num_threads",
		"bake_reflection_type",
		"bake_pathing_num_samples",
		"bake_pathing_vis_range",
		"bake_pathing_path_range",
		"bake_pathing_radius",
		"bake_pathing_threshold",
	]
	for k in keys:
		var old_key := LEGACY_SETTINGS_PREFIX + k
		var new_key := SETTINGS_PREFIX + k
		if not ProjectSettings.has_setting(old_key):
			continue
		if not ProjectSettings.has_setting(new_key):
			ProjectSettings.set_setting(new_key, ProjectSettings.get_setting(old_key))
		ProjectSettings.clear(old_key)

	# Legacy bake/output_dir -> nexus/.../bake/default_output_directory (not the old bus keys).
	var legacy_bake := LEGACY_SETTINGS_PREFIX + "bake/output_dir"
	var new_bake := SETTINGS_PREFIX + "bake/default_output_directory"
	if ProjectSettings.has_setting(legacy_bake) and not ProjectSettings.has_setting(new_bake):
		ProjectSettings.set_setting(new_bake, ProjectSettings.get_setting(legacy_bake))
	if ProjectSettings.has_setting(legacy_bake):
		ProjectSettings.clear(legacy_bake)


func _clear_obsolete_resonance_project_settings() -> void:
	var amb_order_key := SETTINGS_PREFIX + "bake_ambisonics_order"
	if ProjectSettings.has_setting(amb_order_key):
		ProjectSettings.clear(amb_order_key)


func _migrate_nexus_bake_output_dir() -> void:
	var old_k := SETTINGS_PREFIX + "bake/output_dir"
	var new_k := SETTINGS_PREFIX + "bake/default_output_directory"
	if ProjectSettings.has_setting(old_k) and not ProjectSettings.has_setting(new_k):
		ProjectSettings.set_setting(new_k, ProjectSettings.get_setting(old_k))
	if ProjectSettings.has_setting(old_k):
		ProjectSettings.clear(old_k)


func _clear_bus_project_settings() -> void:
	for k in [SETTINGS_PREFIX + "bus", SETTINGS_PREFIX + "reverb_bus_name"]:
		if ProjectSettings.has_setting(k):
			ProjectSettings.clear(k)
	for k in [LEGACY_SETTINGS_PREFIX + "bus", LEGACY_SETTINGS_PREFIX + "reverb_bus_name"]:
		if ProjectSettings.has_setting(k):
			ProjectSettings.clear(k)


func _init_editor_plugin_ui() -> void:
	# ResonanceProbeDataSaver / ResonanceProbeDataLoader: do not call add_resource_format_* here.
	# Global classes that extend ResourceFormatSaver/Loader are registered by the engine
	# (ResourceSaver.add_custom_savers / ResourceLoader.add_custom_loaders). A second manual
	# registration duplicates handlers; remove_resource_format_* in _exit_tree then hits
	# "i >= saver_count" when the engine has already removed the custom instances.

	var export_handler_script: Script = load(RESONANCE_EXPORT_HANDLER_SCRIPT) as Script
	if export_handler_script:
		export_handler = export_handler_script.new(get_editor_interface())
	else:
		push_warning(
			"Nexus Resonance: Failed to load export handler. Export and bake features may be unavailable."
		)
	# bake_runner depends on export_handler for export_static_callback (pre-bake static scene export)
	var runner_script: Script = load(RESONANCE_BAKE_RUNNER_SCRIPT) as Script
	if runner_script and export_handler:
		bake_runner = runner_script.new(get_editor_interface())
		bake_runner.export_static_callback = export_handler.export_active_scene_sync_for_bake

	var pv_inspector_script: Script = load(RESONANCE_PROBE_VOLUME_INSPECTOR_SCRIPT) as Script
	if pv_inspector_script and bake_runner:
		resonance_probe_volume_inspector = pv_inspector_script.new()
		resonance_probe_volume_inspector.bake_runner = bake_runner
		resonance_probe_volume_inspector.editor_interface = get_editor_interface()
		add_inspector_plugin(resonance_probe_volume_inspector)

	_register_probe_volume_gizmo()
	_register_player_gizmo()

	_tool_submenu = PopupMenu.new()
	var base: Control = get_editor_interface().get_base_control()
	var icon_export: Texture2D = ResonanceEditorDialogs.get_icon(
		base, UIStrings.ICON_EXPORT, "Export"
	)
	var icon_bake: Texture2D = ResonanceEditorDialogs.get_icon(base, UIStrings.ICON_BAKE, "Bake")
	var icon_clear: Texture2D = ResonanceEditorDialogs.get_icon(base, UIStrings.ICON_CLEAR, "Clear")
	_tool_submenu.add_icon_item(
		icon_export, tr(UIStrings.MENU_EXPORT_ACTIVE_SCENE), ToolMenuId.EXPORT_ACTIVE_SCENE
	)
	_tool_submenu.add_icon_item(
		icon_export, tr(UIStrings.MENU_EXPORT_ALL_OPEN_SCENES), ToolMenuId.EXPORT_ALL_OPEN_SCENES
	)
	_tool_submenu.add_icon_item(
		icon_export, tr(UIStrings.MENU_EXPORT_ACTIVE_SCENE_OBJ), ToolMenuId.EXPORT_ACTIVE_SCENE_OBJ
	)
	_tool_submenu.add_icon_item(
		icon_export,
		tr(UIStrings.MENU_EXPORT_DYNAMIC_OBJECTS_ACTIVE),
		ToolMenuId.EXPORT_DYNAMIC_OBJECTS_ACTIVE
	)
	_tool_submenu.add_icon_item(
		icon_export,
		tr(UIStrings.MENU_EXPORT_DYNAMIC_OBJECTS_IN_BUILD),
		ToolMenuId.EXPORT_DYNAMIC_OBJECTS_IN_BUILD
	)
	_tool_submenu.add_icon_item(
		icon_export,
		tr(UIStrings.MENU_EXPORT_DYNAMIC_OBJECTS_IN_PROJECT),
		ToolMenuId.EXPORT_DYNAMIC_OBJECTS_IN_PROJECT
	)
	_tool_submenu.add_icon_item(
		icon_bake, tr(UIStrings.MENU_BAKE_ALL_PROBE_VOLUMES), ToolMenuId.BAKE_ALL_PROBE_VOLUMES
	)
	_tool_submenu.add_icon_item(
		icon_clear, tr(UIStrings.MENU_CLEAR_PROBE_BATCHES), ToolMenuId.CLEAR_PROBE_BATCHES
	)
	_tool_submenu.add_icon_item(
		icon_clear,
		tr(UIStrings.MENU_CLEAR_UNREFERENCED_PROBE_DATA),
		ToolMenuId.CLEAR_UNREFERENCED_PROBE_DATA
	)
	_tool_submenu.add_item(
		tr(UIStrings.MENU_UNLINK_PROBE_VOLUME_REFS), ToolMenuId.UNLINK_PROBE_VOLUME_REFS
	)
	_tool_submenu.add_item(
		tr(UIStrings.MENU_CONVERT_ANIMATION_AUDIO_RESONANCE),
		ToolMenuId.CONVERT_ANIMATION_AUDIO_RESONANCE
	)
	_tool_submenu.add_item(
		tr(UIStrings.MENU_CONVERT_ANIMATION_AUDIO_ALL_SCENES),
		ToolMenuId.CONVERT_ANIMATION_AUDIO_ALL_SCENES
	)
	_tool_submenu.id_pressed.connect(_on_tool_submenu_id_pressed)
	_register_tool_shortcuts()
	add_tool_submenu_item(_tool_submenu_name, _tool_submenu)

	var importer_script: Script = load(SOFA_IMPORTER_SCRIPT) as Script
	if importer_script:
		sofa_importer = importer_script.new()
		if sofa_importer:
			add_import_plugin(sofa_importer)

	var inspector_script: Script = load(RESONANCE_GEOMETRY_INSPECTOR_SCRIPT) as Script
	if inspector_script:
		resonance_geometry_inspector = inspector_script.new()
		resonance_geometry_inspector.editor_interface = get_editor_interface()
		add_inspector_plugin(resonance_geometry_inspector)

	var fmod_inspector_script: Script = (
		load(RESONANCE_FMOD_EVENT_EMITTER_INSPECTOR_SCRIPT) as Script
	)
	if fmod_inspector_script:
		resonance_fmod_event_emitter_inspector = fmod_inspector_script.new()
		add_inspector_plugin(resonance_fmod_event_emitter_inspector)

	var export_plugin_script: Script = load(RESONANCE_EXPORT_PLUGIN_SCRIPT) as Script
	if export_plugin_script:
		steam_audio_export_plugin = export_plugin_script.new()
		add_export_plugin(steam_audio_export_plugin)

	print_rich("[color=green]Nexus Resonance: Ready.[/color]")

	if not scene_changed.is_connected(_on_editor_scene_changed_refresh_probe_gizmos):
		scene_changed.connect(_on_editor_scene_changed_refresh_probe_gizmos)


func _on_editor_scene_changed_refresh_probe_gizmos(_scene_root: Node) -> void:
	if _gizmo_refresh_pending:
		return
	_gizmo_refresh_pending = true
	call_deferred("_coalesced_refresh_editor_gizmos")


func _coalesced_refresh_editor_gizmos() -> void:
	_gizmo_refresh_pending = false
	if not _editor_plugin_ui_active:
		return
	if gizmo_instance != null:
		_refresh_resonance_probe_volume_gizmos_in_edited_scene()
	if player_gizmo_instance != null:
		_refresh_resonance_player_gizmos_in_edited_scene()


func _register_probe_volume_gizmo() -> void:
	if not _editor_plugin_ui_active:
		return
	if gizmo_instance != null:
		return
	if not FileAccess.file_exists(GIZMO_SCRIPT):
		return
	var script: Script = load(GIZMO_SCRIPT) as Script
	if script == null:
		return
	var inst: EditorNode3DGizmoPlugin = script.new()
	if inst == null:
		return
	gizmo_instance = inst
	if "undo_redo" in gizmo_instance:
		gizmo_instance.undo_redo = get_undo_redo()
	var base: Control = get_editor_interface().get_base_control()
	if base and "fallback_icon" in gizmo_instance:
		gizmo_instance.fallback_icon = ResonanceEditorDialogs.get_icon(
			base, UIStrings.ICON_PROBE_VOLUME_GIZMO, "ReflectionProbe"
		)
	add_node_3d_gizmo_plugin(gizmo_instance)
	_refresh_resonance_probe_volume_gizmos_in_edited_scene()


func _refresh_resonance_probe_volume_gizmos_in_edited_scene() -> void:
	var scene_root: Node = get_editor_interface().get_edited_scene_root()
	if scene_root == null:
		return
	_refresh_probe_gizmos_recursive(scene_root)


func _refresh_probe_gizmos_recursive(n: Node) -> void:
	if n is Node3D:
		var n3 := n as Node3D
		if n3.is_class(UIStrings.GIZMO_PROBE_VOLUME_CLASS):
			n3.update_gizmos()
	for c in n.get_children():
		_refresh_probe_gizmos_recursive(c)


func _register_player_gizmo() -> void:
	if not _editor_plugin_ui_active:
		return
	if player_gizmo_instance != null:
		return
	if not FileAccess.file_exists(PLAYER_GIZMO_SCRIPT):
		return
	var script: Script = load(PLAYER_GIZMO_SCRIPT) as Script
	if script == null:
		return
	var inst: EditorNode3DGizmoPlugin = script.new()
	if inst == null:
		return
	player_gizmo_instance = inst
	add_node_3d_gizmo_plugin(player_gizmo_instance)
	_refresh_resonance_player_gizmos_in_edited_scene()


func _refresh_resonance_player_gizmos_in_edited_scene() -> void:
	var scene_root: Node = get_editor_interface().get_edited_scene_root()
	if scene_root == null:
		return
	_refresh_player_gizmos_recursive(scene_root)


func _refresh_player_gizmos_recursive(n: Node) -> void:
	if n is Node3D:
		var n3 := n as Node3D
		if n3.is_class(PLAYER_GIZMO_CLASS_NAME):
			n3.update_gizmos()
	for c in n.get_children():
		_refresh_player_gizmos_recursive(c)


func _exit_tree() -> void:
	# Mirrors _init_editor_plugin_ui: inspectors, import/export plugins, 3D gizmo, tool submenu,
	# scene_changed. (Autoload is removed in _disable_plugin; ProjectSettings.add_property_info persists.)
	_editor_plugin_ui_active = false
	if scene_changed.is_connected(_on_editor_scene_changed_refresh_probe_gizmos):
		scene_changed.disconnect(_on_editor_scene_changed_refresh_probe_gizmos)
	_detach_reverb_effect()
	# Break RefCounted reference cycles in bake system (prevents exit leak warnings).
	if resonance_probe_volume_inspector and "bake_runner" in resonance_probe_volume_inspector:
		resonance_probe_volume_inspector.bake_runner = null
	if bake_runner and bake_runner.has_method("shutdown"):
		bake_runner.shutdown()
	if resonance_geometry_inspector:
		remove_inspector_plugin(resonance_geometry_inspector)
		resonance_geometry_inspector = null
	if resonance_fmod_event_emitter_inspector:
		remove_inspector_plugin(resonance_fmod_event_emitter_inspector)
		resonance_fmod_event_emitter_inspector = null
	if resonance_probe_volume_inspector:
		remove_inspector_plugin(resonance_probe_volume_inspector)
		resonance_probe_volume_inspector = null
	bake_runner = null
	export_handler = null
	if _tool_submenu:
		var cb := Callable(self, "_on_tool_submenu_id_pressed")
		if _tool_submenu.id_pressed.is_connected(cb):
			_tool_submenu.id_pressed.disconnect(cb)
	remove_tool_menu_item(_tool_submenu_name)
	_tool_submenu = null
	if steam_audio_export_plugin:
		remove_export_plugin(steam_audio_export_plugin)
		steam_audio_export_plugin = null
	if sofa_importer:
		remove_import_plugin(sofa_importer)
		sofa_importer = null
	if gizmo_instance:
		remove_node_3d_gizmo_plugin(gizmo_instance)
		gizmo_instance = null
	if player_gizmo_instance:
		remove_node_3d_gizmo_plugin(player_gizmo_instance)
		player_gizmo_instance = null


func _enable_plugin() -> void:
	# Before autoload / editor subsystems read export format keys (avoids "nonexistent project setting").
	_ensure_export_format_project_settings()
	# No ProjectSettings.save() here; [_register_export_project_settings] persists when the editor loads the plugin.
	add_autoload_singleton("ResonanceLogger", LOGGER_AUTOLOAD_PATH)
	# Defer bus + gizmo follow-up: ClassDB may not list ResonanceAudioEffect until the next frame(s) after re-enable.
	call_deferred("_deferred_finish_editor_integration", 0)


func _deferred_finish_editor_integration(attempt: int = 0) -> void:
	if not _editor_plugin_ui_active:
		return
	if ClassDB.class_exists(EFFECT_CLASS):
		_setup_audio_bus()
		_finish_editor_probe_gizmo_integration()
		return
	if attempt + 1 < _DEFERRED_EDITOR_INTEGRATION_MAX_FRAMES:
		call_deferred("_deferred_finish_editor_integration", attempt + 1)
		return
	push_warning(
		(
			"Nexus Resonance: ResonanceAudioEffect did not appear in ClassDB after "
			+ str(_DEFERRED_EDITOR_INTEGRATION_MAX_FRAMES)
			+ " deferred attempts. The reverb bus may lack the effect until the editor is restarted. "
			+ "Check that addons/nexus_resonance/bin matches your platform and see the Output panel for GDExtension errors."
		)
	)
	_setup_audio_bus()
	_finish_editor_probe_gizmo_integration()


func _finish_editor_probe_gizmo_integration() -> void:
	if gizmo_instance == null:
		_register_probe_volume_gizmo()
	else:
		_refresh_resonance_probe_volume_gizmos_in_edited_scene()
	if player_gizmo_instance == null:
		_register_player_gizmo()
	else:
		_refresh_resonance_player_gizmos_in_edited_scene()


func _disable_plugin() -> void:
	# Do not call ResonanceServer.clear_probe_batches() here: it wipes the native registry while
	# ResonanceProbeVolume nodes still hold stale probe_batch_handle values, corrupting state after
	# re-enable. Use Project → Tools → Nexus Resonance → Clear Probe Batches when you want that.
	_detach_reverb_effect()
	remove_autoload_singleton("ResonanceLogger")


func _get_bus_editor() -> StringName:
	# Matches ResonanceRuntimeConfig defaults (buses are not Project Settings anymore).
	return &"Master"


func _get_reverb_bus_name_editor() -> StringName:
	return BUS_NAME


func _detach_reverb_effect() -> void:
	var bus_name: StringName = _get_reverb_bus_name_editor()
	var idx: int = AudioServer.get_bus_index(bus_name)
	if idx < 0:
		return
	var n: int = AudioServer.get_bus_effect_count(idx)
	for i in range(n - 1, -1, -1):
		var eff: AudioEffect = AudioServer.get_bus_effect(idx, i)
		if eff and eff.get_class() == EFFECT_CLASS:
			AudioServer.remove_bus_effect(idx, i)
			break


func _setup_audio_bus() -> void:
	var bus_name: StringName = _get_reverb_bus_name_editor()
	var send_name: StringName = _get_bus_editor()
	var idx: int = AudioServer.get_bus_index(bus_name)
	if idx == -1:
		AudioServer.add_bus()
		idx = AudioServer.bus_count - 1
		AudioServer.set_bus_name(idx, bus_name)
		if idx > 1:
			AudioServer.move_bus(idx, 1)
			idx = 1
	if idx >= 0:
		AudioServer.set_bus_send(idx, send_name)
		ResonanceRuntimeBus.ensure_resonance_reverb_effect_on_bus(idx)


func _has_main_screen() -> bool:
	return false


func _get_plugin_name() -> String:
	return "Resonance"


func _get_plugin_icon() -> Texture2D:
	return get_editor_interface().get_base_control().get_theme_icon(
		"AudioStreamPlayer", "EditorIcons"
	)


func _register_logger_project_settings() -> void:
	const PREFIX := SETTINGS_PREFIX + "logger/"
	if not ProjectSettings.has_setting(PREFIX + "categories_enabled"):
		ProjectSettings.set_setting(
			PREFIX + "categories_enabled",
			ResonanceLoggerScript.get_default_categories_enabled_dict()
		)
	else:
		var ce: Variant = ProjectSettings.get_setting(PREFIX + "categories_enabled")
		if ce is Dictionary and (ce as Dictionary).is_empty():
			ProjectSettings.set_setting(
				PREFIX + "categories_enabled",
				ResonanceLoggerScript.get_default_categories_enabled_dict()
			)
	(
		ProjectSettings
		. add_property_info(
			{
				"name": PREFIX + "categories_enabled",
				"type": TYPE_DICTIONARY,
				"hint": PROPERTY_HINT_NONE,
			}
		)
	)
	if not ProjectSettings.has_setting(PREFIX + "output_to_debug"):
		ProjectSettings.set_setting(PREFIX + "output_to_debug", true)
	(
		ProjectSettings
		. add_property_info(
			{
				"name": PREFIX + "output_to_debug",
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
			}
		)
	)
	if not ProjectSettings.has_setting(PREFIX + "output_to_file"):
		ProjectSettings.set_setting(PREFIX + "output_to_file", false)
	(
		ProjectSettings
		. add_property_info(
			{
				"name": PREFIX + "output_to_file",
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
			}
		)
	)
	if not ProjectSettings.has_setting(PREFIX + "file_path"):
		ProjectSettings.set_setting(PREFIX + "file_path", "user://nexus_resonance_log.ndjson")
	(
		ProjectSettings
		. add_property_info(
			{
				"name": PREFIX + "file_path",
				"type": TYPE_STRING,
				"hint": PROPERTY_HINT_NONE,
			}
		)
	)
	if not ProjectSettings.has_setting(PREFIX + "steam_audio_verbose"):
		ProjectSettings.set_setting(PREFIX + "steam_audio_verbose", false)
	(
		ProjectSettings
		. add_property_info(
			{
				"name": PREFIX + "steam_audio_verbose",
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
			}
		)
	)


func _register_editor_project_settings() -> void:
	const EDITOR_PREFIX := SETTINGS_PREFIX + "editor/"
	var k := EDITOR_PREFIX + "auto_convert_animation_audio_on_save"
	if not ProjectSettings.has_setting(k):
		ProjectSettings.set_setting(k, false)
	(
		ProjectSettings
		. add_property_info(
			{
				"name": k,
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
			}
		)
	)


func _register_bake_project_settings() -> void:
	const BAKE_PREFIX := SETTINGS_PREFIX + "bake/"
	const KEY := "default_output_directory"
	if not ProjectSettings.has_setting(BAKE_PREFIX + KEY):
		ProjectSettings.set_setting(BAKE_PREFIX + KEY, "res://audio_data/")
	(
		ProjectSettings
		. add_property_info(
			{
				"name": BAKE_PREFIX + KEY,
				"type": TYPE_STRING,
				"hint": PROPERTY_HINT_DIR,
			}
		)
	)


## Ensures export asset format keys exist with defaults (no save). Returns whether [method ProjectSettings.save] is recommended.
func _ensure_export_format_project_settings() -> bool:
	const EXPORT_PREFIX := SETTINGS_PREFIX + "export/"
	var save_needed := false
	save_needed = (
		_ensure_int_enum_project_setting(EXPORT_PREFIX + "static_scene_asset_format", 0)
		or save_needed
	)
	save_needed = (
		_ensure_int_enum_project_setting(EXPORT_PREFIX + "probe_data_format", 0) or save_needed
	)
	return save_needed


func _ensure_int_enum_project_setting(key: String, default_value: int) -> bool:
	# set_initial_value must run only after the key exists via set_setting; otherwise Godot can log
	# "Request for nonexistent project setting" when enabling the plugin.
	var needs_save := false
	if not ProjectSettings.has_setting(key):
		ProjectSettings.set_setting(key, default_value)
		needs_save = true
	else:
		var cur: Variant = ProjectSettings.get_setting(key)
		var t := typeof(cur)
		if cur == null or (t != TYPE_INT and t != TYPE_FLOAT):
			ProjectSettings.set_setting(key, default_value)
			needs_save = true
		elif t == TYPE_FLOAT:
			ProjectSettings.set_setting(key, clampi(int(round(cur)), 0, 1))
			needs_save = true
		else:
			var iv := int(cur)
			if iv != 0 and iv != 1:
				ProjectSettings.set_setting(key, default_value)
				needs_save = true
	var effective := clampi(int(ProjectSettings.get_setting(key)), 0, 1)
	ProjectSettings.set_initial_value(key, effective)
	return needs_save


func _register_export_project_settings() -> void:
	const EXPORT_PREFIX := SETTINGS_PREFIX + "export/"
	const KEY_STATIC := "static_scene_asset_format"
	const KEY_PROBE := "probe_data_format"
	var save_needed := _ensure_export_format_project_settings()
	(
		ProjectSettings
		. add_property_info(
			{
				"name": EXPORT_PREFIX + KEY_STATIC,
				"type": TYPE_INT,
				"hint": PROPERTY_HINT_ENUM,
				"hint_string": "Text (.tres),Binary (.res)",
			}
		)
	)
	(
		ProjectSettings
		. add_property_info(
			{
				"name": EXPORT_PREFIX + KEY_PROBE,
				"type": TYPE_INT,
				"hint": PROPERTY_HINT_ENUM,
				"hint_string": "Text (.tres),Binary (.res)",
			}
		)
	)
	if save_needed:
		ProjectSettings.save()


func _register_tool_shortcuts() -> void:
	var sc_static: Shortcut = Shortcut.new()
	var ev_static: InputEventKey = InputEventKey.new()
	ev_static.keycode = KEY_E
	ev_static.ctrl_pressed = true
	ev_static.shift_pressed = true
	ev_static.command_or_control_autoremap = true
	sc_static.events = [ev_static]
	_tool_submenu.set_item_shortcut(ToolMenuId.EXPORT_ACTIVE_SCENE, sc_static, true)

	var sc_dynamic: Shortcut = Shortcut.new()
	var ev_dynamic: InputEventKey = InputEventKey.new()
	ev_dynamic.keycode = KEY_D
	ev_dynamic.ctrl_pressed = true
	ev_dynamic.shift_pressed = true
	ev_dynamic.command_or_control_autoremap = true
	sc_dynamic.events = [ev_dynamic]
	_tool_submenu.set_item_shortcut(ToolMenuId.EXPORT_DYNAMIC_OBJECTS_ACTIVE, sc_dynamic, true)

	var sc_bake: Shortcut = Shortcut.new()
	var ev_bake: InputEventKey = InputEventKey.new()
	ev_bake.keycode = KEY_B
	ev_bake.ctrl_pressed = true
	ev_bake.shift_pressed = true
	ev_bake.command_or_control_autoremap = true
	sc_bake.events = [ev_bake]
	_tool_submenu.set_item_shortcut(ToolMenuId.BAKE_ALL_PROBE_VOLUMES, sc_bake, true)


func _on_tool_submenu_id_pressed(id: int) -> void:
	match id:
		ToolMenuId.EXPORT_ACTIVE_SCENE:
			if export_handler:
				export_handler.export_active_scene(null)
		ToolMenuId.EXPORT_ALL_OPEN_SCENES:
			if export_handler:
				export_handler.export_all_open_scenes(null)
		ToolMenuId.EXPORT_ACTIVE_SCENE_OBJ:
			if export_handler:
				export_handler.export_scene_obj(null)
		ToolMenuId.EXPORT_DYNAMIC_OBJECTS_ACTIVE:
			if export_handler:
				export_handler.export_dynamic_mesh(null)
		ToolMenuId.EXPORT_DYNAMIC_OBJECTS_IN_BUILD:
			if export_handler:
				export_handler.export_dynamic_objects_in_build(null)
		ToolMenuId.EXPORT_DYNAMIC_OBJECTS_IN_PROJECT:
			if export_handler:
				export_handler.export_dynamic_objects_in_project(null)
		ToolMenuId.BAKE_ALL_PROBE_VOLUMES:
			_on_tool_bake_all_probe_volumes(null)
		ToolMenuId.CLEAR_PROBE_BATCHES:
			_on_tool_clear_probe_batches(null)
		ToolMenuId.CLEAR_UNREFERENCED_PROBE_DATA:
			if export_handler:
				export_handler.clear_unreferenced_probe_data(null)
		ToolMenuId.UNLINK_PROBE_VOLUME_REFS:
			_on_tool_unlink_probe_refs(null)
		ToolMenuId.CONVERT_ANIMATION_AUDIO_RESONANCE:
			_on_tool_convert_animation_audio_resonance()
		ToolMenuId.CONVERT_ANIMATION_AUDIO_ALL_SCENES:
			_on_tool_convert_animation_audio_all_scenes()


func _on_tool_convert_animation_audio_resonance() -> void:
	var root: Node = get_editor_interface().get_edited_scene_root()
	if root == null:
		ResonanceEditorDialogs.show_warning(get_editor_interface(), UIStrings.WARN_NO_SCENE)
		return
	var r: Dictionary = ResonanceAnimationAudioConverter.convert_all_animation_players(root)
	var msg := (
		"Converted %d audio track(s) on ResonancePlayer (%d AnimationPlayer node(s) scanned)."
		% [int(r.get("tracks_converted", 0)), int(r.get("animation_players", 0))]
	)
	if int(r.get("skipped_blend", 0)) > 0:
		msg += tr(UIStrings.INFO_CONVERT_SKIPPED_BLEND) % int(r.get("skipped_blend", 0))
	if int(r.get("skipped_target", 0)) > 0:
		msg += " Skipped %d non-ResonancePlayer target(s)." % int(r.get("skipped_target", 0))
	get_editor_interface().mark_scene_as_unsaved()
	ResonanceEditorDialogs.show_success_toast(get_editor_interface(), msg)


func _on_tool_convert_animation_audio_all_scenes() -> void:
	ResonanceEditorDialogs.show_confirm_dialog(
		get_editor_interface(),
		tr(UIStrings.DIALOG_CONVERT_ALL_SCENES_TITLE),
		tr(UIStrings.DIALOG_CONVERT_ALL_SCENES_MESSAGE),
		Callable(self, "_run_convert_animation_audio_all_scenes_confirmed")
	)


func _run_convert_animation_audio_all_scenes_confirmed() -> void:
	var fs: EditorFileSystem = get_editor_interface().get_resource_filesystem()
	if fs == null:
		ResonanceEditorDialogs.show_warning(
			get_editor_interface(), tr(UIStrings.ERR_EDITOR_FILESYSTEM_UNAVAILABLE)
		)
		return
	var root_dir: EditorFileSystemDirectory = fs.get_filesystem_root()
	if root_dir == null:
		ResonanceEditorDialogs.show_warning(
			get_editor_interface(), tr(UIStrings.ERR_EDITOR_FILESYSTEM_ROOT)
		)
		return
	var paths: PackedStringArray = (
		ResonanceAnimationAudioConverter.collect_tscn_paths_from_filesystem(root_dir)
	)
	var total_tracks := 0
	var total_saved := 0
	var total_failed := 0
	var total_blend := 0
	var total_target := 0
	for p in paths:
		var one: Dictionary = ResonanceAnimationAudioConverter.convert_packed_scene_file_on_disk(
			String(p)
		)
		if not String(one.get("error", "")).is_empty():
			total_failed += 1
			push_warning(
				(
					UIStrings.PREFIX
					+ "Convert scene failed (%s): %s" % [String(p), String(one.get("error", ""))]
				)
			)
			continue
		total_tracks += int(one.get("tracks_converted", 0))
		total_blend += int(one.get("skipped_blend", 0))
		total_target += int(one.get("skipped_target", 0))
		if one.get("saved", false):
			total_saved += 1
	var summary: String = (
		tr(UIStrings.INFO_CONVERT_ALL_SCENES_SUMMARY)
		% [
			total_tracks,
			total_saved,
			total_failed,
			total_blend,
			total_target,
		]
	)
	ResonanceEditorDialogs.show_success_toast(get_editor_interface(), summary)
	fs.scan()


func _notification(what: int) -> void:
	if what == NOTIFICATION_EDITOR_PRE_SAVE:
		_on_editor_pre_save_animation_audio()
	# NOTE: In GDScript, calling super on virtual callbacks is only valid if the parent is a script
	# that defines the method. EditorPlugin's native implementation is not callable via super here.


func _on_editor_pre_save_animation_audio() -> void:
	if not _editor_plugin_ui_active:
		return
	var key := SETTINGS_PREFIX + "editor/auto_convert_animation_audio_on_save"
	if not bool(ProjectSettings.get_setting(key, false)):
		return
	var root: Node = get_editor_interface().get_edited_scene_root()
	if root == null:
		return
	if not ResonanceAnimationAudioConverter.scene_has_resonance_audio_tracks(root):
		return
	var r: Dictionary = ResonanceAnimationAudioConverter.convert_all_animation_players(root)
	var n: int = int(r.get("tracks_converted", 0))
	if n <= 0:
		return
	var msg: String = tr(UIStrings.INFO_AUTO_CONVERT_ANIMATION_PRE_SAVE) % n
	ResonanceEditorDialogs.show_success_toast(get_editor_interface(), msg)
	if int(r.get("skipped_blend", 0)) > 0:
		push_warning(
			(
				(
					UIStrings.PREFIX
					+ "Auto-convert skipped %d blend track(s); convert manually for Steam Audio."
				)
				% int(r.get("skipped_blend", 0))
			)
		)


func _on_tool_bake_all_probe_volumes(_unused: Variant = null) -> void:
	var root: Node = get_editor_interface().get_edited_scene_root()
	if not root:
		ResonanceEditorDialogs.show_warning(get_editor_interface(), UIStrings.WARN_NO_SCENE)
		return
	var volumes: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_probe_volumes(root, volumes)
	if volumes.is_empty():
		ResonanceEditorDialogs.show_warning(
			get_editor_interface(), tr(UIStrings.WARN_NO_PROBE_VOLUMES)
		)
		return
	if not bake_runner:
		ResonanceEditorDialogs.show_critical(
			get_editor_interface(),
			tr(UIStrings.ERR_BAKE_RUNNER_NOT_INIT),
			tr(UIStrings.DIALOG_BAKE_FAILED_TITLE)
		)
		return
	bake_runner.run_bake(volumes)


func _on_tool_clear_probe_batches(_unused: Variant = null) -> void:
	if ResonanceServerAccess.has_server():
		var srv: Variant = ResonanceServerAccess.get_server()
		if srv and srv.has_method("clear_probe_batches"):
			srv.clear_probe_batches()
			ResonanceEditorDialogs.show_info(tr(UIStrings.INFO_PROBE_BATCHES_CLEARED))
	else:
		ResonanceEditorDialogs.show_critical(
			get_editor_interface(),
			tr(UIStrings.ERR_GDEXTENSION_NOT_LOADED),
			tr(UIStrings.DIALOG_GDEXTENSION_NOT_LOADED_TITLE)
		)


func _on_tool_unlink_probe_refs(_unused: Variant = null) -> void:
	var root: Node = get_editor_interface().get_edited_scene_root()
	if not root:
		ResonanceEditorDialogs.show_warning(get_editor_interface(), UIStrings.WARN_NO_SCENE)
		return
	var selected_volumes: Array[Node] = []
	for n in get_editor_interface().get_selection().get_selected_nodes():
		if n.is_class("ResonanceProbeVolume"):
			selected_volumes.append(n)
	if selected_volumes.is_empty():
		ResonanceEditorDialogs.show_warning(
			get_editor_interface(), tr(UIStrings.WARN_SELECT_PROBE_VOLUMES)
		)
		return
	var ur: EditorUndoRedoManager = get_undo_redo()
	ur.create_action("Nexus Resonance: Unlink Probe Volume References")
	var result: Dictionary = {"count": 0, "nodes": []}
	_collect_probe_refs_to_clear(root, root, selected_volumes, result)
	if result["count"] == 0:
		ResonanceEditorDialogs.show_warning(
			get_editor_interface(), tr(UIStrings.WARN_NO_PLAYER_REFS)
		)
		return
	var nodes_to_clear: Array[Node] = []
	for n in result["nodes"]:
		nodes_to_clear.append(n as Node)
	for n in nodes_to_clear:
		var old_path: NodePath = n.get_pathing_probe_volume()
		ur.add_do_property(n, "pathing_probe_volume", NodePath())
		ur.add_undo_property(n, "pathing_probe_volume", old_path)
	ur.commit_action()
	ResonanceEditorDialogs.show_success_toast(
		get_editor_interface(), tr(UIStrings.INFO_UNLINK_DONE) % result["count"]
	)


func _collect_probe_refs_to_clear(
	node: Node, scene_root: Node, targets: Array[Node], result: Dictionary
) -> void:
	if node.has_method("get_pathing_probe_volume") and node.has_method("set_pathing_probe_volume"):
		var path: NodePath = node.get_pathing_probe_volume()
		if not path.is_empty():
			var target: Node = scene_root.get_node_or_null(path)
			if target and target in targets:
				result["nodes"].append(node)
				result["count"] += 1
	for c in node.get_children():
		_collect_probe_refs_to_clear(c, scene_root, targets, result)
