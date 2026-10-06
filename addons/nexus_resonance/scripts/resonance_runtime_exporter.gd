extends RefCounted
class_name ResonanceRuntimeExporter

## Runtime static acoustic export. Merges visible static geometry, assigns [ResonanceStaticScene], and reloads Phonon.

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")
const ResonanceRuntimeBaker = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_baker.gd"
)
const ResonanceServerAccess = preload(
	"res://addons/nexus_resonance/scripts/resonance_server_access.gd"
)

## Keeps the last helper-started [ResonanceRuntimeBaker] alive until [signal ResonanceRuntimeBaker.bake_finished].
static var _pending_probe_baker: ResonanceRuntimeBaker = null
## Holds a short-lived runner until deferred reload + fire-and-forget probe bake start.
static var _pending_deferred_bake_runner: RefCounted = null


## Merges visible static geometry under [param scene_root] into an in-memory [ResonanceGeometryAsset] (no RSS assign / reload).
static func export_asset(scene_root: Node) -> ResonanceGeometryAsset:
	if not scene_root:
		return null
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null or not srv.has_method("export_static_scene_to_geometry_asset"):
		push_error("Nexus Resonance: ResonanceServer.export_static_scene_to_geometry_asset missing.")
		return null
	var asset: Variant = srv.export_static_scene_to_geometry_asset(scene_root)
	if asset == null:
		return null
	return asset as ResonanceGeometryAsset


## Full replace of runtime static packs (not keyed by [ResonanceStaticScene] nodes). See [method ResonanceServer.replace_static_scenes_from_assets].
static func replace_static(assets: Array, transforms: Array = []) -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("replace_static_scenes_from_assets"):
		push_error("Nexus Resonance: ResonanceServer.replace_static_scenes_from_assets missing.")
		return
	srv.replace_static_scenes_from_assets(assets, transforms)


## Debounced reload of every [ResonanceStaticScene] pack via [method ResonanceRuntime.request_static_scene_reload] (primary [ResonanceRuntime]).
static func reload(scene_root: Node = null) -> void:
	if scene_root:
		var rt := _find_resonance_runtime(scene_root)
		if rt and rt.has_method("request_static_scene_reload"):
			rt.request_static_scene_reload()
		return
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return
	var runtimes := tree.get_nodes_in_group("resonance_runtime")
	for rt in runtimes:
		if rt.has_method("is_primary_runtime") and rt.is_primary_runtime():
			rt.request_static_scene_reload()
			return
	if not runtimes.is_empty() and runtimes[0].has_method("request_static_scene_reload"):
		runtimes[0].request_static_scene_reload()


## Export live static geometry, assign owned [ResonanceStaticScene], reload Phonon. Returns immediately.
## [param opts]: [code]static_scene_node[/code], [code]reload[/code] (default true), [code]bake_probes[/code],
## [code]probe_volumes[/code], [code]baker[/code] ([ResonanceRuntimeBaker]), [code]optional_save_path[/code].
## With [code]bake_probes[/code], RAM bake starts after deferred static reload; use [method export_static_async] to await.
static func export_static(scene_root: Node, opts: Dictionary = {}) -> Error:
	var plan := _plan_export_static(scene_root, opts)
	if plan.error != OK:
		return plan.error
	if plan.bake_volumes.is_empty():
		return OK
	if plan.deferred_static_reload:
		_schedule_probe_bake_after_deferred_reload(scene_root, plan.bake_volumes, plan.caller_baker)
	else:
		_start_probe_bake(scene_root, plan.bake_volumes, plan.caller_baker)
	return OK


## Same as [method export_static], but awaits deferred static reload and optional probe RAM bake. Call with [code]await[/code].
static func export_static_async(scene_root: Node, opts: Dictionary = {}) -> Error:
	var plan := _plan_export_static(scene_root, opts)
	if plan.error != OK:
		return plan.error
	if plan.deferred_static_reload:
		var tree := scene_root.get_tree()
		if tree:
			await tree.process_frame
	if plan.bake_volumes.is_empty():
		return OK
	var baker := _start_probe_bake(scene_root, plan.bake_volumes, plan.caller_baker)
	if baker:
		await baker.bake_finished
	return OK


class _DeferredProbeBakeRunner:
	extends RefCounted

	var _scene_root: Node
	var _volumes: Array
	var _caller_baker: Variant


	func start(scene_root: Node, volumes: Array, caller_baker: Variant) -> void:
		_scene_root = scene_root
		_volumes = volumes
		_caller_baker = caller_baker
		var tree := scene_root.get_tree() if scene_root else null
		if tree == null:
			ResonanceRuntimeExporter._start_probe_bake(_scene_root, _volumes, _caller_baker)
			if ResonanceRuntimeExporter._pending_deferred_bake_runner == self:
				ResonanceRuntimeExporter._pending_deferred_bake_runner = null
			return
		tree.process_frame.connect(_on_process_frame, CONNECT_ONE_SHOT)


	func _on_process_frame() -> void:
		ResonanceRuntimeExporter._start_probe_bake(_scene_root, _volumes, _caller_baker)
		if ResonanceRuntimeExporter._pending_deferred_bake_runner == self:
			ResonanceRuntimeExporter._pending_deferred_bake_runner = null


class _ExportStaticPlan:
	var error: Error = OK
	var deferred_static_reload: bool = false
	var bake_volumes: Array = []
	var caller_baker: Variant = null


static func _plan_export_static(scene_root: Node, opts: Dictionary) -> _ExportStaticPlan:
	var plan := _ExportStaticPlan.new()
	if not scene_root:
		plan.error = ERR_INVALID_PARAMETER
		return plan
	if not ResonanceSceneUtils.scene_has_exportable_resonance_content(scene_root, "static"):
		push_warning("Nexus Resonance: No exportable static geometry under scene_root.")
		plan.error = ERR_UNAVAILABLE
		return plan

	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null:
		push_error("Nexus Resonance: ResonanceServer not loaded.")
		plan.error = ERR_UNAVAILABLE
		return plan

	var static_scene_node: Node = opts.get("static_scene_node", null)
	if static_scene_node == null:
		static_scene_node = ResonanceSceneUtils.find_owned_resonance_static_scene(scene_root)
	ResonanceSceneUtils.warn_static_scenes_without_asset_covering_geometry(scene_root)

	var asset := export_asset(scene_root)
	if asset == null:
		plan.error = ERR_CANT_CREATE
		return plan

	var save_path: String = opts.get("optional_save_path", "")
	if not save_path.is_empty():
		var save_err: int = ResourceSaver.save(asset, save_path)
		if save_err != OK:
			plan.error = save_err
			return plan

	if static_scene_node:
		var current_hash: int = (
			srv.get_static_scene_hash(scene_root) if srv.has_method("get_static_scene_hash") else 0
		)
		static_scene_node.static_scene_asset = asset
		if current_hash != 0:
			static_scene_node.export_hash = current_hash
	else:
		push_warning(
			"Nexus Resonance: No owned ResonanceStaticScene; assign asset manually or use replace_static."
		)

	var do_reload: bool = opts.get("reload", true)
	if do_reload:
		if static_scene_node:
			reload(scene_root)
			plan.deferred_static_reload = true
		else:
			replace_static([asset], [Transform3D.IDENTITY])

	if opts.get("bake_probes", false):
		var volumes: Array = opts.get("probe_volumes", [])
		if volumes.is_empty():
			var collected: Array[Node] = []
			ResonanceSceneUtils.collect_resonance_probe_volumes(scene_root, collected)
			volumes.assign(collected)
		if volumes.is_empty():
			push_warning("Nexus Resonance: bake_probes requested but no ResonanceProbeVolume found.")
		else:
			plan.bake_volumes = volumes
			plan.caller_baker = opts.get("baker", null)

	return plan


static func _schedule_probe_bake_after_deferred_reload(
	scene_root: Node, volumes: Array, caller_baker: Variant
) -> void:
	var runner := _DeferredProbeBakeRunner.new()
	_pending_deferred_bake_runner = runner
	runner.start(scene_root, volumes, caller_baker)


static func _start_probe_bake(
	scene_root: Node, volumes: Array, caller_baker: Variant
) -> ResonanceRuntimeBaker:
	if volumes.is_empty() or not scene_root:
		return null
	if caller_baker is ResonanceRuntimeBaker:
		caller_baker.bake_volumes_to_ram(volumes, scene_root)
		return caller_baker
	var baker := ResonanceRuntimeBaker.new()
	_hold_probe_baker_until_finished(baker)
	baker.bake_volumes_to_ram(volumes, scene_root)
	return baker


static func _hold_probe_baker_until_finished(baker: ResonanceRuntimeBaker) -> void:
	_release_pending_probe_baker()
	_pending_probe_baker = baker
	baker.bake_finished.connect(
		func() -> void:
			if _pending_probe_baker == baker:
				_pending_probe_baker.shutdown()
				_pending_probe_baker = null,
		CONNECT_ONE_SHOT
	)


static func _release_pending_probe_baker() -> void:
	if _pending_probe_baker:
		_pending_probe_baker.shutdown()
		_pending_probe_baker = null


static func _find_resonance_runtime(node: Node) -> Node:
	if not node:
		return null
	if node.is_class("ResonanceRuntime"):
		return node
	for c in node.get_children():
		var found := _find_resonance_runtime(c)
		if found:
			return found
	if not node.is_inside_tree():
		return null
	var tree := node.get_tree()
	if tree == null:
		return null
	var runtimes := tree.get_nodes_in_group("resonance_runtime")
	for rt in runtimes:
		if rt.has_method("is_primary_runtime") and rt.is_primary_runtime():
			return rt
	if not runtimes.is_empty():
		return runtimes[0]
	return null
