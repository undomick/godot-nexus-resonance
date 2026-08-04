extends Node

## Spawned by [ResonancePlayer] when [member ResonancePlayer.convert_anim_audio_runtime] is on.
## Runs the same conversion as the editor tool once per loaded scene (TYPE_AUDIO -> play_animation_audio_clip).

const _META_SCENE_DONE := &"__nexus_resonance_anim_audio_conv"


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	var parent_player = get_parent()
	if parent_player == null:
		return
	if not parent_player.get("player_config"):
		return
	var tree := get_tree()
	if tree == null:
		return
	var sc := tree.current_scene
	if sc == null:
		sc = tree.get_root()
	if sc == null:
		return
	if sc.get_meta(_META_SCENE_DONE, false):
		return
	var conv_path := "res://addons/nexus_resonance/editor/resonance_animation_audio_converter.gd"
	if not ResourceLoader.exists(conv_path):
		push_warning("Nexus Resonance: animation audio converter script missing: " + conv_path)
		return
	var Conv = load(conv_path)
	if Conv == null:
		push_warning("Nexus Resonance: failed to load animation audio converter")
		return
	Conv.convert_all_animation_players(sc)
	sc.set_meta(_META_SCENE_DONE, true)
