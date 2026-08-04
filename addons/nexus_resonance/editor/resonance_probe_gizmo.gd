@tool
extends EditorNode3DGizmoPlugin

## Editor gizmo for ResonanceProbeVolume (box + handles + optional icon).

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const GIZMO_RAY_LENGTH := 10000.0
const MIN_REGION_SIZE := 0.1

var undo_redo: EditorUndoRedoManager
## Set by plugin if ICON_PROBE_VOLUME_GIZMO is missing.
var fallback_icon: Texture2D = null
var _icon_material_created: bool = false
## Unique material names per plugin instance (avoid cache clashes on disable/enable).
var _mat_main: String = ""
var _mat_handles: String = ""
var _mat_icon: String = ""


func _get_valid_region_size(node: Node) -> Vector3:
	if "region_size" in node:
		var size = node.region_size
		if size is Vector3:
			return size
	return Vector3.ONE


func _get_gizmo_name() -> String:
	return UIStrings.GIZMO_PROBE_VOLUME_CLASS


func _has_gizmo(node: Node) -> bool:
	return node.is_class(UIStrings.GIZMO_PROBE_VOLUME_CLASS)


func _init() -> void:
	var sid := str(get_instance_id())
	_mat_main = "nexus_probe_vol_main_" + sid
	_mat_handles = "nexus_probe_vol_handles_" + sid
	_mat_icon = "nexus_probe_vol_icon_" + sid
	create_material(_mat_main, Color(0.1, 0.8, 1.0))
	create_handle_material(_mat_handles)

	# Icon material: lazy in _redraw (no EditorInterface in _init).


func _ensure_icon_material(_gizmo: EditorNode3DGizmo) -> void:
	if _icon_material_created:
		return
	var icon_tex: Texture2D = null
	if FileAccess.file_exists(UIStrings.ICON_PROBE_VOLUME_GIZMO):
		icon_tex = load(UIStrings.ICON_PROBE_VOLUME_GIZMO) as Texture2D
	if not icon_tex and fallback_icon:
		icon_tex = fallback_icon
	if not icon_tex:
		var gui = EditorInterface.get_base_control()
		if gui:
			icon_tex = gui.get_theme_icon("ReflectionProbe", "EditorIcons")
	if icon_tex:
		create_icon_material(_mat_icon, icon_tex)
		_icon_material_created = true


func _redraw(gizmo: EditorNode3DGizmo) -> void:
	gizmo.clear()
	var node = gizmo.get_node_3d()
	_ensure_icon_material(gizmo)

	# Billboard icon
	var icon_mat = get_material(_mat_icon, gizmo)
	if icon_mat:
		gizmo.add_unscaled_billboard(icon_mat, 0.05)

	var size = _get_valid_region_size(node)
	var lines = PackedVector3Array()
	var half = size * 0.5

	var p = [
		Vector3(-half.x, -half.y, -half.z),
		Vector3(half.x, -half.y, -half.z),
		Vector3(half.x, half.y, -half.z),
		Vector3(-half.x, half.y, -half.z),
		Vector3(-half.x, -half.y, half.z),
		Vector3(half.x, -half.y, half.z),
		Vector3(half.x, half.y, half.z),
		Vector3(-half.x, half.y, half.z)
	]

	lines.append_array([p[0], p[1], p[1], p[5], p[5], p[4], p[4], p[0]])
	lines.append_array([p[3], p[2], p[2], p[6], p[6], p[7], p[7], p[3]])
	lines.append_array([p[0], p[3], p[1], p[2], p[5], p[6], p[4], p[7]])

	gizmo.add_lines(lines, get_material(_mat_main, gizmo))
	gizmo.add_collision_segments(lines)

	# Handles
	var handles = PackedVector3Array()
	handles.push_back(Vector3(half.x, 0, 0))
	handles.push_back(Vector3(0, half.y, 0))
	handles.push_back(Vector3(0, 0, half.z))
	handles.push_back(Vector3(-half.x, 0, 0))
	handles.push_back(Vector3(0, -half.y, 0))
	handles.push_back(Vector3(0, 0, -half.z))

	gizmo.add_handles(handles, get_material(_mat_handles, gizmo), [])


func _get_handle_name(_gizmo: EditorNode3DGizmo, handle_id: int, _secondary: bool) -> String:
	var names: Array[String] = [
		UIStrings.GIZMO_HANDLE_SIZE_PX,
		UIStrings.GIZMO_HANDLE_SIZE_PY,
		UIStrings.GIZMO_HANDLE_SIZE_PZ,
		UIStrings.GIZMO_HANDLE_SIZE_MX,
		UIStrings.GIZMO_HANDLE_SIZE_MY,
		UIStrings.GIZMO_HANDLE_SIZE_MZ
	]
	return names[handle_id] if handle_id < names.size() else ""


func _get_handle_value(gizmo: EditorNode3DGizmo, _handle_id: int, _secondary: bool) -> Variant:
	var node = gizmo.get_node_3d()
	return {"region_size": _get_valid_region_size(node), "global_position": node.global_position}


## Live resize; undo in [method _commit_handle]. Alt: symmetric from center; default: one face moves (CSGBox-like).
func _set_handle(
	gizmo: EditorNode3DGizmo, handle_id: int, _secondary: bool, camera: Camera3D, point: Vector2
) -> void:
	var node = gizmo.get_node_3d()
	var size = _get_valid_region_size(node)
	var size_mut = Vector3(size)

	var transform = node.global_transform
	var origin = transform.origin
	var basis = transform.basis

	var axis_dirs: Array[Vector3] = [
		basis.x.normalized(),
		basis.y.normalized(),
		basis.z.normalized(),
		-basis.x.normalized(),
		-basis.y.normalized(),
		-basis.z.normalized(),
	]
	var axis_dir: Vector3 = (
		axis_dirs[handle_id] if handle_id >= 0 and handle_id < axis_dirs.size() else Vector3.ZERO
	)

	var ray_from = camera.project_ray_origin(point)
	var ray_dir = camera.project_ray_normal(point)

	var axis_start = origin - axis_dir * GIZMO_RAY_LENGTH
	var axis_end = origin + axis_dir * GIZMO_RAY_LENGTH

	var points = Geometry3D.get_closest_points_between_segments(
		ray_from, ray_from + ray_dir * GIZMO_RAY_LENGTH, axis_start, axis_end
	)
	var closest_point = points[1]

	var dist: float = (closest_point - origin).dot(axis_dir)
	if Input.is_key_pressed(KEY_CTRL):  # Snap to 0.5 units
		dist = round(dist * 2.0) / 2.0

	var axis_idx = handle_id % 3
	var new_full_size: float
	var symmetric := Input.is_key_pressed(KEY_ALT)
	if symmetric:
		new_full_size = (dist * 2.0) if dist > 0 else MIN_REGION_SIZE
		if new_full_size < MIN_REGION_SIZE:
			new_full_size = MIN_REGION_SIZE
		size_mut[axis_idx] = new_full_size
		node.region_size = size_mut
	else:
		var half: float = size[axis_idx] * 0.5
		if handle_id < 3:
			new_full_size = dist + half
		else:
			new_full_size = half + dist
		new_full_size = maxf(new_full_size, MIN_REGION_SIZE)
		var old_size: float = size[axis_idx]
		var offset: Vector3 = (new_full_size - old_size) / 2.0 * axis_dir
		size_mut[axis_idx] = new_full_size
		node.region_size = size_mut
		node.global_position = origin + offset
	node.update_gizmos()


func _commit_handle(
	gizmo: EditorNode3DGizmo, _handle_id: int, _secondary: bool, restore: Variant, cancel: bool
) -> void:
	var node: Node3D = gizmo.get_node_3d()
	var restored := restore as Dictionary

	if cancel:
		node.region_size = restored["region_size"]
		node.global_position = restored["global_position"]
	else:
		var new_size: Vector3 = node.region_size
		var new_pos: Vector3 = node.global_position
		var old_size: Vector3 = restored["region_size"]
		var old_pos: Vector3 = restored["global_position"]
		var changed := new_size != old_size or new_pos != old_pos
		if undo_redo != null and changed:
			undo_redo.create_action(UIStrings.UNDO_CHANGE_PROBE_VOLUME_SIZE)
			undo_redo.add_do_property(node, "region_size", new_size)
			undo_redo.add_undo_property(node, "region_size", old_size)
			undo_redo.add_do_property(node, "global_position", new_pos)
			undo_redo.add_undo_property(node, "global_position", old_pos)
			undo_redo.add_do_method(node, "update_gizmos")
			undo_redo.add_undo_method(node, "update_gizmos")
			undo_redo.commit_action()
			return
	node.update_gizmos()
