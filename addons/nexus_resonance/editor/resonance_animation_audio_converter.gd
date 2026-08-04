@tool
extends RefCounted

## Converts Animation.TYPE_AUDIO tracks that target a [ResonancePlayer] into TYPE_METHOD keys that call
## [method ResonancePlayer.play_animation_audio_clip]. Same logic as [member ResonancePlayer.convert_anim_audio_runtime] at runtime.

const _METHOD := &"play_animation_audio_clip"


static func scene_has_resonance_audio_tracks(root: Node) -> bool:
	if root == null:
		return false
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n is AnimationPlayer and _player_has_resonance_audio_track(n as AnimationPlayer):
			return true
		for c in n.get_children():
			stack.push_back(c)
	return false


static func _player_has_resonance_audio_track(ap: AnimationPlayer) -> bool:
	for lib_key in ap.get_animation_library_list():
		var lib: AnimationLibrary = ap.get_animation_library(lib_key)
		if lib == null:
			continue
		for anim_name in lib.get_animation_list():
			var anim: Animation = lib.get_animation(anim_name)
			if anim == null:
				continue
			for t in anim.get_track_count():
				if anim.track_get_type(t) != Animation.TYPE_AUDIO:
					continue
				var path: NodePath = anim.track_get_path(t)
				var target: Node = _resolve_track_node(ap, path)
				if target != null and target.get_class() == "ResonancePlayer":
					return true
	return false


static func convert_all_animation_players(root: Node) -> Dictionary:
	var result := {
		"animation_players": 0,
		"tracks_converted": 0,
		"skipped_blend": 0,
		"skipped_target": 0,
	}
	if root == null:
		return result
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n is AnimationPlayer:
			result.animation_players += 1
			_convert_one_player(n as AnimationPlayer, result)
		for c in n.get_children():
			stack.push_back(c)
	return result


static func _collect_audio_keys(anim: Animation, track_idx: int) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	var n: int = anim.track_get_key_count(track_idx)
	for k in range(n):
		(
			out
			. append(
				{
					"time": anim.track_get_key_time(track_idx, k),
					"stream": anim.audio_track_get_key_stream(track_idx, k),
					"start": anim.audio_track_get_key_start_offset(track_idx, k),
				}
			)
		)
	return out


static func _resolve_track_node(ap: AnimationPlayer, path: NodePath) -> Node:
	var rn: NodePath = ap.root_node
	var base_n: Node = (
		ap.get_node_or_null(NodePath("..")) if rn.is_empty() else ap.get_node_or_null(rn)
	)
	return base_n.get_node_or_null(path) if base_n else null


static func _convert_one_player(ap: AnimationPlayer, result: Dictionary) -> void:
	for lib_key in ap.get_animation_library_list():
		var lib: AnimationLibrary = ap.get_animation_library(lib_key)
		if lib == null:
			continue
		for anim_name in lib.get_animation_list():
			var anim: Animation = lib.get_animation(anim_name)
			if anim == null:
				continue
			var t: int = anim.get_track_count() - 1
			while t >= 0:
				if anim.track_get_type(t) != Animation.TYPE_AUDIO:
					t -= 1
					continue
				var path: NodePath = anim.track_get_path(t)
				var target: Node = _resolve_track_node(ap, path)
				if target == null or target.get_class() != "ResonancePlayer":
					result.skipped_target += 1
					t -= 1
					continue
				if anim.audio_track_is_use_blend(t):
					push_warning(
						(
							"Nexus Resonance: skipped audio track with use_blend (library '%s', anim '%s')."
							% [String(lib_key), String(anim_name)]
						)
					)
					result.skipped_blend += 1
					t -= 1
					continue
				var keys: Array[Dictionary] = _collect_audio_keys(anim, t)
				var insert_pos: int = t
				anim.remove_track(t)
				var new_track: int = anim.add_track(Animation.TYPE_METHOD, insert_pos)
				anim.track_set_path(new_track, path)
				for kd in keys:
					var stream: Resource = kd.get("stream") as Resource
					if stream == null:
						continue
					var from_pos: float = float(kd.get("start", 0.0))
					var method_key := {"method": _METHOD, "args": [stream, from_pos]}
					anim.track_insert_key(new_track, float(kd["time"]), method_key)
				result.tracks_converted += 1
				t = anim.get_track_count() - 1


## Loads one PackedScene from disk, converts in memory, and saves only if tracks_converted is greater than zero.
static func convert_packed_scene_file_on_disk(scene_path: String) -> Dictionary:
	var out := {
		"tracks_converted": 0,
		"animation_players": 0,
		"skipped_blend": 0,
		"skipped_target": 0,
		"saved": false,
		"error": "",
	}
	if not scene_path.begins_with("res://") or not scene_path.ends_with(".tscn"):
		out.error = "invalid_path"
		return out
	var packed: PackedScene = load(scene_path) as PackedScene
	if packed == null:
		out.error = "load_failed"
		return out
	var root: Node = packed.instantiate()
	if root == null:
		out.error = "instantiate_failed"
		return out
	var r: Dictionary = convert_all_animation_players(root)
	out["tracks_converted"] = int(r.get("tracks_converted", 0))
	out["animation_players"] = int(r.get("animation_players", 0))
	out["skipped_blend"] = int(r.get("skipped_blend", 0))
	out["skipped_target"] = int(r.get("skipped_target", 0))
	if int(out["tracks_converted"]) > 0:
		var new_packed := PackedScene.new()
		var pack_err: Error = new_packed.pack(root)
		root.queue_free()
		if pack_err != OK:
			out.error = "pack_failed"
			return out
		var save_err: Error = ResourceSaver.save(new_packed, scene_path)
		if save_err != OK:
			out.error = "save_failed"
			return out
		out.saved = true
	else:
		root.queue_free()
	return out


## Converts every .tscn path in [param paths] on disk. Returns aggregate counts for the editor toast.
static func convert_packed_scene_files_on_disk(paths: PackedStringArray) -> Dictionary:
	var summary := {
		"tracks_converted": 0,
		"scenes_saved": 0,
		"scenes_failed": 0,
		"skipped_blend": 0,
		"skipped_target": 0,
	}
	for p in paths:
		var one: Dictionary = convert_packed_scene_file_on_disk(String(p))
		if not String(one.get("error", "")).is_empty():
			summary.scenes_failed += 1
			push_warning(
				(
					"Nexus Resonance: Convert scene failed (%s): %s"
					% [String(p), String(one.get("error", ""))]
				)
			)
			continue
		summary.tracks_converted += int(one.get("tracks_converted", 0))
		summary.skipped_blend += int(one.get("skipped_blend", 0))
		summary.skipped_target += int(one.get("skipped_target", 0))
		if one.get("saved", false):
			summary.scenes_saved += 1
	return summary
