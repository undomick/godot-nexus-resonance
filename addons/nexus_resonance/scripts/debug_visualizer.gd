extends Node3D

## Physics-ray occlusion preview (Godot collision only - not Steam Audio / Embree geometry).
## For real audio occlusion debug, use ResonanceRuntime "Debug Occlusion" (native ray data).

@export var listener_camera: Camera3D
@export var audio_source_node: Node3D

var debug_mesh_instance: MeshInstance3D
var immediate_mesh: ImmediateMesh


func _ready() -> void:
	debug_mesh_instance = MeshInstance3D.new()
	immediate_mesh = ImmediateMesh.new()
	debug_mesh_instance.mesh = immediate_mesh
	debug_mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF

	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	mat.no_depth_test = true
	debug_mesh_instance.material_override = mat

	add_child(debug_mesh_instance)


func _physics_process(_delta: float) -> void:
	if not listener_camera or not audio_source_node:
		return

	var start := listener_camera.global_position
	var end := audio_source_node.global_position

	# One line per frame: fine for debug; not a production path.
	immediate_mesh.clear_surfaces()
	immediate_mesh.surface_begin(Mesh.PRIMITIVE_LINES)

	var color := Color.GREEN
	var w3d := get_world_3d()
	if not w3d:
		return
	var space_state := w3d.direct_space_state
	if space_state:
		var query := PhysicsRayQueryParameters3D.create(start, end)
		var result := space_state.intersect_ray(query)
		if result:
			color = Color.RED

	immediate_mesh.surface_set_color(color)
	immediate_mesh.surface_add_vertex(start - global_position)
	immediate_mesh.surface_add_vertex(end - global_position)
	immediate_mesh.surface_end()
