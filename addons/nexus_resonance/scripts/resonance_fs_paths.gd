extends Object
class_name ResonanceFsPaths

## Map [code]res://[/code] / [code]user://[/code] to OS paths for [DirAccess] / [FileAccess].


## Repo-root [code]logs/[/code] when the Godot project is [code]project/[/code]; else [code]logs/[/code] beside the project folder.
static func repo_logs_dir_absolute() -> String:
	var project_dir := ProjectSettings.globalize_path("res://").replace("\\", "/").rstrip("/")
	if project_dir.get_file() == "project":
		return project_dir.get_base_dir().path_join("logs")
	return project_dir.path_join("logs")


## Creates [method repo_logs_dir_absolute] if missing; returns that path.
static func ensure_repo_logs_dir() -> String:
	var dir := repo_logs_dir_absolute()
	DirAccess.make_dir_recursive_absolute(dir)
	return dir


static func filesystem_path_for_dir_access(path: String) -> String:
	if path.is_empty():
		return path
	var p := path.strip_edges().replace("\\", "/")
	if p.begins_with("res://") or p.begins_with("user://"):
		return ProjectSettings.globalize_path(p)
	return p


## [method DirAccess.open]: globalized path first, then logical (export handler convention).
static func open_dir_for_path(path: String) -> DirAccess:
	if path.is_empty():
		return null
	var fs_path := filesystem_path_for_dir_access(path)
	var d := DirAccess.open(fs_path)
	if d:
		return d
	return DirAccess.open(path)


## [method FileAccess.file_exists] with same globalized-then-logical try order.
static func file_exists_for_path(path: String) -> bool:
	if path.is_empty():
		return false
	var fs_path := filesystem_path_for_dir_access(path)
	if FileAccess.file_exists(fs_path):
		return true
	return FileAccess.file_exists(path)


## UTF-8 text: prefer globalized path if that file exists.
static func read_file_as_string(path: String) -> String:
	if path.is_empty():
		return ""
	var fs_path := filesystem_path_for_dir_access(path)
	if FileAccess.file_exists(fs_path):
		return FileAccess.get_file_as_string(fs_path)
	return FileAccess.get_file_as_string(path)


## Search strings for probe references in [code].tscn[/code] text (path variants).
static func probe_reference_needles_for_path(probe_logical_path: String) -> PackedStringArray:
	var out: PackedStringArray = []
	if probe_logical_path.is_empty():
		return out
	var norm := probe_logical_path.replace("\\", "/")
	out.append(norm)
	var fs := filesystem_path_for_dir_access(norm)
	if not fs.is_empty() and fs != norm and not out.has(fs):
		out.append(fs.replace("\\", "/"))
	var fname := norm.get_file()
	if not fname.is_empty() and not out.has(fname):
		out.append(fname)
	if not fs.is_empty():
		var localized := ProjectSettings.localize_path(fs)
		if not localized.is_empty() and not out.has(localized):
			out.append(localized)
	if norm.begins_with("res://"):
		var alt := norm
		if not out.has(alt):
			out.append(alt)
	return out


static func scene_text_references_probe_path(content: String, probe_logical_path: String) -> bool:
	for needle in probe_reference_needles_for_path(probe_logical_path):
		if needle.is_empty():
			continue
		if needle in content:
			return true
	return false
