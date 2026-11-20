extends RefCounted

## Closed test room, a looping tone and a reverb bus level probe for tests that listen to the wet path.

const WALL_THICKNESS := 0.2
const TONE_RATE := 44100
const TONE_HZ := 440.0
const TONE_AMPLITUDE := 10000.0


## Floor at y = 0, centred on the origin in x and z.
static func add_closed_room(parent: Node, size: Vector3) -> void:
	var half := size * 0.5
	_add_wall(parent, Vector3(size.x, WALL_THICKNESS, size.z), Vector3(0, 0, 0))
	_add_wall(parent, Vector3(size.x, WALL_THICKNESS, size.z), Vector3(0, size.y, 0))
	_add_wall(parent, Vector3(WALL_THICKNESS, size.y, size.z), Vector3(-half.x, half.y, 0))
	_add_wall(parent, Vector3(WALL_THICKNESS, size.y, size.z), Vector3(half.x, half.y, 0))
	_add_wall(parent, Vector3(size.x, size.y, WALL_THICKNESS), Vector3(0, half.y, -half.z))
	_add_wall(parent, Vector3(size.x, size.y, WALL_THICKNESS), Vector3(0, half.y, half.z))


static func make_looping_tone_stream() -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = TONE_RATE
	wav.stereo = false
	wav.loop_mode = AudioStreamWAV.LOOP_FORWARD
	var frames: int = TONE_RATE
	wav.loop_end = frames
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, int(sin(float(i) * TAU * TONE_HZ / TONE_RATE) * TONE_AMPLITUDE))
	wav.data = data
	return wav


static func peak_reverb_bus_db(tree: SceneTree, seconds: float) -> float:
	var bus := AudioServer.get_bus_index(ResonancePaths.DEFAULT_REVERB_BUS_NAME)
	var peak := -200.0
	var end_ms := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < end_ms:
		await tree.process_frame
		peak = maxf(peak, AudioServer.get_bus_peak_volume_left_db(bus, 0))
	return peak


static func _add_wall(parent: Node, size: Vector3, position: Vector3) -> void:
	var mesh := BoxMesh.new()
	mesh.size = size
	var wall := MeshInstance3D.new()
	wall.mesh = mesh
	wall.position = position
	parent.add_child(wall)
	wall.add_child(ClassDB.instantiate("ResonanceStaticGeometry"))
