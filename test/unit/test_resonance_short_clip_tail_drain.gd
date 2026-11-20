extends GutTest

## Clips shorter than, equal to and just over one audio block must emit `finished` once and stop after
## the tail drains, for every reflection type. TrueAudio Next falls back on machines without an AMD GPU.

const Scene := preload("res://test/helpers/tail_drain_scene.gd")

const CLIP_FRAMES := [0, 1, 63, 511, 512, 513, 1024, 4410]

var _root: Node3D
var _finished_count := 0


func after_each() -> void:
	if is_instance_valid(_root):
		_root.queue_free()
	await wait_process_frames(4)


func _cases() -> Array:
	var cases := []
	for reflection_type in Scene.ALL_REFLECTION_TYPES:
		for frames in CLIP_FRAMES:
			cases.append([reflection_type, frames])
	return cases


func _on_finished() -> void:
	_finished_count += 1


func _play_short_clip(reflection_type: int, frames: int) -> ResonancePlayer:
	_root = Scene.build(self, reflection_type)
	await wait_process_frames(6)
	var player := Scene.make_player(_root, Scene.make_clip(frames))
	_finished_count = 0
	player.finished.connect(_on_finished)
	await wait_process_frames(2)
	player.play()
	return player


func test_short_clip_finishes_once_and_stops(params = use_parameters(_cases())) -> void:
	var reflection_type: int = params[0]
	var frames: int = params[1]
	var player := await _play_short_clip(reflection_type, frames)
	var label := "type=%d frames=%d" % [reflection_type, frames]

	var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec())
	assert_true(stopped, "player stops after tail drain (%s)" % label)
	await wait_process_frames(4)
	assert_eq(_finished_count, 1, "finished fires exactly once (%s)" % label)
	assert_true(Scene.output_is_finite(player), "no NaN or Inf in the output (%s)" % label)

	await Scene.wait_seconds(get_tree(), 0.2)
	assert_false(player.playing, "player stays stopped (%s)" % label)
	assert_eq(_finished_count, 1, "no second finished after the tail (%s)" % label)


func test_short_clip_replays_after_drain(params = use_parameters(Scene.ALL_REFLECTION_TYPES)) -> void:
	var reflection_type: int = params
	var player := await _play_short_clip(reflection_type, 64)
	var runs := 3
	for run in runs:
		var stopped: bool = await Scene.wait_until_stopped(get_tree(), player, Scene.drain_deadline_sec())
		assert_true(stopped, "run %d stops (type=%d)" % [run, reflection_type])
		await wait_process_frames(2)
		if run + 1 < runs:
			player.play()
	assert_eq(_finished_count, runs, "finished once per run (type=%d)" % reflection_type)
