@tool
extends EditorNode3DGizmoPlugin

## ResonancePlayer directivity gizmo: dipole curve or omni sphere + arrow from [code]player_config[/code].
## Line geometry from [method ResonancePlayer.build_directivity_gizmo_lines] (matches runtime).

const GIZMO_CLASS_NAME := "ResonancePlayer"
const GIZMO_SIZE_METERS := 1.0
const MAT_DIPOLE := "nexus_player_directivity_dipole"
const MAT_OMNI := "nexus_player_directivity_omni"

var _mat_dipole_name: String = ""
var _mat_omni_name: String = ""


func _init() -> void:
	var sid := str(get_instance_id())
	_mat_dipole_name = MAT_DIPOLE + "_" + sid
	_mat_omni_name = MAT_OMNI + "_" + sid
	create_material(_mat_dipole_name, Color(1.0, 0.75, 0.15))
	create_material(_mat_omni_name, Color(0.25, 0.85, 1.0))


func _get_gizmo_name() -> String:
	return GIZMO_CLASS_NAME


func _has_gizmo(node: Node) -> bool:
	return node != null and node.is_class(GIZMO_CLASS_NAME)


func _redraw(gizmo: EditorNode3DGizmo) -> void:
	gizmo.clear()
	var node := gizmo.get_node_3d()
	if node == null:
		return
	_ensure_live_connection(node)
	# show_directivity_gizmo toggles visibility (editor + runtime).
	if not bool(node.get("show_directivity_gizmo")):
		return
	var cfg: Object = node.get("player_config") as Object
	var enabled := false
	var input_mode := 0
	var weight := 0.0
	var power := 1.0
	var user_value := 1.0
	if cfg != null:
		enabled = bool(cfg.get("directivity_enabled"))
		input_mode = int(cfg.get("directivity_input"))
		weight = float(cfg.get("directivity_weight"))
		power = float(cfg.get("directivity_power"))
		user_value = float(cfg.get("directivity_value"))

	# Instance call is fine for the GDExtension-bound static (global class timing).
	var lines: PackedVector3Array = node.build_directivity_gizmo_lines(
		enabled, input_mode, weight, power, user_value, GIZMO_SIZE_METERS
	)
	if lines.is_empty():
		return

	var is_dipole := enabled and input_mode == 0
	var mat_name := _mat_dipole_name if is_dipole else _mat_omni_name
	gizmo.add_lines(lines, get_material(mat_name, gizmo))


## Connect [signal Resource.changed] on [code]player_config[/code] to [method Node3D.update_gizmos]
## (native nodes may not repaint in editor on config edits). [Callable] match keeps this idempotent.
func _ensure_live_connection(node: Node3D) -> void:
	var cfg: Resource = node.get("player_config") as Resource
	if cfg == null:
		return
	var refresh_cb := Callable(node, "update_gizmos")
	if not cfg.changed.is_connected(refresh_cb):
		cfg.changed.connect(refresh_cb)
