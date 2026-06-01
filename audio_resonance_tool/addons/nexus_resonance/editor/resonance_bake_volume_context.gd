extends RefCounted
class_name VolumeBakeContext

## Per-volume bake state for the pipeline (reflections, pathing, static source/listener).

const _Self = preload("res://addons/nexus_resonance/editor/resonance_bake_volume_context.gd")
const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")
const _BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const _BakeHashes = preload("res://addons/nexus_resonance/editor/resonance_bake_hashes.gd")

var root: Node
var vol: Node
var probe_data: Resource
var need_reflections: bool
var need_pathing: bool
var need_static_source: bool
var need_static_listener: bool
var add_flags: Dictionary
var player_pos: Vector3
var player_radius: float
var listener_pos: Vector3
var listener_radius: float
## bake_sources resolved to { pos, radius, node } per emitter; one bake_static_source pass each.
var static_source_entries: Array = []
## Same pattern for bake_listeners / bake_static_listener.
var static_listener_entries: Array = []
var bc: Resource
var vol_info: String
var static_asset = null  # Packed static geometry for the server bake


static func _influence_radius(vol: Node, default_influence_radius: float) -> float:
	return (
		vol.get("bake_influence_radius")
		if "bake_influence_radius" in vol
		else default_influence_radius
	)


static func _entries_from_nodes(nodes: Array, radius: float) -> Array:
	var out: Array = []
	for n in nodes:
		if n is Node3D:
			out.append({"pos": n.global_position, "radius": radius, "node": n})
	return out


static func _static_pass_need_hash(
	entries: Array, single_pos: Vector3, single_radius: float
) -> int:
	if entries.size() > 1:
		return _BakeHashes.compute_position_radius_list_hash(entries)
	return _BakeHashes.compute_position_radius_hash(single_pos, single_radius)


static func build(
	vol: Node,
	root: Node,
	vol_index: int,
	total: int,
	static_asset,
	get_bake_config_for_volume: Callable,
	default_influence_radius: float,
	is_headless_bake: bool = false
):
	var ctx = _Self.new()
	ctx.root = root
	ctx.vol = vol
	ctx.static_asset = static_asset
	ctx.vol_info = " (volume %d of %d)" % [vol_index, total] if total > 1 else ""
	ctx.bc = get_bake_config_for_volume.call(vol) if get_bake_config_for_volume.is_valid() else null
	if ctx.bc == null:
		ctx.bc = ResonanceBakeConfig.create_default()
	ctx.add_flags = {
		"static_source": ctx.bc.static_source_enabled,
		"static_listener": ctx.bc.static_listener_enabled
	}
	ctx.player_pos = Vector3.ZERO
	var infl := _influence_radius(vol, default_influence_radius)
	ctx.player_radius = infl
	ctx.listener_pos = Vector3.ZERO
	ctx.listener_radius = infl
	if ctx.add_flags.static_source:
		var src_nodes: Array = _BakeDiscovery.resolve_bake_nodes_for_volume(
			vol, root, "bake_sources", "ResonancePlayer"
		)
		ctx.static_source_entries = _entries_from_nodes(src_nodes, ctx.player_radius)
		if ctx.static_source_entries.size() > 0:
			ctx.player_pos = ctx.static_source_entries[0].pos
	if ctx.add_flags.static_listener:
		var lst_nodes: Array = _BakeDiscovery.resolve_bake_nodes_for_volume(
			vol, root, "bake_listeners", "ResonanceListener"
		)
		ctx.static_listener_entries = _entries_from_nodes(lst_nodes, ctx.listener_radius)
		if ctx.static_listener_entries.size() > 0:
			ctx.listener_pos = ctx.static_listener_entries[0].pos
	var want_path = ctx.bc.pathing_enabled
	var path_hash = _BakeHashes.compute_pathing_hash(ctx.bc) if want_path else 0
	var probe_data = vol.get_probe_data() if vol.has_method("get_probe_data") else null
	if not probe_data:
		probe_data = ClassDB.instantiate("ResonanceProbeData")
		if is_headless_bake and "headless_baking_mode" in vol:
			vol.headless_baking_mode = true
		vol.set_probe_data(probe_data)
		if "headless_baking_mode" in vol:
			vol.headless_baking_mode = false
	ctx.probe_data = probe_data
	var ph = (
		probe_data.get_pathing_params_hash()
		if probe_data.has_method("get_pathing_params_hash")
		else 0
	)
	var has_data = probe_data.get_data().size() > 0
	var desired_refl = ctx.bc.reflection_type
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
	ctx.need_reflections = not has_data or not hash_matches or not refl_matches
	var union_static_hash: int = _BakeHashes.compute_all_resonance_static_scenes_params_hash(root)
	if union_static_hash != 0 and probe_data.has_method("get_static_scene_params_hash"):
		var stored_union: int = probe_data.get_static_scene_params_hash()
		if stored_union == 0 or stored_union != union_static_hash:
			ctx.need_reflections = true
	if not want_path and ph > 0:
		ctx.need_reflections = true
	ctx.need_pathing = want_path and (ph == 0 or ph != path_hash)
	if want_path and ctx.need_pathing and (not has_data or not refl_matches):
		ctx.need_reflections = true
	ctx.need_static_source = false
	ctx.need_static_listener = false
	if ctx.add_flags.static_source:
		var sh := _static_pass_need_hash(
			ctx.static_source_entries, ctx.player_pos, ctx.player_radius
		)
		var ssh = (
			probe_data.get_static_source_params_hash()
			if probe_data.has_method("get_static_source_params_hash")
			else 0
		)
		ctx.need_static_source = ssh == 0 or ssh != sh
	if ctx.add_flags.static_listener:
		var lh := _static_pass_need_hash(
			ctx.static_listener_entries, ctx.listener_pos, ctx.listener_radius
		)
		var lsh = (
			probe_data.get_static_listener_params_hash()
			if probe_data.has_method("get_static_listener_params_hash")
			else 0
		)
		ctx.need_static_listener = lsh == 0 or lsh != lh
	return ctx
