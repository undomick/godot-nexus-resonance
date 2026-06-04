@tool
extends RefCounted

## Static/dynamic export, OBJ, probe cleanup. Shared from the editor plugin.

const ResonancePaths = preload("res://addons/nexus_resonance/scripts/resonance_paths.gd")
const ResonanceFsPaths = preload("res://addons/nexus_resonance/scripts/resonance_fs_paths.gd")
const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)
const SceneIndex = preload("res://addons/nexus_resonance/editor/resonance_editor_scene_index.gd")
const JobProgressScript = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_job_progress.gd"
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
		var base: Control = editor_interface.get_base_control()
		if base:
			return base.get_tree()
	var main_loop: MainLoop = Engine.get_main_loop()
	return main_loop if main_loop is SceneTree else null


func _try_begin_export_job() -> bool:
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
	return main_path


## Ensures audio_data directory exists. Returns true on success.
func ensure_audio_data_dir() -> bool:
	var path: String = ResonancePaths.get_audio_data_dir()
	var fs_path: String = ResonanceFsPaths.filesystem_path_for_dir_access(path)
	if DirAccess.dir_exists_absolute(fs_path):
		return true
	var err: int = DirAccess.make_dir_recursive_absolute(fs_path)
	if err != OK or not DirAccess.dir_exists_absolute(fs_path):
		ResonanceEditorDialogs.show_error_dialog(
			editor_interface,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
			tr(UIStrings.ERR_MKDIR_AUDIO_DATA).replace("%d", str(err)),
			"",
			""
		)
		return false
	return true


## Ensures resonance_meshes directory exists. Returns true on success.
func ensure_resonance_meshes_dir() -> bool:
	var fs_meshes: String = ResonanceFsPaths.filesystem_path_for_dir_access(
		ResonancePaths.PATH_RESONANCE_MESHES
	)
	if DirAccess.dir_exists_absolute(fs_meshes):
		return true
	var err: int = DirAccess.make_dir_recursive_absolute(fs_meshes)
	if err != OK or not DirAccess.dir_exists_absolute(fs_meshes):
		ResonanceEditorDialogs.show_error_dialog(
			editor_interface,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
			tr(UIStrings.ERR_MKDIR_RESONANCE_MESHES).replace("%d", str(err)),
			"",
			""
		)
		return false
	return true


## update_file + reimport_files for new OBJ paths (avoids full scan + reimport race).
func _request_obj_reimport(paths: PackedStringArray) -> void:
	if paths.is_empty():
		return
	var fs: EditorFileSystem = editor_interface.get_resource_filesystem()
	if not fs:
		return
	for p in paths:
		fs.update_file(p)
	fs.reimport_files(paths)


func collect_scene_paths_for_obj(node: Node, out: Dictionary) -> void:
	if not node:
		return
	var path_str: String = node.get_scene_file_path()
	if not path_str.is_empty():
		out[path_str] = true
	for c in node.get_children():
		collect_scene_paths_for_obj(c, out)


func filter_scene_paths_by_exportable_static(paths_dict: Dictionary) -> PackedStringArray:
	var filtered: PackedStringArray = []
	for path in paths_dict:
		if SceneIndex.scene_text_has_static_resonance_content(path):
			filtered.append(path)
			continue
		var scene: PackedScene = load(path) as PackedScene
		if not scene:
			continue
		var inst: Node = scene.instantiate()
		var ok: bool = ResonanceSceneUtils.scene_has_exportable_resonance_content(inst, "static")
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
## filter_exportable_static: if true, only paths with exportable static content; if false, all paths.
func _get_scene_paths_from_build(filter_exportable_static: bool = true) -> Dictionary:
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
	if filter_exportable_static:
		var filtered_paths: PackedStringArray = filter_scene_paths_by_exportable_static(paths_dict)
		var skipped: int = paths_dict.size() - filtered_paths.size()
		return {"paths": filtered_paths, "skipped": skipped}
	return {"paths": PackedStringArray(paths_dict.keys()), "skipped": 0}


func _export_one_static_scene(path: String, srv: Variant) -> int:
	var scene: PackedScene = load(path) as PackedScene
	if not scene:
		return ERR_FILE_NOT_FOUND
	var inst: Node = scene.instantiate()
	if not ResonanceSceneUtils.scene_has_exportable_resonance_content(inst, "static"):
		inst.queue_free()
		return 1
	var base_name: String = str(path).get_file().get_basename()
	var save_path: String = ResonancePaths.static_scene_asset_save_path(base_name)
	ResonanceSceneUtils.warn_static_scenes_without_asset_covering_geometry(inst)
	var err: int = srv.export_static_scene_to_asset(inst, save_path)
	inst.queue_free()
	return err


## Exports static geometry from scene paths. Returns {exported: int, skipped: int}.
func _export_static_scenes_batch(paths: PackedStringArray) -> Dictionary:
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_asset")
	if srv == null:
		return {"exported": 0, "skipped": paths.size()}
	if not ensure_audio_data_dir():
		return {"exported": 0, "skipped": paths.size()}
	var exported: int = 0
	var skipped: int = 0
	for path in paths:
		var err: int = _export_one_static_scene(path, srv)
		if err == OK:
			exported += 1
		else:
			skipped += 1
	if exported > 0 and editor_interface:
		editor_interface.get_resource_filesystem().scan()
	return {"exported": exported, "skipped": skipped}


func _finish_static_batch_toasts(result: Dictionary) -> void:
	if result.exported > 0:
		ResonanceEditorDialogs.show_success_toast(
			editor_interface, tr(UIStrings.INFO_ALL_OPEN_SCENES_EXPORTED) % result.exported
		)
	elif result.skipped > 0:
		_show_warning(
			(
				tr(UIStrings.WARN_NO_SCENES_EXPORTED)
				+ " "
				+ (tr(UIStrings.INFO_SCENES_FILTERED) % result.skipped)
			)
		)
	else:
		_show_warning(tr(UIStrings.WARN_NO_SCENES_EXPORTED))


func _export_static_scenes_batch_async(paths: PackedStringArray) -> void:
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_asset")
	if srv == null:
		_end_export_job()
		return
	if not ensure_audio_data_dir():
		_end_export_job()
		return
	if _job_progress:
		_job_progress.show_job(tr(UIStrings.DIALOG_EXPORT_JOB_TITLE), paths.size())
	var exported: int = 0
	var skipped: int = 0
	var tree: SceneTree = _get_editor_tree()
	for i in paths.size():
		if _job_progress and _job_progress.cancel_requested:
			break
		var path: String = paths[i]
		if _job_progress:
			_job_progress.set_step(i + 1, paths.size(), path)
		var err: int = _export_one_static_scene(path, srv)
		if err == OK:
			exported += 1
		else:
			skipped += 1
		if tree:
			await tree.process_frame
	if exported > 0 and editor_interface:
		editor_interface.get_resource_filesystem().scan()
	_finish_static_batch_toasts({"exported": exported, "skipped": skipped})
	_end_export_job()


## Exports ResonanceDynamicGeometry from scene paths. make_save_path(scene_path, scene_base, geom) -> String.
## dedup: if true, skip geoms with same scene_path|parent_name. Returns {exported: int, scenes_saved: int}.
func _export_dynamic_objects_batch(
	paths: PackedStringArray, make_save_path: Callable, dedup: bool = false
) -> Dictionary:
	if not ensure_resonance_meshes_dir():
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
			var parent_name: String = geom.get_parent().name if geom.get_parent() else "mesh"
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
		editor_interface.get_resource_filesystem().scan()
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
		var root: Node = editor_interface.get_edited_scene_root()
		var label: String = root.get_scene_file_path() if root else ""
		_job_progress.show_job(tr(UIStrings.DIALOG_EXPORT_JOB_TITLE), 1)
		_job_progress.set_step(1, 1, label)
	var tree: SceneTree = _get_editor_tree()
	if tree:
		await tree.process_frame
	_export_active_scene_core()
	_end_export_job()


func _export_active_scene_core() -> void:
	var root: Node = editor_interface.get_edited_scene_root()
	if not root:
		_show_warning(tr(UIStrings.WARN_NO_SCENE))
		return
	if not ResonanceSceneUtils.scene_has_exportable_resonance_content(root, "static"):
		_show_warning(tr(UIStrings.WARN_NO_EXPORTABLE_STATIC_CONTENT))
		return
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_asset")
	if srv == null:
		return
	if not ensure_audio_data_dir():
		return
	var scene_name: String = "unsaved"
	var scene_path: String = root.get_scene_file_path()
	if not scene_path.is_empty():
		scene_name = scene_path.get_file().get_basename()
	var save_path: String = ResonancePaths.static_scene_asset_save_path(scene_name)
	var static_scene_node: Node = ResonanceSceneUtils.find_resonance_static_scene(root)
	var current_hash: int = (
		srv.get_static_scene_hash(root) if srv.has_method("get_static_scene_hash") else 0
	)
	if static_scene_node and static_scene_node.export_hash == current_hash and current_hash != 0:
		var has_valid: bool = (
			static_scene_node.has_method("has_valid_asset") and static_scene_node.has_valid_asset()
		)
		var file_exists: bool = ResonanceFsPaths.file_exists_for_path(save_path)
		if has_valid and file_exists:
			if _export_job_running:
				ResonanceEditorDialogs.show_info(tr(UIStrings.INFO_STATIC_UNCHANGED))
			return
	if static_scene_node and static_scene_node.static_scene_asset:
		static_scene_node.static_scene_asset = null
	ResonanceSceneUtils.warn_static_scenes_without_asset_covering_geometry(root)
	var err: int = srv.export_static_scene_to_asset(root, save_path)
	if err != OK:
		ResonanceEditorDialogs.show_critical(
			editor_interface,
			tr(UIStrings.ERR_EXPORT_FAILED) % err,
			tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE)
		)
		return
	if editor_interface:
		editor_interface.get_resource_filesystem().scan()
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
		if editor_interface:
			editor_interface.mark_scene_as_unsaved()
	if _export_job_running and editor_interface:
		ResonanceEditorDialogs.show_success_toast(
			editor_interface, tr(UIStrings.INFO_STATIC_EXPORTED) % save_path
		)


## Export static geometry of all currently open editor scenes to ResonanceGeometryAsset files (.tres or .res per Project Settings).
func export_all_open_scenes(_unused: Variant = null) -> void:
	var open_scenes: PackedStringArray = editor_interface.get_open_scenes()
	if open_scenes.is_empty():
		_show_warning(tr(UIStrings.WARN_NO_SCENES_OPEN))
		return
	_dispatch_export_async(_export_static_scenes_batch_async.bind(open_scenes))


## Export static ResonanceGeometry from active scene to OBJ+MTL (debug/collada workflow).
func export_scene_obj(_unused: Variant = null) -> void:
	var root: Node = editor_interface.get_edited_scene_root()
	if not root:
		_show_warning(tr(UIStrings.WARN_NO_SCENE))
		return
	if not ResonanceSceneUtils.scene_has_exportable_resonance_content(root, "static"):
		_show_warning(tr(UIStrings.WARN_NO_EXPORTABLE_STATIC_CONTENT))
		return
	var srv: Variant = get_resonance_server_or_show_error("export_static_scene_to_obj")
	if srv == null:
		return
	if not ensure_audio_data_dir():
		return
	var scene_name: String = "unsaved"
	var scene_path: String = root.get_scene_file_path()
	if not scene_path.is_empty():
		scene_name = scene_path.get_file().get_basename()
	var save_base: String = ResonancePaths.get_audio_data_dir() + scene_name + "_scene"
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
	var root: Node = editor_interface.get_edited_scene_root()
	if not root:
		_show_warning(tr(UIStrings.WARN_NO_SCENE))
		return
	var dynamic_geoms: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_dynamic_geometry(root, dynamic_geoms)
	if dynamic_geoms.is_empty():
		_show_warning(tr(UIStrings.WARN_NO_DYNAMIC_GEOMETRY))
		return
	if not ensure_resonance_meshes_dir():
		return
	var exported: int = 0
	for geom in dynamic_geoms:
		var parent_name: String = geom.get_parent().name if geom.get_parent() else "mesh"
		var save_path: String = (
			ResonancePaths.PATH_RESONANCE_MESHES + parent_name.to_snake_case() + "_dynamic.tres"
		)
		var err: int = geom.export_dynamic_mesh_to_asset(save_path)
		if err == OK:
			exported += 1
	if exported > 0:
		editor_interface.get_resource_filesystem().scan()
		var scene_path: String = root.get_scene_file_path()
		if not scene_path.is_empty():
			var save_err: int = editor_interface.save_scene()
			if save_err != OK:
				_show_warning(tr(UIStrings.WARN_EXPORTED_BUT_SAVE_FAILED) % [exported, save_err])
			else:
				ResonanceEditorDialogs.show_success_toast(
					editor_interface, tr(UIStrings.INFO_DYNAMIC_MESHES_EXPORTED) % exported
				)
		else:
			editor_interface.mark_scene_as_unsaved()
			ResonanceEditorDialogs.show_success_toast(
				editor_interface,
				(
					tr(UIStrings.INFO_DYNAMIC_MESHES_EXPORTED) % exported
					+ tr(UIStrings.WARN_SAVE_SCENE_TO_PERSIST)
				)
			)


## Export all ResonanceDynamicGeometry from all dependent scenes in the main scene tree.
func export_dynamic_objects_in_build(_unused: Variant = null) -> void:
	var build_data: Dictionary = _get_scene_paths_from_build(false)
	var paths: PackedStringArray = build_data.paths
	if paths.is_empty():
		return
	if not ensure_resonance_meshes_dir():
		return
	var make_save_path: Callable = func(_path: Variant, scene_base: String, geom: Node) -> String:
		var parent_name: String = geom.get_parent().name if geom.get_parent() else "mesh"
		return (
			ResonancePaths.PATH_RESONANCE_MESHES
			+ scene_base
			+ "_"
			+ parent_name.to_snake_case()
			+ "_dynamic.tres"
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
		var parent_name: String = geom.get_parent().name if geom.get_parent() else "mesh"
		return (
			ResonancePaths.PATH_RESONANCE_MESHES
			+ rel_dir
			+ "_"
			+ scene_base
			+ "_"
			+ parent_name.to_snake_case()
			+ "_dynamic.tres"
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
	if not ensure_resonance_meshes_dir():
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
			var parent_name: String = geom.get_parent().name if geom.get_parent() else "mesh"
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
		editor_interface.get_resource_filesystem().scan()
	return {"exported": exported, "scenes_saved": scenes_saved}


func list_probe_data_files() -> PackedStringArray:
	var out: PackedStringArray = []
	var logical_dir: String = ResonancePaths.get_audio_data_dir()
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
	return referenced


## Delete ResonanceProbeData assets in audio_data/ that are not referenced by any scene or prefab.
func clear_unreferenced_probe_data(_unused: Variant = null) -> void:
	_dispatch_export_async(Callable(self, "_clear_unreferenced_probe_data_async"))


func _clear_unreferenced_probe_data_async() -> void:
	var probe_files: PackedStringArray = list_probe_data_files()
	if probe_files.is_empty():
		ResonanceEditorDialogs.show_info(
			tr(UIStrings.INFO_NO_PROBE_DATA_FILES) % ResonancePaths.get_audio_data_dir()
		)
		_end_export_job()
		return
	var referenced: PackedStringArray = await _find_referenced_probe_data_paths_async()
	var to_delete: PackedStringArray = []
	for path in probe_files:
		if path not in referenced:
			to_delete.append(path)
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
				editor_interface.get_resource_filesystem().scan()
				ResonanceEditorDialogs.show_success_toast(
					editor_interface, tr(UIStrings.INFO_UNREFERENCED_PROBE_DATA_CLEARED) % deleted
				)
	)


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
	return referenced
