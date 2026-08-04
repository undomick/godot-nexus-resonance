extends Object
class_name ResonanceBakeDiscovery

## Scene tree discovery for [ResonanceRuntime], [ResonanceStaticScene], and bake source/listener nodes.

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")


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


## Resolve all NodePaths in [param vol].[param property] to live Node3D instances of [param target_class].
## Used by the bake pipeline to issue one STATICSOURCE/STATICLISTENER pass per outdoor emitter so that
## multiple fixed sources (rain, thunder, HVAC, ...) produce position-dependent baked IRs instead of a
## single listener-only REVERB IR.
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


## Scans [member ResonanceProbeVolume.scan_targets] roots (DFS, including each root) and
## [b]replaces[/b] [member ResonanceProbeVolume.bake_sources] / [member ResonanceProbeVolume.bake_listeners]
## with paths relative to [param vol]. Returns `{ sources, listeners, scan_roots_used }`.
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

	# Drop empty NodePaths silently (common when the array editor has a blank slot).
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
