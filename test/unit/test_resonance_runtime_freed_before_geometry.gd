extends GutTest

## Freeing the runtime before its geometry (a level change with the runtime first in the tree) must not
## leave that geometry's triangles counted in the next runtime's scene.

const Scene := preload("res://test/helpers/tail_drain_scene.gd")

var _roots: Array[Node3D] = []


func after_each() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.queue_free()
	_roots.clear()
	await wait_process_frames(4)


func _triangles() -> int:
	return Engine.get_singleton("ResonanceServer").get_global_triangle_count()


func _build() -> Node3D:
	var root := Scene.build(self, Scene.REFLECTION_CONVOLUTION)
	_roots.append(root)
	await wait_process_frames(10)
	return root


func test_next_runtime_counts_only_its_own_geometry() -> void:
	var first := await _build()
	var one_room := _triangles()
	assert_gt(one_room, 0, "the room registers triangles")

	first.get_node("ResonanceRuntime").queue_free()
	await wait_process_frames(3)
	first.queue_free()
	await wait_process_frames(3)

	await _build()
	assert_eq(_triangles(), one_room, "the second level counts one room, not two")
