extends Object

## Scene tree discovery for [ResonanceRuntime], [ResonanceStaticScene], and bake source/listener nodes.

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")
const Constants = preload("res://addons/nexus_resonance/scripts/resonance_config_constants.gd")
const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")


static func _find_resonance_static_scene(node: Node) -> Node:
	return ResonanceSceneUtils.find_resonance_static_scene(node) if node else null


static func _get_branch_root(node: Node) -> Node:
	var n := node
	while n and n.get_parent():
		n = n.get_parent()
	return n


static func _get_scene_root_from_tree(tree: SceneTree) -> Node:
	if tree == null:
		return null
	var edited_root: Node = null
	if tree.has_method("get_edited_scene_root"):
		edited_root = tree.get_edited_scene_root()
	return edited_root if edited_root else tree.root


static func _resolve_nodepath(vol: Node, root: Node, path: NodePath) -> Node:
	if path.is_empty():
		return null
	var n: Node = null
	if vol and vol.is_inside_tree():
		n = vol.get_node_or_null(path)
	if n == null and root:
		n = root.get_node_or_null(path)
	return n


static func find_resonance_runtime(node: Node) -> Node:
	if not node:
		return null
	if node.is_class("ResonanceRuntime"):
		return node
	for c in node.get_children():
		var found = find_resonance_runtime(c)
		if found:
			return found
	return null


## Reflection type from the scene's ResonanceRuntime, else from [param bc], else Hybrid.
static func resolve_bake_reflection_type(root: Node, bc: Resource) -> int:
	var rt_node := find_resonance_runtime(root)
	if rt_node:
		var rt = rt_node.get("runtime")
		if rt != null and "reflection_type" in rt:
			return Constants.bake_reflection_type_from_runtime(int(rt.reflection_type))
	if bc != null and "reflection_type" in bc:
		return int(bc.reflection_type)
	return Constants.REFLECTION_TYPE_HYBRID


## Pathing on/off from the scene's ResonanceRuntime, else from [param bc].
static func resolve_bake_pathing_enabled(root: Node, bc: Resource) -> bool:
	var rt_node := find_resonance_runtime(root)
	if rt_node:
		var rt = rt_node.get("runtime")
		if rt != null and "pathing_enabled" in rt:
			return bool(rt.pathing_enabled)
	if bc != null and "pathing_enabled" in bc:
		return bool(bc.pathing_enabled)
	return false


## Bake ambisonic order from the scene's ResonanceRuntime, clamped to 1-3. Defaults to 1.
static func resolve_global_bake_ambisonic_order(root: Node) -> int:
	var rt_node := find_resonance_runtime(root)
	if rt_node:
		var rt = rt_node.get("runtime")
		if rt != null and "bake_ambisonic_order" in rt:
			return clampi(int(rt.bake_ambisonic_order), 1, 3)
		if rt != null and "ambisonic_order" in rt:
			# Scenes saved before bake_ambisonic_order existed.
			return clampi(int(rt.ambisonic_order), 1, 3)
	return 1


## The order set on [param bc] if any, else the runtime's bake ambisonic order.
static func resolve_bake_ambisonics_order(root: Node, bc: Resource, _vol: Node = null) -> int:
	var global_order := resolve_global_bake_ambisonic_order(root)
	var setting := 0
	if bc != null and "bake_ambisonics_order" in bc:
		setting = int(bc.bake_ambisonics_order)
	if setting <= 0:
		return global_order
	return clampi(setting, 1, 3)


## Bake params from [param bc] with reflection type, ambisonic order and pathing settings taken from the runtime.
static func bake_params_from_runtime(root: Node, bc: Resource, vol: Node = null) -> Dictionary:
	var params: Dictionary = {}
	if bc != null and bc.has_method("get_bake_params"):
		params = bc.get_bake_params()
	else:
		params = ResonanceBakeConfig.create_default().get_bake_params()
	params["bake_reflection_type"] = resolve_bake_reflection_type(root, bc)
	params["bake_ambisonics_order"] = resolve_bake_ambisonics_order(root, bc, vol)
	var num_samples := 4
	var vis_range := 1000.0
	var path_range := 1000.0
	var radius := 1.0
	var threshold := 0.1
	var rt_node := find_resonance_runtime(root)
	if rt_node:
		var rt = rt_node.get("runtime")
		if rt != null:
			if "pathing_num_samples" in rt:
				num_samples = clampi(int(rt.pathing_num_samples), 1, 16)
			if "pathing_vis_range" in rt:
				vis_range = clampf(float(rt.pathing_vis_range), 0.0, 1000.0)
			if "pathing_path_range" in rt:
				path_range = clampf(float(rt.pathing_path_range), 0.0, 1000.0)
			if "pathing_vis_radius" in rt:
				radius = clampf(float(rt.pathing_vis_radius), 0.0, 2.0)
			if "pathing_vis_threshold" in rt:
				threshold = clampf(float(rt.pathing_vis_threshold), 0.0, 1.0)
	params["bake_pathing_num_samples"] = num_samples
	params["bake_pathing_vis_range"] = vis_range
	params["bake_pathing_path_range"] = path_range
	params["bake_pathing_radius"] = radius
	params["bake_pathing_threshold"] = threshold
	return params


static func find_resonance_static_scene_for_bake(volumes: Array[Node], edited_root: Node) -> Node:
	var static_scene := _find_resonance_static_scene(edited_root)
	if static_scene:
		return static_scene

	if volumes.size() > 0:
		var branch_root := _get_branch_root(volumes[0])
		static_scene = _find_resonance_static_scene(branch_root)
		if static_scene:
			return static_scene

	var tree: SceneTree = edited_root.get_tree() if edited_root else null
	if tree == null and volumes.size() > 0 and volumes[0].is_inside_tree():
		tree = volumes[0].get_tree()

	static_scene = _find_resonance_static_scene(_get_scene_root_from_tree(tree))
	return static_scene


static func resolve_bake_node_for_volume(
	vol: Node, root: Node, property: String, target_class: String
) -> Node:
	var arr = vol.get(property) if vol and property in vol else []
	if arr is Array and arr.size() > 0:
		var path_val = arr[0]
		var path := NodePath(str(path_val)) if path_val else NodePath()
		var n := _resolve_nodepath(vol, root, path)
		if n and n.is_class(target_class):
			return n
	return null


## Resolves every NodePath in [param vol].[param property] to a node of [param target_class].
## Paths that do not resolve are skipped.
static func resolve_bake_nodes_for_volume(
	vol: Node, root: Node, property: String, target_class: String
) -> Array:
	var out: Array = []
	var arr = vol.get(property) if vol and property in vol else []
	if not (arr is Array):
		return out
	for path_val in arr:
		var path := NodePath(str(path_val)) if path_val else NodePath()
		var n := _resolve_nodepath(vol, root, path)
		if n and n.is_class(target_class):
			out.append(n)
	return out


static func _collect_bake_targets_under(node: Node, sources: Array, listeners: Array) -> void:
	if node == null:
		return
	if node.is_class("ResonancePlayer"):
		sources.append(node)
	elif node.is_class("ResonanceListener"):
		listeners.append(node)
	for c in node.get_children():
		_collect_bake_targets_under(c, sources, listeners)


static func _append_unique_path(out: Array, path: NodePath) -> void:
	if path.is_empty():
		return
	for existing in out:
		if NodePath(str(existing)) == path:
			return
	out.append(path)


## Scans each [member ResonanceProbeVolume.scan_targets] root and its descendants and [b]replaces[/b]
## [member ResonanceProbeVolume.bake_sources] and [member ResonanceProbeVolume.bake_listeners] with what it finds.
## Returns the counts as [code]{ sources, listeners, scan_roots_used }[/code].
static func update_volume_bake_targets_from_scan(vol: Node) -> Dictionary:
	var result := {"sources": 0, "listeners": 0, "scan_roots_used": 0}
	if vol == null or not vol.is_class("ResonanceProbeVolume"):
		return result

	var tree: SceneTree = vol.get_tree() if vol.is_inside_tree() else null
	var root: Node = _get_scene_root_from_tree(tree)
	# Avoid `in` on native GDExtension nodes (can miss ClassDB properties).
	var scan_arr: Variant = vol.get("scan_targets")
	if not (scan_arr is Array):
		scan_arr = []

	# A blank slot in the array editor is an empty NodePath; drop it without a warning.
	var cleaned: Array = []
	for path_val in scan_arr:
		var path := NodePath(str(path_val)) if path_val else NodePath()
		if path.is_empty():
			continue
		cleaned.append(path)
	if cleaned.size() != scan_arr.size():
		vol.set("scan_targets", cleaned)
	scan_arr = cleaned

	if scan_arr.is_empty():
		push_warning(
			"ResonanceBakeDiscovery: scan_targets is empty; clearing bake_sources and bake_listeners."
		)
		vol.set("bake_sources", [])
		vol.set("bake_listeners", [])
		return result

	var source_nodes: Array = []
	var listener_nodes: Array = []
	for path_val in scan_arr:
		var path := NodePath(str(path_val)) if path_val else NodePath()
		var n := _resolve_nodepath(vol, root, path)
		if n == null:
			push_warning(
				"ResonanceBakeDiscovery: scan_targets entry could not be resolved: %s" % str(path)
			)
			continue
		result["scan_roots_used"] = int(result["scan_roots_used"]) + 1
		_collect_bake_targets_under(n, source_nodes, listener_nodes)

	var sources: Array = []
	var listeners: Array = []
	for n in source_nodes:
		if n is Node and vol.is_inside_tree() and (n as Node).is_inside_tree():
			_append_unique_path(sources, vol.get_path_to(n))
	for n in listener_nodes:
		if n is Node and vol.is_inside_tree() and (n as Node).is_inside_tree():
			_append_unique_path(listeners, vol.get_path_to(n))

	vol.set("bake_sources", sources)
	vol.set("bake_listeners", listeners)
	result["sources"] = sources.size()
	result["listeners"] = listeners.size()
	return result
