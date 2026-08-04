extends Object
class_name ResonanceBakeValidation

## Resolves the edited scene root for bake validation.


static func get_edited_scene_root(volumes: Array[Node], editor_interface: EditorInterface) -> Node:
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


static func static_source_entries_error(static_source_enabled: bool, entries: Array) -> String:
	if not static_source_enabled:
		return ""
	if entries.is_empty():
		return (
			"Static source baking is enabled but bake_sources resolved to no "
			+ "ResonancePlayer nodes. Assign bake_sources on the probe volume."
		)
	return ""


static func static_listener_entries_error(static_listener_enabled: bool, entries: Array) -> String:
	if not static_listener_enabled:
		return ""
	if entries.is_empty():
		return (
			"Static listener baking is enabled but bake_listeners resolved to no "
			+ "ResonanceListener nodes. Assign bake_listeners on the probe volume."
		)
	return ""
