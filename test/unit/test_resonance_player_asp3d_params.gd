extends GutTest

## Exposed AudioStreamPlayer3D knobs on ResonancePlayer (with player_config).
## Volume/max_db: source loudness before Steam DSP (see get_effective_volume_linear_cached).
## pitch_scale / playing / autoplay / stream_paused / max_polyphony: Godot transport.

var _player: ResonancePlayer
var _runtime: Node
var _config: ResonancePlayerConfig


func before_each() -> void:
	_config = ResonancePlayerConfig.new()
	# Keep sim light for unit tests (distance only; no reflections/pathing needed for transport knobs).
	_config.reflections_enabled = 0
	_config.pathing_enabled_override = 0

	_runtime = ClassDB.instantiate("ResonanceRuntime")
	add_child_autoqfree(_runtime)

	_player = ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	_player.player_config = _config
	_player.stream = _make_tone_stream()
	add_child_autoqfree(_player)
	# Volume cache and sim push live in Node._process (not physics).
	await wait_process_frames(2)


func after_each() -> void:
	if not is_instance_valid(_player):
		return
	# Avoid freeing while soft-stop keeps audio-thread playbacks alive (can SIGSEGV on exit).
	if _player.player_config != null:
		_player.player_config = null
	if _player.playing or _player.is_playing():
		_player.stop()
	await wait_process_frames(2)


func _make_tone_stream() -> AudioStreamWAV:
	# One second of mono PCM so play/stop/pause do not depend on AudioStreamGenerator fill.
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 44100
	wav.stereo = false
	var frames: int = 44100
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		var sample: int = int(sin(float(i) * 0.05) * 12000.0)
		data.encode_s16(i * 2, sample)
	wav.data = data
	return wav


func _expected_effective_linear(volume_db: float, max_db: float) -> float:
	return db_to_linear(minf(volume_db, max_db))


func test_volume_db_and_max_db_feed_host_gain_after_steam_not_ipl() -> void:
	_player.volume_db = -6.0
	_player.max_db = 3.0
	await wait_process_frames(2)

	var expected: float = _expected_effective_linear(-6.0, 3.0)
	assert_almost_eq(_player.get_effective_volume_linear_cached(), expected, 0.001,
		"volume_db should update source-loudness cache (pre-Steam)")

	var inst: Dictionary = _player.get_audio_instrumentation()
	assert_true(inst.has("godot_volume_db"), "instrumentation should snapshot volume_db")
	assert_almost_eq(float(inst["godot_volume_db"]), -6.0, 0.001)
	assert_almost_eq(float(inst["godot_max_db"]), 3.0, 0.001)
	assert_almost_eq(float(inst["effective_volume_linear"]), expected, 0.001)

	# max_db ceiling: volume above max must not raise host gain above max_db.
	_player.volume_db = 12.0
	_player.max_db = 0.0
	await wait_process_frames(2)
	assert_almost_eq(_player.get_effective_volume_linear_cached(), 1.0, 0.001,
		"max_db must ceiling volume_db for host gain")


func test_pitch_scale_roundtrip_and_instrumentation() -> void:
	_player.pitch_scale = 1.5
	assert_almost_eq(_player.pitch_scale, 1.5, 0.001)
	await wait_process_frames(1)
	var inst: Dictionary = _player.get_audio_instrumentation()
	assert_almost_eq(float(inst.get("godot_pitch_scale", 0.0)), 1.5, 0.001,
		"pitch_scale should remain Godot transport (rate_scale into decoder), visible in instrumentation")


func test_playing_via_play_and_stop() -> void:
	assert_false(_player.playing, "starts stopped")
	_player.play()
	await wait_process_frames(2)
	assert_true(_player.playing or _player.is_playing(), "play() should mark the player playing")
	_player.stop()
	# Soft-stop drains on the audio thread; Dummy driver may need the main-thread watchdog
	# (max_reverb_duration + 0.5s). Poll with process frames.
	var settled := false
	for _i in 360:
		await wait_process_frames(1)
		if not _player.playing:
			settled = true
			break
	assert_true(settled, "after stop(), playing should become false (tail drain or soft-stop watchdog)")


func test_autoplay_starts_on_ready() -> void:
	var p: ResonancePlayer = ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	p.player_config = ResonancePlayerConfig.new()
	p.stream = _make_tone_stream()
	p.autoplay = true
	add_child_autoqfree(p)
	await wait_process_frames(3)
	assert_true(p.playing or p.is_playing(), "autoplay with player_config should start playback in _ready")
	p.stop()


func test_stream_paused_pauses_and_resumes() -> void:
	_player.play()
	await wait_process_frames(2)
	assert_true(_player.is_playing() or _player.playing)

	_player.stream_paused = true
	await wait_process_frames(2)
	assert_true(_player.stream_paused, "stream_paused should stick")

	_player.stream_paused = false
	await wait_process_frames(2)
	assert_false(_player.stream_paused, "clearing stream_paused should resume")
	assert_true(_player.is_playing() or _player.playing, "playback should still be active after unpause")


func test_max_polyphony_property_and_overlapping_voices() -> void:
	_player.max_polyphony = 3
	assert_eq(_player.max_polyphony, 3)

	_player.play()
	await wait_process_frames(1)
	_player.play()
	await wait_process_frames(1)
	_player.play()
	await wait_process_frames(3)

	var inst: Dictionary = _player.get_audio_instrumentation()
	assert_true(inst.has("polyphony_voice_count") or inst.has("godot_max_db"),
		"with config, instrumentation should be non-empty while playing")
	if inst.has("polyphony_voice_count"):
		var voices: int = int(inst["polyphony_voice_count"])
		assert_gt(voices, 0, "at least one Resonance voice while playing")
		assert_lte(voices, 3, "voice count must not exceed max_polyphony")
