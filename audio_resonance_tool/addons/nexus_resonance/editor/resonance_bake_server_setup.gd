extends RefCounted
class_name ResonanceBakeServerSetup

## Initializes ResonanceServer for editor bakes and surfaces init / config errors.

const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")
const ResonanceRuntimeScript = preload("res://addons/nexus_resonance/scripts/resonance_runtime.gd")
const _BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")

var resonance_editor_dialogs
var _runner: Object


func _init(runner: Object) -> void:
	# Conditionally initialize this variable to avoid crashes when running in headless mode.
	if Engine.is_editor_hint():
		resonance_editor_dialogs = load(
			"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
		)

	_runner = runner


func shutdown() -> void:
	_runner = null


static func _runtime_config_from_node(cfg_node: Node) -> Dictionary:
	if cfg_node == null:
		return {}
	if cfg_node.has_method("get_config_dict"):
		return cfg_node.get_config_dict()
	var rt = cfg_node.get("runtime")
	if rt and rt.has_method("get_config"):
		var c: Dictionary = rt.get_config()
		c["debug_occlusion"] = false
		return c
	return {}


func log_and_show_error(
	message: String,
	solution: String = "",
	cause: String = "",
	volume_name: String = "",
	step: String = ""
) -> void:
	var data := {"error": true}
	if not volume_name.is_empty():
		data["volume"] = volume_name
	if not step.is_empty():
		data["step"] = step
	if Engine.has_singleton("ResonanceLogger"):
		Engine.get_singleton("ResonanceLogger").log(&"bake", "Bake error: " + message, data)

	var ei = _runner.get("editor_interface") if _runner else null
	if ei:
		resonance_editor_dialogs.show_error_dialog(
			ei, tr(UIStrings.DIALOG_BAKE_FAILED_TITLE), message, cause, solution
		)
	else:
		var full_error := "Nexus Resonance: Bake error: " + message
		if not cause.is_empty():
			full_error += " | Cause: " + cause
		if not solution.is_empty():
			full_error += " | Solution: " + solution
		push_error(full_error)
		print(full_error)


func ensure_resonance_server_initialized(volumes: Array[Node]) -> bool:
	if not ResonanceServerAccess.has_server():
		log_and_show_error(
			"GDExtension not loaded.", "Ensure the Nexus Resonance GDExtension is installed."
		)
		return false
	var srv = ResonanceServerAccess.get_server()
	if srv.is_initialized():
		return true

	var root: Node = null
	if _runner and _runner.has_method("_get_edited_scene_root"):
		root = _runner._get_edited_scene_root(volumes)

	var cfg_node := (
		_BakeDiscovery.find_resonance_runtime(root, ResonanceRuntimeScript) if root else null
	)
	var config := _runtime_config_from_node(cfg_node)
	if config.is_empty():
		var data := {"error": true}
		if Engine.has_singleton("ResonanceLogger"):
			Engine.get_singleton("ResonanceLogger").log(
				&"bake", "Bake error: No ResonanceRuntime in scene.", data
			)

		var ei = _runner.get("editor_interface") if _runner else null
		var msg := tr(UIStrings.WARN_RUNTIME_REQUIRED_EDITOR)

		if ei:
			resonance_editor_dialogs.show_warning(ei, msg)
			if ei.has_method("get_editor_toaster"):
				var toaster = ei.get_editor_toaster()
				if toaster and toaster.has_method("push_toast"):
					push_warning(UIStrings.PREFIX + msg)
		else:
			push_warning("Nexus Resonance: " + msg)

		return false
	var bake_params := ResonanceBakeConfig.create_default().get_bake_params()
	if volumes.size() > 0:
		var bc = (
			_runner._get_bake_config_for_volume(volumes[0])
			if _runner and _runner.has_method("_get_bake_config_for_volume")
			else null
		)
		if bc:
			bake_params = bc.get_bake_params()
	srv.set_bake_params(bake_params)
	srv.init_audio_engine(config)
	if not srv.is_initialized():
		log_and_show_error(
			"Server init failed.",
			"Check Editor output and ResonanceRuntime config. Ensure Steam Audio is properly configured."
		)
		return false
	return true
