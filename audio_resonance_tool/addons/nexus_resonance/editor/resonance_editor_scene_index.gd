@tool
extends RefCounted
class_name ResonanceEditorSceneIndex

## Collects .tscn paths from EditorFileSystem (preferred) or a narrow res:// directory walk.

const SKIP_DIRS := ["node_modules", ".godot"]
const ResonanceFsPaths = preload("res://addons/nexus_resonance/scripts/resonance_fs_paths.gd")


static func collect_tscn_paths_under(dir: String) -> PackedStringArray:
	return _collect_tscn_dir_walk(dir)


static func collect_tscn_paths(
	editor_interface: EditorInterface, fallback_dir: String = "res://"
) -> PackedStringArray:
	if editor_interface:
		var fs: EditorFileSystem = editor_interface.get_resource_filesystem()
		if fs:
			var root: EditorFileSystemDirectory = fs.get_filesystem()
			if root:
				var out: PackedStringArray = []
				_collect_tscn_from_efs_dir(root, out)
				if not out.is_empty():
					return out
	return _collect_tscn_dir_walk(fallback_dir)


static func _collect_tscn_from_efs_dir(
	dir: EditorFileSystemDirectory, out: PackedStringArray
) -> void:
	var n: int = dir.get_file_count()
	for i in n:
		var file_name: String = dir.get_file(i)
		if file_name.get_extension().to_lower() == "tscn":
			out.append(dir.get_file_path(i))
	var subdirs: int = dir.get_subdir_count()
	for j in subdirs:
		_collect_tscn_from_efs_dir(dir.get_subdir(j), out)


static func _collect_tscn_dir_walk(
	dir: String, out: PackedStringArray = PackedStringArray()
) -> PackedStringArray:
	var d: DirAccess = ResonanceFsPaths.open_dir_for_path(dir)
	if not d:
		return out
	d.list_dir_begin()
	var name_str: String = d.get_next()
	while name_str != "":
		if name_str.begins_with("."):
			name_str = d.get_next()
			continue
		var clean_name := name_str
		if clean_name.ends_with(".remap"):
			clean_name = clean_name.trim_suffix(".remap")
		elif clean_name.ends_with(".import"):
			clean_name = clean_name.trim_suffix(".import")
		var path_str: String = dir.path_join(clean_name)
		if d.current_is_dir():
			if clean_name not in SKIP_DIRS:
				_collect_tscn_dir_walk(path_str, out)
		elif clean_name.get_extension().to_lower() == "tscn":
			out.append(path_str)
		name_str = d.get_next()
	d.list_dir_end()
	return out


static func scene_text_has_static_resonance_content(scene_path: String) -> bool:
	var content: String = ResonanceFsPaths.read_file_as_string(scene_path)
	if content.is_empty():
		return false
	return (
		"ResonanceStaticGeometry" in content
		or "ResonanceStaticScene" in content
		or 'type="ResonanceStaticGeometry"' in content
		or 'type="ResonanceStaticScene"' in content
	)
