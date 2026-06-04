extends RefCounted
class_name ResonanceCodaBridge

## Connects Nexus Coda event voices to ResonanceServer source handles (Option A).
## Applies occlusion/transmission/distance as volume_db on Coda's AudioStreamPlayer pool.

const VoiceSyncScript := preload(
	"res://addons/nexus_resonance/scripts/resonance_coda_voice_sync.gd"
)

const _POS_EPS := 1e-4

var _coda: Node = null
var _runtime: Node = null
var _active: Dictionary = {}  # coda_handle_id -> Dictionary
var _warned_missing_coda: bool = false


func init(coda_runtime: Node, resonance_runtime: Node) -> bool:
	_coda = coda_runtime
	_runtime = resonance_runtime
	if _coda == null:
		return false
	if not _coda.has_signal("voice_started") or not _coda.has_signal("voice_finished"):
		push_warning(
			"ResonanceCodaBridge: Coda runtime missing voice_started/voice_finished signals."
		)
		return false
	if not _coda.voice_started.is_connected(_on_voice_started):
		_coda.voice_started.connect(_on_voice_started)
	if not _coda.voice_finished.is_connected(_on_voice_finished):
		_coda.voice_finished.connect(_on_voice_finished)
	return true


func shutdown() -> void:
	if _coda != null:
		if _coda.voice_started.is_connected(_on_voice_started):
			_coda.voice_started.disconnect(_on_voice_started)
		if _coda.voice_finished.is_connected(_on_voice_finished):
			_coda.voice_finished.disconnect(_on_voice_finished)
	for key in _active.keys():
		_release_entry(int(key))
	_active.clear()
	_coda = null
	_runtime = null


func is_active() -> bool:
	return _coda != null


func tick(_delta: float) -> void:
	if Engine.is_editor_hint() or _coda == null:
		return
	if not VoiceSyncScript.server_ready():
		return
	for key in _active.keys():
		_sync_entry(int(key))


func _on_voice_started(handle: Variant) -> void:
	if Engine.is_editor_hint() or handle == null:
		return
	var emitter: Node3D = _resolve_emitter(handle)
	if emitter == null:
		return
	if not VoiceSyncScript.server_ready():
		if not _warned_missing_coda:
			_warned_missing_coda = true
			push_warning("ResonanceCodaBridge: ResonanceServer not ready; spatial sync skipped.")
		return
	var res_handle: int = VoiceSyncScript.create_source_at(emitter.global_position)
	if res_handle < 0:
		return
	var handle_id: int = _coda_handle_id(handle)
	_active[handle_id] = {
		"coda_handle": handle,
		"emitter": emitter,
		"resonance_handle": res_handle,
		"last_pos": emitter.global_position,
	}
	_tag_players(handle, res_handle)
	_apply_occlusion_to_handle(handle, res_handle)


func _on_voice_finished(handle: Variant) -> void:
	if handle == null:
		return
	_release_entry(_coda_handle_id(handle))


func _sync_entry(handle_id: int) -> void:
	if not _active.has(handle_id):
		return
	var entry: Dictionary = _active[handle_id]
	var emitter: Node3D = entry.get("emitter") as Node3D
	if emitter == null or not is_instance_valid(emitter):
		_release_entry(handle_id)
		return
	var res_handle: int = int(entry.get("resonance_handle", -1))
	if res_handle < 0:
		return
	var pos: Vector3 = emitter.global_position
	var last_pos: Vector3 = entry.get("last_pos", Vector3(INF, INF, INF))
	var moved: bool = (pos - last_pos).length_squared() >= _POS_EPS * _POS_EPS
	if moved:
		entry["last_pos"] = pos
		_active[handle_id] = entry
		VoiceSyncScript.update_source_position(res_handle, pos)
	var coda_handle: Variant = entry.get("coda_handle")
	if moved:
		_apply_occlusion_to_handle(coda_handle, res_handle)


func _apply_occlusion_to_handle(coda_handle: Variant, res_handle: int) -> void:
	if coda_handle == null or not coda_handle.has_method("get_voice_players"):
		return
	var linear_gain: float = VoiceSyncScript.read_occlusion_linear_gain(res_handle)
	var atten_db: float = VoiceSyncScript.attenuation_db_from_linear_gain(linear_gain)
	var players: Array = coda_handle.get_voice_players()
	for p in players:
		var player: AudioStreamPlayer = p as AudioStreamPlayer
		if player == null or not is_instance_valid(player):
			continue
		player.set_meta(&"_coda_resonance_atten_db", atten_db)
		if not _player_uses_modulation_pipeline(coda_handle, player):
			var base_db: float = _base_volume_db_for_player(coda_handle, player)
			player.volume_db = base_db + atten_db


func _player_uses_modulation_pipeline(coda_handle: Variant, player: AudioStreamPlayer) -> bool:
	if coda_handle == null or not ("_player" in coda_handle):
		return false
	return coda_handle._player == player


func _base_volume_db_for_player(coda_handle: Variant, player: AudioStreamPlayer) -> float:
	if coda_handle == null:
		return player.volume_db
	if coda_handle.has_method("get_voice_players"):
		var primary: AudioStreamPlayer = null
		if "base_volume_db" in coda_handle:
			if coda_handle._player == player:
				return float(coda_handle.base_volume_db)
		for sib in coda_handle.get("graph_parallel_siblings", []):
			if sib != null and sib._player == player and "base_volume_db" in sib:
				return float(sib.base_volume_db)
	return float(coda_handle.get("base_volume_db", player.volume_db))


func _tag_players(coda_handle: Variant, res_handle: int) -> void:
	if coda_handle == null or not coda_handle.has_method("get_voice_players"):
		return
	for p in coda_handle.get_voice_players():
		var player: AudioStreamPlayer = p as AudioStreamPlayer
		if player != null and is_instance_valid(player):
			player.set_meta(&"_coda_resonance_handle", res_handle)
			player.set_meta(&"_coda_resonance_atten_db", 0.0)


func _untag_players(coda_handle: Variant) -> void:
	if coda_handle == null or not coda_handle.has_method("get_voice_players"):
		return
	for p in coda_handle.get_voice_players():
		var player: AudioStreamPlayer = p as AudioStreamPlayer
		if player != null and is_instance_valid(player):
			if player.has_meta(&"_coda_resonance_handle"):
				player.remove_meta(&"_coda_resonance_handle")
			if player.has_meta(&"_coda_resonance_atten_db"):
				player.remove_meta(&"_coda_resonance_atten_db")


func _release_entry(handle_id: int) -> void:
	if not _active.has(handle_id):
		return
	var entry: Dictionary = _active[handle_id]
	var coda_handle: Variant = entry.get("coda_handle")
	_untag_players(coda_handle)
	VoiceSyncScript.destroy_source(int(entry.get("resonance_handle", -1)))
	_active.erase(handle_id)


func _resolve_emitter(handle: Variant) -> Node3D:
	if handle == null:
		return null
	var params: Dictionary = {}
	if "params" in handle:
		params = handle.params as Dictionary
	if params.is_empty():
		return null
	var em: Variant = params.get("_coda_spatial_emitter", null)
	if em is NodePath:
		var path: NodePath = em as NodePath
		if _runtime != null and is_instance_valid(_runtime):
			var node: Node = _runtime.get_node_or_null(path)
			if node is Node3D:
				return node as Node3D
		if _coda != null and is_instance_valid(_coda):
			var node2: Node = _coda.get_node_or_null(path)
			if node2 is Node3D:
				return node2 as Node3D
	elif em is Node and is_instance_valid(em) and em is Node3D:
		return em as Node3D
	return null


func _coda_handle_id(handle: Variant) -> int:
	if handle == null:
		return -1
	if "id" in handle:
		return int(handle.id)
	return handle.get_instance_id()

## TODO: Timeline voices — one Resonance handle per lane player in timeline dispatchers.
## TODO: BLEND siblings — separate source handle per parallel graph voice.
## TODO: Map occlusion/transmission to Coda Get-Properties (handle.get_property via runtime).
## TODO: Wire Coda wet sends to Resonance reverb bus activator (global room tone).
## TODO: Event metadata — min/max distance, directivity from CodaBrowserNode.
## TODO: Phase B — route spatial events through ResonancePlayer DSP instead of volume_db hack.
