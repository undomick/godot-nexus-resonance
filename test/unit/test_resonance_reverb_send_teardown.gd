extends GutTest

## Freeing playing ResonancePlayers with a reverb send child must not crash the audio thread.

const PLAYER_COUNT := 8

var _root: Node3D


func before_each() -> void:
	_root = Node3D.new()
	add_child_autoqfree(_root)
	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	runtime.set("runtime", ResonanceRuntimeConfig.new())
	_root.add_child(runtime)
	var camera := Camera3D.new()
	camera.current = true
	_root.add_child(camera)
	var listener: Node = ClassDB.instantiate("ResonanceListener")
	listener.set("apply_reverb", true)
	camera.add_child(listener)
	await wait_process_frames(4)

func _make_tone_stream() -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 44100
	wav.stereo = false
	var frames: int = 44100
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, int(sin(float(i) * 0.05) * 12000.0))
	wav.data = data
	return wav


func _make_send_player() -> ResonancePlayer:
	var config := ResonancePlayerConfig.new()
	config.pathing_enabled_override = 0
	var player := ClassDB.instantiate("ResonancePlayer") as ResonancePlayer
	player.player_config = config
	player.stream = _make_tone_stream()
	_root.add_child(player)
	return player


func test_freeing_playing_send_players_keeps_the_audio_thread_alive() -> void:
	var players: Array[ResonancePlayer] = []
	for i in PLAYER_COUNT:
		players.append(_make_send_player())
	for player in players:
		player.play()
	await wait_process_frames(10)
	for player in players:
		assert_not_null(player.get_node_or_null("ResonanceReverbOutput"))
		player.queue_free()
	await wait_process_frames(10)
	for player in players:
		assert_false(is_instance_valid(player))
