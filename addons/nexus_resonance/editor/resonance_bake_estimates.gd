extends Object
class_name ResonanceBakeEstimates

## Rough probe counts and bake duration strings for the bake UI (no editor dependency).

const BAKE_RAY_BASE_SEC_PER_PROBE := 0.001
const BAKE_RAY_BASE_COUNT := 4096


static func estimate_probe_count(vol: Node) -> int:
	if not vol or not vol.has_method("get") or not ("region_size" in vol and "spacing" in vol):
		return -1
	var extents: Vector3 = vol.get("region_size") * 0.5
	var spacing: float = vol.get("spacing")
	if spacing <= 0:
		return -1
	var gen_type: int = vol.get("generation_type") if "generation_type" in vol else 1
	var nx := int(ceil(extents.x * 2 / spacing))
	var nz := int(ceil(extents.z * 2 / spacing))
	var count: int
	if gen_type == 0:
		count = 1
	elif gen_type == 1:
		count = nx * nz
	else:
		var ny := int(ceil(extents.y * 2 / spacing))
		count = nx * ny * nz
	# Approximate exclusion cull for UI estimates (axis-aligned overlap only).
	if vol.has_method("collect_exclusion_boxes") and count > 0 and gen_type != 0:
		var boxes: Array = vol.collect_exclusion_boxes()
		if not boxes.is_empty():
			var vol_xform: Transform3D = vol.global_transform if vol is Node3D else Transform3D.IDENTITY
			var kept := 0
			# Coarse: sample grid like Uniform Floor / Volume and count survivors.
			var height: float = float(vol.get("height_above_floor")) if "height_above_floor" in vol else 1.5
			var points := _estimate_sample_points(vol_xform, extents, spacing, gen_type, height)
			for p in points:
				var excluded := false
				for b in boxes:
					if typeof(b) != TYPE_DICTIONARY:
						continue
					var xform: Transform3D = b.get("xform", Transform3D.IDENTITY)
					var size: Vector3 = b.get("size", Vector3.ONE)
					var local: Vector3 = xform.affine_inverse() * p
					var half: Vector3 = size * 0.5
					if (
						absf(local.x) <= half.x
						and absf(local.y) <= half.y
						and absf(local.z) <= half.z
					):
						excluded = true
						break
				if not excluded:
					kept += 1
			return kept
	return count


static func _estimate_sample_points(
	volume_transform: Transform3D, extents: Vector3, spacing: float, gen_type: int, height_above_floor: float
) -> PackedVector3Array:
	var points := PackedVector3Array()
	var size: Vector3 = extents * 2.0
	if gen_type == 0:
		points.append(volume_transform.origin)
		return points
	if gen_type == 1:
		var plane_y: float = -extents.y + height_above_floor
		var count_x: int = maxi(1, int(floor(size.x / spacing)))
		var count_z: int = maxi(1, int(floor(size.z / spacing)))
		var offset_x: float = spacing * 0.5 if size.x >= spacing else extents.x
		var offset_z: float = spacing * 0.5 if size.z >= spacing else extents.z
		for ix in count_x:
			for iz in count_z:
				var local := Vector3(
					-extents.x + ix * spacing + offset_x, plane_y, -extents.z + iz * spacing + offset_z
				)
				points.append(volume_transform * local)
		return points
	var count_x2: int = maxi(1, int(floor(size.x / spacing)))
	var count_y2: int = maxi(1, int(floor(size.y / spacing)))
	var count_z2: int = maxi(1, int(floor(size.z / spacing)))
	var offset := Vector3(spacing * 0.5, spacing * 0.5, spacing * 0.5)
	if size.x < spacing:
		offset.x = extents.x
	if size.y < spacing:
		offset.y = extents.y
	if size.z < spacing:
		offset.z = extents.z
	for ix in count_x2:
		for iy in count_y2:
			for iz in count_z2:
				var local2 := -extents + Vector3(
					ix * spacing + offset.x, iy * spacing + offset.y, iz * spacing + offset.z
				)
				points.append(volume_transform * local2)
	return points


## [param bc] must be a [ResonanceBakeConfig]-like resource with bake_num_rays, pathing_enabled, etc.
static func estimate_bake_time(vol: Node, bc: Resource) -> String:
	if bc == null:
		return ""
	var count := estimate_probe_count(vol)
	if count < 0:
		return ""
	var rays = bc.bake_num_rays
	var bounces = bc.bake_num_bounces
	var threads = bc.bake_num_threads
	var pathing = bc.pathing_enabled
	var sec_per_probe: float = (
		BAKE_RAY_BASE_SEC_PER_PROBE
		* (rays / float(BAKE_RAY_BASE_COUNT))
		* bounces
		/ max(1, threads)
	)
	var total = count * sec_per_probe
	if pathing:
		total *= 2.0
	if total < 60:
		return "~%d s" % int(ceil(total))
	if total < 3600:
		return "~%d min" % int(ceil(total / 60))
	return "~%.1f h" % (total / 3600)
