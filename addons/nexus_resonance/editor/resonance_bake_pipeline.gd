extends RefCounted
class_name ResonanceBakePipeline

## Main-thread and threaded bake steps for [ResonanceProbeVolume]. Owned by [ResonanceBakeRunner].

const ResonancePaths = preload("res://addons/nexus_resonance/scripts/resonance_paths.gd")
const ResonanceFsPaths = preload("res://addons/nexus_resonance/scripts/resonance_fs_paths.gd")
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const _VolumeCtx = preload("res://addons/nexus_resonance/editor/resonance_bake_volume_context.gd")
const _BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const _BakeEstimates = preload("res://addons/nexus_resonance/editor/resonance_bake_estimates.gd")
const _BakeHashes = preload("res://addons/nexus_resonance/editor/resonance_bake_hashes.gd")
const _BakeValidation = preload("res://addons/nexus_resonance/editor/resonance_bake_validation.gd")

const BAKE_INITIAL_DELAY_SEC := 1.5
const BAKE_VOLUME_DELAY_SEC := 0.5
const DEFAULT_BAKE_INFLUENCE_RADIUS := 10000.0
const BAKE_THREAD_JOIN_TIMEOUT_MS := 5000

var _runner: Object
var _active_bake_thread: Thread = null


func _init(runner: Object) -> void:
	_runner = runner


func shutdown() -> void:
	cancel_active_bake_thread_and_join(BAKE_THREAD_JOIN_TIMEOUT_MS)
	_runner = null


func cancel_active_bake_thread_and_join(max_wait_ms: int = BAKE_THREAD_JOIN_TIMEOUT_MS) -> void:
	if _active_bake_thread == null:
		return
	if not _active_bake_thread.is_alive():
		_active_bake_thread = null
		return
	var deadline_ms: int = Time.get_ticks_msec() + max_wait_ms
	while _active_bake_thread.is_alive() and Time.get_ticks_msec() < deadline_ms:
		OS.delay_msec(10)
	if _active_bake_thread.is_alive():
		push_warning(
			(
				"Nexus Resonance: Bake thread did not finish within %d ms during shutdown."
				% max_wait_ms
			)
		)
	else:
		_active_bake_thread.wait_to_finish()
	_active_bake_thread = null


func run_bake_pipeline_main_thread(volumes: Array[Node]) -> void:
	var progress_ui = _runner.get("_progress_ui") if _runner else null

	if progress_ui:
		progress_ui.clear_details()
		progress_ui.set_stage(0, volumes.size())

	_update_status(tr(UIStrings.PROGRESS_PREPARING))

	var root: Node = null
	if _runner and _runner.has_method("_get_edited_scene_root"):
		root = _runner._get_edited_scene_root(volumes)
	elif _runner and "target_root" in _runner:
		root = _runner.target_root

	var tree = _get_active_tree(volumes, root)

	if not await _wait_before_bake(tree):
		# Must finish so _bake_in_progress clears and pre-bake backups are discarded.
		_finish_pipeline_safe(false, null, volumes)
		return
	if _runner_unavailable():
		return
	if not ResonanceServerAccess.has_server():
		_runner._log_and_show_error(
			"GDExtension unloaded", "ResonanceServer is no longer available. Bake aborted."
		)
		_finish_pipeline_safe(false, null, volumes)
		return
	var srv = ResonanceServerAccess.get_server()
	if not srv:
		_finish_pipeline_safe(false, null, volumes)
		return

	if not root:
		_runner._log_and_show_error(
			"No scene root", "Open a scene or assign a target_root before baking."
		)
		_finish_pipeline_safe(false, null, volumes)
		return
	var static_scene_node = _BakeDiscovery.find_resonance_static_scene_for_bake(volumes, root)
	var static_asset = static_scene_node.get("static_scene_asset") if static_scene_node else null
	var baked_probe_datas: Array = []

	var vol_index := 0
	for vol in volumes:
		if _runner_unavailable():
			return
		if _is_canceled():
			_finish_pipeline_safe(false, null, volumes)
			return
		vol_index += 1
		var is_headless = (_runner.get("editor_interface") == null) if _runner else false
		var ctx = _VolumeCtx.build(
			vol,
			root,
			vol_index,
			volumes.size(),
			static_asset,
			Callable(_runner, "_get_bake_config_for_volume"),
			DEFAULT_BAKE_INFLUENCE_RADIUS,
			is_headless
		)
		var bc = _runner._get_bake_config_for_volume(vol)
		if progress_ui:
			progress_ui.set_stage(
				vol_index,
				volumes.size(),
				_BakeEstimates.estimate_bake_time(vol, bc) if vol_index == 1 else ""
			)

		_update_status(tr(UIStrings.PROGRESS_PROCESSING) + ctx.vol_info)

		if tree:
			await tree.process_frame
			await tree.create_timer(BAKE_VOLUME_DELAY_SEC).timeout
		if _runner_unavailable():
			return
		srv.set_bake_params(ctx.bc.get_bake_params())
		var ok = await _run_bake_for_volume(ctx)
		if _runner_unavailable():
			return
		if not ok:
			# Specific step errors are logged inside _run_bake_for_volume / step helpers.
			if not _runner.get("_bake_error_dialog_shown"):
				(
					_runner
					. _log_and_show_error(
						"Bake failed for %s" % ctx.vol.name,
						"Bake failed; previous probe data was kept. Check geometry and probe volume settings.",
						"A bake step returned false",
						ctx.vol.name,
						"pipeline"
					)
				)
			_finish_pipeline_safe(false, null, volumes)
			return

		if _is_canceled():
			_finish_pipeline_safe(false, null, volumes)
			return
		baked_probe_datas.append(vol.get_probe_data())
	_finish_pipeline_safe(true, baked_probe_datas, volumes)


func _wait_before_bake(tree: SceneTree) -> bool:
	if tree:
		await tree.process_frame
		await tree.create_timer(BAKE_INITIAL_DELAY_SEC).timeout
	return not _is_canceled()


func _run_in_thread_with_cancel_poll(bake_callable: Callable) -> Variant:
	# GDScript lambdas capture locals by value - do not assign into an outer Variant.
	# Thread.wait_to_finish() returns the Callable's return value.
	var thread := Thread.new()
	_active_bake_thread = thread
	thread.start(bake_callable)

	var tree = _get_active_tree()
	var srv = ResonanceServerAccess.get_server()
	var progress_ui = _runner.get("_progress_ui") if _runner else null
	while thread.is_alive():
		if progress_ui and progress_ui.has_method("poll_server_progress"):
			progress_ui.poll_server_progress()
		if tree:
			await tree.process_frame
		else:
			# No SceneTree: yield so this cancel loop cannot spin at 100% CPU.
			OS.delay_msec(10)

		if _is_canceled() and srv:
			srv.cancel_reflections_bake()
			srv.cancel_pathing_bake()
	var result: Variant = thread.wait_to_finish()
	if progress_ui and progress_ui.has_method("poll_server_progress"):
		progress_ui.poll_server_progress()
	if _active_bake_thread == thread:
		_active_bake_thread = null
	return result


func _prepare_probe_data_for_bake(vol: Node, probe_data: Resource, root: Node) -> void:
	if not probe_data or not vol or not root:
		return
	var scene_name := "unsaved"
	var scene_path = root.get_scene_file_path()
	if not scene_path.is_empty():
		scene_name = scene_path.get_file().get_basename()
	var node_key: String
	if vol.is_inside_tree():
		var rel_str: String = str(root.get_path_to(vol))
		if rel_str.begins_with("."):
			rel_str = rel_str.substr(1)
		node_key = rel_str.replace("/", "_").replace("@", "_").replace("\\", "_").replace(":", "_")
		node_key = node_key.to_lower().replace(" ", "_")
	else:
		node_key = str(vol.name).to_lower().replace(" ", "_")
	if node_key.is_empty():
		node_key = str(vol.name).to_lower().replace(" ", "_")
	if node_key.length() > 128:
		node_key = (
			"h_%s"
			% str(abs(hash(str(root.get_path_to(vol)) if vol.is_inside_tree() else vol.name)))
		)
	var batches_dir: String = ResonancePaths.get_batches_dir()
	var fs_batches: String = ResonanceFsPaths.filesystem_path_for_dir_access(batches_dir)
	var path: String = ResonancePaths.probe_data_save_path(scene_name, node_key)
	if not DirAccess.dir_exists_absolute(fs_batches):
		var mkdir_err: int = DirAccess.make_dir_recursive_absolute(fs_batches)
		if mkdir_err != OK or not DirAccess.dir_exists_absolute(fs_batches):
			if Engine.has_singleton("ResonanceLogger"):
				Engine.get_singleton("ResonanceLogger").log(
					&"bake",
					"Failed to create batches output directory: %s" % mkdir_err,
					{"step": "prepare", "error": mkdir_err}
				)
			return
	if probe_data.has_method("take_over_path"):
		probe_data.take_over_path(path)
	probe_data.emit_changed()


func _skip_if_up_to_date(ctx: Variant) -> bool:
	return (
		not ctx.need_reflections
		and not ctx.need_pathing
		and not ctx.need_static_source
		and not ctx.need_static_listener
	)


func _bake_reflections(ctx: Variant) -> bool:
	var srv = ResonanceServerAccess.get_server()
	_update_status(tr(UIStrings.PROGRESS_BAKING_REVERB) + ctx.vol_info)
	_prepare_probe_data_for_bake(ctx.vol, ctx.probe_data, ctx.root)
	var volume_transform = ctx.vol.global_transform
	var extents = ctx.vol.get("region_size") * 0.5
	var spacing = ctx.vol.get("spacing")
	var gen_type = ctx.vol.get("generation_type")
	var height = ctx.vol.get("height_above_floor")
	var exclusion_boxes: Array = []
	if ctx.vol.has_method("collect_exclusion_boxes"):
		exclusion_boxes = ctx.vol.collect_exclusion_boxes()
	var do_bake = func() -> bool:
		return srv.bake_probes_for_volume(
			volume_transform, extents, spacing, gen_type, height, ctx.probe_data, exclusion_boxes
		)
	var bake_ok: Variant = await _run_in_thread_with_cancel_poll(do_bake)

	if _is_canceled():
		return false
	if bake_ok != true:
		_runner._log_and_show_error(
			"Reflections bake failed for %s" % ctx.vol.name,
			"Bake failed; previous probe data was kept. Check geometry and probe volume settings.",
			"bake_probes_for_volume returned false",
			ctx.vol.name,
			"reflections"
		)
		return false
	ctx.probe_data = ctx.vol.get_probe_data()
	if not ctx.probe_data or ctx.probe_data.get_data().is_empty():
		_runner._log_and_show_error(
			"Reflections bake returned empty data for %s" % ctx.vol.name,
			"Bake failed; previous probe data was kept. Check geometry and probe volume settings.",
			"probe_data.data empty after bake_probes_for_volume",
			ctx.vol.name,
			"reflections"
		)
		return false
	if (
		ctx.vol.has_method("get_bake_params_hash")
		and ctx.probe_data.has_method("set_bake_params_hash")
	):
		ctx.probe_data.set_bake_params_hash(ctx.vol.get_bake_params_hash())
	if ctx.probe_data.has_method("set_pathing_params_hash"):
		ctx.probe_data.set_pathing_params_hash(0)
	if ctx.probe_data.has_method("set_static_source_params_hash"):
		ctx.probe_data.set_static_source_params_hash(0)
	if ctx.probe_data.has_method("set_static_listener_params_hash"):
		ctx.probe_data.set_static_listener_params_hash(0)
	if ctx.bc.pathing_enabled:
		ctx.need_pathing = true
	_rebake_sets_static_pass_needed(ctx)
	if ctx.probe_data.has_method("set_static_scene_params_hash"):
		var uhash: int = _BakeHashes.compute_all_resonance_static_scenes_params_hash(ctx.root)
		if uhash != 0:
			ctx.probe_data.set_static_scene_params_hash(uhash)
	return true


func _run_bake_step(
	ctx: Variant, status_key: String, bake_callable: Callable, hash_setter: String, hash_value: int
) -> bool:
	_update_status(status_key + ctx.vol_info)
	var ok: Variant = await _run_in_thread_with_cancel_poll(bake_callable)
	if ok == true and ctx.probe_data.has_method(hash_setter):
		ctx.probe_data.call(hash_setter, hash_value)
	ctx.probe_data = ctx.vol.get_probe_data()
	return ok == true


func _bake_pathing(ctx: Variant) -> bool:
	if ctx.probe_data.get_data().is_empty():
		return false
	var srv = ResonanceServerAccess.get_server()
	var do_pathing = func() -> bool: return srv.bake_pathing(ctx.probe_data)
	var ok := await _run_bake_step(
		ctx,
		tr(UIStrings.PROGRESS_BAKING_PATHING),
		do_pathing,
		"set_pathing_params_hash",
		_BakeHashes.compute_pathing_hash(ctx.bc)
	)
	if not ok and Engine.has_singleton("ResonanceLogger"):
		Engine.get_singleton("ResonanceLogger").log(
			&"bake",
			"Pathing bake failed for %s; kept previous probe data." % ctx.vol.name,
			{"volume": ctx.vol.name, "step": "pathing"}
		)
	if not ok:
		_runner._log_and_show_error(
			"Pathing bake failed for %s" % ctx.vol.name,
			"Bake failed; previous probe data was kept. Check pathing params and geometry.",
			"bake_pathing returned false",
			ctx.vol.name,
			"pathing"
		)
	return ok


## Reflections rebake clears static hashes; force static passes when enabled (partial-rebake fix).
static func _rebake_sets_static_pass_needed(ctx: Variant) -> void:
	if ctx.add_flags.get("static_source", false):
		ctx.need_static_source = true
	if ctx.add_flags.get("static_listener", false):
		ctx.need_static_listener = true


func _bake_static_source(ctx: Variant) -> bool:
	var srv = ResonanceServerAccess.get_server()
	var entries: Array = ctx.static_source_entries
	var err := _BakeValidation.static_source_entries_error(ctx.add_flags.static_source, entries)
	if not err.is_empty():
		push_error(err)
		if Engine.has_singleton("ResonanceLogger"):
			Engine.get_singleton("ResonanceLogger").log(
				&"bake", err, {"volume": ctx.vol.name, "step": "static_source"}
			)
		return false

	var total: int = entries.size()
	var all_ok: bool = true
	for i in total:
		var e = entries[i]
		var pos: Vector3 = e.pos
		var radius: float = e.radius

		_update_status(_multi_pass_status(UIStrings.PROGRESS_BAKING_STATIC_SOURCE, ctx, i, total))
		var do_static_source = func() -> bool:
			return srv.bake_static_source(ctx.probe_data, pos, radius)
		var ok = await _run_in_thread_with_cancel_poll(do_static_source)
		if ok != true:
			all_ok = false
		ctx.probe_data = ctx.vol.get_probe_data()
		if _is_canceled():
			return false
	if all_ok and ctx.probe_data and ctx.probe_data.has_method("set_static_source_params_hash"):
		var hash_value: int = _static_source_hash(ctx)
		ctx.probe_data.set_static_source_params_hash(hash_value)
	if not all_ok and Engine.has_singleton("ResonanceLogger"):
		Engine.get_singleton("ResonanceLogger").log(
			&"bake",
			"Static source bake failed for %s; kept previous probe data." % ctx.vol.name,
			{"volume": ctx.vol.name, "step": "static_source"}
		)
	return all_ok


func _bake_static_listener(ctx: Variant) -> bool:
	var srv = ResonanceServerAccess.get_server()
	var entries: Array = ctx.static_listener_entries
	var err := _BakeValidation.static_listener_entries_error(ctx.add_flags.static_listener, entries)
	if not err.is_empty():
		push_error(err)
		if Engine.has_singleton("ResonanceLogger"):
			Engine.get_singleton("ResonanceLogger").log(
				&"bake", err, {"volume": ctx.vol.name, "step": "static_listener"}
			)
		return false

	var total: int = entries.size()
	var all_ok: bool = true
	for i in total:
		var e = entries[i]
		var pos: Vector3 = e.pos
		var radius: float = e.radius

		_update_status(_multi_pass_status(UIStrings.PROGRESS_BAKING_STATIC_LISTENER, ctx, i, total))
		var do_static_listener = func() -> bool:
			return srv.bake_static_listener(ctx.probe_data, pos, radius)
		var ok = await _run_in_thread_with_cancel_poll(do_static_listener)
		if ok != true:
			all_ok = false
		ctx.probe_data = ctx.vol.get_probe_data()
		if _is_canceled():
			return false
	if all_ok and ctx.probe_data and ctx.probe_data.has_method("set_static_listener_params_hash"):
		var hash_value: int = _static_listener_hash(ctx)
		ctx.probe_data.set_static_listener_params_hash(hash_value)
	if not all_ok and Engine.has_singleton("ResonanceLogger"):
		Engine.get_singleton("ResonanceLogger").log(
			&"bake",
			"Static listener bake failed for %s; kept previous probe data." % ctx.vol.name,
			{"volume": ctx.vol.name, "step": "static_listener"}
		)
	return all_ok


func _run_bake_for_volume(ctx: Variant) -> bool:
	var srv = ResonanceServerAccess.get_server()
	srv.set_bake_params(ctx.bc.get_bake_params())
	srv.set_bake_pipeline_pathing(ctx.need_pathing)
	if _skip_if_up_to_date(ctx):
		_update_status(tr(UIStrings.PROGRESS_SKIPPING) + ctx.vol_info)
		return true
	if not await _bake_reflections(ctx):
		return false
	if ctx.need_pathing and ctx.bc.pathing_enabled:
		if not await _bake_pathing(ctx):
			return false
	if ctx.need_static_source and ctx.add_flags.static_source:
		if not await _bake_static_source(ctx):
			return false
	if ctx.need_static_listener and ctx.add_flags.static_listener:
		if not await _bake_static_listener(ctx):
			return false
	return true


func _update_status(msg: String) -> void:
	var pui = _runner.get("_progress_ui") if _runner else null
	if pui:
		pui.set_bake_status(msg)
	elif _runner and _runner.has_signal("bake_progress_updated"):
		_runner.emit_signal("bake_progress_updated", msg)


func _runner_unavailable() -> bool:
	if _runner == null:
		return true
	if _runner.has_method("is_shutdown") and _runner.is_shutdown():
		return true
	return false


func _finish_pipeline_safe(success: bool, probe_data_ref, volumes: Array[Node]) -> void:
	if _runner_unavailable():
		return
	_runner._finish_pipeline(success, probe_data_ref, volumes)


func _is_canceled() -> bool:
	if _runner_unavailable():
		return true
	if _runner.has_method("is_bake_cancel_requested"):
		if _runner.is_bake_cancel_requested():
			return true
	var pui = _runner.get("_progress_ui") if _runner else null
	return pui.cancel_requested if pui else false


func _multi_pass_status(ui_key: String, ctx: Variant, index: int, total: int) -> String:
	var s: String = tr(ui_key) + str(ctx.vol_info)
	if total <= 1:
		return s
	return s + (" [%d/%d]" % [index + 1, total])


func _static_source_hash(ctx: Variant) -> int:
	if ctx.static_source_entries.size() > 1:
		return _BakeHashes.compute_position_radius_list_hash(ctx.static_source_entries)
	return _BakeHashes.compute_position_radius_hash(ctx.player_pos, ctx.player_radius)


func _static_listener_hash(ctx: Variant) -> int:
	if ctx.static_listener_entries.size() > 1:
		return _BakeHashes.compute_position_radius_list_hash(ctx.static_listener_entries)
	return _BakeHashes.compute_position_radius_hash(ctx.listener_pos, ctx.listener_radius)


func _get_active_tree(volumes: Array[Node] = [], fallback_root: Node = null) -> SceneTree:
	var ei = _runner.get("editor_interface") if _runner else null
	if ei and ei.has_method("get_base_control"):
		var base = ei.get_base_control()
		if base:
			return base.get_tree()

	# Try to grab the tree from the live scene arguments.
	if fallback_root and fallback_root.is_inside_tree():
		return fallback_root.get_tree()
	if volumes.size() > 0 and volumes[0].is_inside_tree():
		return volumes[0].get_tree()

	# In a running game, the main loop will usually be the SceneTree.
	var main_loop = Engine.get_main_loop()
	if main_loop is SceneTree:
		return main_loop

	return null
