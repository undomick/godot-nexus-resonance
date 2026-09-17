@tool
extends RefCounted

## Static/dynamic export, OBJ, probe cleanup. Shared from the editor plugin.

const ResonanceFsPaths = preload("res://addons/nexus_resonance/scripts/resonance_fs_paths.gd")
const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)
const SceneIndex = preload("res://addons/nexus_resonance/editor/resonance_editor_scene_index.gd")
const ProbeRefIndex = preload(
	"res://addons/nexus_resonance/editor/resonance_probe_reference_index.gd"
)
const JobProgressScript = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_job_progress.gd"
)
const ProbeClearPolicy = preload(
	"res://addons/nexus_resonance/editor/resonance_probe_clear_policy.gd"
)

var editor_interface: EditorInterface
var _job_progress = null
var _export_job_running: bool = false


func _init(p_editor_interface: EditorInterface) -> void:
	editor_interface = p_editor_interface
	if editor_interface:
		_job_progress = JobProgressScript.new(editor_interface)


func _get_editor_tree() -> SceneTree:
	if editor_interface:
		var base: Control = EditorInterface.get_base_control()
		if base:
			return base.get_tree()
	var main_loop: MainLoop = Engine.get_main_loop()
	return main_loop if main_loop is SceneTree else null


func _block_if_playing_scene() -> bool:
	if not editor_interface:
		return false
	if not EditorInterface.is_playing_scene():
		return false
	ResonanceEditorDialogs.show_error_dialog(
		editor_interface,
		tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
		tr(UIStrings.ERR_EXPORT_WHILE_PLAYING),
		tr(UIStrings.ERR_EXPORT_WHILE_PLAYING_DETAIL),
		""
	)
	return true


func _try_begin_export_job() -> bool:
	if _block_if_playing_scene():
		return false
	if _export_job_running:
		_show_warning(tr(UIStrings.WARN_EXPORT_JOB_ALREADY_RUNNING))
		return false
	_export_job_running = true
	if editor_interface:
		ResonanceEditorDialogs.show_success_toast(
			editor_interface, tr(UIStrings.INFO_EXPORT_JOB_RUNNING)
		)
	return true


func _end_export_job() -> void:
	_export_job_running = false
	if _job_progress:
		_job_progress.hide_job()


## Cancel in-flight export UI and drop editor refs (plugin _exit_tree).
func shutdown() -> void:
	if _job_progress:
		_job_progress.cancel_requested = true
		if _job_progress.has_method("shutdown"):
			_job_progress.shutdown()
		else:
			_job_progress.hide_job()
	_job_progress = null
	_export_job_running = false
	editor_interface = null


func _dispatch_export_async(worker: Callable) -> void:
	if not _try_begin_export_job():
		return
	var tree: SceneTree = _get_editor_tree()
	if tree == null:
		worker.call()
		return
	var cb := func() -> void: await worker.call()
	tree.process_frame.connect(cb, CONNECT_ONE_SHOT)


func _show_warning(message: String) -> void:
	ResonanceEditorDialogs.show_warning(editor_interface, message)


func _show_gdextension_error() -> void:
	ResonanceEditorDialogs.show_critical(
		editor_interface,
		tr(UIStrings.ERR_GDEXTENSION_NOT_LOADED),
		tr(UIStrings.DIALOG_GDEXTENSION_NOT_LOADED_TITLE)
	)


## Returns ResonanceServer singleton or null after showing error. Checks required_method if non-empty.
func get_resonance_server_or_show_error(required_method: String = "") -> Variant:
	if not ResonanceServerAccess.has_server():
		_show_gdextension_error()
		return null
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv and not required_method.is_empty() and not srv.has_method(required_method):
		ResonanceEditorDialogs.show_error_dialog(
			editor_interface,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
			tr(UIStrings.ERR_SERVER_LACKS_EXPORT),
			tr(UIStrings.ERR_GDEXTENSION_SYNC),
			tr(UIStrings.ERR_GDEXTENSION_SYNC_SOLUTION)
		)
		return null
	return srv


## Returns main scene path or empty string after showing error.
func get_main_scene_path_or_show_error() -> String:
	var main_path: String = ProjectSettings.get_setting("application/run/main_scene", "")
	if main_path.is_empty():
		ResonanceEditorDialogs.show_error_dialog(
			editor_interface,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
			tr(UIStrings.ERR_NO_MAIN_SCENE),
			tr(UIStrings.ERR_SET_MAIN_SCENE),
			""
		)
		return ""
	main_path = ResonanceFsPaths.resolve_resource_path(main_path)
	if main_path.begins_with("uid://"):
		ResonanceEditorDialogs.show_critical(
			editor_interface,
			tr(UIStrings.ERR_FAILED_TO_LOAD_MAIN_SCENE) % main_path,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE)
		)
		return ""
	return main_path


## Ensures a logical res:// output directory exists. Returns true on success.
func ensure_output_dir(logical_path: String) -> bool:
	var fs_path: String = ResonanceFsPaths.filesystem_path_for_dir_access(logical_path)
	if DirAccess.dir_exists_absolute(fs_path):
		return true
	var err: int = DirAccess.make_dir_recursive_absolute(fs_path)
	if err != OK or not DirAccess.dir_exists_absolute(fs_path):
		ResonanceEditorDialogs.show_error_dialog(
			editor_interface,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
			tr(UIStrings.ERR_MKDIR_OUTPUT_DIR) % [logical_path, err],
			"",
			""
		)
		return false
	return true


## Ensures statics/ under the bake output root exists.
func ensure_statics_dir() -> bool:
	return ensure_output_dir(ResonancePaths.get_statics_dir())


## Ensures dynamics/ under the bake output root exists.
func ensure_dynamics_dir() -> bool:
	return ensure_output_dir(ResonancePaths.get_dynamics_dir())


## Ensures batches/ under the bake output root exists.
func ensure_batches_dir() -> bool:
	return ensure_output_dir(ResonancePaths.get_batches_dir())


## update_file + reimport_files for new OBJ paths (avoids full scan + reimport race).
func _request_obj_reimport(paths: PackedStringArray) -> void:
	if paths.is_empty():
		return
	var fs: EditorFileSystem = EditorInterface.get_resource_filesystem()
	if not fs:
		return
	for p in paths:
		fs.update_file(p)
	fs.reimport_files(paths)


func collect_scene_paths_for_obj(node: Node, out: Dictionary) -> void:
	if not node:
		return
	var path_str: String = ResonanceFsPaths.resolve_resource_path(node.get_scene_file_path())
	if not path_str.is_empty() and not path_str.begins_with("uid://"):
		out[path_str] = true
	for c in node.get_children():
		collect_scene_paths_for_obj(c, out)


## Only scenes that already own a ResonanceStaticScene (no auto-create candidates).
func filter_scene_paths_by_existing_static_scene(paths_dict: Dictionary) -> PackedStringArray:
	var filtered: PackedStringArray = []
	for path in paths_dict:
		if SceneIndex.scene_text_has_resonance_static_scene(path):
			filtered.append(path)
			continue
		var scene: PackedScene = load(path) as PackedScene
		if not scene:
			continue
		var inst: Node = scene.instantiate()
		var ok: bool = ResonanceSceneUtils.find_owned_resonance_static_scene(inst) != null
		inst.queue_free()
		if ok:
			filtered.append(path)
	return filtered


func collect_project_tscn_paths() -> PackedStringArray:
	if editor_interface:
		return SceneIndex.collect_tscn_paths(editor_interface, "res://")
	return SceneIndex.collect_tscn_paths_under("res://")


func collect_tscn_files_recursive(dir: String, out: PackedStringArray) -> void:
	var paths: PackedStringArray
	if dir == "res://":
		paths = collect_project_tscn_paths()
	else:
		paths = SceneIndex.collect_tscn_paths_under(dir)
	for p in paths:
		out.append(p)


## Returns scene paths from main scene tree (for build). Keys: paths, skipped.
## filter_existing_static_scene: if true, only paths that already declare a ResonanceStaticScene.
func _get_scene_paths_from_build(filter_existing_static_scene: bool = false) -> Dictionary:
	var main_path: String = get_main_scene_path_or_show_error()
	if main_path.is_empty():
		return {"paths": PackedStringArray(), "skipped": 0}
	var packed: PackedScene = load(main_path) as PackedScene
	if not packed:
		ResonanceEditorDialogs.show_critical(
			editor_interface,
			tr(UIStrings.ERR_FAILED_TO_LOAD_MAIN_SCENE) % main_path,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE)
		)
		return {"paths": PackedStringArray(), "skipped": 0}
	var instance: Node = packed.instantiate()
	var paths_dict: Dictionary = {}
	collect_scene_paths_for_obj(instance, paths_dict)
	instance.queue_free()
	paths_dict[main_path] = true
	if filter_existing_static_scene:
		var filtered_paths: PackedStringArray = filter_scene_paths_by_existing_static_scene(
			paths_dict
		)
		var skipped: int = paths_dict.size() - filtered_paths.size()
		return {"paths": filtered_paths, "skipped": skipped}
	return {"paths": PackedStringArray(paths_dict.keys()), "skipped": 0}


## Static export SSOT: asset file + ResonanceStaticScene fields (+ optional .tscn save).
## Returns OK on success; 1 = no exportable static content / missing RSS when create_if_missing false;
## 2 = nested packs only (no local geometry); 3 = unchanged (skip_unchanged); other = export/save errors.
func _export_static_scene_ssot(
	root: Node, scene_path: String, srv: Variant, opts: Dictionary = {}
) -> int:
	if not root:
		return ERR_INVALID_PARAMETER
	var skip_unchanged: bool = opts.get("skip_unchanged", false)
	var persist_scene: bool = opts.get("persist_scene", false)
	var create_if_missing: bool = opts.get("create_if_missing", true)
	scene_path = ResonanceFsPaths.resolve_resource_path(scene_path)
	if persist_scene and (scene_path.is_empty() or scene_path.begins_with("uid://")):
		return ERR_FILE_BAD_PATH
	if not ResonanceSceneUtils.scene_has_exportable_resonance_content(root, "static"):
		return 1
	var scene_name: String = "unsaved"
	if not scene_path.is_empty():
		scene_name = scene_path.get_file().get_basename()
	var save_path: String = ResonancePaths.static_scene_asset_save_path(scene_name)
	var static_scene_node: Node = ResonanceSceneUtils.find_owned_resonance_static_scene(root)
	if not static_scene_node and not create_if_missing:
		return 1
	ResonanceSceneUtils.warn_static_scenes_without_asset_covering_geometry(root)
	var current_hash: int = (
		srv.get_static_scene_hash(root) if srv.has_method("get_static_scene_hash") else 0
	)
	if current_hash == 0:
		if ResonanceSceneUtils.has_nested_resonance_static_scene(root):
			return 2
		return 1
	if (
		skip_unchanged
		and static_scene_node
		and static_scene_node.export_hash == current_hash
		and current_hash != 0
	):
		var has_valid: bool = (
			static_scene_node.has_method("has_valid_asset") and static_scene_node.has_valid_asset()
		)
		var file_exists: bool = ResonanceFsPaths.file_exists_for_path(save_path)
		if has_valid and file_exists:
			return 3
	var err: int = srv.export_static_scene_to_asset(root, save_path)
	if err != OK:
		return err
	if not static_scene_node:
		static_scene_node = ClassDB.instantiate("ResonanceStaticScene")
		static_scene_node.name = "ResonanceStaticScene"
		root.add_child(static_scene_node)
		static_scene_node.owner = root
	var asset: Resource = ResourceLoader.load(save_path, "", ResourceLoader.CACHE_MODE_REPLACE)
	if asset:
		static_scene_node.static_scene_asset = asset
		static_scene_node.scene_name_when_exported = scene_name
		static_scene_node.export_hash = current_hash
	if persist_scene and not scene_path.is_empty():
		var packed_scene: PackedScene = PackedScene.new()
		if packed_scene.pack(root) != OK:
			return ERR_CANT_CREATE
		var save_err: int = ResourceSaver.save(packed_scene, scene_path)
		if save_err != OK:
			push_warning(
				(
					"Nexus Resonance: Failed to save scene %s after static export (error %s)"
					% [scene_path, save_err]
				)
			)
			return save_err
	return OK


func _export_one_static_scene(path: String, srv: Variant) -> int:
	var scene_path: String = ResonanceFsPaths.resolve_resource_path(path)
	if scene_path.is_empty() or scene_path.begins_with("uid://"):
		return ERR_FILE_NOT_FOUND
	var scene: PackedScene = load(scene_path) as PackedScene
	if not scene:
		return ERR_FILE_NOT_FOUND
	var inst: Node = scene.instantiate()
	var err: int = _export_static_scene_ssot(
		inst,
		scene_path,
		srv,
		{"skip_unchanged": false, "persist_scene": true, "create_if_missing": false},
	)
	inst.queue_free()
	return err


## Exports static geometry from scene paths.
## Returns {exported, skipped_no_static, failed}.
func _export_static_scenes_batch(paths: PackedStringArray) -> Dictionary:
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_asset")
	if srv == null:
		return {"exported": 0, "skipped_no_static": 0, "failed": paths.size()}
	if not ensure_statics_dir():
		return {"exported": 0, "skipped_no_static": 0, "failed": paths.size()}
	var exported: int = 0
	var skipped_no_static: int = 0
	var failed: int = 0
	for path in paths:
		var err: int = _export_one_static_scene(path, srv)
		var tallies: Dictionary = _accumulate_static_export_result(
			err, exported, skipped_no_static, failed
		)
		exported = tallies.exported
		skipped_no_static = tallies.skipped_no_static
		failed = tallies.failed
	if exported > 0 and editor_interface:
		EditorInterface.get_resource_filesystem().scan()
	return {"exported": exported, "skipped_no_static": skipped_no_static, "failed": failed}


func _static_export_is_empty_skip(err: int) -> bool:
	# Matches _export_static_scene_ssot: 1 = no content, 2 = nested only, 3 = unchanged.
	return err == 1 or err == 2 or err == 3


func _accumulate_static_export_result(
	err: int, exported: int, skipped_no_static: int, failed: int
) -> Dictionary:
	if err == OK:
		exported += 1
	elif _static_export_is_empty_skip(err):
		skipped_no_static += 1
	else:
		failed += 1
	return {
		"exported": exported,
		"skipped_no_static": skipped_no_static,
		"failed": failed,
	}


func _finish_static_batch_toasts(result: Dictionary, success_fmt: String = "") -> void:
	var exported: int = int(result.get("exported", 0))
	var skipped_no_static: int = int(result.get("skipped_no_static", 0))
	var failed: int = int(result.get("failed", 0))
	var fmt: String = (
		success_fmt
		if not success_fmt.is_empty()
		else UIStrings.INFO_STATIC_SCENES_IN_BUILD_EXPORTED
	)
	if exported > 0:
		var msg: String = tr(fmt) % exported
		if skipped_no_static > 0:
			msg += " " + (tr(UIStrings.INFO_SCENES_FILTERED) % skipped_no_static)
		if failed > 0:
			msg += " " + (tr(UIStrings.INFO_STATIC_EXPORT_FAILED) % failed)
		ResonanceEditorDialogs.show_success_toast(editor_interface, msg)
	elif failed > 0:
		var msg := tr(UIStrings.WARN_NO_SCENES_EXPORTED)
		if skipped_no_static > 0:
			msg += " " + (tr(UIStrings.INFO_SCENES_FILTERED) % skipped_no_static)
		msg += " " + (tr(UIStrings.INFO_STATIC_EXPORT_FAILED) % failed)
		_show_warning(msg)
	elif skipped_no_static > 0:
		_show_warning(
			(
				tr(UIStrings.WARN_NO_SCENES_EXPORTED)
				+ " "
				+ (tr(UIStrings.INFO_SCENES_FILTERED) % skipped_no_static)
			)
		)
	else:
		_show_warning(tr(UIStrings.WARN_NO_SCENES_EXPORTED))


func _export_static_scenes_batch_async(paths: PackedStringArray, success_fmt: String = "") -> void:
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_asset")
	if srv == null:
		_end_export_job()
		return
	if not ensure_statics_dir():
		_end_export_job()
		return
	if _job_progress:
		_job_progress.show_job(tr(UIStrings.DIALOG_EXPORT_JOB_TITLE), paths.size())
	var exported: int = 0
	var skipped_no_static: int = 0
	var failed: int = 0
	var tree: SceneTree = _get_editor_tree()
	for i in paths.size():
		if _job_progress and _job_progress.cancel_requested:
			break
		var path: String = paths[i]
		if _job_progress:
			_job_progress.set_step(i + 1, paths.size(), path)
		var err: int = _export_one_static_scene(path, srv)
		var tallies: Dictionary = _accumulate_static_export_result(
			err, exported, skipped_no_static, failed
		)
		exported = tallies.exported
		skipped_no_static = tallies.skipped_no_static
		failed = tallies.failed
		if tree:
			await tree.process_frame
	if exported > 0 and editor_interface:
		EditorInterface.get_resource_filesystem().scan()
	_finish_static_batch_toasts(
		{
			"exported": exported,
			"skipped_no_static": skipped_no_static,
			"failed": failed,
		},
		success_fmt
	)
	_end_export_job()


## Exports ResonanceDynamicGeometry from scene paths. make_save_path(scene_path, scene_base, geom) -> String.
## dedup: if true, skip geoms with same scene_path|parent_name. Returns {exported: int, scenes_saved: int}.
func _export_dynamic_objects_batch(
	paths: PackedStringArray, make_save_path: Callable, dedup: bool = false
) -> Dictionary:
	if not ensure_dynamics_dir():
		return {"exported": 0, "scenes_saved": 0}
	var exported: int = 0
	var scenes_saved: int = 0
	var seen_geoms: Dictionary = {}
	for path in paths:
		var scene: PackedScene = load(path) as PackedScene
		if not scene:
			continue
		var inst: Node = scene.instantiate()
		var scene_base: String = str(path).get_file().get_basename()
		var dynamic_geoms: Array[Node] = []
		ResonanceSceneUtils.collect_resonance_dynamic_geometry(inst, dynamic_geoms)
		var scene_exported: int = 0
		for geom in dynamic_geoms:
			var parent_name: String = str(geom.get_parent().name) if geom.get_parent() else "mesh"
			var key: String = str(path) + "|" + parent_name
			if dedup and seen_geoms.get(key, false):
				continue
			if dedup:
				seen_geoms[key] = true
			var save_path: String = make_save_path.call(path, scene_base, geom)
			var err: int = geom.export_dynamic_mesh_to_asset(save_path)
			if err == OK:
				exported += 1
				scene_exported += 1
		if scene_exported > 0:
			var packed_scene: PackedScene = PackedScene.new()
			if packed_scene.pack(inst) == OK:
				var save_err: int = ResourceSaver.save(packed_scene, path)
				if save_err == OK:
					scenes_saved += 1
				else:
					push_warning(
						"Nexus Resonance: Failed to save scene %s (error %s)" % [path, save_err]
					)
		inst.queue_free()
	if exported > 0:
		EditorInterface.get_resource_filesystem().scan()
	return {"exported": exported, "scenes_saved": scenes_saved}


## Export static ResonanceGeometry (dynamic=false) to merged asset. Creates/updates ResonanceStaticScene.
## Used as bake_runner.export_static_callback (called before bake when static scene needs export).
func export_active_scene(_unused: Variant = null) -> void:
	_dispatch_export_async(Callable(self, "_export_active_scene_async"))


## Synchronous export for bake_runner.export_static_callback (no export-job guard or menu toast).
func export_active_scene_sync_for_bake(_unused: Variant = null) -> void:
	_export_active_scene_core()


func _export_active_scene_async() -> void:
	if _job_progress and editor_interface:
		var root: Node = EditorInterface.get_edited_scene_root()
		var label: String = root.get_scene_file_path() if root else ""
		_job_progress.show_job(tr(UIStrings.DIALOG_EXPORT_JOB_TITLE), 1)
		_job_progress.set_step(1, 1, label)
	var tree: SceneTree = _get_editor_tree()
	if tree:
		await tree.process_frame
	_export_active_scene_core()
	_end_export_job()


func _export_active_scene_core() -> void:
	var root: Node = EditorInterface.get_edited_scene_root()
	if not root:
		_show_warning(tr(UIStrings.WARN_NO_SCENE))
		return
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_asset")
	if srv == null:
		return
	if not ensure_statics_dir():
		return
	var scene_path: String = root.get_scene_file_path()
	var err: int = _export_static_scene_ssot(
		root, scene_path, srv, {"skip_unchanged": true, "persist_scene": false}
	)
	if err == 1:
		_show_warning(tr(UIStrings.WARN_NO_EXPORTABLE_STATIC_CONTENT))
		return
	if err == 2:
		if _export_job_running:
			ResonanceEditorDialogs.show_info(tr(UIStrings.INFO_STATIC_NOTHING_LOCAL))
		return
	if err == 3:
		if _export_job_running:
			ResonanceEditorDialogs.show_info(tr(UIStrings.INFO_STATIC_UNCHANGED))
		return
	if err != OK:
		ResonanceEditorDialogs.show_critical(
			editor_interface,
			tr(UIStrings.ERR_EXPORT_FAILED) % err,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE)
		)
		return
	if editor_interface:
		EditorInterface.get_resource_filesystem().scan()
		EditorInterface.mark_scene_as_unsaved()
	var scene_name: String = "unsaved"
	if not scene_path.is_empty():
		scene_name = scene_path.get_file().get_basename()
	var save_path: String = ResonancePaths.static_scene_asset_save_path(scene_name)
	if _export_job_running and editor_interface:
		ResonanceEditorDialogs.show_success_toast(
			editor_interface, tr(UIStrings.INFO_STATIC_EXPORTED) % save_path
		)


## Re-export static packs for scenes in the main build tree that already have a ResonanceStaticScene.
## Does not create new ResonanceStaticScene nodes; use Export Active Scene for first-time export.
func export_static_scenes_in_build(_unused: Variant = null) -> void:
	if _block_if_playing_scene():
		return
	var build_data: Dictionary = _get_scene_paths_from_build(true)
	var paths: PackedStringArray = build_data.paths
	if paths.is_empty():
		_show_warning(tr(UIStrings.WARN_NO_SCENES_EXPORTED))
		return
	_dispatch_export_async(
		_export_static_scenes_batch_async.bind(
			paths, UIStrings.INFO_STATIC_SCENES_IN_BUILD_EXPORTED
		)
	)


## Export static ResonanceGeometry from active scene to OBJ+MTL (debug/collada workflow).
func export_scene_obj(_unused: Variant = null) -> void:
	if _block_if_playing_scene():
		return
	var root: Node = EditorInterface.get_edited_scene_root()
	if not root:
		_show_warning(tr(UIStrings.WARN_NO_SCENE))
		return
	if not ResonanceSceneUtils.scene_has_exportable_resonance_content(root, "static"):
		_show_warning(tr(UIStrings.WARN_NO_EXPORTABLE_STATIC_CONTENT))
		return
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_obj")
	if srv == null:
		return
	if not ensure_statics_dir():
		return
	var scene_name: String = "unsaved"
	var scene_path: String = root.get_scene_file_path()
	if not scene_path.is_empty():
		scene_name = scene_path.get_file().get_basename()
	var save_base: String = ResonancePaths.get_statics_dir() + scene_name + "_scene"
	var err: int = srv.export_static_scene_to_obj(root, save_base)
	if err != OK:
		ResonanceEditorDialogs.show_critical(
			editor_interface,
			tr(UIStrings.ERR_EXPORT_FAILED) % err,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE)
		)
		return
	_request_obj_reimport(PackedStringArray([save_base + ".obj"]))
	ResonanceEditorDialogs.show_success_toast(
		editor_interface, tr(UIStrings.INFO_SCENE_OBJ_EXPORTED) % (save_base + ".obj")
	)


## Export all ResonanceDynamicGeometry nodes in active scene to mesh assets.
func export_dynamic_mesh(_unused: Variant = null) -> void:
	if _block_if_playing_scene():
		return
	var root: Node = EditorInterface.get_edited_scene_root()
	if not root:
		_show_warning(tr(UIStrings.WARN_NO_SCENE))
		return
	var dynamic_geoms: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_dynamic_geometry(root, dynamic_geoms)
	if dynamic_geoms.is_empty():
		_show_warning(tr(UIStrings.WARN_NO_DYNAMIC_GEOMETRY))
		return
	if not ensure_dynamics_dir():
		return
	var exported: int = 0
	for geom in dynamic_geoms:
		var parent_name: String = str(geom.get_parent().name) if geom.get_parent() else "mesh"
		var save_path: String = ResonancePaths.dynamic_mesh_asset_save_path(
			parent_name.to_snake_case()
		)
		var err: int = geom.export_dynamic_mesh_to_asset(save_path)
		if err == OK:
			exported += 1
	if exported > 0:
		EditorInterface.get_resource_filesystem().scan()
		var scene_path: String = root.get_scene_file_path()
		if not scene_path.is_empty():
			var save_err: int = EditorInterface.save_scene()
			if save_err != OK:
				_show_warning(tr(UIStrings.WARN_EXPORTED_BUT_SAVE_FAILED) % [exported, save_err])
			else:
				ResonanceEditorDialogs.show_success_toast(
					editor_interface, tr(UIStrings.INFO_DYNAMIC_MESHES_EXPORTED) % exported
				)
		else:
			EditorInterface.mark_scene_as_unsaved()
			ResonanceEditorDialogs.show_success_toast(
				editor_interface,
				(
					tr(UIStrings.INFO_DYNAMIC_MESHES_EXPORTED) % exported
					+ tr(UIStrings.WARN_SAVE_SCENE_TO_PERSIST)
				)
			)


## Export all ResonanceDynamicGeometry from all dependent scenes in the main scene tree.
func export_dynamic_objects_in_build(_unused: Variant = null) -> void:
	if _block_if_playing_scene():
		return
	var build_data: Dictionary = _get_scene_paths_from_build(false)
	var paths: PackedStringArray = build_data.paths
	if paths.is_empty():
		return
	if not ensure_dynamics_dir():
		return
	var make_save_path: Callable = func(_path: Variant, scene_base: String, geom: Node) -> String:
		var parent_name: String = str(geom.get_parent().name) if geom.get_parent() else "mesh"
		return ResonancePaths.dynamic_mesh_asset_save_path(
			scene_base + "_" + parent_name.to_snake_case()
		)
	var result: Dictionary = _export_dynamic_objects_batch(paths, make_save_path, false)
	if result.exported > 0:
		var suffix: String = (
			(" (%d scene(s) saved)" % result.scenes_saved) if result.scenes_saved > 0 else ""
		)
		ResonanceEditorDialogs.show_success_toast(
			editor_interface,
			tr(UIStrings.INFO_DYNAMIC_OBJECTS_IN_BUILD_EXPORTED) % result.exported + suffix
		)
	else:
		_show_warning(tr(UIStrings.WARN_NO_DYNAMIC_EXPORTED))


## Export all ResonanceDynamicGeometry from every scene in the project.
func export_dynamic_objects_in_project(_unused: Variant = null) -> void:
	if get_resonance_server_or_show_error() == null:
		return
	var tscn_files: PackedStringArray = collect_project_tscn_paths()
	if tscn_files.is_empty():
		_show_warning(tr(UIStrings.WARN_NO_SCENE_FILES))
		return
	_dispatch_export_async(_export_dynamic_objects_in_project_async.bind(tscn_files))


func _export_dynamic_objects_in_project_async(tscn_files: PackedStringArray) -> void:
	var make_save_path: Callable = func(
		scene_path: Variant, scene_base: String, geom: Node
	) -> String:
		var rel_dir: String = str(scene_path).get_base_dir().replace("res://", "").replace("/", "_")
		var parent_name: String = str(geom.get_parent().name) if geom.get_parent() else "mesh"
		return ResonancePaths.dynamic_mesh_asset_save_path(
			rel_dir + "_" + scene_base + "_" + parent_name.to_snake_case()
		)
	var result: Dictionary = await _export_dynamic_objects_batch_async(
		tscn_files, make_save_path, true
	)
	if result.exported > 0:
		var suffix: String = (
			(" (%d scene(s) saved)" % result.scenes_saved) if result.scenes_saved > 0 else ""
		)
		ResonanceEditorDialogs.show_success_toast(
			editor_interface,
			tr(UIStrings.INFO_DYNAMIC_OBJECTS_IN_PROJECT_EXPORTED) % result.exported + suffix
		)
	else:
		_show_warning(tr(UIStrings.WARN_NO_DYNAMIC_EXPORTED))
	_end_export_job()


func _export_dynamic_objects_batch_async(
	paths: PackedStringArray, make_save_path: Callable, dedup: bool = false
) -> Dictionary:
	if not ensure_dynamics_dir():
		return {"exported": 0, "scenes_saved": 0}
	if _job_progress:
		_job_progress.show_job(tr(UIStrings.DIALOG_EXPORT_JOB_TITLE), paths.size())
	var exported: int = 0
	var scenes_saved: int = 0
	var seen_geoms: Dictionary = {}
	var tree: SceneTree = _get_editor_tree()
	for path_index in paths.size():
		if _job_progress and _job_progress.cancel_requested:
			break
		var path: String = paths[path_index]
		if _job_progress:
			_job_progress.set_step(path_index + 1, paths.size(), path)
		var scene: PackedScene = load(path) as PackedScene
		if not scene:
			if tree:
				await tree.process_frame
			continue
		var inst: Node = scene.instantiate()
		var scene_base: String = str(path).get_file().get_basename()
		var dynamic_geoms: Array[Node] = []
		ResonanceSceneUtils.collect_resonance_dynamic_geometry(inst, dynamic_geoms)
		var scene_exported: int = 0
		for geom in dynamic_geoms:
			var parent_name: String = str(geom.get_parent().name) if geom.get_parent() else "mesh"
			var key: String = str(path) + "|" + parent_name
			if dedup and seen_geoms.get(key, false):
				continue
			if dedup:
				seen_geoms[key] = true
			var save_path: String = make_save_path.call(path, scene_base, geom)
			var err: int = geom.export_dynamic_mesh_to_asset(save_path)
			if err == OK:
				exported += 1
				scene_exported += 1
		if scene_exported > 0:
			var packed_scene: PackedScene = PackedScene.new()
			if packed_scene.pack(inst) == OK:
				var save_err: int = ResourceSaver.save(packed_scene, path)
				if save_err == OK:
					scenes_saved += 1
				else:
					push_warning(
						"Nexus Resonance: Failed to save scene %s (error %s)" % [path, save_err]
					)
		inst.queue_free()
		if tree:
			await tree.process_frame
	if exported > 0 and editor_interface:
		EditorInterface.get_resource_filesystem().scan()
	return {"exported": exported, "scenes_saved": scenes_saved}


func list_probe_data_files() -> PackedStringArray:
	var out: PackedStringArray = []
	var logical_dir: String = ResonancePaths.get_batches_dir()
	var d: DirAccess = ResonanceFsPaths.open_dir_for_path(logical_dir)
	if not d:
		return out
	d.list_dir_begin()
	var name_str: String = d.get_next()
	while name_str != "":
		var ext_probe := name_str.get_extension().to_lower()
		if (
			(ext_probe == "tres" or ext_probe == "res")
			and ("_batch" in name_str or "_baked_probes" in name_str)
		):
			out.append(logical_dir + name_str)
		name_str = d.get_next()
	d.list_dir_end()
	return out


func find_referenced_probe_data_paths() -> PackedStringArray:
	var probe_files: PackedStringArray = list_probe_data_files()
	var referenced: PackedStringArray = []
	var tscn_files: PackedStringArray = collect_project_tscn_paths()
	var scene_contents: Dictionary = {}
	for scene_path in tscn_files:
		var content: String = ResonanceFsPaths.read_file_as_string(scene_path)
		if not content.is_empty():
			scene_contents[scene_path] = content
	for probe_path in probe_files:
		for content in scene_contents.values():
			if ResonanceFsPaths.scene_text_references_probe_path(content, probe_path):
				if probe_path not in referenced:
					referenced.append(probe_path)
				break
	return ProbeRefIndex.merge_referenced_paths(referenced, _collect_live_edited_probe_data_paths())


## Delete ResonanceProbeData assets in batches/ that are not referenced by any scene or prefab.
func clear_unreferenced_probe_data(_unused: Variant = null) -> void:
	_dispatch_export_async(Callable(self, "_clear_unreferenced_probe_data_async"))


func _clear_unreferenced_probe_data_async() -> void:
	var probe_files: PackedStringArray = list_probe_data_files()
	if probe_files.is_empty():
		ResonanceEditorDialogs.show_info(
			tr(UIStrings.INFO_NO_PROBE_DATA_FILES) % ResonancePaths.get_batches_dir()
		)
		_end_export_job()
		return
	var referenced: PackedStringArray = await _find_referenced_probe_data_paths_async()
	var scan_cancelled: bool = _job_progress != null and _job_progress.cancel_requested
	var plan: Dictionary = ProbeClearPolicy.build_clear_plan(
		probe_files, referenced, scan_cancelled
	)
	if plan.get("aborted", false):
		# Partial scan must not offer deletes: unscanned scenes still reference live bake data.
		_end_export_job()
		return
	var to_delete: PackedStringArray = plan.get("to_delete", PackedStringArray())
	if to_delete.is_empty():
		ResonanceEditorDialogs.show_info(tr(UIStrings.INFO_ALL_PROBE_DATA_REFERENCED))
		_end_export_job()
		return
	var msg: String = (
		tr(UIStrings.INFO_DELETE_UNREFERENCED_PROBE_DATA) % [to_delete.size(), "\n".join(to_delete)]
	)
	_end_export_job()
	ResonanceEditorDialogs.show_confirm_dialog(
		editor_interface,
		tr(UIStrings.DIALOG_CLEAR_UNREFERENCED_TITLE),
		msg,
		func() -> void:
			var deleted: int = 0
			for p in to_delete:
				var abs_p: String = ResonanceFsPaths.filesystem_path_for_dir_access(p)
				var err: int = DirAccess.remove_absolute(abs_p)
				if err == OK:
					deleted += 1
			if deleted > 0:
				EditorInterface.get_resource_filesystem().scan()
				ResonanceEditorDialogs.show_success_toast(
					editor_interface, tr(UIStrings.INFO_UNREFERENCED_PROBE_DATA_CLEARED) % deleted
				)
	)


func _collect_live_edited_probe_data_paths() -> PackedStringArray:
	# Bake saves probe .res immediately but only marks the scene unsaved. Disk .tscn
	# scans miss that reference until Save — protect live edited-tree probe_data paths.
	if editor_interface == null:
		return PackedStringArray()
	var edited_root: Node = EditorInterface.get_edited_scene_root()
	if edited_root == null:
		return PackedStringArray()
	return ProbeRefIndex.collect_live_probe_data_paths(edited_root)


func _find_referenced_probe_data_paths_async() -> PackedStringArray:
	var probe_files: PackedStringArray = list_probe_data_files()
	var referenced: PackedStringArray = []
	var tscn_files: PackedStringArray = collect_project_tscn_paths()
	if _job_progress:
		_job_progress.show_job(tr(UIStrings.DIALOG_EXPORT_JOB_TITLE), tscn_files.size())
	var scene_contents: Dictionary = {}
	var tree: SceneTree = _get_editor_tree()
	for i in tscn_files.size():
		if _job_progress and _job_progress.cancel_requested:
			break
		var scene_path: String = tscn_files[i]
		if _job_progress:
			_job_progress.set_step(i + 1, tscn_files.size(), scene_path)
		var content: String = ResonanceFsPaths.read_file_as_string(scene_path)
		if not content.is_empty():
			scene_contents[scene_path] = content
		if tree:
			await tree.process_frame
	for probe_path in probe_files:
		for content in scene_contents.values():
			if ResonanceFsPaths.scene_text_references_probe_path(content, probe_path):
				if probe_path not in referenced:
					referenced.append(probe_path)
				break
	return ProbeRefIndex.merge_referenced_paths(referenced, _collect_live_edited_probe_data_paths())
