extends SceneTree

## Quits the game while ResonancePlayers are in a given state. scripts/check_shutdown_leaks.sh runs it
## once per state and reflection type and fails on leak reports in Godot's exit output.
## Usage: godot --headless --audio-driver Dummy --path project -s res://test/smoke/run_shutdown_leak_smoke.gd
##        -- <state> <reflection_type>
## States: idle, just_started, playing, draining, soft_stopping, stopped, freed_players, prepared.

const Scene := preload("res://test/helpers/tail_drain_scene.gd")
const ReverbBusRoom := preload("res://test/helpers/reverb_bus_room.gd")

const SETTLE_FRAMES := 10
const PLAYER_COUNT := 6

var _state := "playing"
var _root: Node3D
var _players: Array[ResonancePlayer] = []


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		_state = args[0]
	var reflection_type := Scene.REFLECTION_CONVOLUTION
	if args.size() > 1:
		reflection_type = int(args[1])
	_run.call_deferred(reflection_type)


func _run(reflection_type: int) -> void:
	_root = Scene.build(root, reflection_type)
	await _frames(SETTLE_FRAMES)
	for i in PLAYER_COUNT:
		var looping := i % 2 == 0
		var stream: AudioStream = ReverbBusRoom.make_looping_tone_stream() if looping else Scene.make_clip(512)
		var player := Scene.make_player(_root, stream)
		player.max_polyphony = 4
		_players.append(player)
	await _frames(2)
	if _state != "idle":
		for player in _players:
			player.play()
	await _enter_state()
	print("shutdown smoke: quitting in state '%s' (reflection_type=%d)" % [_state, reflection_type])
	quit()


func _enter_state() -> void:
	match _state:
		"idle", "just_started":
			return
		"playing":
			await _seconds(0.5)
		"draining":
			await _seconds(0.2)
			for player in _players:
				player.stop()
				player.play()
			await _seconds(0.05)
			for i in _players.size():
				if i % 2 == 0:
					_players[i].stop()
		"soft_stopping":
			await _seconds(0.3)
			for player in _players:
				player.stop()
			await _frames(1)
		"stopped":
			await _seconds(0.3)
			for player in _players:
				player.stop()
			await _seconds(Scene.drain_deadline_sec())
		"freed_players":
			await _seconds(0.3)
			for player in _players:
				player.queue_free()
			await _frames(1)
		"prepared":
			await _seconds(0.3)
			_root.get_node("ResonanceRuntime").prepare_for_shutdown()
			await _seconds(0.2)
		_:
			push_error("unknown state '%s'" % _state)


func _frames(count: int) -> void:
	for i in count:
		await process_frame


func _seconds(seconds: float) -> void:
	var end_ms := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < end_ms:
		await process_frame
