extends RefCounted
class_name ResonanceCodaVoiceSync

## Occlusion/transmission readback helpers for Nexus Coda voices (Option A bridge).

const DEFAULT_SOURCE_RADIUS := 1.0
const MIN_LINEAR_GAIN := 0.0001


static func server_ready() -> bool:
	if not ResonanceServerAccess.has_server():
		return false
	var srv = ResonanceServerAccess.get_server_if_initialized()
	return srv != null


## Mirror C++ resonance_source_handle_policy: recycled IDs after reinit need a matching epoch.
static func handle_matches_lifecycle_epoch(
	handle: int, handle_epoch: int, server_epoch: int
) -> bool:
	return handle >= 0 and handle_epoch != 0 and handle_epoch == server_epoch


static func lifecycle_epoch() -> int:
	if not ResonanceServerAccess.has_server():
		return 0
	var srv = ResonanceServerAccess.get_server()
	if srv != null and srv.has_method("get_source_lifecycle_epoch"):
		return int(srv.get_source_lifecycle_epoch())
	return 0


static func create_source_at(position: Vector3, radius: float = DEFAULT_SOURCE_RADIUS) -> int:
	if not server_ready():
		return -1
	var srv = ResonanceServerAccess.get_server()
	if not srv.has_method("create_source_handle"):
		return -1
	return int(srv.create_source_handle(position, radius))


static func destroy_source(handle: int) -> void:
	if handle < 0 or not ResonanceServerAccess.has_server():
		return
	var srv = ResonanceServerAccess.get_server()
	if srv != null and srv.has_method("destroy_source_handle"):
		srv.destroy_source_handle(handle)


## Only destroy when the handle still belongs to this client (not a recycled post-reinit ID).
static func destroy_source_if_epoch_matches(handle: int, handle_epoch: int) -> void:
	if not handle_matches_lifecycle_epoch(handle, handle_epoch, lifecycle_epoch()):
		return
	destroy_source(handle)


static func update_source_position(
	handle: int,
	position: Vector3,
	radius: float = DEFAULT_SOURCE_RADIUS,
	use_sim_distance_attenuation: bool = true,
	min_distance: float = 1.0,
	handle_epoch: int = 0
) -> void:
	if handle < 0 or not server_ready():
		return
	if (
		handle_epoch != 0
		and not handle_matches_lifecycle_epoch(handle, handle_epoch, lifecycle_epoch())
	):
		return
	var srv = ResonanceServerAccess.get_server()
	if srv == null or not srv.has_method("update_source"):
		return
	srv.update_source(handle, position, radius, use_sim_distance_attenuation, min_distance)


static func read_occlusion_linear_gain(handle: int, handle_epoch: int = 0) -> float:
	if handle < 0 or not server_ready():
		return 1.0
	if (
		handle_epoch != 0
		and not handle_matches_lifecycle_epoch(handle, handle_epoch, lifecycle_epoch())
	):
		return 1.0
	var srv = ResonanceServerAccess.get_server()
	if srv != null and srv.has_method("get_source_occlusion_linear_gain"):
		return float(srv.get_source_occlusion_linear_gain(handle))
	return 1.0


static func read_occlusion_dict(handle: int, handle_epoch: int = 0) -> Dictionary:
	if handle < 0 or not server_ready():
		return {}
	if (
		handle_epoch != 0
		and not handle_matches_lifecycle_epoch(handle, handle_epoch, lifecycle_epoch())
	):
		return {}
	var srv = ResonanceServerAccess.get_server()
	if srv == null or not srv.has_method("get_source_occlusion_data"):
		return {}
	var data: Variant = srv.get_source_occlusion_data(handle)
	if typeof(data) != TYPE_DICTIONARY:
		return {}
	return data as Dictionary


## TODO: Smooth occlusion/transmission (ResonancePlayer-style smoothing constants).
static func linear_gain_from_occlusion(occ: Dictionary) -> float:
	if occ.is_empty():
		return 1.0
	var occlusion: float = clampf(float(occ.get("occlusion", 0.0)), 0.0, 1.0)
	var distance_atten: float = maxf(0.0, float(occ.get("distance_attenuation", 1.0)))
	var tx_avg: float = _average_transmission(occ.get("transmission", null))
	return maxf(MIN_LINEAR_GAIN, (1.0 - occlusion) * tx_avg * distance_atten)


static func attenuation_db_from_linear_gain(linear_gain: float) -> float:
	return linear_to_db(maxf(MIN_LINEAR_GAIN, linear_gain))


static func attenuation_db_from_occlusion(occ: Dictionary) -> float:
	return linear_to_db(linear_gain_from_occlusion(occ))


## TODO: Air absorption bands instead of scalar transmission average.
static func _average_transmission(tx: Variant) -> float:
	if tx is PackedFloat32Array:
		var arr: PackedFloat32Array = tx as PackedFloat32Array
		if arr.size() >= 3:
			return (arr[0] + arr[1] + arr[2]) / 3.0
		if arr.size() > 0:
			return arr[0]
	if tx is Array:
		var a: Array = tx as Array
		if a.size() >= 3:
			return (float(a[0]) + float(a[1]) + float(a[2])) / 3.0
		if a.size() > 0:
			return float(a[0])
	return 1.0
