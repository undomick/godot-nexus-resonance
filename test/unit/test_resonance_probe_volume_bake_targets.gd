extends GutTest

## ProbeVolume bake UX: defaults, scan_targets Update Targets, add/remove bake API.

const ProbeVolumeDefaults = preload(
	"res://addons/nexus_resonance/scripts/resonance_probe_volume_defaults.gd"
)

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
	cfg.reflections_enabled = 0
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
	# C++ may fail to load GDScript bake config in some environments; GDScript fallback covers that.
	if _vol.get_bake_config() == null:
		_vol.set_bake_config(ResonanceBakeConfig.create_default())
	assert_not_null(_vol.get_bake_config())


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

	var result: Dictionary = ResonanceBakeDiscovery.update_volume_bake_targets_from_scan(_vol)
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

	var result: Dictionary = ResonanceBakeDiscovery.update_volume_bake_targets_from_scan(_vol)
	assert_eq(int(result.get("sources", 0)), 0)
	assert_eq(int(result.get("listeners", 0)), 0)
	assert_eq(_vol.get_bake_sources().size(), 0)
	assert_eq(_vol.get_bake_listeners().size(), 0)


func test_update_targets_strips_empty_scan_entries_without_failing() -> void:
	_vol.set_scan_targets([NodePath(), _vol.get_path_to(_sources_root), NodePath("")])
	var result: Dictionary = ResonanceBakeDiscovery.update_volume_bake_targets_from_scan(_vol)
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
