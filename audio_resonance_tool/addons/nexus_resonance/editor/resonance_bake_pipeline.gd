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

const BAKE_INITIAL_DELAY_SEC := 1.5
const BAKE_VOLUME_DELAY_SEC := 0.5
const DEFAULT_BAKE_INFLUENCE_RADIUS := 10000.0

var _runner: Object


func _init(runner: Object) -> void:
	_runner = runner


func shutdown() -> void:
	# Break RefCounted reference cycles (runner <-> pipeline).
	_runner = null


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
		return
	if not ResonanceServerAccess.has_server():
		_runner._log_and_show_error(
			"GDExtension unloaded", "ResonanceServer is no longer available. Bake aborted."
		)
		_runner._finish_pipeline(false, null, volumes)
		return
	var srv = ResonanceServerAccess.get_server()
	if not srv:
		_runner._finish_pipeline(false, null, volumes)
		return

	if not root:
		_runner._log_and_show_error(
			"No scene root", "Open a scene or assign a target_root before baking."
		)
		_runner._finish_pipeline(false, null, volumes)
		return
	var static_scene_node = _BakeDiscovery.find_resonance_static_scene_for_bake(volumes, root)
	var static_asset = static_scene_node.get("static_scene_asset") if static_scene_node else null
	var baked_probe_datas: Array = []

	var vol_index := 0
	for vol in volumes:
		if _is_canceled():
			_runner.call_deferred("_on_bake_pipeline_finished", false, null, volumes)
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
		srv.set_bake_params(ctx.bc.get_bake_params())
		var ok = await _run_bake_for_volume(ctx)
		if not ok:
			_runner._log_and_show_error(
				"Reflections bake failed for %s" % ctx.vol.name,
				"Check geometry and probe volume settings.",
				"bake_probes_for_volume returned false",
				ctx.vol.name,
				"reflections"
			)
			_runner._finish_pipeline(false, null, volumes)
			return

		if _is_canceled():
			_runner._finish_pipeline(false, null, volumes)
			return
		baked_probe_datas.append(vol.get_probe_data())
	_runner._finish_pipeline(true, baked_probe_datas, volumes)


func _wait_before_bake(tree: SceneTree) -> bool:
	if tree:
		await tree.process_frame
		await tree.create_timer(BAKE_INITIAL_DELAY_SEC).timeout
	return not _is_canceled()


func _run_in_thread_with_cancel_poll(bake_callable: Callable) -> Variant:
	var result: Variant = null
	var thread = Thread.new()
	thread.start(func() -> void: result = bake_callable.call())

	var tree = _get_active_tree()
	var srv = ResonanceServerAccess.get_server()
	while thread.is_alive():
		if tree:
			await tree.process_frame
		else:
			# No SceneTree: yield so this cancel loop cannot spin at 100% CPU.
			OS.delay_msec(10)

		if _is_canceled() and srv:
			srv.cancel_reflections_bake()
			srv.cancel_pathing_bake()
	thread.wait_to_finish()
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
	var audio_dir: String = ResonancePaths.get_audio_data_dir()
	var fs_audio: String = ResonanceFsPaths.filesystem_path_for_dir_access(audio_dir)
	var path: String = ResonancePaths.probe_data_save_path(scene_name, node_key)
	if not DirAccess.dir_exists_absolute(fs_audio):
		var mkdir_err: int = DirAccess.make_dir_recursive_absolute(fs_audio)
		if mkdir_err != OK or not DirAccess.dir_exists_absolute(fs_audio):
			if Engine.has_singleton("ResonanceLogger"):
				Engine.get_singleton("ResonanceLogger").log(
					&"bake",
					"Failed to create audio output directory: %s" % mkdir_err,
					{"step": "prepare", "error": mkdir_err}
				)
			return
	if probe_data.has_method("take_over_path"):
		probe_data.take_over_path(path)
	probe_data.emit_changed()
	if vol.has_method("get_bake_params_hash") and probe_data.has_method("set_bake_params_hash"):
		probe_data.set_bake_params_hash(vol.get_bake_params_hash())


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
	var do_bake = func() -> bool:
		return srv.bake_probes_for_volume(
			volume_transform, extents, spacing, gen_type, height, ctx.probe_data
		)
	await _run_in_thread_with_cancel_poll(do_bake)

	if _is_canceled():
		return false
	ctx.probe_data = ctx.vol.get_probe_data()
	if not ctx.probe_data or ctx.probe_data.get_data().is_empty():
		if Engine.has_singleton("ResonanceLogger"):
			Engine.get_singleton("ResonanceLogger").log(
				&"bake",
				"Reflections bake returned empty data for %s" % ctx.vol.name,
				{"volume": ctx.vol.name, "step": "reflections"}
			)
		return false
	if ctx.probe_data.has_method("set_pathing_params_hash"):
		ctx.probe_data.set_pathing_params_hash(0)
	if ctx.probe_data.has_method("set_static_source_params_hash"):
		ctx.probe_data.set_static_source_params_hash(0)
	if ctx.probe_data.has_method("set_static_listener_params_hash"):
		ctx.probe_data.set_static_listener_params_hash(0)
	if ctx.bc.pathing_enabled:
		ctx.need_pathing = true
	if ctx.probe_data.has_method("set_static_scene_params_hash"):
		var uhash: int = _BakeHashes.compute_all_resonance_static_scenes_params_hash(ctx.root)
		if uhash != 0:
			ctx.probe_data.set_static_scene_params_hash(uhash)
	return true


func _run_bake_step(
	ctx: Variant, status_key: String, bake_callable: Callable, hash_setter: String, hash_value: int
) -> void:
	_update_status(status_key + ctx.vol_info)
	var ok = await _run_in_thread_with_cancel_poll(bake_callable)
	if ok and ctx.probe_data.has_method(hash_setter):
		ctx.probe_data.call(hash_setter, hash_value)
	ctx.probe_data = ctx.vol.get_probe_data()


func _bake_pathing(ctx: Variant) -> void:
	if ctx.probe_data.get_data().is_empty():
		return
	var srv = ResonanceServerAccess.get_server()
	var do_pathing = func() -> bool: return srv.bake_pathing(ctx.probe_data)
	await _run_bake_step(
		ctx,
		tr(UIStrings.PROGRESS_BAKING_PATHING),
		do_pathing,
		"set_pathing_params_hash",
		_BakeHashes.compute_pathing_hash(ctx.bc)
	)


func _bake_static_source(ctx: Variant) -> void:
	var srv = ResonanceServerAccess.get_server()
	# One STATICSOURCE pass per bake_sources entry (separate IR layer per outdoor emitter).
	var entries: Array = ctx.static_source_entries
	if entries.is_empty():
		entries = [{"pos": ctx.player_pos, "radius": ctx.player_radius}]

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
		if not ok:
			all_ok = false
		ctx.probe_data = ctx.vol.get_probe_data()
		if _is_canceled():
			return
	if all_ok and ctx.probe_data and ctx.probe_data.has_method("set_static_source_params_hash"):
		var hash_value: int = _static_source_hash(ctx)
		ctx.probe_data.set_static_source_params_hash(hash_value)


func _bake_static_listener(ctx: Variant) -> void:
	var srv = ResonanceServerAccess.get_server()
	var entries: Array = ctx.static_listener_entries
	if entries.is_empty():
		entries = [{"pos": ctx.listener_pos, "radius": ctx.listener_radius}]

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
		if not ok:
			all_ok = false
		ctx.probe_data = ctx.vol.get_probe_data()
		if _is_canceled():
			return
	if all_ok and ctx.probe_data and ctx.probe_data.has_method("set_static_listener_params_hash"):
		var hash_value: int = _static_listener_hash(ctx)
		ctx.probe_data.set_static_listener_params_hash(hash_value)


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
		await _bake_pathing(ctx)
	if ctx.need_static_source and ctx.add_flags.static_source:
		await _bake_static_source(ctx)
	if ctx.need_static_listener and ctx.add_flags.static_listener:
		await _bake_static_listener(ctx)
	return true


func _update_status(msg: String) -> void:
	var pui = _runner.get("_progress_ui") if _runner else null
	if pui:
		pui.set_bake_status(msg)
	elif _runner and _runner.has_signal("bake_progress_updated"):
		_runner.emit_signal("bake_progress_updated", msg)


func _is_canceled() -> bool:
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
