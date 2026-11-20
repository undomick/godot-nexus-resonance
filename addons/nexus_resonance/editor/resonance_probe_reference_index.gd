extends RefCounted

## Probe data referenced by scenes open in the editor, which the .tscn files on disk may not show yet.


## Normalizes slashes and strips .bak suffixes so paths compare equal.
static func canonical_probe_resource_path(path: String) -> String:
	var out := path.strip_edges().replace("\\", "/")
	while out.ends_with(".bak"):
		out = out.get_basename()
	return out


## Probe data paths of every node under [param root] that has get_probe_data.
static func collect_live_probe_data_paths(root: Node) -> PackedStringArray:
	var out: PackedStringArray = []
	if root == null:
		return out
	_collect_live_probe_data_paths_recursive(root, out)
	return out


static func _collect_live_probe_data_paths_recursive(node: Node, out: PackedStringArray) -> void:
	if node.has_method("get_probe_data"):
		var pd: Variant = node.get_probe_data()
		if pd is Resource:
			var path: String = canonical_probe_resource_path((pd as Resource).resource_path)
			if not path.is_empty() and path not in out:
				out.append(path)
	for child in node.get_children():
		_collect_live_probe_data_paths_recursive(child, out)


## Union of both lists in first-seen order.
static func merge_referenced_paths(
	disk_referenced: PackedStringArray, live_referenced: PackedStringArray
) -> PackedStringArray:
	var out: PackedStringArray = []
	for path in disk_referenced:
		var canon: String = canonical_probe_resource_path(path)
		if not canon.is_empty() and canon not in out:
			out.append(canon)
	for path in live_referenced:
		var canon_live: String = canonical_probe_resource_path(path)
		if not canon_live.is_empty() and canon_live not in out:
			out.append(canon_live)
	return out
