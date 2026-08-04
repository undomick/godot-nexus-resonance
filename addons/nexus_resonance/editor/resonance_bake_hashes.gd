extends Object
class_name ResonanceBakeHashes

## Hashes for bake params and static source/listener positions (shared by bake context + UI).

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")


static func hash_dict(d: Dictionary) -> int:
	return hash(var_to_str(d))


static func compute_pathing_hash(bc: Resource) -> int:
	var params = bc.get_bake_params()
	return hash_dict(
		{
			"vis_range": params.get("bake_pathing_vis_range", 500),
			"path_range": params.get("bake_pathing_path_range", 100),
			"num_samples": params.get("bake_pathing_num_samples", 16),
			"radius": params.get("bake_pathing_radius", 0.5),
			"threshold": params.get("bake_pathing_threshold", 0.1)
		}
	)


static func compute_position_radius_hash(pos: Vector3, radius: float) -> int:
	return hash_dict({"pos": pos, "radius": radius})


## Combined hash for (position, radius) entries. Changes invalidate STATICSOURCE / STATICLISTENER bakes.
static func _entry_pos_radius(e: Variant) -> Dictionary:
	if e is Dictionary:
		return {"pos": e.get("pos", Vector3.ZERO), "radius": e.get("radius", 0.0)}
	return {"pos": Vector3.ZERO, "radius": 0.0}


static func compute_position_radius_list_hash(entries: Array) -> int:
	if entries.is_empty():
		return 0
	var buckets: Array = []
	for e in entries:
		buckets.append(_entry_pos_radius(e))
	return hash_dict({"entries": buckets})


## Hash for every ResonanceStaticScene under [param root] (asset hash + global transform). Order matches
## [method ResonanceSceneUtils.collect_resonance_static_scenes] so all packs affect invalidation.
static func compute_all_resonance_static_scenes_params_hash(root: Node) -> int:
	if root == null or not ResonanceServerAccess.has_server():
		return 0
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null or not srv.has_method("get_geometry_asset_hash"):
		return 0
	var scenes: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_static_scenes(root, scenes)
	if scenes.is_empty():
		return 0
	var parts: Array = []
	for ss in scenes:
		var asset = ss.get("static_scene_asset") if "static_scene_asset" in ss else null
		if asset == null:
			continue
		if not (ss.has_method("has_valid_asset") and ss.has_valid_asset()):
			continue
		var ah: int = int(srv.get_geometry_asset_hash(asset))
		var xf := (ss as Node3D).global_transform if ss is Node3D else Transform3D.IDENTITY
		parts.append({"h": ah, "t": xf})
	if parts.is_empty():
		return 0
	return hash(var_to_str(parts))
