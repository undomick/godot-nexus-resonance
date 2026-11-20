extends GutTest

## With the direct path and the reverb send muted, every reflection type must put audible wet signal on
## the player's bus while it plays, and the bus must fall silent once the player has drained.

const Scene := preload("res://test/helpers/tail_drain_scene.gd")
const ReverbBusRoom := preload("res://test/helpers/reverb_bus_room.gd")

const AUDIBLE_DB := -60.0
const SILENT_DB := -90.0
const LISTEN_SECONDS := 1.0

var _root: Node3D


func after_each() -> void:
	if is_instance_valid(_root):
		_root.queue_free()
	await wait_process_frames(4)


func _peak_master_db(seconds: float) -> float:
	var peak := -200.0
	var end_ms := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < end_ms:
		await get_tree().process_frame
		peak = maxf(peak, AudioServer.get_bus_peak_volume_left_db(0, 0))
		peak = maxf(peak, AudioServer.get_bus_peak_volume_right_db(0, 0))
	return peak


func _make_wet_only_player(stream: AudioStream) -> ResonancePlayer:
	var player := Scene.make_player(_root, stream)
	var config := player.player_config
	config.direct_mix_level = 0.0
	config.reverb_send = false
	player.player_config = config
	return player


func test_wet_signal_audible_and_silent_after_drain(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	_root = Scene.build(self, params)
	await wait_process_frames(6)
	var player := _make_wet_only_player(ReverbBusRoom.make_looping_tone_stream())
	await wait_process_frames(2)
	player.play()
	var playing_peak := await _peak_master_db(LISTEN_SECONDS)
	assert_gt(playing_peak, AUDIBLE_DB, "reflections are audible (type=%d, peak %.1f dB)" % [params, playing_peak])
	assert_true(Scene.output_is_finite(player))

	player.stop()
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec())
	assert_true(stopped, "player drains after stop (type=%d)" % params)
	await Scene.wait_seconds(get_tree(), 0.3)
	var after_peak := await _peak_master_db(0.3)
	assert_lt(after_peak, SILENT_DB, "bus is silent after the drain (type=%d, peak %.1f dB)" % [params, after_peak])
