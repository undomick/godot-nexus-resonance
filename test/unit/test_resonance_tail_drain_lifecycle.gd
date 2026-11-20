extends GutTest

## Interrupting a draining player (stop, replay, free, runtime teardown) must never leave it playing
## forever, emit `finished` twice for one run or crash the audio thread.

const Scene := preload("res://test/helpers/tail_drain_scene.gd")

const SHORT_FRAMES := 256
const LONG_FRAMES := 44100
const POLYPHONY := 8

var _root: Node3D
var _finished_count := 0


func after_each() -> void:
	if is_instance_valid(_root):
		_root.queue_free()
	await wait_process_frames(4)


func _on_finished() -> void:
	_finished_count += 1


func _setup(reflection_type: int, frames: int) -> ResonancePlayer:
	_root = Scene.build(self, reflection_type)
	await wait_process_frames(6)
	var player := Scene.make_player(_root, Scene.make_clip(frames))
	_finished_count = 0
	player.finished.connect(_on_finished)
	await wait_process_frames(2)
	return player


## Returns once `finished` fired, so the player is in its wet tail drain.
func _wait_for_finished(timeout_sec: float) -> bool:
	var end_ms := Time.get_ticks_msec() + int(timeout_sec * 1000.0)
	while Time.get_ticks_msec() < end_ms:
		if _finished_count > 0:
			return true
		await get_tree().process_frame
	return _finished_count > 0


func test_stop_during_tail_drain_stops(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var player := await _setup(params, SHORT_FRAMES)
	player.play()
	assert_true(await _wait_for_finished(Scene.drain_deadline_sec()), "finished fires")
	player.stop()
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec())
	assert_true(stopped, "stop() during tail drain stops the player (type=%d)" % params)
	await wait_process_frames(4)
	assert_eq(_finished_count, 1)


func test_stop_mid_clip_drains_without_finished(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var player := await _setup(params, LONG_FRAMES)
	player.play()
	await Scene.wait_seconds(get_tree(), 0.2)
	player.stop()
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec())
	assert_true(stopped, "soft stop finishes its drain (type=%d)" % params)
	await wait_process_frames(4)
	assert_eq(_finished_count, 0, "stop() never emits finished")


func test_replay_during_tail_drain(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var player := await _setup(params, SHORT_FRAMES)
	player.play()
	assert_true(await _wait_for_finished(Scene.drain_deadline_sec()), "first run finishes")
	player.play()
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec() * 2.0)
	assert_true(stopped, "second run stops (type=%d)" % params)
	await wait_process_frames(4)
	assert_eq(_finished_count, 2, "finished once per run")


func test_replay_from_finished_handler(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var player := await _setup(params, SHORT_FRAMES)
	var replays := 3
	var on_finished := func() -> void:
		if _finished_count <= replays:
			player.play()
	player.finished.connect(on_finished)
	player.play()
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec() * (replays + 1))
	assert_true(stopped, "chain of replays ends (type=%d)" % params)
	await wait_process_frames(4)
	assert_eq(_finished_count, replays + 1, "finished once per run")


func test_rapid_polyphonic_short_clips_all_drain(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var player := await _setup(params, SHORT_FRAMES)
	player.max_polyphony = POLYPHONY
	for i in POLYPHONY * 2:
		player.play()
		await get_tree().process_frame
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec() * 2.0)
	assert_true(stopped, "all voices drain (type=%d)" % params)
	assert_true(Scene.output_is_finite(player))


func test_free_during_tail_drain(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var player := await _setup(params, SHORT_FRAMES)
	player.play()
	assert_true(await _wait_for_finished(Scene.drain_deadline_sec()), "finished fires")
	player.queue_free()
	await wait_process_frames(10)
	assert_false(is_instance_valid(player))


func test_runtime_freed_during_tail_drain(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var player := await _setup(params, SHORT_FRAMES)
	player.play()
	assert_true(await _wait_for_finished(Scene.drain_deadline_sec()), "finished fires")
	_root.get_node("ResonanceRuntime").queue_free()
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec())
	assert_true(stopped, "draining player stops once the runtime is gone (type=%d)" % params)


func test_reflection_type_switch_during_tail_drain() -> void:
	var player := await _setup(Scene.REFLECTION_CONVOLUTION, SHORT_FRAMES)
	player.play()
	assert_true(await _wait_for_finished(Scene.drain_deadline_sec()), "finished fires")
	var runtime: Node = _root.get_node("ResonanceRuntime")
	var config: ResonanceRuntimeConfig = runtime.get("runtime")
	config.reflection_type = Scene.REFLECTION_PARAMETRIC
	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec() * 2.0)
	assert_true(stopped, "drain survives an engine reinit")
	player.play()
	stopped = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec() * 2.0)
	assert_true(stopped, "player plays and drains again after the switch")
	assert_eq(_finished_count, 2)
