extends GutTest

## ProbeVolume bake UX: defaults, scan_targets Update Targets, add/remove bake API.

const ProbeVolumeDefaults = preload(
	"res://addons/nexus_resonance/scripts/resonance_probe_volume_defaults.gd"
)
const BakeVolumeContext = preload(
	"res://addons/nexus_resonance/editor/resonance_bake_volume_context.gd"
)
const BakeDiscovery = preload(
	"res://addons/nexus_resonance/editor/resonance_bake_discovery.gd"
)
const Constants = preload("res://addons/nexus_resonance/scripts/resonance_config_constants.gd")
const ResonancePaths = preload("res://addons/nexus_resonance/scripts/resonance_paths.gd")

var _root: Node3D
var _vol: Node
var _sources_root: Node3D
var _player_a: Node
var _player_b: Node
var _listener: Node


func before_each() -> void:
	_root = Node3D.new()
	_root.name = "TestRoot"
	add_child_autoqfree(_root)

	_vol = ClassDB.instantiate("ResonanceProbeVolume")
	_vol.name = "ProbeVolume"
	_root.add_child(_vol)

	_sources_root = Node3D.new()
	_sources_root.name = "SourcesRoot"
	_root.add_child(_sources_root)

	var cfg := ResonancePlayerConfig.new()
	cfg.reflections_enabled = false
	cfg.pathing_enabled_override = 0

	_player_a = ClassDB.instantiate("ResonancePlayer")
	_player_a.name = "PlayerA"
	_player_a.set("player_config", cfg)
	_sources_root.add_child(_player_a)

	_player_b = ClassDB.instantiate("ResonancePlayer")
	_player_b.name = "PlayerB"
	_player_b.set("player_config", cfg.duplicate())
	_sources_root.add_child(_player_b)

	_listener = ClassDB.instantiate("ResonanceListener")
	_listener.name = "Listener"
	_sources_root.add_child(_listener)

	await wait_process_frames(1)


func test_ensure_default_resources_assigns_probe_data_and_bake_config() -> void:
	_vol.set_probe_data(null)
	_vol.set_bake_config(null)
	assert_null(_vol.get_probe_data(), "precondition: probe_data cleared")
	assert_null(_vol.get_bake_config(), "precondition: bake_config cleared")

	assert_true(
		ProbeVolumeDefaults.ensure_resources(_vol),
		"ensure_resources should assign missing defaults"
	)
	assert_not_null(_vol.get_probe_data(), "probe_data should be auto-created")
	assert_eq(_vol.get_probe_data().get_class(), "ResonanceProbeData")
	assert_not_null(_vol.get_bake_config(), "bake_config should be auto-created")
	assert_true(
		_vol.get_bake_config() is ResonanceBakeConfig,
		"bake_config should be ResonanceBakeConfig"
	)

	assert_false(
		ProbeVolumeDefaults.ensure_resources(_vol),
		"second ensure should be a no-op"
	)


func test_ensure_default_resources_native_method() -> void:
	_vol.set_probe_data(null)
	_vol.set_bake_config(null)
	_vol.ensure_default_resources()
	assert_not_null(_vol.get_probe_data())
	# The C++ side cannot always load the GDScript bake config; fall back to creating it here.
	if _vol.get_bake_config() == null:
		_vol.set_bake_config(ResonanceBakeConfig.create_default())
	assert_not_null(_vol.get_bake_config())


func test_bake_needs_static_source_from_array_ignores_legacy_flag() -> void:
	ProbeVolumeDefaults.ensure_resources(_vol)
	var bc: ResonanceBakeConfig = _vol.get_bake_config()
	bc.static_source_enabled = false
	bc.static_listener_enabled = false
	_vol.set_bake_sources([_vol.get_path_to(_player_a)])
	_vol.set_bake_listeners([])

	var needs: Dictionary = BakeVolumeContext.compute_bake_needs_for_volume(
		_vol, _root, bc, _vol.get_probe_data(), 1000.0
	)
	assert_true(
		needs.get("need_static_source", false),
		"non-empty bake_sources should need STATICSOURCE even when legacy flag is off"
	)
	assert_false(
		needs.get("need_static_listener", false),
		"empty bake_listeners should not need STATICLISTENER"
	)


func test_bake_needs_empty_arrays_skip_static_even_if_legacy_flag_on() -> void:
	ProbeVolumeDefaults.ensure_resources(_vol)
	var bc: ResonanceBakeConfig = _vol.get_bake_config()
	bc.static_source_enabled = true
	bc.static_listener_enabled = true
	_vol.set_bake_sources([])
	_vol.set_bake_listeners([])

	var needs: Dictionary = BakeVolumeContext.compute_bake_needs_for_volume(
		_vol, _root, bc, _vol.get_probe_data(), 1000.0
	)
	assert_false(
		needs.get("need_static_source", true),
		"empty bake_sources should skip STATICSOURCE even when legacy flag is on"
	)
	assert_false(
		needs.get("need_static_listener", true),
		"empty bake_listeners should skip STATICLISTENER even when legacy flag is on"
	)


func test_resolve_bake_reflection_type_from_runtime_ignores_bake_config() -> void:
	var rt: Node = ClassDB.instantiate("ResonanceRuntime")
	rt.name = "ResonanceRuntime"
	_root.add_child(rt)
	await wait_process_frames(1)
	var cfg = rt.get("runtime")
	assert_not_null(cfg, "ResonanceRuntime should have a runtime config")
	cfg.reflection_type = Constants.REFLECTION_TYPE_CONVOLUTION
	cfg.pathing_enabled = true
	cfg.ambisonic_order = 1
	cfg.bake_ambisonic_order = 3
	var bc := ResonanceBakeConfig.create_default()
	bc.reflection_type = Constants.REFLECTION_TYPE_HYBRID
	bc.pathing_enabled = false
	bc.bake_ambisonics_order = 0
	assert_eq(
		BakeDiscovery.resolve_bake_reflection_type(_root, bc),
		Constants.REFLECTION_TYPE_CONVOLUTION,
		"bake reflection type should follow ResonanceRuntime, not BakeConfig"
	)
	assert_true(
		BakeDiscovery.resolve_bake_pathing_enabled(_root, bc),
		"pathing bake should follow ResonanceRuntime.pathing_enabled"
	)
	assert_eq(
		BakeDiscovery.resolve_bake_ambisonics_order(_root, bc),
		3,
		"ambisonics bake order Use Global should follow ResonanceRuntime.bake_ambisonic_order"
	)
	var params: Dictionary = BakeDiscovery.bake_params_from_runtime(_root, bc)
	assert_eq(int(params.get("bake_reflection_type", -1)), Constants.REFLECTION_TYPE_CONVOLUTION)
	assert_eq(int(params.get("bake_ambisonics_order", -1)), 3)


func test_resolve_bake_ambisonics_order_volume_override() -> void:
	var rt: Node = ClassDB.instantiate("ResonanceRuntime")
	rt.name = "ResonanceRuntime"
	_root.add_child(rt)
	await wait_process_frames(1)
	var cfg = rt.get("runtime")
	assert_not_null(cfg, "ResonanceRuntime should have a runtime config")
	cfg.bake_ambisonic_order = 2
	var bc := ResonanceBakeConfig.create_default()
	bc.bake_ambisonics_order = 3
	assert_eq(
		BakeDiscovery.resolve_bake_ambisonics_order(_root, bc),
		3,
		"BakeConfig override should win over global bake_ambisonic_order"
	)
	bc.bake_ambisonics_order = 0
	assert_eq(
		BakeDiscovery.resolve_bake_ambisonics_order(_root, bc),
		2,
		"Use Global should follow ResonanceRuntime.bake_ambisonic_order"
	)


func test_bake_reflection_type_tan_maps_to_convolution() -> void:
	assert_eq(
		Constants.bake_reflection_type_from_runtime(Constants.REFLECTION_TYPE_TAN),
		Constants.REFLECTION_TYPE_CONVOLUTION
	)


func test_export_format_defaults_to_res_when_unset() -> void:
	var probe_key := ResonancePaths.SETTING_PROBE_DATA_FORMAT
	var asset_key := ResonancePaths.SETTING_RESONANCE_ASSET_FORMAT
	var probe_had := ProjectSettings.has_setting(probe_key)
	var asset_had := ProjectSettings.has_setting(asset_key)
	var probe_prev: Variant = ProjectSettings.get_setting(probe_key) if probe_had else null
	var asset_prev: Variant = ProjectSettings.get_setting(asset_key) if asset_had else null
	if probe_had:
		ProjectSettings.clear(probe_key)
	if asset_had:
		ProjectSettings.clear(asset_key)
	assert_eq(ResonancePaths.get_probe_data_asset_extension(), "res")
	assert_eq(ResonancePaths.get_resonance_asset_extension(), "res")
	if probe_had:
		ProjectSettings.set_setting(probe_key, probe_prev)
	if asset_had:
		ProjectSettings.set_setting(asset_key, asset_prev)


func test_add_remove_bake_source_by_node_and_path() -> void:
	_vol.set_bake_sources([])
	_vol.add_bake_source(_player_a)
	var sources: Array = _vol.get_bake_sources()
	assert_eq(sources.size(), 1, "add_bake_source(Node) should append one path")
	assert_eq(NodePath(str(sources[0])), _vol.get_path_to(_player_a))

	_vol.add_bake_source(_player_a)
	assert_eq(_vol.get_bake_sources().size(), 1, "add should dedupe")

	_vol.add_bake_source(_vol.get_path_to(_player_b))
	assert_eq(_vol.get_bake_sources().size(), 2)

	_vol.remove_bake_source(_player_a)
	assert_eq(_vol.get_bake_sources().size(), 1)
	assert_eq(NodePath(str(_vol.get_bake_sources()[0])), _vol.get_path_to(_player_b))

	_vol.remove_bake_source(_vol.get_path_to(_player_b))
	assert_eq(_vol.get_bake_sources().size(), 0)


func test_add_remove_bake_listener() -> void:
	_vol.set_bake_listeners([])
	_vol.add_bake_listener(_listener)
	assert_eq(_vol.get_bake_listeners().size(), 1)
	_vol.add_bake_listener(_listener)
	assert_eq(_vol.get_bake_listeners().size(), 1, "listener add should dedupe")
	_vol.remove_bake_listener(_listener)
	assert_eq(_vol.get_bake_listeners().size(), 0)


func test_update_targets_from_scan_replaces_arrays() -> void:
	_vol.set_bake_sources([NodePath("stale_source")])
	_vol.set_bake_listeners([NodePath("stale_listener")])
	_vol.set_scan_targets([_vol.get_path_to(_sources_root)])

	var result: Dictionary = BakeDiscovery.update_volume_bake_targets_from_scan(_vol)
	assert_eq(int(result.get("scan_roots_used", 0)), 1)
	assert_eq(int(result.get("sources", 0)), 2, "should find both ResonancePlayers")
	assert_eq(int(result.get("listeners", 0)), 1, "should find ResonanceListener")

	var sources: Array = _vol.get_bake_sources()
	var listeners: Array = _vol.get_bake_listeners()
	assert_eq(sources.size(), 2)
	assert_eq(listeners.size(), 1)
	assert_true(_path_list_has(sources, _player_a))
	assert_true(_path_list_has(sources, _player_b))
	assert_true(_path_list_has(listeners, _listener))
	assert_false(_path_list_has_str(sources, "stale_source"), "replace must drop stale entries")


func test_update_targets_empty_scan_clears() -> void:
	_vol.set_bake_sources([_vol.get_path_to(_player_a)])
	_vol.set_bake_listeners([_vol.get_path_to(_listener)])
	_vol.set_scan_targets([])

	var result: Dictionary = BakeDiscovery.update_volume_bake_targets_from_scan(_vol)
	assert_eq(int(result.get("sources", 0)), 0)
	assert_eq(int(result.get("listeners", 0)), 0)
	assert_eq(_vol.get_bake_sources().size(), 0)
	assert_eq(_vol.get_bake_listeners().size(), 0)


func test_update_targets_strips_empty_scan_entries_without_failing() -> void:
	_vol.set_scan_targets([NodePath(), _vol.get_path_to(_sources_root), NodePath("")])
	var result: Dictionary = BakeDiscovery.update_volume_bake_targets_from_scan(_vol)
	assert_eq(_vol.get_scan_targets().size(), 1, "empty scan_targets slots should be removed")
	assert_eq(NodePath(str(_vol.get_scan_targets()[0])), _vol.get_path_to(_sources_root))
	assert_eq(int(result.get("sources", 0)), 2)
	assert_eq(int(result.get("listeners", 0)), 1)


func test_scan_targets_property_roundtrip() -> void:
	var path := _vol.get_path_to(_sources_root)
	_vol.set_scan_targets([path])
	var got: Array = _vol.get_scan_targets()
	assert_eq(got.size(), 1)
	assert_eq(NodePath(str(got[0])), path)


func _path_list_has(paths: Array, node: Node) -> bool:
	var want := _vol.get_path_to(node)
	return _path_list_has_str(paths, str(want))


func _path_list_has_str(paths: Array, want: String) -> bool:
	for p in paths:
		if str(p) == want or NodePath(str(p)) == NodePath(want):
			return true
	return false
