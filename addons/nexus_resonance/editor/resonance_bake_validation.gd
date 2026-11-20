extends Object

## Scene root lookup and error messages for bake validation.


# editor_interface stays untyped: this script is preloaded by the runtime baker, and
# exported builds have no EditorInterface class to parse the annotation against.
static func get_edited_scene_root(volumes: Array[Node], editor_interface = null) -> Node:
	if editor_interface:
		var root = editor_interface.get_edited_scene_root()
		if root:
			return root
	if volumes.size() > 0:
		var n: Node = volumes[0]
		while n and n.get_parent():
			n = n.get_parent()
		return n
	return null


## Returns an error if a static source pass was requested but no source resolved.
static func static_source_entries_error(static_source_wanted: bool, entries: Array) -> String:
	if not static_source_wanted:
		return ""
	if entries.is_empty():
		return (
			"Static source bake requested but bake_sources resolved to no "
			+ "ResonancePlayer nodes. Assign bake_sources on the probe volume."
		)
	return ""


static func static_listener_entries_error(static_listener_wanted: bool, entries: Array) -> String:
	if not static_listener_wanted:
		return ""
	if entries.is_empty():
		return (
			"Static listener bake requested but bake_listeners resolved to no "
			+ "ResonanceListener nodes. Assign bake_listeners on the probe volume."
		)
	return ""


## Returns a warning if [param property] has paths but none of them resolved to [param target_class].
static func stale_bake_paths_warning(
	property: String, path_count: int, resolved_count: int, target_class: String
) -> String:
	if path_count <= 0 or resolved_count > 0:
		return ""
	return (
		(
			"Nexus Resonance: %s has %d NodePath(s) but none resolved to %s. "
			+ "Remove stale paths or use Update Targets."
		)
		% [property, path_count, target_class]
	)
