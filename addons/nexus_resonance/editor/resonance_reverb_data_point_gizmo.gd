@tool
extends EditorNode3DGizmoPlugin

## ResonanceReverbDataPoint: billboard icon + neighbor_radius wire sphere (color by energy).

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const QueryUI = preload(
	"res://addons/nexus_resonance/scripts/resonance_reverb_data_point_query_ui.gd"
)
const GIZMO_CLASS_NAME := "ResonanceReverbDataPoint"
const SPHERE_SEGMENTS := 24
const SPHERE_RINGS := 12
const COLOR_HAS_ENERGY := Color(0.35, 0.85, 0.55)
const COLOR_NO_ENERGY := Color(0.55, 0.55, 0.55)

var fallback_icon: Texture2D = null
var _icon_material_created: bool = false
var _mat_sphere_energy: String = ""
var _mat_sphere_empty: String = ""
var _mat_icon: String = ""


func _init() -> void:
	var sid := str(get_instance_id())
	_mat_sphere_energy = "nexus_reverb_dp_sphere_e_" + sid
	_mat_sphere_empty = "nexus_reverb_dp_sphere_x_" + sid
	_mat_icon = "nexus_reverb_dp_icon_" + sid
	create_material(_mat_sphere_energy, COLOR_HAS_ENERGY)
	create_material(_mat_sphere_empty, COLOR_NO_ENERGY)


func _get_gizmo_name() -> String:
	return GIZMO_CLASS_NAME


func _has_gizmo(node: Node) -> bool:
	return node != null and node.is_class(GIZMO_CLASS_NAME)


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
		create_icon_material(_mat_icon, icon_tex, false, COLOR_HAS_ENERGY)
		_icon_material_created = true


func _redraw(gizmo: EditorNode3DGizmo) -> void:
	gizmo.clear()
	var node := gizmo.get_node_3d()
	if node == null:
		return
	_ensure_icon_material(gizmo)

	var has_energy := QueryUI.query_has_energy(node.get("last_query"))
	var sphere_mat_name := _mat_sphere_energy if has_energy else _mat_sphere_empty

	var icon_mat = get_material(_mat_icon, gizmo)
	if icon_mat:
		gizmo.add_unscaled_billboard(icon_mat, 0.05)

	var radius := 4.0
	if "neighbor_radius" in node:
		radius = float(node.get("neighbor_radius"))
	if radius <= 0.0:
		return

	var lines := _wire_sphere_lines(radius)
	if lines.is_empty():
		return
	gizmo.add_lines(lines, get_material(sphere_mat_name, gizmo))
	gizmo.add_collision_segments(lines)


func _wire_sphere_lines(radius: float) -> PackedVector3Array:
	var lines := PackedVector3Array()
	for r in range(1, SPHERE_RINGS):
		var phi := PI * float(r) / float(SPHERE_RINGS)
		var y := cos(phi) * radius
		var ring_r := sin(phi) * radius
		for s in range(SPHERE_SEGMENTS):
			var a0 := TAU * float(s) / float(SPHERE_SEGMENTS)
			var a1 := TAU * float(s + 1) / float(SPHERE_SEGMENTS)
			lines.append(Vector3(cos(a0) * ring_r, y, sin(a0) * ring_r))
			lines.append(Vector3(cos(a1) * ring_r, y, sin(a1) * ring_r))
	for s in range(SPHERE_SEGMENTS / 2):
		var theta := TAU * float(s) / float(SPHERE_SEGMENTS)
		for r in range(SPHERE_RINGS):
			var p0 := PI * float(r) / float(SPHERE_RINGS)
			var p1 := PI * float(r + 1) / float(SPHERE_RINGS)
			lines.append(
				Vector3(
					sin(p0) * cos(theta) * radius, cos(p0) * radius, sin(p0) * sin(theta) * radius
				)
			)
			lines.append(
				Vector3(
					sin(p1) * cos(theta) * radius, cos(p1) * radius, sin(p1) * sin(theta) * radius
				)
			)
	return lines
