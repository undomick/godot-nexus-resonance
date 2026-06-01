@tool
extends RefCounted
class_name ResonanceBakeRunner

## Public bake entry for ResonanceProbeVolume (inspector / project menu). Delegates to bake_* modules.

const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")
const ResonanceRuntimeScript = preload("res://addons/nexus_resonance/scripts/resonance_runtime.gd")
const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")
const _BakeEstimates = preload("res://addons/nexus_resonance/editor/resonance_bake_estimates.gd")
const _BakeHashes = preload("res://addons/nexus_resonance/editor/resonance_bake_hashes.gd")
const _BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const _BakeServerSetup = preload(
	"res://addons/nexus_resonance/editor/resonance_bake_server_setup.gd"
)
const _BakePipeline = preload("res://addons/nexus_resonance/editor/resonance_bake_pipeline.gd")
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")

var resonance_bake_progress_ui
var _bake_validation
var resonance_editor_dialogs
var resonance_bake_backup

const DEFAULT_BAKE_INFLUENCE_RADIUS := 10000.0

var editor_interface  # Untyped to allow null passing in Godot 4.x safely
## When set, used as quick link in validation dialog when static scene not exported.
var export_static_callback: Callable = Callable()
var _bake_in_progress: bool = false

# --- RUNTIME VARIABLES ---
var target_root: Node
var save_to_disk: bool = true
signal bake_progress_updated(status_message: String)
signal bake_finished
# -------------------------

var _progress_ui  # Untyped to allow headless operation
var _backup  # Untyped to allow headless operation
var _server_setup: RefCounted
var _pipeline: RefCounted


# Changed parameter to loosely typed to explicitly allow null instantiation at runtime
func _init(p_editor_interface = null) -> void:
	# Conditionally initialize these variables to avoid crashes when running in headless mode.
	if Engine.is_editor_hint():
		_bake_validation = load("res://addons/nexus_resonance/editor/resonance_bake_validation.gd")
		resonance_editor_dialogs = load(
			"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
		)
		resonance_bake_progress_ui = load(
			"res://addons/nexus_resonance/editor/resonance_bake_progress_ui.gd"
		)
		resonance_bake_backup = load("res://addons/nexus_resonance/editor/resonance_bake_backup.gd")

	if p_editor_interface:
		editor_interface = p_editor_interface
		_progress_ui = resonance_bake_progress_ui.new(p_editor_interface)
		_backup = resonance_bake_backup.new()

	_server_setup = _BakeServerSetup.new(self)
	_pipeline = _BakePipeline.new(self)


func shutdown() -> void:
	# Drop RefCounted cycles; release UI and backup state on editor shutdown.
	_bake_in_progress = false
	if _progress_ui and _progress_ui.has_method("shutdown"):
		_progress_ui.shutdown()
	if _backup and _backup.has_method("shutdown"):
		_backup.shutdown()
	if _server_setup and _server_setup.has_method("shutdown"):
		_server_setup.shutdown()
	if _pipeline and _pipeline.has_method("shutdown"):
		_pipeline.shutdown()

	_progress_ui = null
	_backup = null
	_server_setup = null
	_pipeline = null
	export_static_callback = Callable()
	editor_interface = null


# Added runtime parameters root and save_results
func run_bake(volumes: Array[Node], root: Node = null, save_results: bool = true) -> void:
	if volumes.is_empty() or _bake_in_progress:
		return

	save_to_disk = save_results
	if root:
		target_root = root

	var actual_root
	if not Engine.is_editor_hint():
		actual_root = target_root
	else:
		actual_root = _get_edited_scene_root(volumes)

	if not actual_root:
		_log_and_show_error("No scene open", "Open a scene before baking.")
		return

	var static_scene_node = _BakeDiscovery.find_resonance_static_scene_for_bake(
		volumes, actual_root
	)
	_auto_reexport_static_scene_if_stale(actual_root, static_scene_node)
	var static_asset = static_scene_node.get("static_scene_asset") if static_scene_node else null
	_do_run_bake_with_backup(volumes, actual_root, static_scene_node, static_asset)


## Calls export_static_callback when the static asset is missing or export_hash != live scene hash
## (avoids baking against stale geometry).
func _auto_reexport_static_scene_if_stale(root: Node, static_scene_node: Node) -> void:
	if not static_scene_node or not export_static_callback.is_valid():
		return
	if not ResonanceServerAccess.has_server():
		return
	var srv: Variant = ResonanceServerAccess.get_server()
	if not srv or not srv.has_method("get_static_scene_hash"):
		return
	var current_hash: int = srv.get_static_scene_hash(root)
	if current_hash == 0:
		return
	var has_valid: bool = (
		static_scene_node.has_method("has_valid_asset") and static_scene_node.has_valid_asset()
	)
	var stored_hash: int = (
		static_scene_node.export_hash if "export_hash" in static_scene_node else 0
	)
	if has_valid and stored_hash == current_hash:
		return
	export_static_callback.call(null)


func _do_run_bake_with_backup(
	volumes: Array[Node], root: Node, static_scene_node: Node, static_asset
) -> void:
	if save_to_disk and _backup:
		_backup.create_backups(volumes)
	_do_run_bake_after_validation(volumes, root, static_scene_node, static_asset, [])


func _do_run_bake_after_validation(
	volumes: Array[Node], root: Node, static_scene_node: Node, static_asset, _checklist: Array
) -> void:
	if not static_scene_node or not static_scene_node.has_valid_asset():
		_log_and_show_error(
			"Static scene not exported",
			"Use Tools > Nexus Resonance > Export Static Scene before baking.",
			"Scene must be exported first."
		)
		return
	if not ResonanceServerAccess.has_server():
		_log_and_show_error(
			"GDExtension not loaded", "The ResonanceServer GDExtension is not available."
		)
		return
	if not _server_setup.ensure_resonance_server_initialized(volumes):
		return
	var srv = ResonanceServerAccess.get_server()
	if srv and srv.has_method("set_bake_static_scene_asset"):
		srv.set_bake_static_scene_asset(static_asset)
	_bake_in_progress = true
	if _progress_ui:
		_progress_ui.show_ui()

	var base_ctrl = editor_interface.get_base_control() if editor_interface else null
	var tree: SceneTree = base_ctrl.get_tree() if base_ctrl else null

	if not tree and root and root.is_inside_tree():
		tree = root.get_tree()

	if tree:
		var cb = func() -> void: _run_bake_pipeline_main_thread(volumes)
		tree.process_frame.connect(cb, CONNECT_ONE_SHOT)
	else:
		call_deferred("_run_bake_pipeline_main_thread", volumes)


func _get_edited_scene_root(volumes: Array[Node]) -> Node:
	if target_root:
		return target_root
	if editor_interface:
		if Engine.is_editor_hint() and editor_interface:
			return _bake_validation.get_edited_scene_root(volumes, editor_interface)
	if volumes.size() > 0 and volumes[0].is_inside_tree():
		return volumes[0].get_tree().root
	return null


func estimate_probe_count(vol: Node) -> int:
	return _BakeEstimates.estimate_probe_count(vol)


func estimate_bake_time(vol: Node) -> String:
	return _BakeEstimates.estimate_bake_time(vol, _get_bake_config_for_volume(vol))


func get_volume_bake_status(vol: Node) -> String:
	if not vol or not vol.has_method("get_probe_data"):
		return "Not baked"
	var probe_data = vol.get_probe_data()
	if not probe_data:
		return "Not baked"
	var has_data = probe_data.get_data().size() > 0
	if not has_data:
		return "Not baked"
	var bc = _get_bake_config_for_volume(vol)
	var want_path = bc.pathing_enabled
	var path_hash = _BakeHashes.compute_pathing_hash(bc) if want_path else 0
	var ph = (
		probe_data.get_pathing_params_hash()
		if probe_data.has_method("get_pathing_params_hash")
		else 0
	)
	var desired_refl = bc.reflection_type
	var refl_matches = (
		probe_data.get_baked_reflection_type() == desired_refl
		if probe_data.has_method("get_baked_reflection_type")
		else false
	)
	var hash_matches = (
		probe_data.get_bake_params_hash() == vol.get_bake_params_hash()
		if vol.has_method("get_bake_params_hash")
		else false
	)
	if not want_path and ph > 0:
		return "Outdated"
	if not hash_matches or not refl_matches:
		return "Outdated"
	if want_path and (ph == 0 or ph != path_hash):
		return "Outdated"
	var vols: Array[Node] = [vol]
	var root = _get_edited_scene_root(vols)
	var union_static_hash: int = _BakeHashes.compute_all_resonance_static_scenes_params_hash(root)
	if union_static_hash != 0 and probe_data.has_method("get_static_scene_params_hash"):
		var stored_union: int = probe_data.get_static_scene_params_hash()
		if stored_union == 0 or stored_union != union_static_hash:
			return "Outdated"
	var rad = (
		vol.get("bake_influence_radius")
		if "bake_influence_radius" in vol
		else DEFAULT_BAKE_INFLUENCE_RADIUS
	)
	if bc.static_source_enabled and root:
		var src_hash := _bake_entries_hash(vol, root, "bake_sources", "ResonancePlayer", rad)
		var ssh = (
			probe_data.get_static_source_params_hash()
			if probe_data.has_method("get_static_source_params_hash")
			else 0
		)
		if src_hash != 0 and (ssh == 0 or ssh != src_hash):
			return "Outdated"
	if bc.static_listener_enabled and root:
		var lst_hash := _bake_entries_hash(vol, root, "bake_listeners", "ResonanceListener", rad)
		var lsh = (
			probe_data.get_static_listener_params_hash()
			if probe_data.has_method("get_static_listener_params_hash")
			else 0
		)
		if lst_hash != 0 and (lsh == 0 or lsh != lst_hash):
			return "Outdated"
	return "Probes baked"


func _bake_entries_hash(
	vol: Node, root: Node, property: String, target_class: String, rad: float
) -> int:
	var nodes: Array = _BakeDiscovery.resolve_bake_nodes_for_volume(
		vol, root, property, target_class
	)
	var entries: Array = []
	for n in nodes:
		if n is Node3D:
			entries.append({"pos": n.global_position, "radius": rad})
	if entries.is_empty():
		return 0
	if entries.size() == 1:
		return _BakeHashes.compute_position_radius_hash(entries[0].pos, entries[0].radius)
	return _BakeHashes.compute_position_radius_list_hash(entries)


## Ensures the server is initialized and refreshes probe visuals (e.g. inspector selection).
func ensure_resonance_server_for_volumes(volumes: Array[Node]) -> bool:
	if not _server_setup.ensure_resonance_server_initialized(volumes):
		return false
	_notify_volumes_viz_updated(volumes)
	return true


func _log_and_show_error(
	message: String,
	solution: String = "",
	cause: String = "",
	volume_name: String = "",
	step: String = ""
) -> void:
	_server_setup.log_and_show_error(message, solution, cause, volume_name, step)


func _get_bake_config_for_volume(vol) -> Resource:
	if vol and vol.has_method("get_bake_config"):
		var bc = vol.get_bake_config()
		if bc:
			return bc
	return ResonanceBakeConfig.create_default()


func _run_bake_pipeline_main_thread(volumes: Array[Node]) -> void:
	await _pipeline.run_bake_pipeline_main_thread(volumes)


func _finish_pipeline(success: bool, probe_data_ref, volumes: Array[Node]) -> void:
	call_deferred("_on_bake_pipeline_finished", success, probe_data_ref, volumes)


func _on_bake_pipeline_finished(success: bool, probe_data_ref, volumes: Array[Node]) -> void:
	_bake_in_progress = false
	var srv = ResonanceServerAccess.get_server()
	if srv and srv.has_method("set_bake_pipeline_pathing"):
		srv.set_bake_pipeline_pathing(false)
	if success and probe_data_ref:
		if _progress_ui:
			_progress_ui.set_bake_status(tr("Saving probe data..."))

		if save_to_disk:
			match probe_data_ref:
				var arr when arr is Array:
					for pd in arr:
						_save_and_reload_probe_data(pd, volumes)
				_:
					_save_and_reload_probe_data(probe_data_ref, volumes)
			_show_bake_complete_dialog(volumes)
		else:
			# Runtime: apply to memory only (no save / completion dialog).
			bake_progress_updated.emit("Applying generated acoustics to memory...")
			match probe_data_ref:
				var arr when arr is Array:
					for pd in arr:
						_reload_volumes_using_probe_data(pd, volumes)
				_:
					_reload_volumes_using_probe_data(probe_data_ref, volumes)

	elif not success:
		var cancel_req = _progress_ui.cancel_requested if _progress_ui else false
		if not cancel_req:
			_log_and_show_error("Bake failed.", "Check Editor output and ensure geometry is valid.")

	if _progress_ui:
		_progress_ui.hide_ui()

	if save_to_disk and editor_interface:
		editor_interface.mark_scene_as_unsaved()
	_notify_volumes_viz_updated(volumes)
	bake_finished.emit()


func _save_and_reload_probe_data(probe_data_ref: Resource, volumes: Array[Node]) -> void:
	# Strip legacy .bak resource_path so saves go to the canonical .res (avoids .bak chains).
	var path: String = probe_data_ref.resource_path
	while path.ends_with(".bak"):
		path = path.get_basename()
	if (
		path != probe_data_ref.resource_path
		and not path.is_empty()
		and probe_data_ref.has_method("take_over_path")
	):
		probe_data_ref.take_over_path(path)
	if not path.is_empty():
		var err = ResourceSaver.save(probe_data_ref)
		if err != OK:
			var vol_name := volumes[0].name if volumes.size() > 0 else "?"
			if Engine.has_singleton("ResonanceLogger"):
				Engine.get_singleton("ResonanceLogger").log(
					&"bake",
					"Failed to save probe data: %s" % err,
					{"volume": vol_name, "step": "save", "error_code": err}
				)
			if Engine.is_editor_hint() and editor_interface:
				resonance_editor_dialogs.show_error_dialog(
					editor_interface,
					tr(UIStrings.DIALOG_SAVE_FAILED_TITLE),
					tr(UIStrings.ERR_FAILED_TO_SAVE) % err,
					"ResourceSaver.save returned non-OK.",
					"Ensure res://audio_data/ is writable."
				)
	_reload_volumes_using_probe_data(probe_data_ref, volumes)


func _reload_volumes_using_probe_data(probe_data_ref: Resource, volumes: Array[Node]) -> void:
	var root = _get_edited_scene_root(volumes)
	if not root:
		return
	var collected: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_probe_volumes(root, collected)
	var all_volumes: Array[Node] = []
	for v in collected:
		all_volumes.append(v)
	for vol in all_volumes:
		if vol.has_method("get_probe_data") and vol.get_probe_data() == probe_data_ref:
			if vol.has_method("reload_probe_batch"):
				vol.reload_probe_batch()
	_notify_volumes_viz_updated(all_volumes)


func _notify_volumes_viz_updated(volumes: Array[Node]) -> void:
	var root = _get_edited_scene_root(volumes)
	if not root:
		return
	var cfg = _BakeDiscovery.find_resonance_runtime(root, ResonanceRuntimeScript)
	if not cfg:
		return
	var rt = cfg.get("runtime") if cfg else null
	var refl: int = rt.get("reflection_type") if rt else 0
	var pathing: bool = rt.get("pathing_enabled") if rt else false
	for vol in volumes:
		if vol.has_method("notify_runtime_config_changed"):
			vol.notify_runtime_config_changed(refl, pathing)


func _show_bake_complete_dialog(volumes: Array) -> void:
	if not editor_interface:
		# Headless: no dialog; drop .bak files immediately.
		if _backup:
			_backup.discard_backups()
		return
	var base = editor_interface.get_base_control()
	if not base:
		if _backup:
			_backup.discard_backups()
		return
	var has_backups = _backup.has_backups() if _backup else false
	var msg = "Bake completed for %d Probe Volume(s)." % volumes.size()
	if has_backups:
		msg += "\n\nYou can Undo to restore the previous Probe Volume data."
	var dialog = AcceptDialog.new()
	dialog.title = tr(UIStrings.ADDON_NAME)
	dialog.dialog_text = msg
	dialog.theme = editor_interface.get_editor_theme()
	# True if user chose Undo; otherwise on_dialog_close discards .bak files.
	var undo_invoked := [false]
	var on_dialog_close := func() -> void:
		if not undo_invoked[0] and _backup:
			_backup.discard_backups()
		dialog.queue_free()
	dialog.confirmed.connect(on_dialog_close)
	dialog.close_requested.connect(on_dialog_close)
	if has_backups:
		var undo_btn = dialog.add_button(tr(UIStrings.BTN_UNDO), false, "undo")
		undo_btn.pressed.connect(
			func():
				undo_invoked[0] = true
				_backup.restore(
					volumes,
					editor_interface,
					_reload_volumes_using_probe_data,
					func(): _on_restore_complete(volumes)
				)
				dialog.queue_free()
		)
	base.add_child(dialog)
	dialog.popup_centered()


func _on_restore_complete(volumes: Array) -> void:
	if editor_interface:
		editor_interface.mark_scene_as_unsaved()
	_notify_volumes_viz_updated(volumes)
