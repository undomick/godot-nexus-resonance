@tool
extends RefCounted
class_name ResonanceSceneUtils

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")

## Shared scene walks (runtime, static scene, probe volumes, dynamic geometry, export checks).


## Returns true if node or any descendant is ResonanceRuntime.
static func scene_has_resonance_runtime(node: Node) -> bool:
	if not node:
		return false
	if node.is_class("ResonanceRuntime"):
		return true
	for c in node.get_children():
		if scene_has_resonance_runtime(c):
			return true
	return false


## [param export_type] [code]"static"[/code] (runtime, static mesh, or static scene with asset) or [code]"dynamic"[/code] ([ResonanceDynamicGeometry]).
static func scene_has_exportable_resonance_content(node: Node, export_type: StringName) -> bool:
	if not node:
		return false
	match export_type:
		"static":
			if node.is_class("ResonanceRuntime"):
				return true
			if node.is_class("ResonanceStaticGeometry"):
				return true
			if node.is_class("ResonanceStaticScene"):
				if node.has_method("has_valid_asset") and node.has_valid_asset():
					return true
			for c in node.get_children():
				if scene_has_exportable_resonance_content(c, export_type):
					return true
			return false
		"dynamic":
			if node.is_class("ResonanceDynamicGeometry"):
				return true
			for c in node.get_children():
				if scene_has_exportable_resonance_content(c, export_type):
					return true
			return false
		_:
			return false


## Finds first ResonanceStaticScene in node tree (DFS). Prefer [method find_owned_resonance_static_scene] for export.
static func find_resonance_static_scene(node: Node) -> Node:
	if not node:
		return null
	if node.is_class("ResonanceStaticScene"):
		return node
	for c in node.get_children():
		var found = find_resonance_static_scene(c)
		if found:
			return found
	return null


## ResonanceStaticScene belonging to the edited scene file (owner == edited_root), not nested instances.
static func find_owned_resonance_static_scene(edited_root: Node) -> Node:
	if not edited_root:
		return null
	var collected: Array[Node] = []
	collect_resonance_static_scenes(edited_root, collected)
	for ss in collected:
		if ss == edited_root or ss.owner == edited_root:
			return ss
	return null


## Collects all ResonanceProbeVolume nodes in node tree.
static func collect_resonance_probe_volumes(node: Node, collected: Array[Node]) -> void:
	if not node:
		return
	if node.is_class("ResonanceProbeVolume"):
		collected.append(node)
	for c in node.get_children():
		collect_resonance_probe_volumes(c, collected)


## Collects all ResonanceDynamicGeometry nodes in node tree.
static func collect_resonance_dynamic_geometry(node: Node, collected: Array[Node]) -> void:
	if not node:
		return
	if node.is_class("ResonanceDynamicGeometry"):
		collected.append(node)
	for c in node.get_children():
		collect_resonance_dynamic_geometry(c, collected)


## Collects all ResonanceStaticScene nodes in node tree.
static func collect_resonance_static_scenes(node: Node, collected: Array[Node]) -> void:
	if not node:
		return
	if node.is_class("ResonanceStaticScene"):
		collected.append(node)
	for c in node.get_children():
		collect_resonance_static_scenes(c, collected)


## Nested [ResonanceStaticScene] without asset: geometry is excluded from parent export (C++ prune); warn per node.
## Skips the edited scene's owned ResonanceStaticScene (owner == root).
static func warn_static_scenes_without_asset_covering_geometry(root: Node) -> void:
	if not root:
		return
	_warn_rss_no_asset_rec(root, root)


static func _warn_rss_no_asset_rec(node: Node, export_root: Node) -> void:
	if not node:
		return
	if node != export_root and node.is_class("ResonanceStaticScene"):
		# Owned parent pack is updated by this export; do not treat it as a nested section.
		if node.owner == export_root:
			return
		var has_asset: bool = node.has_method("has_valid_asset") and node.has_valid_asset()
		if not has_asset:
			var rel: String = String(export_root.get_path_to(node))
			if rel.is_empty():
				rel = str(node.name)
			var msg: String = TranslationServer.translate(
				UIStrings.WARN_STATIC_SCENE_NO_ASSET_EXCLUDED
			)
			push_warning(msg % rel)
		# Nested RSS always pruned; do not walk into its children for further merge warnings.
		return
	for c in node.get_children():
		_warn_rss_no_asset_rec(c, export_root)


## True if [param root] has any nested ResonanceStaticScene (not the root itself).
static func has_nested_resonance_static_scene(root: Node) -> bool:
	if not root:
		return false
	for c in root.get_children():
		if _subtree_has_resonance_static_scene(c):
			return true
	return false


static func _subtree_has_resonance_static_scene(node: Node) -> bool:
	if not node:
		return false
	if node.is_class("ResonanceStaticScene"):
		return true
	for c in node.get_children():
		if _subtree_has_resonance_static_scene(c):
			return true
	return false


## Collects valid bake packs under [param root]: {assets: Array, transforms: Array, missing: Array}.
static func collect_bake_static_scene_packs(root: Node) -> Dictionary:
	var assets: Array = []
	var transforms: Array = []
	var missing: Array = []
	var scenes: Array[Node] = []
	collect_resonance_static_scenes(root, scenes)
	for ss in scenes:
		var has_asset: bool = ss.has_method("has_valid_asset") and ss.has_valid_asset()
		if not has_asset:
			missing.append(ss)
			continue
		var asset = ss.get("static_scene_asset") if "static_scene_asset" in ss else null
		if asset == null:
			missing.append(ss)
			continue
		assets.append(asset)
		if ss is Node3D:
			transforms.append((ss as Node3D).global_transform)
		else:
			transforms.append(Transform3D.IDENTITY)
	return {"assets": assets, "transforms": transforms, "missing": missing}


## Collects nodes whose pathing_probe_volume points at one of [param targets].
## [param result] keys: [code]nodes[/code] (Array), [code]count[/code] (int).
static func collect_probe_refs_to_clear(
	node: Node, scene_root: Node, targets: Array[Node], result: Dictionary
) -> void:
	if not node or not scene_root:
		return
	if node.has_method("get_pathing_probe_volume") and node.has_method("set_pathing_probe_volume"):
		var path: NodePath = node.get_pathing_probe_volume()
		if not path.is_empty():
			var target: Node = scene_root.get_node_or_null(path)
			if target and target in targets:
				result["nodes"].append(node)
				result["count"] += 1
	for c in node.get_children():
		collect_probe_refs_to_clear(c, scene_root, targets, result)
