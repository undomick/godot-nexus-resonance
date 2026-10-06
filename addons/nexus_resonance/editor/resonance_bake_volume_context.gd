extends RefCounted

## Per-volume bake state for the pipeline (reflections, pathing, static source/listener).

const _Self = preload("res://addons/nexus_resonance/editor/resonance_bake_volume_context.gd")
const _BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const _BakeHashes = preload("res://addons/nexus_resonance/editor/resonance_bake_hashes.gd")
const ResonanceBakeValidation = preload(
	"res://addons/nexus_resonance/editor/resonance_bake_validation.gd"
)

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


## Bake STATICSOURCE/STATICLISTENER probe influence. Runtime always queries layers with
## kBakedEndpointRadius (10000 m in C++). Keep bake_influence_radius at default unless you
## accept probes outside the bake sphere having no static layer at runtime.
static func _influence_radius(p_vol: Node, default_influence_radius: float) -> float:
	return (
		p_vol.get("bake_influence_radius")
		if "bake_influence_radius" in p_vol
		else default_influence_radius
	)


static func _entries_from_nodes(nodes: Array, radius: float) -> Array:
	var out: Array = []
	for n in nodes:
		if n is Node3D:
			out.append({"pos": n.global_position, "radius": radius, "node": n})
	return out


## Non-empty NodePaths on the volume property (stale paths still count).
static func _bake_path_count(p_vol: Node, property: String) -> int:
	var arr = p_vol.get(property) if p_vol and property in p_vol else []
	if not (arr is Array):
		return 0
	var n := 0
	for path_val in arr:
		if path_val and not NodePath(str(path_val)).is_empty():
			n += 1
	return n


static func _static_pass_need_hash(
	entries: Array, single_pos: Vector3, single_radius: float
) -> int:
	if entries.size() > 1:
		return _BakeHashes.compute_position_radius_list_hash(entries)
	return _BakeHashes.compute_position_radius_hash(single_pos, single_radius)


## Resolve bake_sources / bake_listeners; gate static passes on resolved nodes (not BakeConfig flags).
static func _resolve_static_endpoints(
	p_vol: Node, p_root: Node, default_influence_radius: float
) -> Dictionary:
	var infl := _influence_radius(p_vol, default_influence_radius)
	var src_nodes: Array = _BakeDiscovery.resolve_bake_nodes_for_volume(
		p_vol, p_root, "bake_sources", "ResonancePlayer"
	)
	var lst_nodes: Array = _BakeDiscovery.resolve_bake_nodes_for_volume(
		p_vol, p_root, "bake_listeners", "ResonanceListener"
	)
	var resolved_sources: Array = _entries_from_nodes(src_nodes, infl)
	var resolved_listeners: Array = _entries_from_nodes(lst_nodes, infl)
	var resolved_player_pos := Vector3.ZERO
	var resolved_listener_pos := Vector3.ZERO
	if resolved_sources.size() > 0:
		resolved_player_pos = resolved_sources[0].pos
	if resolved_listeners.size() > 0:
		resolved_listener_pos = resolved_listeners[0].pos
	var src_paths := _bake_path_count(p_vol, "bake_sources")
	var lst_paths := _bake_path_count(p_vol, "bake_listeners")
	return {
		"add_flags":
		{
			"static_source": resolved_sources.size() > 0,
			"static_listener": resolved_listeners.size() > 0,
		},
		"static_source_entries": resolved_sources,
		"static_listener_entries": resolved_listeners,
		"player_pos": resolved_player_pos,
		"player_radius": infl,
		"listener_pos": resolved_listener_pos,
		"listener_radius": infl,
		"src_path_count": src_paths,
		"lst_path_count": lst_paths,
	}


static func _emit_stale_path_warnings(endpoints: Dictionary) -> void:
	var src_warn := ResonanceBakeValidation.stale_bake_paths_warning(
		"bake_sources",
		int(endpoints.get("src_path_count", 0)),
		(endpoints.static_source_entries as Array).size(),
		"ResonancePlayer"
	)
	if not src_warn.is_empty():
		push_warning(src_warn)
	var lst_warn := ResonanceBakeValidation.stale_bake_paths_warning(
		"bake_listeners",
		int(endpoints.get("lst_path_count", 0)),
		(endpoints.static_listener_entries as Array).size(),
		"ResonanceListener"
	)
	if not lst_warn.is_empty():
		push_warning(lst_warn)


static func build(
	p_vol: Node,
	p_root: Node,
	vol_index: int,
	total: int,
	get_bake_config_for_volume: Callable,
	default_influence_radius: float,
	is_headless_bake: bool = false
):
	var ctx = _Self.new()
	ctx.root = p_root
	ctx.vol = p_vol
	ctx.vol_info = " (volume %d of %d)" % [vol_index, total] if total > 1 else ""
	ctx.bc = (
		get_bake_config_for_volume.call(p_vol) if get_bake_config_for_volume.is_valid() else null
	)
	if ctx.bc == null:
		ctx.bc = ResonanceBakeConfig.create_default()
	var endpoints: Dictionary = _resolve_static_endpoints(p_vol, p_root, default_influence_radius)
	_emit_stale_path_warnings(endpoints)
	ctx.add_flags = endpoints.add_flags
	ctx.static_source_entries = endpoints.static_source_entries
	ctx.static_listener_entries = endpoints.static_listener_entries
	ctx.player_pos = endpoints.player_pos
	ctx.player_radius = endpoints.player_radius
	ctx.listener_pos = endpoints.listener_pos
	ctx.listener_radius = endpoints.listener_radius
	var resolved_probe_data = p_vol.get_probe_data() if p_vol.has_method("get_probe_data") else null
	if not resolved_probe_data:
		resolved_probe_data = ClassDB.instantiate("ResonanceProbeData")
		if is_headless_bake and "headless_baking_mode" in p_vol:
			p_vol.headless_baking_mode = true
		p_vol.set_probe_data(resolved_probe_data)
		if "headless_baking_mode" in p_vol:
			p_vol.headless_baking_mode = false
	ctx.probe_data = resolved_probe_data
	var needs: Dictionary = compute_bake_needs(
		p_vol,
		p_root,
		ctx.bc,
		resolved_probe_data,
		default_influence_radius,
		ctx.add_flags,
		ctx.static_source_entries,
		ctx.static_listener_entries,
		ctx.player_pos,
		ctx.player_radius,
		ctx.listener_pos,
		ctx.listener_radius
	)
	ctx.need_reflections = needs.need_reflections
	ctx.need_pathing = needs.need_pathing
	ctx.need_static_source = needs.need_static_source
	ctx.need_static_listener = needs.need_static_listener
	return ctx


## Resolves static endpoints and returns the shared invalidation plan (pipeline + inspector SSOT).
static func compute_bake_needs_for_volume(
	p_vol: Node,
	p_root: Node,
	p_bc: Resource,
	p_probe_data: Resource,
	default_influence_radius: float
) -> Dictionary:
	var bake_cfg: Resource = p_bc
	if bake_cfg == null:
		bake_cfg = ResonanceBakeConfig.create_default()
	var endpoints: Dictionary = _resolve_static_endpoints(p_vol, p_root, default_influence_radius)
	return compute_bake_needs(
		p_vol,
		p_root,
		bake_cfg,
		p_probe_data,
		default_influence_radius,
		endpoints.add_flags,
		endpoints.static_source_entries,
		endpoints.static_listener_entries,
		endpoints.player_pos,
		endpoints.player_radius,
		endpoints.listener_pos,
		endpoints.listener_radius
	)


## Shared invalidation plan for pipeline skips and inspector bake status (SSOT).
static func compute_bake_needs(
	p_vol: Node,
	p_root: Node,
	p_bc: Resource,
	p_probe_data: Resource,
	_default_influence_radius: float,
	p_add_flags: Dictionary,
	p_static_source_entries: Array,
	p_static_listener_entries: Array,
	p_player_pos: Vector3,
	p_player_radius: float,
	p_listener_pos: Vector3,
	p_listener_radius: float
) -> Dictionary:
	var out := {
		"need_reflections": true,
		"need_pathing": false,
		"need_static_source": false,
		"need_static_listener": false,
	}
	var bake_cfg: Resource = p_bc
	if bake_cfg == null:
		bake_cfg = ResonanceBakeConfig.create_default()
	if not p_probe_data:
		return out
	var ph = (
		p_probe_data.get_pathing_params_hash()
		if p_probe_data.has_method("get_pathing_params_hash")
		else 0
	)
	var has_data = p_probe_data.get_data().size() > 0
	var want_path = _BakeDiscovery.resolve_bake_pathing_enabled(p_root, bake_cfg)
	# Bake layer from ResonanceRuntime (or BakeConfig fallback); not BakeConfig.reflection_type alone.
	var desired_refl = _BakeDiscovery.resolve_bake_reflection_type(p_root, bake_cfg)
	var baked_refl = (
		p_probe_data.get_baked_reflection_type()
		if p_probe_data.has_method("get_baked_reflection_type")
		else -1
	)
	# Legacy -1 = both layers; otherwise exact bake-layer match.
	var refl_matches = baked_refl < 0 or baked_refl == desired_refl
	var desired_amb = _BakeDiscovery.resolve_bake_ambisonics_order(p_root, bake_cfg, p_vol)
	var baked_amb = (
		p_probe_data.get_baked_ambisonics_order()
		if p_probe_data.has_method("get_baked_ambisonics_order")
		else -1
	)
	# Legacy -1 treated as order 1; otherwise exact match for rebake needs.
	var amb_matches = (baked_amb < 0 and desired_amb == 1) or baked_amb == desired_amb
	var hash_matches = (
		p_probe_data.get_bake_params_hash() == p_vol.get_bake_params_hash()
		if p_vol.has_method("get_bake_params_hash")
		else false
	)
	out.need_reflections = not has_data or not hash_matches or not refl_matches or not amb_matches
	var union_static_hash: int = _BakeHashes.compute_all_resonance_static_scenes_params_hash(p_root)
	if union_static_hash != 0 and p_probe_data.has_method("get_static_scene_params_hash"):
		var stored_union: int = p_probe_data.get_static_scene_params_hash()
		if stored_union == 0 or stored_union != union_static_hash:
			out.need_reflections = true
	# Runtime pathing off: do not force reflections rebake just because an old pathing hash exists.
	var path_hash = _BakeHashes.compute_pathing_hash(p_root, bake_cfg, p_vol) if want_path else 0
	out.need_pathing = want_path and (ph == 0 or ph != path_hash)
	if want_path and out.need_pathing and (not has_data or not refl_matches or not amb_matches):
		out.need_reflections = true
	if p_add_flags.get("static_source", false):
		var sh := _static_pass_need_hash(p_static_source_entries, p_player_pos, p_player_radius)
		var ssh = (
			p_probe_data.get_static_source_params_hash()
			if p_probe_data.has_method("get_static_source_params_hash")
			else 0
		)
		out.need_static_source = ssh == 0 or ssh != sh
	if p_add_flags.get("static_listener", false):
		var lh := _static_pass_need_hash(
			p_static_listener_entries, p_listener_pos, p_listener_radius
		)
		var lsh = (
			p_probe_data.get_static_listener_params_hash()
			if p_probe_data.has_method("get_static_listener_params_hash")
			else 0
		)
		out.need_static_listener = lsh == 0 or lsh != lh
	return out
